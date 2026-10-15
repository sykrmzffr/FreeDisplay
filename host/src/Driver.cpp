#include "Driver.h"
#include <setupapi.h>
#include <devguid.h>
#include <newdev.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include <iterator>
#include <vector>
#include "Resources.h"
#include "Util.h"

namespace driver {
namespace {

constexpr const wchar_t* kHardwareId = L"Root\\MttVDD";
constexpr const wchar_t* kDefaultDir = L"C:\\VirtualDisplayDriver"; // where VDD's own tools look
constexpr const wchar_t* kVddKey = L"SOFTWARE\\MikeTheTech\\VirtualDisplayDriver";
constexpr const wchar_t* kOurKey = L"SOFTWARE\\FreeDisplay";

// Exit codes of the elevated copy besides Win32 errors.
constexpr int kErrNoPayload = 0x20000001;
constexpr int kErrNotOurs = 0x20000002;

// Resolutions offered by the virtual monitor, each at every global refresh rate
// plus its own. This driver version can only report a limited number of modes:
// with 24 resolutions x 5 rates (~120 modes) it logged "Input buffer is too small
// for target modes" and created no monitor at all; ~49 modes worked. Keep
// kResolutions x (kRefreshRates + 1) well below that. The host scales the picture
// to the tablet, so a close match is enough.
constexpr int kResolutions[][2] = {
    {1920, 1080}, {1920, 1200}, {2000, 1200}, {2048, 1536}, {2560, 1600},
    {2800, 1752}, {2880, 1800}, {2960, 1848}, {3000, 2000}, {3200, 2136},
};
constexpr int kRefreshRates[] = {60, 90, 120};
static_assert(std::size(kResolutions) * (std::size(kRefreshRates) + 1) <= 40, "too many display modes for VDD");

// Name of the GPU driving the primary display. VDD renders on it, which avoids
// copies between GPUs on hybrid laptops; left to "default" it picked the
// discrete GPU on the test laptop.
std::string PrimaryGpuName() {
    DISPLAY_DEVICEW dd{sizeof dd};
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); i++, dd.cb = sizeof dd)
        if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) return Narrow(dd.DeviceString);
    return "default";
}

std::string XmlEscape(const std::string& s) {
    std::string r;
    for (char c : s) {
        if (c == '&') r += "&amp;";
        else if (c == '<') r += "&lt;";
        else if (c == '>') r += "&gt;";
        else r += c;
    }
    return r;
}

bool HasHardwareId(HDEVINFO set, SP_DEVINFO_DATA& dev) {
    wchar_t hw[1024] = {};
    if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, nullptr, reinterpret_cast<BYTE*>(hw),
                                           sizeof hw - 2 * sizeof(wchar_t), nullptr))
        return false;
    for (const wchar_t* p = hw; *p; p += wcslen(p) + 1) // REG_MULTI_SZ
        if (_wcsicmp(p, kHardwareId) == 0) return true;
    return false;
}

std::wstring InstallDir() {
    wchar_t buf[MAX_PATH] = {};
    DWORD size = sizeof buf;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kVddKey, L"VDDPATH", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS &&
        buf[0])
        return buf;
    return kDefaultDir;
}

bool WriteResource(int id, const std::wstring& path) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL mem = res ? LoadResource(nullptr, res) : nullptr;
    const void* data = mem ? LockResource(mem) : nullptr;
    if (!data) return false;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD size = SizeofResource(nullptr, res), wrote = 0;
    bool ok = WriteFile(f, data, size, &wrote, nullptr) && wrote == size;
    CloseHandle(f);
    return ok;
}

bool WriteText(const std::wstring& path, const std::string& text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    bool ok = WriteFile(f, text.data(), DWORD(text.size()), &wrote, nullptr) && wrote == text.size();
    CloseHandle(f);
    return ok;
}

