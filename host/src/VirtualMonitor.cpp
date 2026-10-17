#include "VirtualMonitor.h"
#include <dwmapi.h>
#include <algorithm>
#include <climits>
#include <cmath>
#include <vector>
#include "Util.h"

namespace {

struct MonitorRects {
    HMONITOR handle = nullptr;
    RECT work{}; // monitor area minus taskbars
};

bool FindMonitor(const std::wstring& gdiName, MonitorRects& out) {
    struct Ctx {
        const std::wstring* name;
        MonitorRects* out;
        bool found;
    } ctx{&gdiName, &out, false};
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof mi;
            if (GetMonitorInfoW(m, &mi) && *c->name == mi.szDevice) {
                c->out->handle = m;
                c->out->work = mi.rcWork;
                c->found = true;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

// Windows a user would see in Alt+Tab, plus visible owned dialogs.
bool IsMovableWindow(HWND w) {
    if (!IsWindowVisible(w) || IsHungAppWindow(w)) return false;
    if (GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return false;
    BOOL cloaked = FALSE; // hidden UWP frames, windows on other virtual desktops
    if (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked) return false;
    wchar_t cls[64] = {};
    GetClassNameW(w, cls, 64);
    for (const wchar_t* shell : {L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Progman", L"WorkerW"})
        if (!wcscmp(cls, shell)) return false;
    RECT r;
    return GetWindowRect(w, &r) && r.right > r.left && r.bottom > r.top;
}

// Maps `r` from one work area to another, keeping its relative place and size.
RECT MapRect(const RECT& r, const RECT& from, const RECT& to) {
    double sx = double(to.right - to.left) / std::max<LONG>(1, from.right - from.left);
    double sy = double(to.bottom - to.top) / std::max<LONG>(1, from.bottom - from.top);
    RECT o;
    o.left = to.left + LONG((r.left - from.left) * sx);
    o.top = to.top + LONG((r.top - from.top) * sy);
    o.right = o.left + LONG((r.right - r.left) * sx);
    o.bottom = o.top + LONG((r.bottom - r.top) * sy);
    // Keep it fully inside the target work area.
    LONG dx = std::max(to.left - o.left, std::min<LONG>(0, to.right - o.right));
    LONG dy = std::max(to.top - o.top, std::min<LONG>(0, to.bottom - o.bottom));
    OffsetRect(&o, dx, dy);
    return o;
}

bool FindDevice(DISPLAY_DEVICEW& out) {
    DISPLAY_DEVICEW dd{sizeof dd};
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); i++, dd.cb = sizeof dd) {
        if (wcsstr(dd.DeviceID, L"MttVDD") || wcsstr(dd.DeviceString, L"Virtual Display Driver")) {
            out = dd;
            return true;
        }
    }
    return false;
}

} // namespace

namespace VirtualMonitor {

std::wstring Find() {
    DISPLAY_DEVICEW dd{};
    return FindDevice(dd) ? dd.DeviceName : L"";
}

bool IsAttached() {
    DISPLAY_DEVICEW dd{};
    return FindDevice(dd) && (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP);
}

bool MatchTablet(const std::wstring& gdiName, UINT tabletW, UINT tabletH, UINT hz) {
    DEVMODEW cur{};
    cur.dmSize = sizeof cur;
    if (!EnumDisplaySettingsW(gdiName.c_str(), ENUM_CURRENT_SETTINGS, &cur) || !tabletW || !tabletH) return false;

    // Resolution: same orientation first, then the closest shape, then the closest size.
    const bool portrait = tabletH > tabletW;
    const double aspect = std::log(double(tabletW) / tabletH);
    DWORD bestW = cur.dmPelsWidth, bestH = cur.dmPelsHeight;
    double bestScore = 1e18;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    for (DWORD i = 0; EnumDisplaySettingsW(gdiName.c_str(), i, &dm); i++) {
        if (!dm.dmPelsWidth || !dm.dmPelsHeight) continue;
        double score = ((dm.dmPelsHeight > dm.dmPelsWidth) != portrait ? 1000.0 : 0.0) +
                       std::round(std::abs(std::log(double(dm.dmPelsWidth) / dm.dmPelsHeight) - aspect) * 100) +
                       std::abs(std::log(double(dm.dmPelsWidth) * dm.dmPelsHeight / (double(tabletW) * tabletH))) * 0.01;
        if (score < bestScore) {
            bestScore = score;
            bestW = dm.dmPelsWidth;
            bestH = dm.dmPelsHeight;
        }
    }
    // Refresh rate at that resolution: the requested one, else the closest.
    DWORD bestHz = cur.dmDisplayFrequency;
    int bestDiff = INT_MAX;
    for (DWORD i = 0; EnumDisplaySettingsW(gdiName.c_str(), i, &dm); i++) {
        if (dm.dmPelsWidth != bestW || dm.dmPelsHeight != bestH) continue;
        int diff = std::abs(int(dm.dmDisplayFrequency) - int(hz));
        if (diff < bestDiff) {
            bestDiff = diff;
            bestHz = dm.dmDisplayFrequency;
        }
    }
    if (cur.dmPelsWidth == bestW && cur.dmPelsHeight == bestH && cur.dmDisplayFrequency == bestHz) return false;

    DEVMODEW want{};
    want.dmSize = sizeof want;
    want.dmPelsWidth = bestW;
    want.dmPelsHeight = bestH;
    want.dmDisplayFrequency = bestHz;
    want.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    LONG rc = ChangeDisplaySettingsExW(gdiName.c_str(), &want, nullptr, 0, nullptr); // not saved to the registry
    Log("Virtual monitor %lux%lu @ %lu Hz -> %lux%lu @ %lu Hz for a %ux%u tablet: %s", cur.dmPelsWidth,
        cur.dmPelsHeight, cur.dmDisplayFrequency, bestW, bestH, bestHz, tabletW, tabletH,
        rc == DISP_CHANGE_SUCCESSFUL ? "ok" : "failed");
    return rc == DISP_CHANGE_SUCCESSFUL;
}

bool HasMode(const std::wstring& gdiName, UINT w, UINT h) {
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    for (DWORD i = 0; EnumDisplaySettingsW(gdiName.c_str(), i, &dm); i++)
        if (dm.dmPelsWidth == w && dm.dmPelsHeight == h) return true;
    return false;
}

int Topology() {
    UINT32 paths = 0, modes = 0;
    if (GetDisplayConfigBufferSizes(QDC_DATABASE_CURRENT, &paths, &modes) != ERROR_SUCCESS) return -1;
    std::vector<DISPLAYCONFIG_PATH_INFO> p(paths);
    std::vector<DISPLAYCONFIG_MODE_INFO> m(modes);
    DISPLAYCONFIG_TOPOLOGY_ID topology{};
    if (QueryDisplayConfig(QDC_DATABASE_CURRENT, &paths, p.data(), &modes, m.data(), &topology) != ERROR_SUCCESS)
        return -1;
    switch (topology) {
    case DISPLAYCONFIG_TOPOLOGY_EXTEND: return 0;
    case DISPLAYCONFIG_TOPOLOGY_CLONE: return 1;
    case DISPLAYCONFIG_TOPOLOGY_INTERNAL: return 2;
    default: return -1;
    }
}

bool SetTopology(int mode) {
    static const UINT32 flags[] = {SDC_TOPOLOGY_EXTEND, SDC_TOPOLOGY_CLONE, SDC_TOPOLOGY_INTERNAL};
    static const char* names[] = {"extend", "duplicate", "PC screen only"};
    if (mode < 0 || mode > 2) return false;
    LONG rc = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | flags[mode]);
    Log("Screen mode -> %s: %s", names[mode], rc == ERROR_SUCCESS ? "ok" : "failed");
    return rc == ERROR_SUCCESS;
}

bool OnlyVddBesidesBuiltIn() {
    UINT32 paths = 0, modes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) != ERROR_SUCCESS) return false;
    std::vector<DISPLAYCONFIG_PATH_INFO> p(paths);
    std::vector<DISPLAYCONFIG_MODE_INFO> m(modes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, p.data(), &modes, m.data(), nullptr) != ERROR_SUCCESS)
        return false;
    int builtIn = 0, vdd = 0, other = 0;
    for (UINT32 i = 0; i < paths; i++) {
        DISPLAYCONFIG_TARGET_DEVICE_NAME name{};
        name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        name.header.size = sizeof name;
        name.header.adapterId = p[i].targetInfo.adapterId;
        name.header.id = p[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS) return false;
        switch (name.outputTechnology) {
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL:
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED:
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED:
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS: builtIn++; break;
        default:
            if (wcsstr(name.monitorFriendlyDeviceName, L"VDD")) vdd++;
            else other++;
        }
    }
    return builtIn > 0 && vdd > 0 && other == 0;
}

