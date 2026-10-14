#include "VirtualMonitor.h"
#include "Util.h"

namespace {

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

} // namespace VirtualMonitor
