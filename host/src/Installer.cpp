#include "Installer.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <objbase.h>
#include <vector>
#include "Driver.h"
#include "Resources.h"
#include "Settings.h"
#include "Util.h"

namespace installer {
namespace {

constexpr const wchar_t* kAppName = L"FreeDisplay";
constexpr const wchar_t* kExeName = L"FreeDisplay.exe";
constexpr const wchar_t* kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\FreeDisplay";
constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

struct PayloadFile {
    int id;
    const wchar_t* path; // relative to the payload directory
};
constexpr PayloadFile kPayload[] = {
    {IDR_APK, L"FreeDisplay.apk"},
    {IDR_ADB, L"adb\\adb.exe"},
    {IDR_ADBWINAPI, L"adb\\AdbWinApi.dll"},
    {IDR_ADBWINUSBAPI, L"adb\\AdbWinUsbApi.dll"},
    {IDR_ADB_NOTICE, L"adb\\NOTICE.txt"},
};

std::wstring g_payloadDir;

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p))) s = p;
    CoTaskMemFree(p);
    return s;
}

std::wstring SelfPath() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

std::wstring ShortcutPath() { return KnownFolder(FOLDERID_Programs) + L"\\FreeDisplay.lnk"; }

bool FileExists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool SamePath(const std::wstring& a, const std::wstring& b) {
    wchar_t fa[MAX_PATH], fb[MAX_PATH];
    GetFullPathNameW(a.c_str(), MAX_PATH, fa, nullptr);
    GetFullPathNameW(b.c_str(), MAX_PATH, fb, nullptr);
    return _wcsicmp(fa, fb) == 0;
}

bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& data) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    data.resize(size_t(size.QuadPart));
    DWORD got = 0;
    bool ok = data.empty() || (ReadFile(f, data.data(), DWORD(data.size()), &got, nullptr) && got == data.size());
    CloseHandle(f);
    return ok;
}

bool SameContents(const std::wstring& path, const void* data, size_t size) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return false;
    if ((uint64_t(a.nFileSizeHigh) << 32 | a.nFileSizeLow) != size) return false;
    std::vector<uint8_t> existing;
    return ReadWholeFile(path, existing) && memcmp(existing.data(), data, size) == 0;
}

// Writes via a temporary file. A file that is in use (a running exe, adb server)
// cannot be overwritten but can be renamed, so it is moved aside to *.old first.
bool WriteFileReplacing(const std::wstring& path, const void* data, size_t size) {
    if (SameContents(path, data, size)) return true;
    std::wstring tmp = path + L".new";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    bool ok = WriteFile(f, data, DWORD(size), &wrote, nullptr) && wrote == size;
    CloseHandle(f);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
    std::wstring old = path + L".old";
    DeleteFileW(old.c_str());
    if (MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING) &&
        MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExW(old.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT); // best effort; may need admin
        return true;
    }
    DeleteFileW(tmp.c_str());
    return false;
}

// Runs a command without a window and without inheriting our handles.
DWORD RunHidden(std::wstring cmdline, DWORD waitMs, const wchar_t* cwd = nullptr) {
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, cwd, &si, &pi))
        return DWORD(-1);
    DWORD code = 0;
    if (waitMs) {
        WaitForSingleObject(pi.hProcess, waitMs);
        GetExitCodeProcess(pi.hProcess, &code);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

void KillAdbServer(const std::wstring& dir) {
    std::wstring adb = dir + L"adb\\adb.exe";
    if (FileExists(adb)) RunHidden(L"\"" + adb + L"\" kill-server", 5000);
}

bool CreateShortcut(const std::wstring& lnk, const std::wstring& target, const std::wstring& dir) {
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;
    IShellLinkW* link = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
        link->SetPath(target.c_str());
        link->SetWorkingDirectory(dir.c_str());
        link->SetDescription(L"Use an Android tablet as a second monitor");
        link->SetIconLocation(target.c_str(), 0);
        IPersistFile* file = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))) {
            ok = SUCCEEDED(file->Save(lnk.c_str(), TRUE));
            file->Release();
        }
        link->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return ok;
}

void SetString(HKEY key, const wchar_t* name, const std::wstring& value) {
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                   DWORD((value.size() + 1) * sizeof(wchar_t)));
}

void SetDword(HKEY key, const wchar_t* name, DWORD value) {
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof value);
}

DWORD DirSizeKb(const std::wstring& dir) {
    uint64_t total = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) total += uint64_t(DirSizeKb(dir + fd.cFileName + L"\\")) * 1024;
        else total += uint64_t(fd.nFileSizeHigh) << 32 | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return DWORD(total / 1024);
}

// Deletes a directory tree; files that are in use are left behind.
void DeleteTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            std::wstring p = dir + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) DeleteTree(p + L"\\");
            else DeleteFileW(p.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

void Message(bool quiet, const std::wstring& text, UINT icon) {
    if (!quiet) MessageBoxW(nullptr, text.c_str(), kAppName, MB_OK | icon | MB_SETFOREGROUND);
}

} // namespace