bool Bounds(RECT& out) {
    std::wstring name = Find();
    if (name.empty()) return false;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    if (!EnumDisplaySettingsW(name.c_str(), ENUM_CURRENT_SETTINGS, &dm) || !dm.dmPelsWidth) return false;
    MonitorRects m;
    if (!FindMonitor(name, m)) return false;
    MONITORINFO mi{sizeof mi};
    if (!GetMonitorInfoW(m.handle, &mi)) return false;
    out = mi.rcMonitor;
    return true;
}

int MoveStrandedWindows(const RECT& area) {
    HMONITOR primary = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO pmi{sizeof pmi};
    if (!GetMonitorInfoW(primary, &pmi)) return 0;
    std::vector<HWND> windows;
    EnumWindows(
        [](HWND w, LPARAM lp) -> BOOL {
            reinterpret_cast<std::vector<HWND>*>(lp)->push_back(w);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&windows));
    int moved = 0;
    for (HWND w : windows) {
        if (!IsMovableWindow(w) || IsIconic(w) || MonitorFromWindow(w, MONITOR_DEFAULTTONULL)) continue;
        RECT r, overlap;
        if (!GetWindowRect(w, &r) || r.right - r.left < 40 || r.bottom - r.top < 40 ||
            !IntersectRect(&overlap, &r, &area))
            continue;
        RECT target = MapRect(r, area, pmi.rcWork);
        if (SetWindowPos(w, nullptr, target.left, target.top, target.right - target.left,
                         target.bottom - target.top, SWP_NOZORDER | SWP_NOACTIVATE))
            moved++;
    }
    return moved;
}

