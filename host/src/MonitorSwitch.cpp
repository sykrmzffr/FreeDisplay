#include "MonitorSwitch.h"
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <shellapi.h>
#include <sddl.h>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include "Util.h"
#include "VirtualMonitor.h"

namespace MonitorSwitch {
namespace {

constexpr const wchar_t* kTaskOn = L"\\FreeDisplay\\Virtual monitor on";
constexpr const wchar_t* kTaskOff = L"\\FreeDisplay\\Virtual monitor off";

// Safety margins. Restarting the driver while it is busy, or capturing a monitor
// that just appeared, preceded a system freeze in testing.
constexpr uint64_t kMinGapUs = 5000000; // between two switches
constexpr DWORD kSettleMs = 2000;       // after the monitor appears, before anyone captures it

std::mutex g_mu;                 // one switch at a time
std::atomic<int> g_captures{0};  // live CaptureGuards
std::atomic<bool> g_broken{false};
uint64_t g_lastSwitchUs = 0;

std::wstring SystemDir() {
    wchar_t dir[MAX_PATH];
    GetSystemDirectoryW(dir, MAX_PATH);
    return dir;
}

// Runs a console tool without a window and without inheriting handles.
DWORD RunHidden(std::wstring cmdline, DWORD waitMs = 15000) {
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                        &pi))
        return DWORD(-1);
    DWORD code = DWORD(-1);
    if (WaitForSingleObject(pi.hProcess, waitMs) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

std::wstring Schtasks() { return L"\"" + SystemDir() + L"\\schtasks.exe\" "; }

// Instance ID of the VDD device (e.g. ROOT\DISPLAY\0000), enabled or not.
std::wstring FindVddInstanceId() {
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return {};
    std::wstring id;
    SP_DEVINFO_DATA dev{sizeof dev};
    for (DWORD i = 0; id.empty() && SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        wchar_t hw[1024] = {};
        if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, nullptr, reinterpret_cast<BYTE*>(hw),
                                               sizeof hw - sizeof(wchar_t), nullptr))
            continue;
        for (const wchar_t* p = hw; *p; p += wcslen(p) + 1) { // REG_MULTI_SZ
            if (_wcsicmp(p, L"Root\\MttVDD") == 0) {
                wchar_t inst[512];
                if (SetupDiGetDeviceInstanceIdW(set, &dev, inst, 512, nullptr)) id = inst;
                break;
            }
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return id;
}

std::wstring CurrentUserSid() {
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    std::vector<uint8_t> buf(256);
    DWORD len = 0;
    std::wstring sid;
    if (GetTokenInformation(token, TokenUser, buf.data(), DWORD(buf.size()), &len)) {
        LPWSTR s = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &s)) {
            sid = s;
            LocalFree(s);
        }
    }
    CloseHandle(token);
    return sid;
}

std::wstring XmlEscape(const std::wstring& s) {
    std::wstring r;
    for (wchar_t c : s) {
        if (c == L'&') r += L"&amp;";
        else if (c == L'<') r += L"&lt;";
        else if (c == L'>') r += L"&gt;";
        else if (c == L'"') r += L"&quot;";
        else r += c;
    }
    return r;
}

// On-demand task (no triggers) that runs pnputil as SYSTEM. Administrators and
// SYSTEM may manage it; `userSid` may only read and start it.
bool RegisterTask(const wchar_t* name, const std::wstring& args, const std::wstring& userSid,
                  const std::wstring& description) {
    std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.3\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"  <RegistrationInfo>\r\n"
        L"    <Description>" + XmlEscape(description) + L"</Description>\r\n"
        L"    <SecurityDescriptor>D:(A;;FA;;;BA)(A;;FA;;;SY)(A;;GRGX;;;" + userSid + L")</SecurityDescriptor>\r\n"
        L"  </RegistrationInfo>\r\n"
        L"  <Principals><Principal id=\"Author\"><UserId>S-1-5-18</UserId><RunLevel>HighestAvailable</RunLevel>"
        L"</Principal></Principals>\r\n"
        L"  <Settings>\r\n"
        L"    <MultipleInstancesPolicy>Queue</MultipleInstancesPolicy>\r\n"
        L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n"
        L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n"
        L"    <AllowStartOnDemand>true</AllowStartOnDemand>\r\n"
        L"    <Enabled>true</Enabled>\r\n"
        L"    <ExecutionTimeLimit>PT1M</ExecutionTimeLimit>\r\n"
        L"    <Priority>4</Priority>\r\n"
        L"  </Settings>\r\n"
        L"  <Actions Context=\"Author\"><Exec>\r\n"
        L"    <Command>" + XmlEscape(SystemDir() + L"\\pnputil.exe") + L"</Command>\r\n"
        L"    <Arguments>" + XmlEscape(args) + L"</Arguments>\r\n"
        L"  </Exec></Actions>\r\n"
        L"</Task>\r\n";

    wchar_t tmpDir[MAX_PATH], tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmpDir);
    GetTempFileNameW(tmpDir, L"fdt", 0, tmp);
    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote;
    const wchar_t bom = 0xFEFF;
    WriteFile(f, &bom, sizeof bom, &wrote, nullptr);
    WriteFile(f, xml.data(), DWORD(xml.size() * sizeof(wchar_t)), &wrote, nullptr);
    CloseHandle(f);
    DWORD rc = RunHidden(Schtasks() + L"/create /f /tn \"" + name + L"\" /xml \"" + tmp + L"\"");
    DeleteFileW(tmp);
    return rc == 0;
}

