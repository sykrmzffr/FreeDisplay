// Test load: a window on the given display that repaints a moving bar every
// frame, so the streamer always has new frames. Closes itself after N seconds.
// Usage: animate <displayIndex> <seconds>
#include <windows.h>
#include <timeapi.h>
#include "../src/Capture.h"

static int g_x = 0;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        RECT bar{g_x, 0, g_x + 120, rc.bottom};
        FillRect(dc, &bar, (HBRUSH)GetStockObject(WHITE_BRUSH));
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_TIMER) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        g_x = (g_x + 24) % (rc.right > 0 ? rc.right : 1);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    if (msg == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int index = argc > 1 ? _wtoi(argv[1]) : 1;
    int seconds = argc > 2 ? _wtoi(argv[2]) : 15;
    auto outs = ListOutputs();
    if (index >= int(outs.size())) return 1;
    RECT r = outs[index].desktop;
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FdAnimate";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, wc.lpszClassName, L"animate",
                                WS_POPUP | WS_VISIBLE, r.left + 200, r.top + 200, 1600, 900, nullptr,
                                nullptr, wc.hInstance, nullptr);
    timeBeginPeriod(1);                  // let the 1 ms timer actually tick every ~1 ms
    SetTimer(hwnd, 1, 1, nullptr);
    SetTimer(hwnd, 2, seconds * 1000, [](HWND h, UINT, UINT_PTR, DWORD) { DestroyWindow(h); });
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
    return 0;
}
