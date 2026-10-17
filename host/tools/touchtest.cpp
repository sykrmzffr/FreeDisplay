// Diagnostic: injects taps with several Windows input APIs at the centre of a
// monitor and logs which ones reach a test window covering that monitor.
// Usage: touchtest <displayIndex>   (index as printed by Tandem --list)
#include <windows.h>
#include <cstdio>
#include <thread>
#include "../src/Capture.h"
#include "../src/Util.h"

static RECT g_target;
static const char* g_phase = "";

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_POINTERDOWN || msg == WM_POINTERUP) {
        POINTER_INFO pi{};
        GetPointerInfo(GET_POINTERID_WPARAM(wp), &pi);
        Log("  [%s] window got %s type=%d pixel=(%ld,%ld)", g_phase,
            msg == WM_POINTERDOWN ? "POINTERDOWN" : "POINTERUP", int(pi.pointerType),
            pi.ptPixelLocation.x, pi.ptPixelLocation.y);
    } else if (msg == WM_LBUTTONDOWN) {
        Log("  [%s] window got WM_LBUTTONDOWN", g_phase);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static POINTER_TYPE_INFO TouchInfo(POINT c, POINTER_FLAGS flags) {
    POINTER_TYPE_INFO p{};
    p.type = PT_TOUCH;
    p.touchInfo.pointerInfo.pointerType = PT_TOUCH;
    p.touchInfo.pointerInfo.pointerId = 0;
    p.touchInfo.pointerInfo.ptPixelLocation = c;
    p.touchInfo.pointerInfo.pointerFlags = flags;
    p.touchInfo.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_ORIENTATION | TOUCH_MASK_PRESSURE;
    p.touchInfo.rcContact = RECT{c.x - 2, c.y - 2, c.x + 2, c.y + 2};
    p.touchInfo.orientation = 90;
    p.touchInfo.pressure = 32000;
    return p;
}

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int index = argc > 1 ? _wtoi(argv[1]) : 1;
    auto outs = ListOutputs();
    if (index >= int(outs.size())) return 1;
    g_target = outs[index].desktop;
    Log("Target display %d: (%ld,%ld)-(%ld,%ld)", index, g_target.left, g_target.top,
        g_target.right, g_target.bottom);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FdTouchTest";
    wc.hbrBackground = (HBRUSH)GetStockObject(DKGRAY_BRUSH);
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"touch test", WS_POPUP | WS_VISIBLE,
                                g_target.left, g_target.top, g_target.right - g_target.left,
                                g_target.bottom - g_target.top, nullptr, nullptr, wc.hInstance, nullptr);

    std::thread([hwnd] {
        POINT c{(g_target.left + g_target.right) / 2, (g_target.top + g_target.bottom) / 2};
        Sleep(500);

        // 1. Synthetic touch device (InjectSyntheticPointerInput).
        g_phase = "synthetic touch";
        Log("%s:", g_phase);
        HSYNTHETICPOINTERDEVICE touch = CreateSyntheticPointerDevice(PT_TOUCH, 1, POINTER_FEEDBACK_DEFAULT);
        Sleep(500);
        for (POINTER_FLAGS f : {POINTER_FLAGS(POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT),
                                POINTER_FLAGS(POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT),
                                POINTER_FLAGS(POINTER_FLAG_UP)}) {
            auto p = TouchInfo(c, f);
            if (!InjectSyntheticPointerInput(touch, &p, 1)) Log("  inject failed %lu", GetLastError());
            Sleep(30);
        }
        Sleep(700);

        // 2. Legacy touch injection (InjectTouchInput).
        g_phase = "InjectTouchInput";
        Log("%s:", g_phase);
        if (!InitializeTouchInjection(1, TOUCH_FEEDBACK_DEFAULT)) Log("  init failed %lu", GetLastError());
        for (POINTER_FLAGS f : {POINTER_FLAGS(POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT),
                                POINTER_FLAGS(POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT),
                                POINTER_FLAGS(POINTER_FLAG_UP)}) {
            auto p = TouchInfo(c, f);
            if (!InjectTouchInput(1, &p.touchInfo)) Log("  inject failed %lu", GetLastError());
            Sleep(30);
        }
        Sleep(700);

        // 3. Synthetic pen.
        g_phase = "synthetic pen";
        Log("%s:", g_phase);
        HSYNTHETICPOINTERDEVICE pen = CreateSyntheticPointerDevice(PT_PEN, 1, POINTER_FEEDBACK_DEFAULT);
        Sleep(500);
        for (POINTER_FLAGS f : {POINTER_FLAGS(POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE),
                                POINTER_FLAGS(POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_FIRSTBUTTON),
                                POINTER_FLAGS(POINTER_FLAG_UP | POINTER_FLAG_INRANGE),
                                POINTER_FLAGS(POINTER_FLAG_UPDATE)}) {
            POINTER_TYPE_INFO p{};
            p.type = PT_PEN;
            p.penInfo.pointerInfo.pointerType = PT_PEN;
            p.penInfo.pointerInfo.ptPixelLocation = c;
            p.penInfo.pointerInfo.pointerFlags = f;
            p.penInfo.penMask = PEN_MASK_PRESSURE;
            p.penInfo.pressure = 512;
            if (!InjectSyntheticPointerInput(pen, &p, 1)) Log("  inject failed %lu", GetLastError());
            Sleep(30);
        }
        Sleep(700);
        POINT cur;
        GetCursorPos(&cur);
        Log("Cursor now (%ld,%ld)", cur.x, cur.y);
        DestroySyntheticPointerDevice(touch);
        DestroySyntheticPointerDevice(pen);
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }).detach();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (!IsWindow(hwnd)) break;
    }
    return 0;
}
