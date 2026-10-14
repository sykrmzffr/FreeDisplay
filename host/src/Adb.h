#pragma once
#include <windows.h>
#include <string>
#include <vector>

// Thin wrapper over the adb command-line tool.
class Adb {
public:
    // Finds adb.exe next to the host, then in the Android SDK. Returns false if none.
    bool Locate();
    const std::wstring& Path() const { return path_; }

    // Runs `adb <args>` and returns its exit code; stdout+stderr go to `out`.
    int Run(const std::wstring& args, std::string* out = nullptr, DWORD timeoutMs = 30000) const;

    // Serial of the first device in the "device" state, or empty.
    std::string FirstReadyDevice() const;

    // State of the first attached device ("device", "unauthorized", "offline", ...),
    // or empty when none is attached.
    std::string FirstDeviceState() const;

    // Runs `adb -s <serial> <args>`.
    int RunOn(const std::string& serial, const std::wstring& args, std::string* out = nullptr,
              DWORD timeoutMs = 30000) const;

private:
    std::wstring path_;
};
