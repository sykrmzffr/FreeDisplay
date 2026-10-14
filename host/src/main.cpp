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
#include "Protocol.h"
#include "Util.h"

namespace {

constexpr const wchar_t* kPackage = L"com.freedisplay.client";
constexpr const wchar_t* kActivity = L"com.freedisplay.client/.MainActivity";
constexpr int kAppVersionCode = 6; // keep in sync with android/app/build.gradle.kts

struct Options {
    int display = -1;     // -1: auto (VDD monitor, else primary)
    UINT fps = 120;       // upper bound; the tablet's refresh rate also caps it
    UINT bitrateMbps = 0; // 0: automatic
    int scalePct = 75;    // encode size as % of the monitor size (lower = less latency)
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

BOOL WINAPI OnCtrl(DWORD) {
    g_quit = true;
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
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
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
            std::wstring apk = ExeDir() + L"FreeDisplay.apk";
            if (GetFileAttributesW(apk.c_str()) == INVALID_FILE_ATTRIBUTES) {
                Log("FreeDisplay.apk not found next to the host; install the app manually");
            } else {
                Log("Installing tablet app (installed version %d, need %d)...", installed, kAppVersionCode);
                std::string out;
                int rc = adb.RunOn(serial, L"install -r -g \"" + apk + L"\"", &out, 120000);
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

// Switches a display to `hz` at its current resolution, if that mode exists.
void SetRefreshRate(const std::wstring& gdiName, UINT hz) {
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    if (!EnumDisplaySettingsW(gdiName.c_str(), ENUM_CURRENT_SETTINGS, &dm)) return;
    if (dm.dmDisplayFrequency == hz) return;
    dm.dmDisplayFrequency = hz;
    dm.dmFields = DM_DISPLAYFREQUENCY;
    LONG rc = ChangeDisplaySettingsExW(gdiName.c_str(), &dm, nullptr, 0, nullptr);
    Log("Set %s to %u Hz: %s", Narrow(gdiName).c_str(), hz, rc == DISP_CHANGE_SUCCESSFUL ? "ok" : "not supported");
}

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
    std::string name = hello.rest();
    if (magic != proto::kMagic || version != proto::kVersion) {
        Log("Protocol mismatch (magic %08X version %u)", magic, version);
        return;
    }
    Log("Tablet: %s, %ux%u @ %u dpi, up to %u Hz", name.c_str(), tabW, tabH, dpi, tabHz);

    OutputInfo output;
    if (!ChooseOutput(opt.display, output)) {
        Log("No display to capture");
        return;
    }
    if (output.monitorName.find(L"VDD") == std::wstring::npos && opt.display < 0)
        Log("Virtual Display Driver monitor not active; mirroring %s instead",
            Narrow(output.gdiName).c_str());

    // Run the virtual monitor and the stream at the tablet's refresh rate (capped by --fps).
    UINT fps = std::min<UINT>(opt.fps, tabHz ? tabHz : 60);
    if (output.monitorName.find(L"VDD") != std::wstring::npos) SetRefreshRate(output.gdiName, fps);

    UINT w = UINT((output.desktop.right - output.desktop.left) * opt.scalePct / 100) & ~1u;
    UINT h = UINT((output.desktop.bottom - output.desktop.top) * opt.scalePct / 100) & ~1u;
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
    if (!SendMsg(sock, proto::kVideoConfig, cfg.buf)) return;

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
    std::thread reader([&] {
        uint8_t t;
        std::vector<uint8_t> p;
        while (alive && RecvMsg(sock, t, p)) {
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
    while (alive && !g_quit && !sendFailed) {
        input.KeepAlive();
        if (NowUs() - lastPing > 500000) { // measures the bare USB/adb round trip
            lastPing = NowUs();
            proto::Writer pw;
            pw.u64(lastPing);
            if (!SendMsg(sock, proto::kPing, pw.buf)) break;
        }
        if (!enc->Pump(onOutput)) break;
        if (!enc->WantsInput()) {
            Sleep(1);
            continue;
        }
        int r = cap.Acquire(pending ? 0 : 2);
        if (r < 0) {
            Sleep(200);
            if (!cap.Init(output, w, h)) break;
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

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetConsoleCtrlHandler(OnCtrl, TRUE);

    Options opt;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        auto next = [&] { return i + 1 < argc ? _wtoi(argv[++i]) : 0; };
        if (a == L"--list") {
            PrintOutputs();
            return 0;
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
        } else {
            printf("Usage: FreeDisplayHost [--list] [--display N] [--fps N] [--bitrate Mbps]\n"
                   "                       [--scale PERCENT] [--speed 0-100] [--encoder auto|amf|nvenc|mf]\n                       [--no-install]\n");
            return 1;
        }
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    timeBeginPeriod(1);

    Log("FreeDisplay host starting. Displays:");
    PrintOutputs();

    Adb adb;
    if (!adb.Locate()) {
        Log("adb.exe not found (put it in .\\adb\\ or install Android platform-tools)");
        return 1;
    }
    Log("Using %s", Narrow(adb.Path()).c_str());
    adb.Run(L"start-server");

    SOCKET listener = Listen(proto::kPort);
    if (listener == INVALID_SOCKET) {
        Log("Cannot listen on 127.0.0.1:%u (another host running?)", proto::kPort);
        return 1;
    }

    std::string current;
    bool waitingLogged = false;
    while (!g_quit) {
        std::string serial = adb.FirstReadyDevice();
        if (serial.empty()) {
            if (!waitingLogged) Log("Waiting for a tablet (USB cable, USB debugging enabled)...");
            waitingLogged = true;
            current.clear();
            Sleep(1000);
            continue;
        }
        waitingLogged = false;
        if (serial != current) {
            Log("Tablet connected: %s", serial.c_str());
            if (!PrepareDevice(adb, serial, opt)) {
                Sleep(2000);
                continue;
            }
            current = serial;
        }
        SOCKET c = AcceptWithTimeout(listener, 1000);
        if (c == INVALID_SOCKET) continue;
        Log("Tablet app connected");
        RunSession(c, opt);
        closesocket(c);
    }

    closesocket(listener);
    timeEndPeriod(1);
    WSACleanup();
    MFShutdown();
    CoUninitialize();
    return 0;
}