std::wstring InstallDir() { return KnownFolder(FOLDERID_UserProgramFiles) + L"\\FreeDisplay\\"; }

const std::wstring& PayloadDir() {
    if (g_payloadDir.empty()) g_payloadDir = ExeDir();
    return g_payloadDir;
}

bool RunningInPlace() { return FileExists(ExeDir() + L"FreeDisplay.apk"); }

bool ExtractPayload(const std::wstring& dir) {
    SHCreateDirectoryExW(nullptr, (dir + L"adb").c_str(), nullptr);
    bool ok = true;
    bool adbServerStopped = false;
    for (const auto& file : kPayload) {
        HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(file.id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
        HGLOBAL mem = res ? LoadResource(nullptr, res) : nullptr;
        const void* data = mem ? LockResource(mem) : nullptr;
        if (!data) {
            Log("Installer: resource %d missing", file.id);
            ok = false;
            continue;
        }
        size_t size = SizeofResource(nullptr, res);
        std::wstring path = dir + file.path;
        // A running adb server keeps adb.exe locked; stop it before replacing.
        if (!adbServerStopped && wcsstr(file.path, L"adb\\") && !SameContents(path, data, size)) {
            KillAdbServer(dir);
            adbServerStopped = true;
        }
        if (!WriteFileReplacing(path, data, size)) {
            Log("Installer: could not write %s (error %lu)", Narrow(path).c_str(), GetLastError());
            ok = false;
        }
    }
    return ok;
}

bool PreparePortable() {
    std::wstring dir = KnownFolder(FOLDERID_LocalAppData) + L"\\FreeDisplay\\portable\\";
    if (!ExtractPayload(dir)) return false;
    g_payloadDir = dir;
    return true;
}

bool StopRunningInstance(DWORD timeoutMs) {
    HWND hwnd = FindWindowW(L"FreeDisplayTray", nullptr);
    if (!hwnd) {
        // No tray window: either nothing is running or it has no icon yet.
        HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\FreeDisplayHost");
        if (!m) return true;
        CloseHandle(m);
        Sleep(1000);
        hwnd = FindWindowW(L"FreeDisplayTray", nullptr);
        if (!hwnd) return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return true;
    HANDLE proc = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
    PostMessageW(hwnd, kQuitMsg, 0, 0);
    if (!proc) {
        Sleep(2000);
        return !FindWindowW(L"FreeDisplayTray", nullptr);
    }
    bool stopped = WaitForSingleObject(proc, timeoutMs) == WAIT_OBJECT_0;
    if (!stopped) {
        Log("Installer: running copy did not quit; terminating it");
        TerminateProcess(proc, 1);
        stopped = WaitForSingleObject(proc, 3000) == WAIT_OBJECT_0;
    }
    CloseHandle(proc);
    return stopped;
}

bool Install(bool quiet, bool launch) {
    std::wstring dir = InstallDir();
    std::wstring exe = dir + kExeName;
    if (!StopRunningInstance()) {
        Message(quiet, L"FreeDisplay is running and could not be closed. Quit it from its icon in the "
                       L"notification area, then try again.", MB_ICONWARNING);
        return false;
    }
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);

    bool ok = ExtractPayload(dir);
    if (ok && !SamePath(SelfPath(), exe)) {
        std::vector<uint8_t> self;
        ok = ReadWholeFile(SelfPath(), self) && WriteFileReplacing(exe, self.data(), self.size());
    }
    if (!ok) {
        Message(quiet, L"Could not copy FreeDisplay into\n" + dir + L"\n\nSee whether another program is using it.",
                MB_ICONERROR);
        return false;
    }

    if (!CreateShortcut(ShortcutPath(), exe, dir)) Log("Installer: could not create the Start menu shortcut");

    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) ==
        ERROR_SUCCESS) {
        SetString(key, L"DisplayName", kAppName);
        SetString(key, L"DisplayVersion", FD_VERSION_WSTR);
        SetString(key, L"Publisher", kAppName);
        SetString(key, L"DisplayIcon", exe + L",0");
        SetString(key, L"InstallLocation", dir);
        SetString(key, L"UninstallString", L"\"" + exe + L"\" --uninstall");
        SetString(key, L"QuietUninstallString", L"\"" + exe + L"\" --uninstall --quiet");
        SetDword(key, L"NoModify", 1);
        SetDword(key, L"NoRepair", 1);
        SetDword(key, L"EstimatedSize", DirSizeKb(dir));
        RegCloseKey(key);
    }

    // Start with Windows by default; the tray menu can turn it off.
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        SetString(key, L"FreeDisplay", L"\"" + exe + L"\"");
        RegCloseKey(key);
    }
    Settings& s = AppSettings();
    s.Load();
    s.startWithWindows = true;
    s.Save();

    // The extra monitor comes from Virtual Display Driver; install it if it is missing.
    bool restartNeeded = driver::RestartPending();
    if (!quiet && !driver::Present()) {
        int choice = MessageBoxW(
            nullptr,
            L"FreeDisplay needs Virtual Display Driver to create the extra screen for your tablet. It is free "
            L"and open source, and it is included with FreeDisplay.\n\n"
            L"Windows will ask for permission to install it. Install it now?",
            kAppName, MB_OKCANCEL | MB_ICONINFORMATION | MB_SETFOREGROUND);
        if (choice == IDOK) {
            if (driver::RunElevatedInstall()) {
                restartNeeded = true;
            } else {
                Message(false,
                        L"Virtual Display Driver was not installed. FreeDisplay will mirror your main screen "
                        L"instead of extending it. Run this installer again to retry.",
                        MB_ICONWARNING);
            }
        }
    }

    // An older FreeDisplay wrote driver settings without portrait resolutions.
    bool settingsUpdated = false;
    if (driver::Present() && driver::SettingsOutdated()) {
        settingsUpdated = driver::UpdateSettings() == 0 || (!quiet && driver::RunElevatedUpdateSettings());
        Log("Driver settings update: %s", settingsUpdated ? "done" : "failed");
    }
    if (settingsUpdated && !restartNeeded && !quiet) {
        Message(false,
                L"FreeDisplay updated the extra screen's settings (portrait mode). They take effect after "
                L"your next restart; until then the tablet works in landscape as before.",
                MB_ICONINFORMATION);
    }

    if (restartNeeded) {
        // The new monitor is only safe to capture after a restart (see Driver.h).
        if (!quiet &&
            MessageBoxW(nullptr,
                        L"Almost done: restart your PC to finish setting up the extra screen. FreeDisplay starts "
                        L"by itself after the restart.\n\nRestart now?",
                        kAppName, MB_YESNO | MB_ICONINFORMATION | MB_SETFOREGROUND) == IDYES &&
            !driver::RestartWindows())
            Message(false, L"Windows did not restart. Please restart it yourself.", MB_ICONWARNING);
        return true;
    }

    if (launch) RunHidden(L"\"" + exe + L"\"", 0, dir.c_str());
    Message(quiet,
            L"FreeDisplay is installed and running - look for its icon in the notification area.\n\n"
            L"On the tablet, turn on USB debugging (Developer options), then plug it into this PC "
            L"and allow the USB debugging prompt. The tablet app installs itself the first time.",
            MB_ICONINFORMATION);
    return true;
}