int MoveWindowsToPrimary() {
    MonitorRects vdd;
    std::wstring name = Find();
    if (name.empty() || !FindMonitor(name, vdd)) return 0;
    HMONITOR primary = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (primary == vdd.handle) return 0;
    MONITORINFO pmi{sizeof pmi};
    if (!GetMonitorInfoW(primary, &pmi)) return 0;
    // Window placements use workspace coordinates (origin at the primary work area).
    POINT ws{pmi.rcWork.left - pmi.rcMonitor.left, pmi.rcWork.top - pmi.rcMonitor.top};

    std::vector<HWND> windows;
    EnumWindows(
        [](HWND w, LPARAM lp) -> BOOL {
            reinterpret_cast<std::vector<HWND>*>(lp)->push_back(w);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&windows));

    int moved = 0;
    for (HWND w : windows) {
        if (!IsMovableWindow(w)) continue;
        WINDOWPLACEMENT wp{sizeof wp};
        if (!GetWindowPlacement(w, &wp)) continue;
        RECT normal = wp.rcNormalPosition;
        OffsetRect(&normal, ws.x, ws.y);
        bool minimized = IsIconic(w) != FALSE;
        HMONITOR on = minimized ? MonitorFromRect(&normal, MONITOR_DEFAULTTONEAREST)
                                : MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
        if (on != vdd.handle) continue;

        RECT target = MapRect(normal, vdd.work, pmi.rcWork);
        OffsetRect(&target, -ws.x, -ws.y);
        wp.rcNormalPosition = target;
        wp.flags &= WPF_RESTORETOMAXIMIZED;
        bool maximized = !minimized && IsZoomed(w);
        // A maximized window keeps its monitor when only its restore spot changes,
        // so restore it onto the main screen first and maximize it there.
        wp.showCmd = minimized ? SW_SHOWMINNOACTIVE : SW_SHOWNOACTIVATE;
        if (!SetWindowPlacement(w, &wp)) continue; // fails for elevated windows; those stay put
        if (maximized) {
            wp.showCmd = SW_SHOWMAXIMIZED;
            SetWindowPlacement(w, &wp);
        }
        moved++;
    }

    POINT cursor;
    if (GetCursorPos(&cursor) && MonitorFromPoint(cursor, MONITOR_DEFAULTTONULL) == vdd.handle)
        SetCursorPos((pmi.rcWork.left + pmi.rcWork.right) / 2, (pmi.rcWork.top + pmi.rcWork.bottom) / 2);
    return moved;
}

} // namespace VirtualMonitor
