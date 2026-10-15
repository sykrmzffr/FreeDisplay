#include <windows.h>
#include <shlobj.h>
#include "Settings.h"
#include "Util.h"

namespace {
constexpr const wchar_t* kSection = L"FreeDisplay";
constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kRunValue = L"FreeDisplay";

std::wstring IniPath() { return Settings::DataDir() + L"settings.ini"; }

bool ReadBool(const wchar_t* key, bool def) {
    return GetPrivateProfileIntW(kSection, key, def ? 1 : 0, IniPath().c_str()) != 0;
}

void WriteInt(const wchar_t* key, int value) {
    WritePrivateProfileStringW(kSection, key, std::to_wstring(value).c_str(), IniPath().c_str());
}
} // namespace

Settings& AppSettings() {
    static Settings s;
    return s;
}

std::wstring Settings::DataDir() {
    PWSTR appData = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        dir = std::wstring(appData) + L"\\FreeDisplay\\";
        CoTaskMemFree(appData);
    } else {
        dir = ExeDir();
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

bool Settings::SaverActive() const {
    if (batterySaver == kSaverAlways) return true;
    if (batterySaver == kSaverOff) return false;
    SYSTEM_POWER_STATUS ps;
    return GetSystemPowerStatus(&ps) && ps.ACLineStatus == 0; // 0 = on battery
}

void Settings::Load() {
    touch = ReadBool(L"Touch", true);
    pen = ReadBool(L"Pen", true);
    int q = GetPrivateProfileIntW(kSection, L"Quality", 75, IniPath().c_str());
    qualityPct = (q == 100 || q == 75 || q == 50) ? q : 75;
    startWithWindows = ReadBool(L"StartWithWindows", false);
    moveWindowsBack = ReadBool(L"MoveWindowsBack", true);
    int saver = GetPrivateProfileIntW(kSection, L"BatterySaver", kSaverOnBattery, IniPath().c_str());
    batterySaver = (saver >= kSaverOnBattery && saver <= kSaverOff) ? saver : kSaverOnBattery;
    int orient = GetPrivateProfileIntW(kSection, L"Orientation", 0, IniPath().c_str());
    orientation = (orient >= 0 && orient <= 2) ? orient : 0;
}

void Settings::Save() const {
    WriteInt(L"Touch", touch ? 1 : 0);
    WriteInt(L"Pen", pen ? 1 : 0);
    WriteInt(L"Quality", qualityPct);
    WriteInt(L"StartWithWindows", startWithWindows ? 1 : 0);
    WriteInt(L"MoveWindowsBack", moveWindowsBack ? 1 : 0);
    WriteInt(L"BatterySaver", batterySaver);
    WriteInt(L"Orientation", orientation);
}

bool Settings::ApplyStartWithWindows(bool enable) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return false;
    LSTATUS rc;
    if (enable) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
        rc = RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                            DWORD((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        rc = RegDeleteValueW(key, kRunValue);
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}
