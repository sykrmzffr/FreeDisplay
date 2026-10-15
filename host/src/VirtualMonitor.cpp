#include "VirtualMonitor.h"
#include <dwmapi.h>
#include <algorithm>
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
