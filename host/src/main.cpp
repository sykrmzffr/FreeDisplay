// FreeDisplay host: streams a Windows monitor to an Android tablet over USB (adb)
// and injects the tablet's touch and pen input back into Windows.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mfapi.h>
#include <timeapi.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "Adb.h"
#include "Capture.h"
#include "Encoder.h"
#include "NvencEncoder.h"
#include "Input.h"
#include "Driver.h"
#include "Installer.h"
#include "Protocol.h"
#include "Settings.h"
#include "Tray.h"
#include "Util.h"
#include "VirtualMonitor.h"
#include <shellapi.h>
#include <share.h>

namespace {

constexpr const wchar_t* kPackage = L"com.freedisplay.client";
constexpr const wchar_t* kActivity = L"com.freedisplay.client/.MainActivity";
constexpr int kAppVersionCode = 11; // keep in sync with android/app/build.gradle.kts

struct Options {
    int display = -1;     // -1: auto (VDD monitor, else primary)
    UINT fps = 120;       // upper bound; the tablet's refresh rate also caps it
    UINT bitrateMbps = 0; // 0: automatic
    int scalePct = -1;    // encode size as % of the monitor; -1: the tray's Quality setting
    int speed = -1;       // encoder quality/speed 0 (fastest) .. 100 (best); -1: default
    std::wstring encoder = L"auto"; // auto | amf | nvenc | mf
    bool install = true;
};

// Picks the lowest-latency encoder available for the capture GPU, falling back
// to Media Foundation, which works with any vendor's hardware encoder.
std::unique_ptr<VideoEncoder> OpenEncoder(const Options& opt, Capture& cap, UINT w, UINT h,
                                          UINT fps, UINT bitrate) {
    std::vector<std::unique_ptr<VideoEncoder>> candidates;
    bool amd = cap.AdapterVendor() == 0x1002;
    bool nvidia = cap.AdapterVendor() == 0x10DE;
    bool any = opt.encoder == L"auto";
    if (opt.encoder == L"amf" || (any && amd)) candidates.push_back(CreateAmfEncoder());
    if (opt.encoder == L"nvenc" || (any && nvidia)) candidates.push_back(std::make_unique<NvencEncoder>());
    if (opt.encoder == L"mf" || any) candidates.push_back(std::make_unique<MfEncoder>());
    for (auto& enc : candidates) {
        if (enc->Init(cap.Device(), cap.AdapterLuid(), w, h, fps, bitrate, opt.speed)) {
            Log("Encoder: %s", enc->Name());
            return std::move(enc);
        }
        Log("%s encoder unavailable, trying the next one", enc->Name());
    }
    return nullptr;
}

std::atomic<bool> g_quit{false};
std::atomic<bool> g_restart{false}; // end the current session so it restarts with new settings
std::atomic<uint64_t> g_displayChangedAt{0}; // last WM_DISPLAYCHANGE (NowUs), 0 when handled
DWORD g_uiThread = 0;
Tray* g_tray = nullptr;

void Status(const std::wstring& text, bool notify = false) {
    if (g_tray) g_tray->SetStatus(text, notify);
}

BOOL WINAPI OnCtrl(DWORD) {
    g_quit = true;
    PostThreadMessageW(g_uiThread, WM_QUIT, 0, 0);
    return TRUE;
}

// ---- socket helpers -------------------------------------------------------

bool SendAll(SOCKET s, const void* data, size_t len) {
    auto p = static_cast<const char*>(data);
    while (len > 0) {
        int n = send(s, p, int(std::min<size_t>(len, 1 << 20)), 0);
        if (n <= 0) return false;
        p += n;
        len -= size_t(n);
    }
    return true;
}

bool RecvAll(SOCKET s, void* data, size_t len) {
    auto p = static_cast<char*>(data);
    while (len > 0) {
        int n = recv(s, p, int(len), 0);
        if (n <= 0) return false;
        p += n;
        len -= size_t(n);
    }
    return true;
}

bool SendMsg(SOCKET s, uint8_t type, const std::vector<uint8_t>& payload) {
    uint8_t hdr[5] = {type, uint8_t(payload.size() >> 24), uint8_t(payload.size() >> 16),
                      uint8_t(payload.size() >> 8), uint8_t(payload.size())};
    return SendAll(s, hdr, 5) && SendAll(s, payload.data(), payload.size());
}

bool RecvMsg(SOCKET s, uint8_t& type, std::vector<uint8_t>& payload) {
    uint8_t hdr[5];
    if (!RecvAll(s, hdr, 5)) return false;
    type = hdr[0];
    uint32_t len = (uint32_t(hdr[1]) << 24) | (uint32_t(hdr[2]) << 16) | (uint32_t(hdr[3]) << 8) | hdr[4];
    if (len > (1u << 20)) return false;
    payload.resize(len);
    return len == 0 || RecvAll(s, payload.data(), len);
}

SOCKET Listen(uint16_t port) {
    // Not inheritable: child processes (the adb server) must not keep the port open.
    SOCKET s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET) return s;
    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char*>(&exclusive), sizeof exclusive);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(s, 1) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