bool RunSelfElevated(const wchar_t* args) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    SHELLEXECUTEINFOW sei{sizeof sei};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = args;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) return false; // declined or failed
    WaitForSingleObject(sei.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return code == 0;
}

} // namespace

// The device is started and reports no problem (e.g. not crashed or failed).
bool DeviceHealthy() {
    static const std::wstring id = FindVddInstanceId();
    DEVINST dn;
    if (id.empty() ||
        CM_Locate_DevNodeW(&dn, const_cast<wchar_t*>(id.c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
        return false;
    ULONG status = 0, problem = 0;
    if (CM_Get_DevNode_Status(&status, &problem, dn, 0) != CR_SUCCESS) return false;
    if (problem || !(status & DN_STARTED)) {
        Log("Virtual Display Driver device status 0x%lx, problem %lu", status, problem);
        return false;
    }
    return true;
}

bool VddInstalled() { return !FindVddInstanceId().empty(); }

bool Available() { return RunHidden(Schtasks() + L"/query /tn \"" + kTaskOff + L"\"", 10000) == 0; }

bool IsOn() { return VirtualMonitor::IsAttached(); }

CaptureGuard::CaptureGuard() { g_captures++; }
CaptureGuard::~CaptureGuard() { g_captures--; }

bool Broken() { return g_broken; }

bool Set(bool on, DWORD timeoutMs) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_broken) return false;
    if (IsOn() == on) return true;
    if (g_captures > 0) {
        Log("Not switching the virtual monitor %s: it is being captured", on ? "on" : "off");
        return false;
    }
    uint64_t since = NowUs() - g_lastSwitchUs;
    if (g_lastSwitchUs && since < kMinGapUs) Sleep(DWORD((kMinGapUs - since) / 1000));

    uint64_t start = NowUs();
    bool reached = false;
    if (RunHidden(Schtasks() + L"/run /tn \"" + (on ? kTaskOn : kTaskOff) + L"\"", 10000) != 0) {
        Log("Could not start the virtual monitor %s task", on ? "on" : "off");
    } else {
        while (!reached && NowUs() - start < uint64_t(timeoutMs) * 1000) {
            reached = IsOn() == on;
            if (!reached) Sleep(100);
        }
    }
    g_lastSwitchUs = NowUs();
    if (reached && on) {
        Sleep(kSettleMs);
        reached = IsOn() && DeviceHealthy();
    }
    if (reached) {
        Log("Virtual monitor turned %s in %.1f s", on ? "on" : "off", (NowUs() - start) / 1e6);
        return true;
    }
    // Something is wrong with the driver: stop switching for this run and leave the
    // monitor on, which is how FreeDisplay behaved before this feature.
    g_broken = true;
    Log("Virtual monitor did not turn %s properly; automatic switching paused until FreeDisplay restarts",
        on ? "on" : "off");
    if (!IsOn()) RunHidden(Schtasks() + L"/run /tn \"" + kTaskOn + L"\"", 10000);
    return false;
}

bool RunElevatedSetup() { return RunSelfElevated(L"--setup-monitor-switch"); }

bool RunElevatedRemoval() { return RunSelfElevated(L"--remove-monitor-switch"); }

int SetupTasks() {
    std::wstring id = FindVddInstanceId();
    std::wstring sid = CurrentUserSid();
    if (id.empty() || sid.empty()) return 2;
    std::wstring quoted = L"\"" + id + L"\"";
    bool ok = RegisterTask(kTaskOn, L"/enable-device " + quoted, sid,
                           L"FreeDisplay: enables the Virtual Display Driver when the tablet connects.") &&
              RegisterTask(kTaskOff, L"/disable-device " + quoted, sid,
                           L"FreeDisplay: disables the Virtual Display Driver when the tablet disconnects.");
    return ok ? 0 : 1;
}

int RemoveTasks() {
    std::wstring id = FindVddInstanceId();
    if (!id.empty()) RunHidden(L"\"" + SystemDir() + L"\\pnputil.exe\" /enable-device \"" + id + L"\"", 30000);
    RunHidden(Schtasks() + L"/delete /f /tn \"" + kTaskOn + L"\"");
    RunHidden(Schtasks() + L"/delete /f /tn \"" + kTaskOff + L"\"");
    return 0;
}

} // namespace MonitorSwitch
