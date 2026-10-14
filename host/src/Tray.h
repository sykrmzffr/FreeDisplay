#pragma once
#include <windows.h>
#include <functional>
#include <string>

// Notification-area icon and menu for the host. Runs on the UI thread.
class Tray {
public:
    struct Callbacks {
        std::function<void()> onQualityChanged; // the stream must restart at the new size
        std::function<void()> onQuit;
    };

    bool Create(HINSTANCE inst, Callbacks cb);
    void Destroy();

    // Thread-safe: updates the tooltip / status line, optionally with a balloon.
    void SetStatus(const std::wstring& status, bool notify = false);

    // Pumps messages until Quit is chosen or WM_QUIT arrives.
    int Run();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void ShowMenu();
    void ApplyStatus();
    HICON MakeIcon();

    HWND hwnd_ = nullptr;
    HICON icon_ = nullptr;
    Callbacks cb_;
    std::wstring status_ = L"Starting...";
    bool notifyPending_ = false;
    CRITICAL_SECTION cs_{};
};
