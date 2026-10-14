#include "Tray.h"
#include <shellapi.h>
#include "Settings.h"
#include "Util.h"

namespace {
constexpr UINT kTrayMsg = WM_APP + 1;    // icon clicks
constexpr UINT kStatusMsg = WM_APP + 2;  // status changed on another thread
constexpr UINT kIconId = 1;

enum MenuId : UINT {
    kIdTouch = 100,
    kIdPen,
    kIdSharp,
    kIdBalanced,
    kIdFast,
    kIdAutostart,
    kIdLog,
    kIdQuit,
};
} // namespace

bool Tray::Create(HINSTANCE inst, Callbacks cb) {
    cb_ = std::move(cb);
    InitializeCriticalSection(&cs_);
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"FreeDisplayTray";
    RegisterClassW(&wc);
    // A message-only window would not receive the taskbar's re-create broadcast.
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"FreeDisplay", 0, 0, 0, 0, 0, nullptr, nullptr, inst, this);
    if (!hwnd_) return false;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    icon_ = MakeIcon();
    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = hwnd_;
    nid.uID = kIconId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = kTrayMsg;
    nid.hIcon = icon_;
    wcscpy_s(nid.szTip, L"FreeDisplay");
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) return false;
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    return true;
}

void Tray::Destroy() {
    if (hwnd_) {
        NOTIFYICONDATAW nid{sizeof nid};
        nid.hWnd = hwnd_;
        nid.uID = kIconId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (icon_) DestroyIcon(icon_);
    icon_ = nullptr;
}

void Tray::SetStatus(const std::wstring& status, bool notify) {
    EnterCriticalSection(&cs_);
    status_ = status;
    notifyPending_ |= notify;
    LeaveCriticalSection(&cs_);
    if (hwnd_) PostMessageW(hwnd_, kStatusMsg, 0, 0);
}

void Tray::ApplyStatus() {
    EnterCriticalSection(&cs_);
    std::wstring status = status_;
    bool notify = notifyPending_;
    notifyPending_ = false;
    LeaveCriticalSection(&cs_);

    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = hwnd_;
    nid.uID = kIconId;
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    std::wstring tip = L"FreeDisplay - " + status;
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    if (notify) {
        nid.uFlags |= NIF_INFO;
        wcscpy_s(nid.szInfoTitle, L"FreeDisplay");
        wcsncpy_s(nid.szInfo, status.c_str(), _TRUNCATE);
        nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    }
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void Tray::ShowMenu() {
    Settings& s = AppSettings();
    EnterCriticalSection(&cs_);
    std::wstring status = status_;
    LeaveCriticalSection(&cs_);

    HMENU quality = CreatePopupMenu();
    AppendMenuW(quality, MF_STRING | (s.qualityPct == 100 ? MF_CHECKED : 0), kIdSharp, L"&Sharp (full resolution)");
    AppendMenuW(quality, MF_STRING | (s.qualityPct == 75 ? MF_CHECKED : 0), kIdBalanced, L"&Balanced (75%)");
    AppendMenuW(quality, MF_STRING | (s.qualityPct == 50 ? MF_CHECKED : 0), kIdFast, L"&Fastest (50%)");

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, status.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (s.touch ? MF_CHECKED : 0), kIdTouch, L"&Touch input");
    AppendMenuW(menu, MF_STRING | (s.pen ? MF_CHECKED : 0), kIdPen, L"&Pen input");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(quality), L"&Video quality");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (s.startWithWindows ? MF_CHECKED : 0), kIdAutostart, L"Start with &Windows");
    AppendMenuW(menu, MF_STRING, kIdLog, L"Open &log");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdQuit, L"&Quit FreeDisplay");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_); // so the menu closes when clicking elsewhere
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu); // also destroys the submenu

    int oldQuality = s.qualityPct;
    switch (cmd) {
    case kIdTouch: s.touch = !s.touch; break;
    case kIdPen: s.pen = !s.pen; break;
    case kIdSharp: s.qualityPct = 100; break;
    case kIdBalanced: s.qualityPct = 75; break;
    case kIdFast: s.qualityPct = 50; break;
    case kIdAutostart:
        if (Settings::ApplyStartWithWindows(!s.startWithWindows)) s.startWithWindows = !s.startWithWindows;
        break;
    case kIdLog:
        ShellExecuteW(nullptr, L"open", (Settings::DataDir() + L"host.log").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    case kIdQuit:
        if (cb_.onQuit) cb_.onQuit();
        PostQuitMessage(0);
        return;
    default:
        return;
    }
    s.Save();
    Log("Settings: touch=%d pen=%d quality=%d%% startWithWindows=%d", int(s.touch), int(s.pen),
        int(s.qualityPct), int(s.startWithWindows));
    if (s.qualityPct != oldQuality && cb_.onQualityChanged) cb_.onQualityChanged();
}

LRESULT CALLBACK Tray::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    static UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    auto* self = reinterpret_cast<Tray*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        if (msg == kTrayMsg) {
            UINT event = LOWORD(lp);
            if (event == WM_CONTEXTMENU || event == WM_LBUTTONUP || event == NIN_SELECT) self->ShowMenu();
            return 0;
        }
        if (msg == kStatusMsg) {
            self->ApplyStatus();
            return 0;
        }
        if (msg == taskbarCreated) { // Explorer restarted: put the icon back
            NOTIFYICONDATAW nid{sizeof nid};
            nid.hWnd = hwnd;
            nid.uID = kIconId;
            nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
            nid.uCallbackMessage = kTrayMsg;
            nid.hIcon = self->icon_;
            wcscpy_s(nid.szTip, L"FreeDisplay");
            Shell_NotifyIconW(NIM_ADD, &nid);
            nid.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &nid);
            self->ApplyStatus();
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int Tray::Run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

// Draws a small tablet-with-screen glyph so no .ico resource is needed.
HICON Tray::MakeIcon() {
    const int n = GetSystemMetrics(SM_CXSMICON) * 2;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP color = CreateCompatibleBitmap(screen, n, n);
    HBITMAP mask = CreateBitmap(n, n, 1, 1, nullptr);
    ReleaseDC(nullptr, screen);

    HGDIOBJ old = SelectObject(dc, mask);
    PatBlt(dc, 0, 0, n, n, WHITENESS); // transparent everywhere...
    RECT body{n / 16, n / 6, n - n / 16, n - n / 6};
    FillRect(dc, &body, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH))); // ...except the tablet
    SelectObject(dc, color);
    HBRUSH frame = CreateSolidBrush(RGB(40, 40, 48));
    HBRUSH glass = CreateSolidBrush(RGB(64, 156, 255));
    FillRect(dc, &body, frame);
    RECT screenRect{body.left + n / 10, body.top + n / 10, body.right - n / 10, body.bottom - n / 10};
    FillRect(dc, &screenRect, glass);
    SelectObject(dc, old);
    DeleteObject(frame);
    DeleteObject(glass);
    DeleteDC(dc);

    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}
