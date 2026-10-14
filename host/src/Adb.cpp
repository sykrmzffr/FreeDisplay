#include <windows.h>
#include "Adb.h"
#include "Util.h"
#include <sstream>

bool Adb::Locate() {
    std::vector<std::wstring> candidates = {ExeDir() + L"adb\\adb.exe", ExeDir() + L"adb.exe"};
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

    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + path_ + L"\" " + args;
    BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                  nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
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

std::string Adb::FirstReadyDevice() const {
    std::string out;
    if (Run(L"devices", &out) != 0) return {};
    std::istringstream lines(out);
    std::string line;
    while (std::getline(lines, line)) {
        auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        std::string state = line.substr(tab + 1);
        while (!state.empty() && (state.back() == '\r' || state.back() == ' ')) state.pop_back();
        if (state == "device") return line.substr(0, tab);
    }
    return {};
}
