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
    std::atomic<bool> moveWindowsBack{true}; // rescue windows from the tablet screen when it goes away
    enum Saver : int { kSaverOnBattery = 0, kSaverAlways = 1, kSaverOff = 2 };
    std::atomic<int> batterySaver{kSaverOnBattery}; // 60 Hz instead of up to 120 Hz
    std::atomic<int> orientation{0}; // 0 follow the tablet, 1 landscape, 2 portrait (proto::Orientation)

    // True when battery saver applies right now (setting + power source).
    bool SaverActive() const;

    void Load();
    void Save() const;

    // Adds or removes the HKCU Run entry that starts the host at sign-in.
    static bool ApplyStartWithWindows(bool enable);

    static std::wstring DataDir(); // %APPDATA%\FreeDisplay, created on demand
};

Settings& AppSettings();
