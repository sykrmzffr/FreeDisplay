#pragma once
#include <windows.h>
#include <functional>
#include <string>

// Posted to the tray window by a second copy of FreeDisplay: show the settings window.
constexpr UINT kShowSettingsMsg = WM_APP + 4;

// Notification-area icon, its menu and the settings window. Runs on the UI thread.
class Tray {
public:
    struct Callbacks {
        std::function<void()> onQualityChanged; // the stream must restart at the new size
        std::function<void()> onQuit;
        std::function<void()> onDisplayChanged; // monitors added/removed/moved (e.g. Win+P)
        std::function<void()> onRestart;        // start a fresh copy, then quit
    };

    bool Create(HINSTANCE inst, Callbacks cb);
    void Destroy();

    // Thread-safe: updates the tooltip / status line, optionally with a balloon.
    void SetStatus(const std::wstring& status, bool notify = false);

    // Pumps messages until Quit is chosen or WM_QUIT arrives.
    int Run();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK SettingsProc(HWND, UINT, WPARAM, LPARAM);
    void ShowMenu();
    void ApplyStatus();
    std::wstring CurrentStatus();

    // Saves settings after a change from the menu or the settings window and
    // restarts the stream if the picture size changed.
    void SettingsChanged(int oldQuality);

    // Settings window (created on demand, one at a time).
    void ShowSettings();
    void LayoutSettings();
    void RefreshSettings(); // controls <- settings
    void OnSettingsCommand(int id, int code);

    HWND hwnd_ = nullptr;
    HWND settings_ = nullptr;
    HFONT font_ = nullptr, titleFont_ = nullptr;
    HINSTANCE inst_ = nullptr;
    HICON icon_ = nullptr;
    Callbacks cb_;
    std::wstring status_ = L"Starting...";
    bool notifyPending_ = false;
    CRITICAL_SECTION cs_{};
};