bool Uninstall(bool quiet) {
    if (!quiet && MessageBoxW(nullptr, L"Remove FreeDisplay from this PC?\n\n(The app on your tablet is left as is.)",
                              kAppName, MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) != IDYES)
        return false;

    std::wstring dir = InstallDir();
    if (!StopRunningInstance()) {
        Message(quiet, L"FreeDisplay is running and could not be closed. Quit it from its icon in the "
                       L"notification area, then try again.", MB_ICONWARNING);
        return false;
    }
    KillAdbServer(dir);
    KillAdbServer(KnownFolder(FOLDERID_LocalAppData) + L"\\FreeDisplay\\portable\\");

    // Remove the driver only if FreeDisplay installed it, and ask, since other apps may
    // use it. Done before deleting files: the elevated step runs from this executable.
    if (!quiet && driver::InstalledByUs() &&
        MessageBoxW(nullptr,
                    L"Also remove Virtual Display Driver, which FreeDisplay installed?\n\n"
                    L"Choose No if other apps (for example game streaming) use the virtual screen.",
                    kAppName, MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) == IDYES)
        driver::RunElevatedUninstall();

    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, L"FreeDisplay");
        RegCloseKey(key);
    }
    RegDeleteKeyW(HKEY_CURRENT_USER, kUninstallKey);
    DeleteFileW(ShortcutPath().c_str());
    DeleteTree(Settings::DataDir());
    DeleteTree(KnownFolder(FOLDERID_LocalAppData) + L"\\FreeDisplay\\");

    // The running executable cannot delete itself; let a short-lived cmd finish the job.
    bool removingSelf = SamePath(SelfPath(), dir + kExeName);
    DeleteTree(dir);
    if (removingSelf) {
        std::wstring tempDir = KnownFolder(FOLDERID_LocalAppData) + L"\\Temp";
        SetCurrentDirectoryW(tempDir.c_str());
        wchar_t comspec[MAX_PATH] = L"cmd.exe";
        GetEnvironmentVariableW(L"ComSpec", comspec, MAX_PATH);
        std::wstring target = dir.substr(0, dir.size() - 1);
        RunHidden(L"\"" + std::wstring(comspec) + L"\" /c ping -n 3 127.0.0.1 >nul & rmdir /s /q \"" + target + L"\"",
                  0, tempDir.c_str());
    }
    Message(quiet, L"FreeDisplay has been removed.", MB_ICONINFORMATION);
    return true;
}

} // namespace installer
