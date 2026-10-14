#include "VirtualMonitor.h"
#include "Settings.h"
#include "Util.h"

namespace {

bool IsVdd(const DISPLAY_DEVICEW& dd) {
    return wcsstr(dd.DeviceID, L"MttVDD") != nullptr ||
           wcsstr(dd.DeviceString, L"Virtual Display Driver") != nullptr;
}

bool FindDevice(DISPLAY_DEVICEW& out) {
    DISPLAY_DEVICEW dd{sizeof dd};
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); i++, dd.cb = sizeof dd) {
        if (IsVdd(dd)) {
            out = dd;
            return true;
        }
    }
    return false;
}

bool HasMode(const std::wstring& name, UINT w, UINT h, UINT hz) {
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    for (DWORD i = 0; EnumDisplaySettingsW(name.c_str(), i, &dm); i++) {
        if (dm.dmPelsWidth == w && dm.dmPelsHeight == h && (hz == 0 || dm.dmDisplayFrequency == hz)) return true;
    }
    return false;
}

LONG Apply(const std::wstring& name, DEVMODEW& dm) {
    LONG rc = ChangeDisplaySettingsExW(name.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
    if (rc == DISP_CHANGE_SUCCESSFUL) rc = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    return rc;
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

bool Attach(UINT width, UINT height, UINT hz) {
    DISPLAY_DEVICEW dd{};
    if (!FindDevice(dd)) return false;
    std::wstring name = dd.DeviceName;
    bool attached = (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;

    DEVMODEW cur{};
    cur.dmSize = sizeof cur;
    bool haveCur = EnumDisplaySettingsW(name.c_str(), ENUM_CURRENT_SETTINGS, &cur) != FALSE;

    // Prefer the tablet's native size at its refresh rate; else keep what the monitor has.
    UINT w = width, h = height, f = hz;
    if (!HasMode(name, w, h, f)) f = 0;
    if (!HasMode(name, w, h, 0)) {
        Log("Virtual monitor has no %ux%u mode; add it to vdd_settings.xml for a pixel-exact picture", width, height);
        if (haveCur && cur.dmPelsWidth) {
            w = cur.dmPelsWidth;
            h = cur.dmPelsHeight;
        } else {
            w = 1920;
            h = 1080;
        }
        f = 0;
    }
    if (attached && haveCur && cur.dmPelsWidth == w && cur.dmPelsHeight == h && (f == 0 || cur.dmDisplayFrequency == f))
        return true; // already as wanted

    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    dm.dmPelsWidth = w;
    dm.dmPelsHeight = h;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_POSITION;
    if (f) {
        dm.dmDisplayFrequency = f;
        dm.dmFields |= DM_DISPLAYFREQUENCY;
    }
    Settings& s = AppSettings();
    if (attached && haveCur) {
        dm.dmPosition = cur.dmPosition; // only the mode changes
    } else if (s.hasMonitorPos) {
        dm.dmPosition = POINTL{s.monitorX, s.monitorY};
    } else {
        dm.dmPosition = POINTL{-LONG(w), 0}; // left of the primary monitor
    }
    LONG rc = Apply(name, dm);
    Log("Virtual monitor %s %ux%u @ %u Hz at (%ld,%ld): %s", attached ? "set to" : "attached", w, h, f,
        dm.dmPosition.x, dm.dmPosition.y, rc == DISP_CHANGE_SUCCESSFUL ? "ok" : "failed");
    return rc == DISP_CHANGE_SUCCESSFUL;
}

bool Detach() {
    DISPLAY_DEVICEW dd{};
    if (!FindDevice(dd) || !(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) return true;
    if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) return false; // never remove the main screen
    std::wstring name = dd.DeviceName;

    DEVMODEW cur{};
    cur.dmSize = sizeof cur;
    if (EnumDisplaySettingsW(name.c_str(), ENUM_CURRENT_SETTINGS, &cur)) {
        Settings& s = AppSettings();
        s.monitorX = cur.dmPosition.x;
        s.monitorY = cur.dmPosition.y;
        s.hasMonitorPos = true;
        s.Save();
    }
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_POSITION;
    LONG rc = Apply(name, dm);
    Log("Virtual monitor detached: %s", rc == DISP_CHANGE_SUCCESSFUL ? "ok" : "failed");
    return rc == DISP_CHANGE_SUCCESSFUL;
}

} // namespace VirtualMonitor