std::string DefaultSettingsXml() {
    std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
        "<!-- Written by FreeDisplay. Virtual Display Driver settings; see\r\n"
        "     https://github.com/VirtualDrivers/Virtual-Display-Driver -->\r\n"
        "<vdd_settings>\r\n"
        "    <monitors>\r\n        <count>1</count>\r\n    </monitors>\r\n"
        "    <gpu>\r\n        <friendlyname>" + XmlEscape(PrimaryGpuName()) + "</friendlyname>\r\n    </gpu>\r\n"
        "    <global>\r\n";
    for (int hz : kRefreshRates) xml += "        <g_refresh_rate>" + std::to_string(hz) + "</g_refresh_rate>\r\n";
    xml +=
        "    </global>\r\n"
        "    <resolutions>\r\n";
    for (const auto& r : kResolutions) {
        xml += "        <resolution><width>" + std::to_string(r[0]) + "</width><height>" + std::to_string(r[1]) +
               "</height><refresh_rate>60</refresh_rate></resolution>\r\n";
    }
    xml +=
        "    </resolutions>\r\n"
        "    <options>\r\n"
        "        <CustomEdid>false</CustomEdid>\r\n"
        "        <PreventSpoof>false</PreventSpoof>\r\n"
        "        <EdidCeaOverride>false</EdidCeaOverride>\r\n"
        "        <HardwareCursor>true</HardwareCursor>\r\n"
        "        <SDR10bit>false</SDR10bit>\r\n"
        "        <HDRPlus>false</HDRPlus>\r\n"
        "        <logging>false</logging>\r\n"
        "        <debuglogging>false</debuglogging>\r\n"
        "    </options>\r\n"
        "</vdd_settings>\r\n";
    return xml;
}

// True when a settings file exists that FreeDisplay did not write.
bool HasOwnSettings(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    char head[512] = {};
    DWORD got = 0;
    ReadFile(f, head, sizeof head - 1, &got, nullptr);
    CloseHandle(f);
    return strstr(head, "Written by FreeDisplay") == nullptr;
}

ULONGLONG NowFiletime() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (ULONGLONG(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

void SetString(HKEY key, const wchar_t* name, const std::wstring& value) {
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                   DWORD((value.size() + 1) * sizeof(wchar_t)));
}

// Removes every Root\MttVDD device node.
void RemoveDevices() {
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, 0);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVINFO_DATA dev{sizeof dev};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        if (!HasHardwareId(set, dev)) continue;
        BOOL reboot = FALSE;
        if (!DiUninstallDevice(nullptr, set, &dev, 0, &reboot))
            SetupDiCallClassInstaller(DIF_REMOVE, set, &dev);
    }
    SetupDiDestroyDeviceInfoList(set);
}

// Deletes the MttVDD package from the driver store (oemNN.inf whose catalog is MttVDD.cat).
void DeleteDriverPackage() {
    wchar_t windir[MAX_PATH];
    GetWindowsDirectoryW(windir, MAX_PATH);
    std::wstring infDir = std::wstring(windir) + L"\\INF\\";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((infDir + L"oem*.inf").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> ours;
    do {
        HINF inf = SetupOpenInfFileW((infDir + fd.cFileName).c_str(), nullptr, INF_STYLE_WIN4, nullptr);
        if (inf == INVALID_HANDLE_VALUE) continue;
        wchar_t cat[MAX_PATH] = {};
        INFCONTEXT ctx;
        if (SetupFindFirstLineW(inf, L"Version", L"CatalogFile", &ctx) &&
            SetupGetStringFieldW(&ctx, 1, cat, MAX_PATH, nullptr) && _wcsicmp(cat, L"MttVDD.cat") == 0)
            ours.push_back(fd.cFileName);
        SetupCloseInfFile(inf);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (const auto& name : ours) SetupUninstallOEMInfW(name.c_str(), SUOI_FORCEDELETE, nullptr);
}

bool RunSelfElevated(const wchar_t* args, DWORD* exitCode) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    SHELLEXECUTEINFOW sei{sizeof sei};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = args;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        Log("Elevated %s not started (declined or failed, error %lu)", Narrow(args).c_str(), GetLastError());
        return false;
    }
    WaitForSingleObject(sei.hProcess, 5 * 60 * 1000);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    if (exitCode) *exitCode = code;
    Log("Elevated %s finished with code 0x%lx", Narrow(args).c_str(), code);
    return code == 0;
}

} // namespace

bool Present() {
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    SP_DEVINFO_DATA dev{sizeof dev};
    for (DWORD i = 0; !found && SetupDiEnumDeviceInfo(set, i, &dev); i++) found = HasHardwareId(set, dev);
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

bool InstalledByUs() {
    DWORD v = 0, size = sizeof v;
    return RegGetValueW(HKEY_LOCAL_MACHINE, kOurKey, L"InstalledDriver", RRF_RT_REG_DWORD, nullptr, &v, &size) ==
               ERROR_SUCCESS &&
           v == 1;
}

bool RestartPending() {
    ULONGLONG installedAt = 0;
    DWORD size = sizeof installedAt;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kOurKey, L"DriverInstalledAt", RRF_RT_REG_QWORD, nullptr, &installedAt,
                     &size) != ERROR_SUCCESS)
        return false;
    ULONGLONG bootedAt = NowFiletime() - GetTickCount64() * 10000; // FILETIME is in 100 ns units
    return installedAt > bootedAt;
}