SOCKET AcceptWithTimeout(SOCKET listener, int timeoutMs) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(listener, &set);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    if (select(0, &set, nullptr, nullptr, &tv) <= 0) return INVALID_SOCKET;
    SOCKET c = accept(listener, nullptr, nullptr);
    if (c != INVALID_SOCKET) {
        BOOL nodelay = TRUE;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&nodelay), sizeof nodelay);
        int sndbuf = 4 * 1024 * 1024;
        setsockopt(c, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char*>(&sndbuf), sizeof sndbuf);
    }
    return c;
}

// ---- device setup ---------------------------------------------------------

int InstalledVersionCode(const Adb& adb, const std::string& serial) {
    std::string out;
    adb.RunOn(serial, std::wstring(L"shell dumpsys package ") + kPackage, &out);
    auto pos = out.find("versionCode=");
    return pos == std::string::npos ? -1 : atoi(out.c_str() + pos + 12);
}

bool PrepareDevice(const Adb& adb, const std::string& serial, const Options& opt) {
    if (opt.install) {
        int installed = InstalledVersionCode(adb, serial);
        if (installed != kAppVersionCode) {
            std::wstring apk = installer::PayloadDir() + L"FreeDisplay.apk";
            if (GetFileAttributesW(apk.c_str()) == INVALID_FILE_ATTRIBUTES) {
                Log("FreeDisplay.apk not found; install the tablet app manually");
            } else {
                Log("Installing tablet app (installed version %d, need %d)...", installed, kAppVersionCode);
                std::string out;
                int rc = adb.RunOn(serial, L"install -r -g \"" + apk + L"\"", &out, 120000);
                // Builds up to 1.1.1 were signed with a development key; Android refuses to
                // update an app whose signing key changed, so replace it (it keeps no data).
                if (rc != 0 && (out.find("INSTALL_FAILED_UPDATE_INCOMPATIBLE") != std::string::npos ||
                                out.find("signatures do not match") != std::string::npos)) {
                    Log("Tablet app was signed with another key; reinstalling it");
                    adb.RunOn(serial, std::wstring(L"uninstall ") + kPackage, &out);
                    rc = adb.RunOn(serial, L"install -g \"" + apk + L"\"", &out, 120000);
                }
                Log("adb install: %s", rc == 0 ? "ok" : out.c_str());
                if (rc != 0) return false;
            }
        }
    }
    std::string out;
    std::wstring port = std::to_wstring(proto::kPort);
    if (adb.RunOn(serial, L"reverse tcp:" + port + L" tcp:" + port, &out) != 0) {
        Log("adb reverse failed: %s", out.c_str());
        return false;
    }
    adb.RunOn(serial, std::wstring(L"shell am start -n ") + kActivity, &out);
    return true;
}

// ---- streaming session ----------------------------------------------------

UINT AutoBitrate(UINT w, UINT h, UINT fps) {
    double bps = double(w) * h * fps * 0.07;
    return UINT(std::clamp(bps, 8e6, 40e6));
}

