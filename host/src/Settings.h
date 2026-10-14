#pragma once
#include <atomic>
#include <string>

// User preferences, persisted in %APPDATA%\FreeDisplay\settings.ini and shared
// between the tray menu (UI thread) and the streaming thread.
struct Settings {
    std::atomic<bool> touch{true};          // inject finger input
    std::atomic<bool> pen{true};            // inject stylus input
    std::atomic<int> qualityPct{75};        // encode size as % of the monitor (100/75/50)
    std::atomic<bool> startWithWindows{false};

    void Load();
    void Save() const;

    // Adds or removes the HKCU Run entry that starts the host at sign-in.
    static bool ApplyStartWithWindows(bool enable);

    static std::wstring DataDir(); // %APPDATA%\FreeDisplay, created on demand
};

Settings& AppSettings();
