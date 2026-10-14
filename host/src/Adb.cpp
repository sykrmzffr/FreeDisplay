#include <windows.h>
#include "Adb.h"
#include "Installer.h"
#include "Util.h"
#include <sstream>

bool Adb::Locate() {
    std::vector<std::wstring> candidates = {installer::PayloadDir() + L"adb\\adb.exe", ExeDir() + L"adb.exe"};
    wchar_t buf[MAX_PATH];
    for (const wchar_t* var : {L"ANDROID_HOME", L"ANDROID_SDK_ROOT"}) {
        if (GetEnvironmentVariableW(var, buf, MAX_PATH))
            candidates.push_back(std::wstring(buf) + L"\\platform-tools\\adb.exe");
    }
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH))
        candidates.push_back(std::wstring(buf) + L"\\Android\\Sdk\\platform-tools\\adb.exe");
    for (auto& c : candidates) {
        if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) {
            path_ = c;
            return true;
        }
    }
    return false;
}

int Adb::Run(const std::wstring& args, std::string* out, DWORD timeoutMs) const {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    // Pass the child only its std handles. `adb` may start the long-lived adb
    // server, which would otherwise inherit our listening socket and log file
    // and keep them open after we exit.
    HANDLE inherit[] = {wr, nul};
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<uint8_t> attrBuf(attrSize);
    auto attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize);
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                              (nul != INVALID_HANDLE_VALUE ? 2 : 1) * sizeof(HANDLE), nullptr, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = wr;
    si.StartupInfo.hStdError = wr;
    si.StartupInfo.hStdInput = nul;
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + path_ + L"\" " + args;
    BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                  CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                                  &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!started) {
        CloseHandle(rd);
        return -1;
    }

    std::string text;
    char chunk[4096];
    DWORD n = 0;
    while (ReadFile(rd, chunk, sizeof chunk, &n, nullptr) && n > 0) text.append(chunk, n);
    CloseHandle(rd);

    DWORD code = DWORD(-1);
    if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0)
        GetExitCodeProcess(pi.hProcess, &code);
    else
        TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (out) *out = text;
    return int(code);
}

int Adb::RunOn(const std::string& serial, const std::wstring& args, std::string* out,
               DWORD timeoutMs) const {
    return Run(L"-s " + Widen(serial) + L" " + args, out, timeoutMs);
}

namespace {
// Parses `adb devices` into (serial, state) pairs.
std::vector<std::pair<std::string, std::string>> ParseDevices(const std::string& out) {
    std::vector<std::pair<std::string, std::string>> devices;
    std::istringstream lines(out);
    std::string line;
    while (std::getline(lines, line)) {
        auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        std::string state = line.substr(tab + 1);
        while (!state.empty() && (state.back() == '\r' || state.back() == ' ')) state.pop_back();
        devices.emplace_back(line.substr(0, tab), state);
    }
    return devices;
}
} // namespace

std::string Adb::FirstReadyDevice() const {
    std::string out;
    if (Run(L"devices", &out) != 0) return {};
    for (auto& [serial, state] : ParseDevices(out))
        if (state == "device") return serial;
    return {};
}

std::string Adb::FirstDeviceState() const {
    std::string out;
    if (Run(L"devices", &out) != 0) return {};
    auto devices = ParseDevices(out);
    return devices.empty() ? std::string() : devices.front().second;
}