void RunSession(SOCKET sock, const Options& opt) {
    uint8_t type;
    std::vector<uint8_t> payload;
    if (!RecvMsg(sock, type, payload) || type != proto::kHello) {
        Log("Tablet did not say hello");
        return;
    }
    proto::Reader hello(payload.data(), payload.size());
    uint32_t magic = hello.u32();
    uint16_t version = hello.u16();
    uint32_t tabW = hello.u32(), tabH = hello.u32(), dpi = hello.u32();
    uint32_t tabHz = hello.u32();
    uint32_t maxDecW = hello.u32(), maxDecH = hello.u32();
    std::string name = hello.rest();
    if (magic != proto::kMagic || version != proto::kVersion) {
        Log("Protocol mismatch (magic %08X version %u)", magic, version);
        return;
    }
    Log("Tablet: %s, %ux%u @ %u dpi, up to %u Hz, decodes up to %ux%u", name.c_str(), tabW, tabH, dpi, tabHz,
        maxDecW, maxDecH);

    // Orientation setting: the tablet rotates (and reconnects) if it does not match.
    // Skip a few sessions while it turns, but never wait for it forever.
    static int wrongWayCount = 0;
    const int orient = AppSettings().orientation;
    proto::Writer ow;
    ow.u8(uint8_t(orient));
    if (!SendMsg(sock, proto::kOrientation, ow.buf)) return;
    bool tabletPortrait = tabH > tabW;
    if (orient != proto::kOrientAuto && tabletPortrait != (orient == proto::kOrientPortrait) && wrongWayCount < 3) {
        wrongWayCount++;
        Log("Waiting for the tablet to turn %s", orient == proto::kOrientPortrait ? "portrait" : "landscape");
        Sleep(1500); // the tablet closes this connection when it rotates
        return;
    }
    wrongWayCount = 0;

    OutputInfo output;
    if (!ChooseOutput(opt.display, output)) {
        Log("No display to capture");
        return;
    }
    // In Duplicate mode the VDD monitor shares an output with the main screen, which
    // then reports the VDD name too; only the VDD's own output is the virtual monitor.
    const std::wstring vddName = VirtualMonitor::Find();
    bool isVdd = output.monitorName.find(L"VDD") != std::wstring::npos && output.gdiName == vddName;
    if (!isVdd && opt.display < 0) {
        bool duplicate = !vddName.empty() && output.monitorName.find(L"VDD") != std::wstring::npos;
        Log("Mirroring %s (%s)", Narrow(output.gdiName).c_str(),
            duplicate ? "Duplicate mode" : vddName.empty() ? "no Virtual Display Driver monitor" : "VDD monitor off");
        Status(duplicate         ? L"Duplicate mode: mirroring the main screen. Press Win+P > Extend for a second screen"
               : vddName.empty() ? L"Mirroring the main screen: install Virtual Display Driver to extend"
                                 : L"Mirroring the main screen: enable the Virtual Display Driver monitor to extend",
               true);
    }

    // Run the virtual monitor and the stream at the tablet's refresh rate (capped by --fps).
    UINT fps = std::min<UINT>(opt.fps, tabHz ? tabHz : 60);
    // Battery saver: 60 Hz monitor and stream. Changing it restarts the session, so
    // the refresh rate is only ever switched here, before capture starts.
    const bool saver = AppSettings().SaverActive();
    if (saver) fps = std::min(fps, 60u);
    // Shape the virtual monitor like the tablet (portrait or landscape, closest
    // resolution) at the stream rate. Only here, before capture starts.
    if (isVdd && VirtualMonitor::MatchTablet(output.gdiName, tabW, tabH, fps)) {
        Sleep(300);
        OutputInfo changed;
        if (ChooseOutput(opt.display, changed)) output = changed;
    }
    // A tablet whose exact size the virtual monitor lacks gets a scaled picture; its
    // size is added to the driver settings so it fits exactly after a restart.
    bool restartForExactFit = false;
    if (isVdd) {
        auto [modeW, modeH] = driver::ModeFor(tabW, tabH);
        UINT wantW = tabletPortrait ? modeH : modeW, wantH = tabletPortrait ? modeW : modeH;
        restartForExactFit = !VirtualMonitor::HasMode(output.gdiName, wantW, wantH) &&
                             driver::AddResolution(tabW, tabH);
    }

    int scalePct = opt.scalePct > 0 ? opt.scalePct : AppSettings().qualityPct.load();
    UINT w = UINT((output.desktop.right - output.desktop.left) * scalePct / 100) & ~1u;
    UINT h = UINT((output.desktop.bottom - output.desktop.top) * scalePct / 100) & ~1u;
    // Stay within what the tablet's decoder accepts (some stop at 4096x2176).
    if (maxDecW && maxDecH && (w > maxDecW || h > maxDecH)) {
        double f = std::min(double(maxDecW) / w, double(maxDecH) / h);
        w = UINT(w * f) & ~1u;
        h = UINT(h * f) & ~1u;
    }
    UINT bitrate = opt.bitrateMbps ? opt.bitrateMbps * 1000000 : AutoBitrate(w, h, std::min(fps, 60u));

    Capture cap;
    Input input;
    if (!cap.Init(output, w, h)) {
        Log("Capture setup failed");
        return;
    }
    std::unique_ptr<VideoEncoder> enc = OpenEncoder(opt, cap, w, h, fps, bitrate);
    if (!enc) {
        Log("Encoder setup failed");
        return;
    }
    if (!input.Init()) Log("Input injection unavailable; display only");
    input.SetTarget(output.desktop);

    proto::Writer cfg;
    cfg.u32(w);
    cfg.u32(h);
    cfg.u32(fps); // the tablet matches its screen refresh to this
    if (!SendMsg(sock, proto::kVideoConfig, cfg.buf)) return;
    Status(L"Streaming to " + Widen(name) + L" (" + std::to_wstring(w) + L"x" + std::to_wstring(h) + L" @ " +
               std::to_wstring(fps) + L" Hz" + (saver ? L", battery saver" : L"") + L")" +
               (restartForExactFit ? L". Restart Windows once for a sharper, exact fit on this tablet" : L""),
           true);
    g_restart = false;

    // Per-frame timestamps (host clock, microseconds) for the latency breakdown.
    struct Timing {
        uint64_t captured = 0, encoded = 0, sent = 0;
    };
    std::mutex timingMu;
    std::map<uint64_t, Timing> timings; // keyed by pts
    struct Totals {
        uint64_t n = 0, encode = 0, send = 0, transit = 0, wait = 0, decode = 0, total = 0, worst = 0;
        uint64_t pings = 0, rtt = 0;
    } totals;

    std::atomic<bool> alive{true};
    std::atomic<bool> keyRequested{true};
    std::atomic<uint64_t> lastHeard{NowUs()}; // the tablet answers pings every 500 ms
    std::thread reader([&] {
        uint8_t t;
        std::vector<uint8_t> p;
        while (alive && RecvMsg(sock, t, p)) {
            lastHeard = NowUs();
            if (t == proto::kInput) {
                proto::InputEvent ev;
                if (proto::ParseInput(p, ev)) input.Handle(ev);
            } else if (t == proto::kRequestKeyframe) {
                keyRequested = true;
            } else if (t == proto::kFrameStats) {
                uint64_t ackAt = NowUs();
                proto::Reader r(p.data(), p.size());
                uint64_t pts = (uint64_t(r.u32()) << 32) | r.u32();
                uint32_t decodeUs = r.u32();
                uint32_t waitUs = r.u32();
                std::lock_guard<std::mutex> lock(timingMu);
                auto it = timings.find(pts);
                if (it == timings.end() || !it->second.sent) continue;
                const Timing& tm = it->second;
                uint64_t afterSend = ackAt - tm.sent;
                uint64_t onTablet = uint64_t(decodeUs) + waitUs;
                totals.n++;
                totals.encode += tm.encoded - tm.captured;
                totals.send += tm.sent - tm.encoded;
                totals.decode += decodeUs;
                totals.wait += waitUs;
                totals.transit += afterSend > onTablet ? afterSend - onTablet : 0;
                totals.total += ackAt - tm.captured;
                totals.worst = std::max(totals.worst, ackAt - tm.captured);
                timings.erase(timings.begin(), std::next(it));
            } else if (t == proto::kPong) {
                proto::Reader r(p.data(), p.size());
                uint64_t sentAt = (uint64_t(r.u32()) << 32) | r.u32();
                std::lock_guard<std::mutex> lock(timingMu);
                totals.pings++;
                totals.rtt += NowUs() - sentAt;
            } else if (t == proto::kDecoderInfo) {
                Log("Tablet decoder: %.*s", int(p.size()), reinterpret_cast<const char*>(p.data()));
            }
        }
        alive = false;
    });

    uint64_t start = NowUs();
    uint64_t lastSubmit = 0;
    uint64_t frames = 0, bytes = 0, statsAt = NowUs();
    bool sendFailed = false;
    auto onOutput = [&](const uint8_t* data, size_t size, uint64_t ptsUs, bool key) {
        {
            std::lock_guard<std::mutex> lock(timingMu);
            auto it = timings.find(ptsUs);
            if (it != timings.end()) it->second.encoded = NowUs();
        }
        proto::Writer w;
        w.buf.reserve(size + 9);
        w.u64(ptsUs);
        w.u8(key ? 1 : 0);
        w.bytes(data, size);
        if (!SendMsg(sock, proto::kVideoFrame, w.buf)) sendFailed = true;
        {
            std::lock_guard<std::mutex> lock(timingMu);
            auto it = timings.find(ptsUs);
            if (it != timings.end()) it->second.sent = NowUs();
        }
        frames++;
        bytes += size;
    };

    // Submit a changed desktop at most once per frame interval; a change that
    // arrives early stays pending instead of being dropped.
    const uint64_t minIntervalUs = 1000000 / fps * 3 / 4;
    bool pending = false;
    uint64_t lastPing = 0;
    while (alive && !g_quit && !g_restart && !sendFailed) {
        input.KeepAlive();
        if (NowUs() - lastPing > 500000) { // measures the bare USB/adb round trip
            lastPing = NowUs();
            proto::Writer pw;
            pw.u64(lastPing);
            if (!SendMsg(sock, proto::kPing, pw.buf)) break;
            if (lastPing - lastHeard > 1500000) {
                Log("Tablet stopped responding");
                break;
            }
            // Display layout changed (Duplicate/Extend, monitor moved or resized): once it
            // settles, restart if the screen we should stream is no longer this one.
            uint64_t changedAt = g_displayChangedAt;
            if (changedAt && lastPing - changedAt > 700000) {
                g_displayChangedAt.compare_exchange_strong(changedAt, 0);
                OutputInfo now;
                if (ChooseOutput(opt.display, now) &&
                    (now.gdiName != output.gdiName || !EqualRect(&now.desktop, &output.desktop))) {
                    Log("Display layout changed (%s -> %s): restarting the stream", Narrow(output.gdiName).c_str(),
                        Narrow(now.gdiName).c_str());
                    break;
                }
            }
            if (AppSettings().SaverActive() != saver) {
                Log("Battery saver %s: restarting the stream", saver ? "off" : "on");
                break;
            }
        }
        if (!enc->Pump(onOutput)) break;
        if (!enc->WantsInput()) {
            Sleep(1);
            continue;
        }
        int r = cap.Acquire(pending ? 0 : 2);
        if (r < 0) {
            Sleep(200);
            // Keep the D3D device the encoder was set up with; restart the session if
            // the output changed too much for that.
            if (!cap.Reacquire(output)) break;
            keyRequested = true;
            continue;
        }
        if (r == 1) pending = true;
        uint64_t now = NowUs();
        bool needKey = keyRequested.exchange(false);
        if (needKey) enc->ForceKeyframe();
        if ((pending && now - lastSubmit >= minIntervalUs) || needKey || lastSubmit == 0) {
            uint64_t pts = now - start;
            {
                std::lock_guard<std::mutex> lock(timingMu);
                timings[pts].captured = now;
                if (timings.size() > 600) timings.erase(timings.begin());
            }
            if (!enc->Submit(cap, pts)) break;
            lastSubmit = now;
            pending = false;
        } else if (pending) {
            Sleep(1);
        }
        if (now - statsAt > 5000000) {
            std::lock_guard<std::mutex> lock(timingMu);
            double n = double(std::max<uint64_t>(totals.n, 1));
            Log("%.1f fps, %.1f Mbps | latency avg %.1f ms (encode %.1f, send %.1f, usb+return %.1f "
                "[ping rtt %.1f], decoder input wait %.1f, decode %.1f) worst %.1f ms",
                frames * 1e6 / double(now - statsAt), bytes * 8.0 / double(now - statsAt),
                totals.total / n / 1000, totals.encode / n / 1000, totals.send / n / 1000,
                totals.transit / n / 1000,
                totals.rtt / double(std::max<uint64_t>(totals.pings, 1)) / 1000,
                totals.wait / n / 1000, totals.decode / n / 1000, totals.worst / 1000.0);
            totals = Totals{};
            frames = bytes = 0;
            statsAt = now;
        }
    }

    alive = false;
    shutdown(sock, SD_BOTH);
    reader.join();
    enc->Shutdown();
    cap.Shutdown();
    input.Shutdown();
    Log("Session ended");
}

