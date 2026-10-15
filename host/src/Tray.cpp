#include "Tray.h"
#include <shellapi.h>
#include <commctrl.h>
#include "Installer.h"
#include "Resources.h"
#include "Settings.h"
#include "Util.h"
#include <vector>

namespace {
constexpr UINT kTrayMsg = WM_APP + 1;    // icon clicks
constexpr UINT kStatusMsg = WM_APP + 2;  // status changed on another thread
constexpr UINT kIconId = 1;
constexpr UINT_PTR kStatusTimer = 1;

enum MenuId : UINT {
    kIdTouch = 100,
    kIdPen,
    kIdSharp,
    kIdBalanced,
    kIdFast,
    kIdSaverOnBattery,
    kIdSaverAlways,
    kIdSaverOff,
    kIdAutostart,
    kIdMoveBack,
    kIdSettings,
    kIdExtend,
    kIdDuplicate,
    kIdRestart,
    kIdOrientAuto,
    kIdOrientLandscape,
    kIdOrientPortrait,
    kIdLog,
    kIdQuit,
};

// Settings window controls.
enum ControlId : int {
    kCtlTitle = 200,
    kCtlVersion,
    kCtlStatus,
    kCtlInputLabel,
    kCtlTouch,
    kCtlPen,
    kCtlQualityLabel,
    kCtlQuality,
    kCtlSaverLabel,
    kCtlSaver,
    kCtlMoveBack,
    kCtlAutostart,
    kCtlLog,
    kCtlModeLabel,
    kCtlMode,
    kCtlRestart,
    kCtlOrientLabel,
    kCtlOrient,
};

constexpr int kQualityValues[] = {100, 75, 50};
constexpr const wchar_t* kQualityNames[] = {L"Sharp - full resolution", L"Balanced - 75% (recommended)",
                                            L"Fastest - 50%, lowest delay"};
constexpr const wchar_t* kSaverNames[] = {L"When on battery - 60 Hz", L"Always - 60 Hz",
                                          L"Off - up to 120 Hz"};

constexpr const wchar_t* kOrientNames[] = {L"Auto - follow the tablet's rotation", L"Landscape",
                                           L"Portrait"};
constexpr const wchar_t* kModeNames[] = {L"Extend - tablet is a second screen",
                                         L"Duplicate - tablet mirrors this screen"};

// Current projection mode as in Win+P: 0 extend, 1 duplicate, -1 other (e.g. one screen only).
int CurrentMode() {
    UINT32 paths = 0, modes = 0;
    if (GetDisplayConfigBufferSizes(QDC_DATABASE_CURRENT, &paths, &modes) != ERROR_SUCCESS) return -1;
    std::vector<DISPLAYCONFIG_PATH_INFO> p(paths);
    std::vector<DISPLAYCONFIG_MODE_INFO> m(modes);
    DISPLAYCONFIG_TOPOLOGY_ID topology{};
    if (QueryDisplayConfig(QDC_DATABASE_CURRENT, &paths, p.data(), &modes, m.data(), &topology) != ERROR_SUCCESS)
        return -1;
    return topology == DISPLAYCONFIG_TOPOLOGY_EXTEND ? 0 : topology == DISPLAYCONFIG_TOPOLOGY_CLONE ? 1 : -1;
}

// Same as choosing Extend or Duplicate in Win+P; the stream follows on its own.
void SetMode(int mode) {
    LONG rc = SetDisplayConfig(0, nullptr, 0, nullptr,
                               SDC_APPLY | (mode == 1 ? SDC_TOPOLOGY_CLONE : SDC_TOPOLOGY_EXTEND));
    Log("Screen mode -> %s: %s", mode == 1 ? "duplicate" : "extend", rc == ERROR_SUCCESS ? "ok" : "failed");
}

void OpenLog() {
    ShellExecuteW(nullptr, L"open", (Settings::DataDir() + L"host.log").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
} // namespace

bool Tray::Create(HINSTANCE inst, Callbacks cb) {
    cb_ = std::move(cb);
    inst_ = inst;
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

    WNDCLASSEXW sc{sizeof sc};
    sc.lpfnWndProc = SettingsProc;
    sc.hInstance = inst;
    sc.lpszClassName = L"FreeDisplaySettings";
    sc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    sc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    LoadIconMetric(inst, MAKEINTRESOURCEW(IDI_APP), LIM_LARGE, &sc.hIcon);
    LoadIconMetric(inst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &sc.hIconSm);
    RegisterClassExW(&sc);

    LoadIconMetric(inst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &icon_);
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
    if (settings_) DestroyWindow(settings_);
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
    if (font_) DeleteObject(font_);
    if (titleFont_) DeleteObject(titleFont_);
    font_ = titleFont_ = nullptr;
}

void Tray::SetStatus(const std::wstring& status, bool notify) {
    EnterCriticalSection(&cs_);
    status_ = status;
    notifyPending_ |= notify;
    LeaveCriticalSection(&cs_);
    if (hwnd_) PostMessageW(hwnd_, kStatusMsg, 0, 0);
}

std::wstring Tray::CurrentStatus() {
    EnterCriticalSection(&cs_);
    std::wstring status = status_;
    LeaveCriticalSection(&cs_);
    return status;
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
    if (settings_) SetDlgItemTextW(settings_, kCtlStatus, status.c_str());
}

void Tray::SettingsChanged(int oldQuality, int oldOrientation) {
    Settings& s = AppSettings();
    s.Save();
    Log("Settings: touch=%d pen=%d quality=%d%% batterySaver=%d startWithWindows=%d moveWindowsBack=%d",
        int(s.touch), int(s.pen), int(s.qualityPct), int(s.batterySaver), int(s.startWithWindows),
        int(s.moveWindowsBack));
    if ((s.qualityPct != oldQuality || s.orientation != oldOrientation) && cb_.onQualityChanged)
        cb_.onQualityChanged();
    // Battery saver changes are picked up by the stream itself (it restarts at the new rate).
    if (settings_) RefreshSettings();
}

void Tray::ShowMenu() {
    Settings& s = AppSettings();
    std::wstring status = CurrentStatus();

    HMENU quality = CreatePopupMenu();
    AppendMenuW(quality, MF_STRING | (s.qualityPct == 100 ? MF_CHECKED : 0), kIdSharp, L"&Sharp (full resolution)");
    AppendMenuW(quality, MF_STRING | (s.qualityPct == 75 ? MF_CHECKED : 0), kIdBalanced, L"&Balanced (75%)");
    AppendMenuW(quality, MF_STRING | (s.qualityPct == 50 ? MF_CHECKED : 0), kIdFast, L"&Fastest (50%)");

    HMENU saver = CreatePopupMenu();
    AppendMenuW(saver, MF_STRING | (s.batterySaver == Settings::kSaverOnBattery ? MF_CHECKED : 0), kIdSaverOnBattery,
                L"When on &battery (60 Hz)");
    AppendMenuW(saver, MF_STRING | (s.batterySaver == Settings::kSaverAlways ? MF_CHECKED : 0), kIdSaverAlways,
                L"&Always (60 Hz)");
    AppendMenuW(saver, MF_STRING | (s.batterySaver == Settings::kSaverOff ? MF_CHECKED : 0), kIdSaverOff,
                L"&Off (up to 120 Hz)");

    int mode = CurrentMode();
    HMENU screen = CreatePopupMenu();
    AppendMenuW(screen, MF_STRING | (mode == 0 ? MF_CHECKED : 0), kIdExtend, L"&Extend (second screen)");
    AppendMenuW(screen, MF_STRING | (mode == 1 ? MF_CHECKED : 0), kIdDuplicate, L"&Duplicate (mirror)");

    HMENU orient = CreatePopupMenu();
    AppendMenuW(orient, MF_STRING | (s.orientation == 0 ? MF_CHECKED : 0), kIdOrientAuto, L"&Auto (follow the tablet)");
    AppendMenuW(orient, MF_STRING | (s.orientation == 1 ? MF_CHECKED : 0), kIdOrientLandscape, L"&Landscape");
    AppendMenuW(orient, MF_STRING | (s.orientation == 2 ? MF_CHECKED : 0), kIdOrientPortrait, L"&Portrait");

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, status.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdSettings, L"&Settings...");
    SetMenuDefaultItem(menu, kIdSettings, FALSE);
    AppendMenuW(menu, MF_STRING | (s.touch ? MF_CHECKED : 0), kIdTouch, L"&Touch input");
    AppendMenuW(menu, MF_STRING | (s.pen ? MF_CHECKED : 0), kIdPen, L"&Pen input");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(quality), L"&Video quality");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(screen), L"Screen m&ode");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(orient), L"&Orientation");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(saver), L"&Battery saver");
    AppendMenuW(menu, MF_STRING | (s.moveWindowsBack ? MF_CHECKED : 0), kIdMoveBack,
                L"&Move windows back when the tablet disconnects");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (s.startWithWindows ? MF_CHECKED : 0), kIdAutostart, L"Start with &Windows");
    AppendMenuW(menu, MF_STRING, kIdLog, L"Open &log");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdRestart, L"&Restart FreeDisplay");
    AppendMenuW(menu, MF_STRING, kIdQuit, L"&Quit FreeDisplay");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_); // so the menu closes when clicking elsewhere
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu); // also destroys the submenus

    int oldQuality = s.qualityPct, oldOrientation = s.orientation;
    switch (cmd) {
    case kIdOrientAuto: s.orientation = 0; break;
    case kIdOrientLandscape: s.orientation = 1; break;
    case kIdOrientPortrait: s.orientation = 2; break;
    case kIdTouch: s.touch = !s.touch; break;
    case kIdPen: s.pen = !s.pen; break;
    case kIdSharp: s.qualityPct = 100; break;
    case kIdBalanced: s.qualityPct = 75; break;
    case kIdFast: s.qualityPct = 50; break;
    case kIdSaverOnBattery: s.batterySaver = Settings::kSaverOnBattery; break;
    case kIdSaverAlways: s.batterySaver = Settings::kSaverAlways; break;
    case kIdSaverOff: s.batterySaver = Settings::kSaverOff; break;
    case kIdMoveBack: s.moveWindowsBack = !s.moveWindowsBack; break;
    case kIdAutostart:
        if (Settings::ApplyStartWithWindows(!s.startWithWindows)) s.startWithWindows = !s.startWithWindows;
        break;
    case kIdSettings: ShowSettings(); return;
    case kIdExtend: SetMode(0); return;
    case kIdDuplicate: SetMode(1); return;
    case kIdRestart:
        if (cb_.onRestart) cb_.onRestart();
        return;
    case kIdLog: OpenLog(); return;
    case kIdQuit:
        if (cb_.onQuit) cb_.onQuit();
        PostQuitMessage(0);
        return;
    default:
        return;
    }
    SettingsChanged(oldQuality, oldOrientation);
}