bool RunElevatedInstall() { return RunSelfElevated(L"--install-driver", nullptr); }

bool RunElevatedUninstall() { return RunSelfElevated(L"--uninstall-driver", nullptr); }

int Install() {
    std::wstring dir = InstallDir();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    std::wstring base = dir + L"\\";
    if (!WriteResource(IDR_VDD_INF, base + L"MttVDD.inf") || !WriteResource(IDR_VDD_DLL, base + L"MttVDD.dll") ||
        !WriteResource(IDR_VDD_CAT, base + L"MttVDD.cat") ||
        !WriteResource(IDR_VDD_LICENSE, base + L"LICENSE-VirtualDisplayDriver.txt"))
        return kErrNoPayload;
    // Write the settings unless the user has their own (ours carry a marker comment,
    // so a file from an older FreeDisplay gets replaced).
    if (!HasOwnSettings(base + L"vdd_settings.xml")) WriteText(base + L"vdd_settings.xml", DefaultSettingsXml());

    // Tell the driver (and VDD's own tools) where its settings live.
    HKEY key;
    DWORD disposition = 0;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kVddKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, &disposition) ==
        ERROR_SUCCESS) {
        SetString(key, L"VDDPATH", dir);
        if (disposition == REG_CREATED_NEW_KEY) SetString(key, L"InstalledBy", L"FreeDisplay");
        RegCloseKey(key);
    }

    // A root-enumerated device node for the driver to attach to (what VDD's installer does).
    bool createdNode = false;
    if (!Present()) {
        HDEVINFO set = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_DISPLAY, nullptr);
        if (set == INVALID_HANDLE_VALUE) return int(GetLastError());
        SP_DEVINFO_DATA dev{sizeof dev};
        static const wchar_t hwid[] = L"Root\\MttVDD\0"; // REG_MULTI_SZ: the array adds the final \0
        bool ok = SetupDiCreateDeviceInfoW(set, L"Display", &GUID_DEVCLASS_DISPLAY, nullptr, nullptr,
                                           DICD_GENERATE_ID, &dev) &&
                  SetupDiSetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, reinterpret_cast<const BYTE*>(hwid),
                                                    sizeof hwid) &&
                  SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &dev);
        DWORD err = GetLastError();
        SetupDiDestroyDeviceInfoList(set);
        if (!ok) return int(err);
        createdNode = true;
    }
    BOOL reboot = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, kHardwareId, (base + L"MttVDD.inf").c_str(), INSTALLFLAG_FORCE,
                                            &reboot)) {
        DWORD err = GetLastError();
        if (createdNode) RemoveDevices();
        return int(err);
    }

    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kOurKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) ==
        ERROR_SUCCESS) {
        DWORD one = 1;
        ULONGLONG now = NowFiletime();
        RegSetValueExW(key, L"InstalledDriver", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof one);
        RegSetValueExW(key, L"DriverInstalledAt", 0, REG_QWORD, reinterpret_cast<const BYTE*>(&now), sizeof now);
        RegCloseKey(key);
    }
    return 0;
}

int Uninstall() {
    if (!InstalledByUs()) return kErrNotOurs; // never remove a driver someone else installed
    RemoveDevices();
    DeleteDriverPackage();

    wchar_t by[64] = {};
    DWORD size = sizeof by;
    bool ourFolder = RegGetValueW(HKEY_LOCAL_MACHINE, kVddKey, L"InstalledBy", RRF_RT_REG_SZ, nullptr, by, &size) ==
                         ERROR_SUCCESS &&
                     _wcsicmp(by, L"FreeDisplay") == 0;
    if (ourFolder) {
        std::wstring base = InstallDir() + L"\\";
        for (const wchar_t* f : {L"MttVDD.inf", L"MttVDD.dll", L"MttVDD.cat", L"LICENSE-VirtualDisplayDriver.txt",
                                 L"vdd_settings.xml"})
            DeleteFileW((base + f).c_str());
        RemoveDirectoryW(InstallDir().c_str()); // only if now empty
        RegDeleteKeyW(HKEY_LOCAL_MACHINE, kVddKey);
        RegDeleteKeyW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\MikeTheTech"); // only succeeds when empty
    }
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, kOurKey);
    return 0;
}

bool RestartWindows() {
    HANDLE token;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        TOKEN_PRIVILEGES tp{1};
        LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr);
        CloseHandle(token);
    }
    return ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_MINOR_INSTALLATION |
                                         SHTDN_REASON_FLAG_PLANNED) != FALSE;
}

} // namespace driver