void PrintOutputs() {
    for (auto& o : ListOutputs())
        printf("  %d: %s  %ldx%ld at (%ld,%ld)  monitor=\"%s\"  gpu=\"%s\"%s\n", o.index,
               Narrow(o.gdiName).c_str(), o.desktop.right - o.desktop.left,
               o.desktop.bottom - o.desktop.top, o.desktop.left, o.desktop.top,
               Narrow(o.monitorName).c_str(), Narrow(o.adapterName).c_str(),
               o.primary ? "  [primary]" : "");
}

// True when SuperDisplay's Windows service is running. Its bundled adb is an
// older version that keeps restarting the adb server and breaks our tunnel.
bool SuperDisplayRunning() {
    static ULONGLONG checkedAt = 0;
    static bool running = false;
    if (checkedAt && GetTickCount64() - checkedAt < 10000) return running;
    checkedAt = GetTickCount64();
    running = false;
    if (SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)) {
        if (SC_HANDLE svc = OpenServiceW(scm, L"SuperDisplay", SERVICE_QUERY_STATUS)) {
            SERVICE_STATUS st{};
            running = QueryServiceStatus(svc, &st) && st.dwCurrentState == SERVICE_RUNNING;
            CloseServiceHandle(svc);
        }
        CloseServiceHandle(scm);
    }
    return running;
}