// ---- settings window ------------------------------------------------------

void Tray::ShowSettings() {
    if (settings_) {
        ShowWindow(settings_, SW_RESTORE);
        SetForegroundWindow(settings_);
        return;
    }
    settings_ = CreateWindowExW(0, L"FreeDisplaySettings", L"FreeDisplay settings",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
                                400, 400, nullptr, nullptr, inst_, this);
    if (!settings_) return;
    auto add = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
        CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, settings_,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst_, nullptr);
    };
    add(L"STATIC", L"FreeDisplay", SS_LEFT, kCtlTitle);
    add(L"STATIC", L"Version " FD_VERSION_WSTR, SS_RIGHT, kCtlVersion);
    add(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, kCtlStatus);
    add(L"STATIC", L"Tablet input", SS_LEFT, kCtlInputLabel);
    add(L"BUTTON", L"&Touch", BS_AUTOCHECKBOX | WS_TABSTOP, kCtlTouch);
    add(L"BUTTON", L"&Pen", BS_AUTOCHECKBOX | WS_TABSTOP, kCtlPen);
    add(L"STATIC", L"Video &quality", SS_LEFT, kCtlQualityLabel);
    add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, kCtlQuality);
    add(L"STATIC", L"&Battery saver", SS_LEFT, kCtlSaverLabel);
    add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, kCtlSaver);
    add(L"STATIC", L"Screen m&ode", SS_LEFT, kCtlModeLabel);
    add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, kCtlMode);
    add(L"STATIC", L"Tablet o&rientation", SS_LEFT, kCtlOrientLabel);
    add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, kCtlOrient);
    add(L"BUTTON", L"&Move windows back when the tablet disconnects", BS_AUTOCHECKBOX | WS_TABSTOP,
        kCtlMoveBack);
    add(L"BUTTON", L"Start with &Windows", BS_AUTOCHECKBOX | WS_TABSTOP, kCtlAutostart);
    add(L"BUTTON", L"Open &log", BS_PUSHBUTTON | WS_TABSTOP, kCtlLog);
    add(L"BUTTON", L"&Restart app", BS_PUSHBUTTON | WS_TABSTOP, kCtlRestart);
    add(L"BUTTON", L"Close", BS_DEFPUSHBUTTON | WS_TABSTOP, IDCANCEL);
    for (const wchar_t* name : kQualityNames)
        SendDlgItemMessageW(settings_, kCtlQuality, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
    for (const wchar_t* name : kSaverNames)
        SendDlgItemMessageW(settings_, kCtlSaver, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
    for (const wchar_t* name : kOrientNames)
        SendDlgItemMessageW(settings_, kCtlOrient, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
    for (const wchar_t* name : kModeNames)
        SendDlgItemMessageW(settings_, kCtlMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));

    LayoutSettings();
    RefreshSettings();
    SetDlgItemTextW(settings_, kCtlStatus, CurrentStatus().c_str());

    // Center on the monitor the pointer is on.
    RECT wr;
    GetWindowRect(settings_, &wr);
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    SetWindowPos(settings_, nullptr, (mi.rcWork.left + mi.rcWork.right - w) / 2,
                 (mi.rcWork.top + mi.rcWork.bottom - h) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    ShowWindow(settings_, SW_SHOW);
    SetForegroundWindow(settings_);
    SetTimer(settings_, kStatusTimer, 1000, nullptr);
}

// Positions the controls for the window's current DPI (all sizes in 96-DPI units).
void Tray::LayoutSettings() {
    UINT dpi = GetDpiForWindow(settings_);
    auto px = [dpi](int v) { return MulDiv(v, int(dpi), 96); };

    NONCLIENTMETRICSW ncm{sizeof ncm};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi);
    if (font_) DeleteObject(font_);
    if (titleFont_) DeleteObject(titleFont_);
    font_ = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW title = ncm.lfMessageFont;
    title.lfHeight = title.lfHeight * 3 / 2;
    title.lfWeight = FW_SEMIBOLD;
    titleFont_ = CreateFontIndirectW(&title);

    const int margin = 20, width = 380, inner = width - 2 * margin;
    struct Place {
        int id, x, y, w, h;
    };
    const Place places[] = {
        {kCtlTitle, margin, 16, 200, 28},
        {kCtlVersion, width - margin - 120, 24, 120, 20},
        {kCtlStatus, margin, 50, inner, 36},
        {kCtlInputLabel, margin, 96, inner, 20},
        {kCtlTouch, margin, 118, 120, 22},
        {kCtlPen, margin + 130, 118, 120, 22},
        {kCtlQualityLabel, margin, 152, inner, 20},
        {kCtlQuality, margin, 174, inner, 200},
        {kCtlSaverLabel, margin, 210, inner, 20},
        {kCtlSaver, margin, 232, inner, 200},
        {kCtlModeLabel, margin, 268, inner, 20},
        {kCtlMode, margin, 290, inner, 200},
        {kCtlOrientLabel, margin, 326, inner, 20},
        {kCtlOrient, margin, 348, inner, 200},
        {kCtlMoveBack, margin, 388, inner, 22},
        {kCtlAutostart, margin, 414, inner, 22},
        {kCtlLog, margin, 456, 105, 30},
        {kCtlRestart, margin + 117, 456, 106, 30},
        {IDCANCEL, width - margin - 105, 456, 105, 30},
    };
    for (const Place& p : places) {
        HWND c = GetDlgItem(settings_, p.id);
        SetWindowPos(c, nullptr, px(p.x), px(p.y), px(p.w), px(p.h), SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(p.id == kCtlTitle ? titleFont_ : font_), TRUE);
    }

    RECT rc{0, 0, px(width), px(504)};
    AdjustWindowRectExForDpi(&rc, GetWindowLongW(settings_, GWL_STYLE), FALSE, 0, dpi);
    SetWindowPos(settings_, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void Tray::RefreshSettings() {
    Settings& s = AppSettings();
    CheckDlgButton(settings_, kCtlTouch, s.touch ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(settings_, kCtlPen, s.pen ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(settings_, kCtlMoveBack, s.moveWindowsBack ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(settings_, kCtlAutostart, s.startWithWindows ? BST_CHECKED : BST_UNCHECKED);
    int q = 1;
    for (int i = 0; i < 3; i++)
        if (kQualityValues[i] == s.qualityPct) q = i;
    SendDlgItemMessageW(settings_, kCtlQuality, CB_SETCURSEL, q, 0);
    SendDlgItemMessageW(settings_, kCtlSaver, CB_SETCURSEL, s.batterySaver.load(), 0);
    SendDlgItemMessageW(settings_, kCtlMode, CB_SETCURSEL, CurrentMode(), 0); // -1 selects nothing
    SendDlgItemMessageW(settings_, kCtlOrient, CB_SETCURSEL, s.orientation.load(), 0);
}

void Tray::OnSettingsCommand(int id, int code) {
    Settings& s = AppSettings();
    int oldQuality = s.qualityPct, oldOrientation = s.orientation;
    auto checked = [&](int ctl) { return IsDlgButtonChecked(settings_, ctl) == BST_CHECKED; };
    switch (id) {
    case kCtlTouch: s.touch = checked(kCtlTouch); break;
    case kCtlPen: s.pen = checked(kCtlPen); break;
    case kCtlMoveBack: s.moveWindowsBack = checked(kCtlMoveBack); break;
    case kCtlAutostart: {
        bool want = checked(kCtlAutostart);
        if (Settings::ApplyStartWithWindows(want)) s.startWithWindows = want;
        break;
    }
    case kCtlQuality: {
        if (code != CBN_SELCHANGE) return;
        LRESULT i = SendDlgItemMessageW(settings_, kCtlQuality, CB_GETCURSEL, 0, 0);
        if (i < 0 || i > 2) return;
        s.qualityPct = kQualityValues[i];
        break;
    }
    case kCtlSaver: {
        if (code != CBN_SELCHANGE) return;
        LRESULT i = SendDlgItemMessageW(settings_, kCtlSaver, CB_GETCURSEL, 0, 0);
        if (i < Settings::kSaverOnBattery || i > Settings::kSaverOff) return;
        s.batterySaver = int(i);
        break;
    }
    case kCtlMode: {
        if (code != CBN_SELCHANGE) return;
        LRESULT i = SendDlgItemMessageW(settings_, kCtlMode, CB_GETCURSEL, 0, 0);
        if (i == 0 || i == 1) SetMode(int(i));
        return;
    }
    case kCtlRestart:
        if (cb_.onRestart) cb_.onRestart();
        return;
    case kCtlOrient: {
        if (code != CBN_SELCHANGE) return;
        LRESULT i = SendDlgItemMessageW(settings_, kCtlOrient, CB_GETCURSEL, 0, 0);
        if (i < 0 || i > 2) return;
        s.orientation = int(i);
        break;
    }
    case kCtlLog: OpenLog(); return;
    case IDCANCEL: DestroyWindow(settings_); return;
    default: return;
    }
    SettingsChanged(oldQuality, oldOrientation);
}

LRESULT CALLBACK Tray::SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<Tray*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_COMMAND:
        if (self->settings_) self->OnSettingsCommand(LOWORD(wp), HIWORD(wp));
        return 0;
    case WM_CTLCOLORSTATIC: { // labels on the window background; the status line in gray
        HDC dc = reinterpret_cast<HDC>(wp);
        SetBkMode(dc, TRANSPARENT);
        int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
        SetTextColor(dc, GetSysColor(id == kCtlStatus || id == kCtlVersion ? COLOR_GRAYTEXT : COLOR_WINDOWTEXT));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_TIMER:
        SetDlgItemTextW(hwnd, kCtlStatus, self->CurrentStatus().c_str());
        return 0;
    case WM_DPICHANGED: {
        auto* r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        if (self->settings_) self->LayoutSettings();
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kStatusTimer);
        self->settings_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- tray window ----------------------------------------------------------

LRESULT CALLBACK Tray::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    static UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    auto* self = reinterpret_cast<Tray*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        if (msg == kTrayMsg) {
            UINT event = LOWORD(lp);
            if (event == WM_CONTEXTMENU) self->ShowMenu();
            else if (event == WM_LBUTTONUP || event == NIN_SELECT || event == NIN_KEYSELECT) self->ShowSettings();
            return 0;
        }
        if (msg == kStatusMsg) {
            self->ApplyStatus();
            return 0;
        }
        if (msg == WM_DISPLAYCHANGE) {
            if (self->settings_) self->RefreshSettings(); // e.g. Win+P changed the screen mode
            if (self->cb_.onDisplayChanged) self->cb_.onDisplayChanged();
            return 0;
        }
        if (msg == kShowSettingsMsg) { // FreeDisplay was started again (e.g. from the Start menu)
            self->ShowSettings();
            return 0;
        }
        if (msg == installer::kQuitMsg) { // an installer or uninstaller wants us gone
            if (self->cb_.onQuit) self->cb_.onQuit();
            PostQuitMessage(0);
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
        if (settings_ && IsDialogMessageW(settings_, &msg)) continue; // Tab, Esc, mnemonics
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
