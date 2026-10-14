#pragma once
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <string>

// Timestamped log line to stdout.
inline void Log(const char* fmt, ...) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    printf("[%02d:%02d:%02d.%03d] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
    fflush(stdout);
}

// Logs and returns false when an HRESULT failed.
#define HR_OK(expr) HrOk((expr), #expr, __FILE__, __LINE__)
inline bool HrOk(HRESULT hr, const char* what, const char* file, int line) {
    if (SUCCEEDED(hr)) return true;
    Log("FAILED 0x%08lX: %s (%s:%d)", static_cast<unsigned long>(hr), what, file, line);
    return false;
}

inline std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

inline std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// Directory containing the running executable, with trailing backslash.
inline std::wstring ExeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    return p.substr(0, p.find_last_of(L'\\') + 1);
}

inline uint64_t NowUs() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return uint64_t(c.QuadPart * 1000000 / freq.QuadPart);
}