// Detects the tablet, prepares it and runs streaming sessions until g_quit.
void HostLoop(Options opt) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Log("FreeDisplay host starting. Displays:");
    PrintOutputs();

    Adb adb;
    if (!adb.Locate()) {
        Log("adb.exe not found (put it in .\\adb\\ or install Android platform-tools)");
        Status(L"Error: adb.exe not found", true);
        return;
    }
    Log("Using %s", Narrow(adb.Path()).c_str());

    // A freshly installed display driver is only captured after a restart (see Driver.h).
    if (driver::RestartPending()) {
        Log("Virtual Display Driver was installed; waiting for Windows to restart");
        Status(L"Restart your PC to finish installing the display driver", true);
        while (!g_quit) Sleep(500);
        return;
    }
    adb.Run(L"start-server");

    SOCKET listener = Listen(proto::kPort);
    if (listener == INVALID_SOCKET) {
        Log("Cannot listen on 127.0.0.1:%u (another host running?)", proto::kPort);
        Status(L"Error: port " + std::to_wstring(proto::kPort) + L" is in use", true);
        return;
    }

    std::string current;
    std::string lastState = "?";
    uint64_t noDeviceSince = 0;  // debounces brief adb hiccups
    uint64_t sessionEndedAt = 0; // last stream end while the cable stayed in, 0 if none
    // The tablet's screen stays on the desktop only while the tablet shows it: when it
    // goes away, switch to "PC screen only" like Win+P (Windows moves the windows
    // back), and to the previous mode when it returns. With other monitors attached
    // that would switch them off too, so then only the windows are moved back.
    // Switching the VDD device itself off froze Windows (see VirtualMonitor.h).
    Settings& settings = AppSettings();
    bool needRescue = !VirtualMonitor::Find().empty() && VirtualMonitor::Topology() != 2; // off at start without a tablet
    auto rescueWindows = [&](const char* why) {
        needRescue = false;
        if (!settings.moveWindowsBack) return;
        int mode = VirtualMonitor::Topology();
        if ((mode == 0 || mode == 1) && VirtualMonitor::OnlyVddBesidesBuiltIn() && VirtualMonitor::SetTopology(2)) {
            settings.restoreMode = mode;
            settings.Save();
            Log("%s: turned the tablet screen off", why);
            Status(L"Tablet screen off until the tablet is connected");
            return;
        }
        int n = VirtualMonitor::MoveWindowsToPrimary();
        Log("%s: moved %d window(s) back to the main screen", why, n);
    };
    auto screenOn = [&] {
        int mode = settings.restoreMode.exchange(-1);
        if (mode < 0) return;
        settings.Save();
        if (VirtualMonitor::Topology() != 2) return; // the user changed it meanwhile
        if (VirtualMonitor::SetTopology(mode)) Sleep(1500); // let the desktop settle before capturing
    };
    while (!g_quit) {
        if (SuperDisplayRunning()) {
            static bool warned = false;
            if (!warned) {
                Log("The SuperDisplay service is running; its adb conflicts with ours");
                Status(L"SuperDisplay's service is running and blocks USB. Stop or uninstall SuperDisplay.", true);
            }
            warned = true;
        }
        std::string serial = adb.FirstReadyDevice();
        if (serial.empty()) {
            std::string state = adb.FirstDeviceState();
            if (state != lastState) {
                if (state == "unauthorized") {
                    Log("Tablet found but USB debugging is not authorized for this PC");
                    Status(L"On the tablet, tap Allow on the \"Allow USB debugging?\" prompt", true);
                } else if (!state.empty()) {
                    Log("Tablet found in state '%s'", state.c_str());
                    Status(L"Tablet found but not ready (" + Widen(state) + L"); reconnect the cable");
                } else {
                    Log("Waiting for a tablet (USB cable, USB debugging enabled)...");
                    Status(L"Waiting for tablet (plug in USB)");
                }
                lastState = state;
            }
            current.clear();
            sessionEndedAt = 0;
            if (!noDeviceSince) noDeviceSince = NowUs();
            if (needRescue && NowUs() - noDeviceSince > 1500000) rescueWindows("Tablet unplugged");
            Sleep(500);
            continue;
        }
        lastState = "device";
        noDeviceSince = 0;
        if (serial != current) {
            Log("Tablet connected: %s", serial.c_str());
            Status(L"Tablet connected, starting app...");
            screenOn();
            if (!PrepareDevice(adb, serial, opt)) {
                Status(L"Could not start the tablet app (see log)");
                Sleep(2000);
                continue;
            }
            current = serial;
        }
        SOCKET c = AcceptWithTimeout(listener, 1000);
        if (c == INVALID_SOCKET) {
            // Cable still in but the tablet app was closed (quality restarts reconnect at once).
            if (needRescue && sessionEndedAt && NowUs() - sessionEndedAt > 3000000) rescueWindows("Tablet app closed");
            continue;
        }
        Log("Tablet app connected");
        screenOn(); // reopened after the screen was turned off
        sessionEndedAt = 0;
        needRescue = true;
        RunSession(c, opt);
        closesocket(c);
        sessionEndedAt = NowUs();
        if (!g_quit) Status(L"Tablet connected, waiting for the app");
    }
    closesocket(listener);
    CoUninitialize();
}

// Sends stdout to the console of the terminal that started us, if any.
void AttachParentConsole() {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f;
        freopen_s(&f, "CONOUT$", "w", stdout);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_uiThread = GetCurrentThreadId();
    // When stdout is a file or pipe (scripts, benchmarks) the CRT already uses it.
    bool stdoutRedirected = GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) != FILE_TYPE_UNKNOWN &&
                            GetStdHandle(STD_OUTPUT_HANDLE) != nullptr;

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    Options opt;
    bool console = false;
    enum class Setup { None, Install, Uninstall } setup = Setup::None;
    bool quiet = false;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        auto next = [&] { return i + 1 < argc ? _wtoi(argv[++i]) : 0; };
        if (a == L"--list") {
            if (!stdoutRedirected) AttachParentConsole();
            PrintOutputs();
            return 0;
        } else if (a == L"--console") {
            console = true;
        } else if (a == L"--display") {
            opt.display = next();
        } else if (a == L"--fps") {
            opt.fps = UINT(std::clamp(next(), 10, 144));
        } else if (a == L"--bitrate") {
            opt.bitrateMbps = UINT(std::clamp(next(), 1, 200));
        } else if (a == L"--scale") {
            opt.scalePct = std::clamp(next(), 25, 100);
        } else if (a == L"--speed") {
            opt.speed = std::clamp(next(), 0, 100);
        } else if (a == L"--encoder") {
            opt.encoder = i + 1 < argc ? argv[++i] : L"auto";
        } else if (a == L"--no-install") {
            opt.install = false;
        } else if (a == L"--install") {
            setup = Setup::Install;
        } else if (a == L"--uninstall") {
            setup = Setup::Uninstall;
        } else if (a == L"--quiet") {
            quiet = true;
        } else if (a == L"--wait-for") { // restart: let the previous copy exit first
            if (HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, DWORD(next()))) {
                WaitForSingleObject(old, 15000);
                CloseHandle(old);
            }
        } else if (a == L"--install-driver") { // run elevated by the installer
            return driver::Install();
        } else if (a == L"--uninstall-driver") {
            return driver::Uninstall();
        } else if (a == L"--update-driver-settings") {
            return driver::UpdateSettings();
        } else if (a == L"--move-windows-back") {
            if (!stdoutRedirected) AttachParentConsole();
            printf("Moved %d window(s) to the main screen\n", VirtualMonitor::MoveWindowsToPrimary());
            return 0;
        } else {
            if (!stdoutRedirected) AttachParentConsole();
            printf("Usage: FreeDisplay [--console] [--list] [--display N] [--fps N] [--bitrate Mbps]\n"
                   "                   [--scale PERCENT] [--speed 0-100] [--encoder auto|amf|nvenc|mf]\n"
                   "                   [--no-install]\n"
                   "       FreeDisplay --install|--uninstall [--quiet]\n"
                   "Runs in the notification area; --console also shows the log in a console window.\n");
            return 1;
        }
    }
    LocalFree(argv);

    if (setup == Setup::Install) return installer::Install(quiet, !quiet) ? 0 : 1;
    if (setup == Setup::Uninstall) return installer::Uninstall(quiet) ? 0 : 1;
    if (!installer::RunningInPlace() && !console && !stdoutRedirected) {
        // Started from a download: offer to install, or just run from a cache.
        int choice = MessageBoxW(nullptr,
                                 L"Install FreeDisplay on this PC?\n\n"
                                 L"Yes:  install for your user (Start menu shortcut, starts with Windows)\n"
                                 L"No:  just run it this time\n\n"
                                 L"No administrator rights are needed.",
                                 L"FreeDisplay", MB_YESNOCANCEL | MB_ICONQUESTION | MB_SETFOREGROUND);
        if (choice == IDYES) return installer::Install(false, true) ? 0 : 1;
        if (choice != IDNO) return 0;
    }
    if (!installer::RunningInPlace() && !installer::PreparePortable()) {
        MessageBoxW(nullptr, L"FreeDisplay could not unpack its files.", L"FreeDisplay", MB_ICONERROR);
        return 1;
    }

    if (console && !stdoutRedirected) {
        AllocConsole();
        FILE* f;
        freopen_s(&f, "CONOUT$", "w", stdout);
        SetConsoleCtrlHandler(OnCtrl, TRUE);
    }

    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\FreeDisplayHost");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Started again (Start menu, desktop): bring up the running copy's settings window.
        HWND tray = FindWindowW(L"FreeDisplayTray", nullptr);
        if (tray) {
            DWORD pid = 0;
            GetWindowThreadProcessId(tray, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(tray, kShowSettingsMsg, 0, 0);
        } else if (!stdoutRedirected && !console) {
            MessageBoxW(nullptr, L"FreeDisplay is already running. Look for its icon in the notification area.",
                        L"FreeDisplay", MB_ICONINFORMATION);
        }
        return 0;
    }

    // Keep the previous run's log next to the current one.
    // A previous instance that is still exiting may hold the file for a moment.
    std::wstring logPath = Settings::DataDir() + L"host.log";
    for (int i = 0; i < 20 && !g_logFile; i++) {
        MoveFileExW(logPath.c_str(), (Settings::DataDir() + L"host.previous.log").c_str(), MOVEFILE_REPLACE_EXISTING);
        g_logFile = _wfsopen(logPath.c_str(), L"wN", _SH_DENYWR); // N: not inherited by children
        if (!g_logFile) Sleep(100);
    }
    AppSettings().Load();

    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    timeBeginPeriod(1);

    Tray tray;
    Tray::Callbacks cb;
    cb.onQualityChanged = [] { g_restart = true; };
    cb.onQuit = [] { g_quit = true; };
    cb.onDisplayChanged = [] { g_displayChangedAt = NowUs(); };
    cb.onRestart = [] {
        // The new copy waits for this one to exit (it needs the single-instance mutex).
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --wait-for " + std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW si{sizeof si};
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            Log("Restarting FreeDisplay");
        }
        g_quit = true;
        PostQuitMessage(0);
    };
    if (tray.Create(inst, cb)) g_tray = &tray;
    else Log("Notification-area icon unavailable");

    std::thread worker(HostLoop, opt);
    if (g_tray) {
        tray.Run();
    } else {
        while (!g_quit) Sleep(200);
    }
    g_quit = true;
    worker.join();
    g_tray = nullptr;
    tray.Destroy();

    timeEndPeriod(1);
    WSACleanup();
    MFShutdown();
    Log("FreeDisplay host stopped");
    if (g_logFile) fclose(g_logFile);
    if (single) CloseHandle(single);
    return 0;
}
