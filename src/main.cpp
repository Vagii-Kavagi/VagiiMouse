// Vagii Mouse - pointer speed booster that goes beyond Windows' 20 speed levels,
// with separate settings for the mouse and the trackpad. Per-user, no admin.
//  Mouse:    levels 1-20 = Windows pointer speed; 21-100 = Windows 20 + boost via a low-level hook.
//  Trackpad: levels 1-20 = Windows precision-touchpad cursor speed; 21-100 = 20 + boost.
// Trackpad moves are told apart from mouse moves by the touchpad's own raw HID reports
// (Digitizer / Touch Pad), which arrive continuously while a finger is on the pad.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <algorithm>

// ---------------------------------------------------------------- settings
static const int MAX_LEVEL = 100;
static const int WIN_MAX = 20;
static const double BOOST_STEP = 0.0625;  // each level above 20 adds 6.25% (level 100 = x6 of Windows max)
enum { DEV_MOUSE = 0, DEV_PAD = 1 };

struct DevSettings {
    int level = 10;
    int accelMode = 1;      // mouse: 0 off, 1 Windows EPP, 2 Vagii curve | trackpad: 0 Windows built-in, 1 Vagii curve on top
    int accelStrength = 6;  // 1..20
    int haveOrig = 0, origSpeed = 10, origAccel = 1;
};
struct Settings {
    DevSettings dev[2];
    int mouseDpi = 800;
    int enabled = 1;
    int gameSafe = 1;
    int startup = 1;
    int closeToTray = 1;
    int tab = 0;
} S;

static wchar_t g_ini[MAX_PATH];
static bool g_test = false;  // VAGII_TEST=1: screenshot instance, own data, changes nothing on the system

static void IniPath() {
    wchar_t base[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, base))) GetTempPathW(MAX_PATH, base);
    std::wstring dir = std::wstring(base) + (g_test ? L"\\VagiiMouseTest" : L"\\VagiiMouse");
    CreateDirectoryW(dir.c_str(), nullptr);
    wcscpy_s(g_ini, (dir + L"\\settings.ini").c_str());
}

// key name, field. Mouse keys keep their 1.0 names.
#define SETTINGS_FIELDS(X) \
    X(L"level", dev[0].level) X(L"accelMode", dev[0].accelMode) X(L"accelStrength", dev[0].accelStrength) \
    X(L"haveOrig", dev[0].haveOrig) X(L"origSpeed", dev[0].origSpeed) X(L"origAccel", dev[0].origAccel) \
    X(L"tpLevel", dev[1].level) X(L"tpAccelMode", dev[1].accelMode) X(L"tpAccelStrength", dev[1].accelStrength) \
    X(L"tpHaveOrig", dev[1].haveOrig) X(L"tpOrigSpeed", dev[1].origSpeed) \
    X(L"mouseDpi", mouseDpi) X(L"enabled", enabled) X(L"gameSafe", gameSafe) X(L"startup", startup) \
    X(L"closeToTray", closeToTray) X(L"tab", tab)

static void LoadSettings() {
    S.dev[1].accelMode = 0;
#define X(k, f) S.f = (int)GetPrivateProfileIntW(L"mouse", k, S.f, g_ini);
    SETTINGS_FIELDS(X)
#undef X
    for (auto& d : S.dev) {
        d.level = std::clamp(d.level, 1, MAX_LEVEL);
        d.accelStrength = std::clamp(d.accelStrength, 1, 20);
    }
    S.dev[0].accelMode = std::clamp(S.dev[0].accelMode, 0, 2);
    S.dev[1].accelMode = std::clamp(S.dev[1].accelMode, 0, 1);
    S.mouseDpi = std::clamp(S.mouseDpi, 100, 64000);
    S.tab = std::clamp(S.tab, 0, 1);
}
static void SaveSettings() {
    wchar_t buf[32];
#define X(k, f) swprintf_s(buf, L"%d", S.f); WritePrivateProfileStringW(L"mouse", k, buf, g_ini);
    SETTINGS_FIELDS(X)
#undef X
}

// Windows pointer-speed factor (Enhance pointer precision OFF), speeds 1..20.
static const double kWinFactor[21] = {0, 1.0/32, 1.0/16, 1.0/8, 2.0/8, 3.0/8, 4.0/8, 5.0/8, 6.0/8, 7.0/8, 1.0,
                                      1.25, 1.5, 1.75, 2.0, 2.25, 2.5, 2.75, 3.0, 3.25, 3.5};
static int WinSpeedFor(int level) { return std::min(level, WIN_MAX); }
static double BoostFor(int level) { return level <= WIN_MAX ? 1.0 : 1.0 + (level - WIN_MAX) * BOOST_STEP; }
static double TotalFactor(int level) { return kWinFactor[WinSpeedFor(level)] * BoostFor(level); }
static bool HasCurve(int dev) { return dev == DEV_MOUSE ? S.dev[0].accelMode == 2 : S.dev[1].accelMode == 1; }
static bool NeedsHook(int dev) { return BoostFor(S.dev[dev].level) > 1.0001 || HasCurve(dev); }

// ---------------------------------------------------------------- touchpad (Windows 11 24H2+ API)
static bool GetPad(TOUCHPAD_PARAMETERS_V1& tp) {
    tp = {};
    tp.versionNumber = TOUCHPAD_PARAMETERS_VERSION_1;
    return SystemParametersInfoW(SPI_GETTOUCHPADPARAMETERS, sizeof(tp), &tp, 0) != FALSE;
}
static bool g_padApi = false, g_padPresent = false;
static void ProbePad() {
    TOUCHPAD_PARAMETERS_V1 tp;
    g_padApi = GetPad(tp);
    g_padPresent = g_padApi ? (tp.touchpadPresent != 0) : false;
}

// ---------------------------------------------------------------- hook (own thread)
struct DevHook {
    std::atomic<bool> on{false};
    std::atomic<double> mult{1.0};
    std::atomic<bool> curve{false};
    std::atomic<double> strength{0.6};
};
static DevHook g_dh[2];
static std::atomic<bool> g_gameSafe{true};
static std::atomic<bool> g_paused{false};
static std::atomic<long long> g_lastPadReport{0};  // QPC of the last touchpad HID report
static std::atomic<long long> g_lastMove[2];       // QPC of the last real move per device
static DWORD g_hookThreadId = 0;
static HANDLE g_hookReady = nullptr;
static const UINT HM_SETHOOK = WM_APP + 10;
static LARGE_INTEGER g_qpf{};
static long long Qpc() { LARGE_INTEGER n; QueryPerformanceCounter(&n); return n.QuadPart; }
static double MsSince(long long t) { return (Qpc() - t) * 1000.0 / g_qpf.QuadPart; }

static bool GameLikeCursor() {
    CURSORINFO ci{sizeof(ci)};
    if (GetCursorInfo(&ci) && !(ci.flags & CURSOR_SHOWING)) return true;  // hidden cursor (FPS, video)
    RECT clip;
    if (GetClipCursor(&clip)) {
        RECT vs{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
        vs.right = vs.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        vs.bottom = vs.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (clip.left > vs.left || clip.top > vs.top || clip.right < vs.right || clip.bottom < vs.bottom) return true;
    }
    return false;
}

static double g_accX = 0, g_accY = 0;
static long long g_lastQpc = 0;
static int g_lastDev = -1;

static LRESULT CALLBACK LowLevelMouse(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && wp == WM_MOUSEMOVE) {
        auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
        bool injected = (ms->flags & (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED)) != 0;
        bool penOrTouch = (ms->dwExtraInfo & 0xFFFFFF00) == 0xFF515700;
        if (!injected && !penOrTouch) {
            long long now = Qpc();
            int dev = ((now - g_lastPadReport.load(std::memory_order_relaxed)) * 1000.0 / g_qpf.QuadPart < 80.0) ? DEV_PAD : DEV_MOUSE;
            g_lastMove[dev].store(now, std::memory_order_relaxed);
            DevHook& d = g_dh[dev];
            if (!d.on.load(std::memory_order_relaxed)) return CallNextHookEx(nullptr, code, wp, lp);
            if (g_gameSafe.load(std::memory_order_relaxed) && GameLikeCursor()) {
                g_paused.store(true, std::memory_order_relaxed);
                return CallNextHookEx(nullptr, code, wp, lp);
            }
            g_paused.store(false, std::memory_order_relaxed);
            if (dev != g_lastDev) { g_accX = g_accY = 0; g_lastDev = dev; }
            POINT cur;
            if (GetCursorPos(&cur)) {
                long dx = ms->pt.x - cur.x, dy = ms->pt.y - cur.y;
                if (dx || dy) {
                    double m = d.mult.load(std::memory_order_relaxed);
                    if (d.curve.load(std::memory_order_relaxed)) {
                        double dtMs = std::clamp((now - g_lastQpc) * 1000.0 / g_qpf.QuadPart, 1.0, 20.0);
                        double speed = std::sqrt(double(dx) * dx + double(dy) * dy) / dtMs;  // px per ms
                        m *= 1.0 + d.strength.load(std::memory_order_relaxed) * std::min(speed / 1.5, 2.5);
                    }
                    g_lastQpc = now;
                    double fx = dx * m + g_accX, fy = dy * m + g_accY;
                    long ix = (long)std::trunc(fx), iy = (long)std::trunc(fy);
                    g_accX = fx - ix;
                    g_accY = fy - iy;
                    POINT np{cur.x + ix, cur.y + iy};
                    SetCursorPos(np.x, np.y);
                    POINT after;
                    if (GetCursorPos(&after)) {  // hit a screen edge: drop the carried fraction
                        if (after.x != np.x) g_accX = 0;
                        if (after.y != np.y) g_accY = 0;
                    }
                    return 1;  // swallow the original move; we placed the cursor ourselves
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static LRESULT CALLBACK RawWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INPUT) {
        RAWINPUTHEADER hdr;
        UINT sz = sizeof(hdr);
        if (GetRawInputData((HRAWINPUT)lp, RID_HEADER, &hdr, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 && hdr.dwType == RIM_TYPEHID)
            g_lastPadReport.store(Qpc(), std::memory_order_relaxed);
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static DWORD WINAPI HookThread(LPVOID) {
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // create the queue
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    // Listen to precision-touchpad HID reports (Digitizer page 0x0D, Touch Pad usage 0x05) in the background.
    WNDCLASSW wc{};
    wc.lpfnWndProc = RawWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"VagiiMouseRaw";
    RegisterClassW(&wc);
    HWND raw = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    RAWINPUTDEVICE rid{0x0D, 0x05, RIDEV_INPUTSINK, raw};
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
    SetEvent(g_hookReady);

    HHOOK hook = nullptr;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == HM_SETHOOK && !msg.hwnd) {
            if (msg.wParam && !hook) {
                g_accX = g_accY = 0;
                hook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouse, GetModuleHandleW(nullptr), 0);
            } else if (!msg.wParam && hook) {
                UnhookWindowsHookEx(hook);
                hook = nullptr;
            }
            continue;
        }
        DispatchMessageW(&msg);
    }
    if (hook) UnhookWindowsHookEx(hook);
    DestroyWindow(raw);
    return 0;
}

// ---------------------------------------------------------------- applying
static HWND g_hwnd;
static void UpdateHook() {
    if (g_test) return;
    bool boost = false;
    for (int i = 0; i < 2; i++) {
        bool on = S.enabled && NeedsHook(i);
        g_dh[i].on = on;
        boost |= on;
    }
    // Also hook while the window is open so the tabs can show which device is moving.
    bool visible = g_hwnd && IsWindowVisible(g_hwnd);
    PostThreadMessageW(g_hookThreadId, HM_SETHOOK, boost || visible, 0);
}

static void ApplyAll(bool persist) {
    if (g_test) return;
    UINT fl = persist ? (SPIF_UPDATEINIFILE | SPIF_SENDCHANGE) : 0;
    const DevSettings& m = S.dev[DEV_MOUSE];
    SystemParametersInfoW(SPI_SETMOUSESPEED, 0, (PVOID)(INT_PTR)WinSpeedFor(m.level), fl);
    int acc[3] = {0, 0, 0};
    if (m.accelMode == 1) { acc[0] = 6; acc[1] = 10; acc[2] = 1; }
    SystemParametersInfoW(SPI_SETMOUSE, 0, acc, fl);

    if (g_padPresent) {
        TOUCHPAD_PARAMETERS_V1 tp;
        if (GetPad(tp) && tp.cursorSpeed != (UINT)WinSpeedFor(S.dev[DEV_PAD].level)) {
            tp.cursorSpeed = WinSpeedFor(S.dev[DEV_PAD].level);
            SystemParametersInfoW(SPI_SETTOUCHPADPARAMETERS, sizeof(tp), &tp, fl);
        }
    }

    for (int i = 0; i < 2; i++) {
        g_dh[i].mult = BoostFor(S.dev[i].level);
        g_dh[i].curve = HasCurve(i);
        g_dh[i].strength = S.dev[i].accelStrength * 0.1;
    }
    g_gameSafe = S.gameSafe != 0;
    UpdateHook();
}

static void CaptureOriginal() {
    DevSettings& m = S.dev[DEV_MOUSE];
    if (!m.haveOrig) {
        int sp = 10, acc[3] = {6, 10, 1};
        SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &sp, 0);
        SystemParametersInfoW(SPI_GETMOUSE, 0, acc, 0);
        m.origSpeed = std::clamp(sp, 1, 20);
        m.origAccel = acc[2] ? 1 : 0;
        m.haveOrig = 1;
        m.level = m.origSpeed;
        m.accelMode = m.origAccel ? 1 : 0;
    }
    DevSettings& p = S.dev[DEV_PAD];
    TOUCHPAD_PARAMETERS_V1 tp;
    if (!p.haveOrig && g_padPresent && GetPad(tp)) {
        p.origSpeed = std::clamp((int)tp.cursorSpeed, 1, 20);
        p.origAccel = 0;
        p.haveOrig = 1;
        p.level = p.origSpeed;
        p.accelMode = 0;
    }
}

static void SetStartup(bool on) {
    if (g_test) return;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --tray";
        RegSetValueExW(k, L"VagiiMouse", 0, REG_SZ, (const BYTE*)cmd.c_str(), DWORD((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"VagiiMouse");
    }
    RegCloseKey(k);
}

// ---------------------------------------------------------------- UI
enum {
    IDC_LEVEL = 101, IDC_MINUS, IDC_PLUS, IDC_ACCEL, IDC_STRENGTH, IDC_DPI,
    IDC_ENABLED, IDC_GAMESAFE, IDC_STARTUP, IDC_TRAY, IDC_RESET, IDC_ORIGINAL,
    IDM_OPEN = 201, IDM_TOGGLE, IDM_RESET, IDM_EXIT
};
static const UINT WM_TRAY = WM_APP + 1, WM_SHOWME = WM_APP + 2;
static const UINT TIMER_PERSIST = 1, TIMER_STATUS = 2;
static UINT WM_TASKBARCREATED = 0;

static HWND g_level, g_minus, g_plus, g_accel, g_strength, g_dpi, g_enabled, g_gameSafeChk, g_startup, g_tray, g_reset, g_orig;
static HFONT g_fUi, g_fSmall, g_fTitle, g_fBig, g_fLabel, g_fTab;
static HBRUSH g_bg;
static UINT g_dpiNow = 96;
static HICON g_iconBig, g_iconSmall;
static bool g_updating = false;
static int g_comboFor = -1;
static const COLORREF C_BG = RGB(250, 250, 252), C_TEXT = RGB(24, 24, 32), C_DIM = RGB(110, 112, 125),
                      C_ACCENT = RGB(37, 99, 235), C_LINE = RGB(226, 228, 236), C_WARN = RGB(200, 110, 0),
                      C_TABBG = RGB(236, 238, 244), C_LIVE = RGB(22, 163, 74);

static int Sc(int v) { return MulDiv(v, g_dpiNow, 96); }
static DevSettings& Cur() { return S.dev[S.tab]; }
static bool PadTabDisabled() { return S.tab == DEV_PAD && !g_padPresent; }

static HICON MakeIcon(int size) {
    // Fallback icon (rounded mouse body), drawn with 4x4 supersampling.
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi); bi.bV5Width = size; bi.bV5Height = -size; bi.bV5Planes = 1; bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000; bi.bV5GreenMask = 0x0000FF00; bi.bV5BlueMask = 0x000000FF; bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    auto* px = (DWORD*)bits;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            double a = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    double fx = (x + (sx + 0.5) / 4) / size, fy = (y + (sy + 0.5) / 4) / size;
                    double l = 0.24, r = 0.76, t = 0.06, b = 0.94, rad = 0.26;
                    if (fx < l || fx > r || fy < t || fy > b) continue;
                    double cx = std::clamp(fx, l + rad, r - rad), cy = std::clamp(fy, t + rad, b - rad);
                    if ((fx - cx) * (fx - cx) + (fy - cy) * (fy - cy) <= rad * rad) a += 1;
                }
            a /= 16;
            px[y * size + x] = (DWORD(a * 255) << 24) | (DWORD(a * 37) << 16) | (DWORD(a * 99) << 8) | DWORD(a * 235);
        }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return ic;
}

static void MakeFonts() {
    for (HFONT f : {g_fUi, g_fSmall, g_fTitle, g_fBig, g_fLabel, g_fTab}) if (f) DeleteObject(f);
    auto mk = [](int pt, int weight) {
        return CreateFontW(-MulDiv(pt, g_dpiNow, 72), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    };
    g_fUi = mk(10, FW_NORMAL);
    g_fSmall = mk(9, FW_NORMAL);
    g_fTitle = mk(17, FW_SEMIBOLD);
    g_fBig = mk(26, FW_SEMIBOLD);
    g_fLabel = mk(8, FW_BOLD);
    g_fTab = mk(10, FW_SEMIBOLD);
}

static const int CLIENT_W = 840, CLIENT_H = 540;
static const int LX = 28, LW = 470, RX = 560, RW = 252;
static const int DY = 50;  // content offset below the device tabs
static const int TAB_Y = 84, TAB_H = 34, TAB_W = 150;

static RECT TabRect(int i) {
    return RECT{Sc(LX + i * (TAB_W + 4)), Sc(TAB_Y), Sc(LX + i * (TAB_W + 4) + TAB_W), Sc(TAB_Y + TAB_H)};
}

static void Layout() {
    auto mv = [](HWND h, int x, int y, int w, int hgt) { MoveWindow(h, Sc(x), Sc(y + DY), Sc(w), Sc(hgt), FALSE); };
    mv(g_level, LX - 6, 172, 404, 36);
    mv(g_minus, LX + 404, 174, 30, 30);
    mv(g_plus, LX + 440, 174, 30, 30);
    mv(g_accel, LX, 276, 320, 300);
    mv(g_strength, LX - 6, 340, 404, 32);
    mv(g_dpi, RX + 160, 168, 92, 26);
    mv(g_enabled, RX, 240, RW, 24);
    mv(g_gameSafeChk, RX, 266, RW, 24);
    mv(g_startup, RX, 292, RW, 24);
    mv(g_tray, RX, 318, RW, 24);
    mv(g_reset, RX, 360, RW, 32);
    mv(g_orig, RX, 400, RW, 32);
    for (HWND h : {g_level, g_minus, g_plus, g_accel, g_strength, g_dpi, g_enabled, g_gameSafeChk, g_startup, g_tray, g_reset, g_orig})
        SendMessageW(h, WM_SETFONT, (WPARAM)g_fUi, TRUE);
    InvalidateRect(g_hwnd, nullptr, TRUE);
}

static void SyncControls() {
    g_updating = true;
    DevSettings& d = Cur();
    if (g_comboFor != S.tab) {
        ComboBox_ResetContent(g_accel);
        if (S.tab == DEV_MOUSE) {
            ComboBox_AddString(g_accel, L"Off \x2014 flat, no acceleration");
            ComboBox_AddString(g_accel, L"Windows \x2014 Enhance pointer precision");
            ComboBox_AddString(g_accel, L"Vagii boost curve \x2014 adjustable");
        } else {
            ComboBox_AddString(g_accel, L"Windows touchpad curve (built in)");
            ComboBox_AddString(g_accel, L"Vagii boost curve on top \x2014 adjustable");
        }
        g_comboFor = S.tab;
    }
    SendMessageW(g_level, TBM_SETPOS, TRUE, d.level);
    SendMessageW(g_strength, TBM_SETPOS, TRUE, d.accelStrength);
    ComboBox_SetCurSel(g_accel, d.accelMode);
    bool dis = PadTabDisabled();
    for (HWND h : {g_level, g_minus, g_plus, g_accel, g_reset, g_orig}) EnableWindow(h, !dis);
    EnableWindow(g_strength, !dis && HasCurve(S.tab));
    ShowWindow(g_dpi, S.tab == DEV_MOUSE ? SW_SHOW : SW_HIDE);
    wchar_t buf[16];
    swprintf_s(buf, L"%d", S.mouseDpi);
    wchar_t cur[16];
    GetWindowTextW(g_dpi, cur, 16);
    if (wcscmp(cur, buf) != 0) SetWindowTextW(g_dpi, buf);
    Button_SetCheck(g_enabled, S.enabled ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(g_gameSafeChk, S.gameSafe ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(g_startup, S.startup ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(g_tray, S.closeToTray ? BST_CHECKED : BST_UNCHECKED);
    g_updating = false;
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static NOTIFYICONDATAW g_nid{};
static void TrayTip() {
    if (g_padPresent)
        swprintf_s(g_nid.szTip, L"Vagii Mouse - mouse %d, trackpad %d%s", S.dev[0].level, S.dev[1].level, S.enabled ? L"" : L" (boost off)");
    else
        swprintf_s(g_nid.szTip, L"Vagii Mouse - level %d/%d%s", S.dev[0].level, MAX_LEVEL, S.enabled ? L"" : L" (boost off)");
}
static void TrayAdd() {
    if (g_test) return;
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_iconSmall;
    TrayTip();
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}
static void TrayUpdate() { TrayTip(); Shell_NotifyIconW(NIM_MODIFY, &g_nid); }

// Live change: apply now without broadcasting, persist shortly after the user stops.
static void Changed(bool live = true) {
    ApplyAll(!live);
    if (live) SetTimer(g_hwnd, TIMER_PERSIST, 400, nullptr);
    else SaveSettings();
    SyncControls();
    TrayUpdate();
}

static void SetLevel(int lv) {
    Cur().level = std::clamp(lv, 1, MAX_LEVEL);
    Changed();
}

static void ResetDefault(int dev) {
    DevSettings& d = S.dev[dev];
    d.level = 10;
    d.accelMode = dev == DEV_MOUSE ? 1 : 0;
    d.accelStrength = 6;
    Changed(false);
}
static void RestoreOriginal(int dev) {
    DevSettings& d = S.dev[dev];
    d.level = d.origSpeed;
    d.accelMode = dev == DEV_MOUSE ? (d.origAccel ? 1 : 0) : 0;
    Changed(false);
}

static void ShowMain() {
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_hwnd);
    UpdateHook();
}

static void FitOnScreen(HWND h) {
    RECT wr; GetWindowRect(h, &wr);
    HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(mon, &mi);
    RECT wa = mi.rcWork;
    int w = std::min<int>(wr.right - wr.left, wa.right - wa.left), hh = std::min<int>(wr.bottom - wr.top, wa.bottom - wa.top);
    int x = std::clamp<int>(wr.left, wa.left, wa.right - w), y = std::clamp<int>(wr.top, wa.top, wa.bottom - hh);
    SetWindowPos(h, nullptr, x, y, w, hh, SWP_NOZORDER | SWP_NOACTIVATE);
}

static std::wstring Commas(long long v) {
    std::wstring s = std::to_wstring(v);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert(i, L",");
    return s;
}

static void Text(HDC dc, HFONT f, COLORREF c, int x, int y, int w, int h, const std::wstring& s, UINT fmt = DT_LEFT | DT_SINGLELINE | DT_VCENTER) {
    SelectObject(dc, f);
    SetTextColor(dc, c);
    RECT r{Sc(x), Sc(y), Sc(x + w), Sc(y + h)};
    DrawTextW(dc, s.c_str(), -1, &r, fmt | DT_NOPREFIX);
}

static void FillRound(HDC dc, RECT r, int rad, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(p);
}

static bool DevLive(int dev) {
    long long t = g_lastMove[dev].load();
    return t && MsSince(t) < 350;
}

static void Paint(HDC dc) {
    RECT rc; GetClientRect(g_hwnd, &rc);
    FillRect(dc, &rc, g_bg);
    SetBkMode(dc, TRANSPARENT);
    wchar_t buf[256];
    const int dev = S.tab;
    const DevSettings& d = Cur();
    const bool pad = dev == DEV_PAD;

    Text(dc, g_fTitle, C_TEXT, LX, 16, 400, 34, L"Vagii Mouse");
    Text(dc, g_fSmall, C_DIM, LX, 48, 560, 20, L"Pointer speed beyond Windows' 20 levels \x2014 per-user, no admin, applies instantly");

    // device tabs
    const wchar_t* names[2] = {L"Mouse", L"Trackpad"};
    for (int i = 0; i < 2; i++) {
        RECT r = TabRect(i);
        bool act = i == dev;
        FillRound(dc, r, Sc(10), act ? C_ACCENT : C_TABBG);
        SelectObject(dc, g_fTab);
        SetTextColor(dc, act ? RGB(255, 255, 255) : C_TEXT);
        RECT tr = r; tr.left += Sc(16);
        swprintf_s(buf, L"%s   %d", names[i], S.dev[i].level);
        DrawTextW(dc, buf, -1, &tr, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        if (DevLive(i)) {  // green dot while this device is moving the pointer
            int cx = r.right - Sc(18), cy = (r.top + r.bottom) / 2, rr = Sc(4);
            RECT dr{cx - rr, cy - rr, cx + rr, cy + rr};
            FillRound(dc, dr, rr * 2, act ? RGB(187, 247, 208) : C_LIVE);
        }
    }
    Text(dc, g_fSmall, C_DIM, LX + 2 * (TAB_W + 4) + 8, TAB_Y, RX + RW - (LX + 2 * (TAB_W + 4) + 8), TAB_H,
         L"Each device has its own speed. A green dot shows which one is moving the pointer.", DT_RIGHT | DT_SINGLELINE | DT_VCENTER);

    HPEN pen = CreatePen(PS_SOLID, std::max(1, Sc(1)), C_LINE);
    HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc, Sc(LX), Sc(80 + DY), nullptr); LineTo(dc, Sc(RX + RW), Sc(80 + DY));
    MoveToEx(dc, Sc(RX - 30), Sc(96 + DY), nullptr); LineTo(dc, Sc(RX - 30), Sc(436 + DY));
    MoveToEx(dc, Sc(LX), Sc(448 + DY), nullptr); LineTo(dc, Sc(RX + RW), Sc(448 + DY));
    SelectObject(dc, old);
    DeleteObject(pen);

    if (PadTabDisabled()) {
        Text(dc, g_fUi, C_TEXT, LX, 110 + DY, LW, 24, g_padApi ? L"No precision touchpad found on this PC." : L"This Windows version has no touchpad speed API.");
        Text(dc, g_fSmall, C_DIM, LX, 136 + DY, LW, 60, L"Trackpad settings need a Windows precision touchpad (Windows 11 24H2 or newer). Use the Mouse tab instead.", DT_LEFT | DT_WORDBREAK);
    } else {
        // speed block
        Text(dc, g_fLabel, C_DIM, LX, 96 + DY, 300, 18, pad ? L"TRACKPAD SPEED" : L"POINTER SPEED");
        swprintf_s(buf, L"Level %d", d.level);
        Text(dc, g_fBig, d.level > WIN_MAX ? C_ACCENT : C_TEXT, LX, 112 + DY, 260, 52, buf);
        double boost = BoostFor(d.level);
        const wchar_t* win = pad ? L"Windows touchpad" : L"Windows";
        if (d.level > WIN_MAX) swprintf_s(buf, L"%s 20/20  +  Vagii boost \x00D7%.2f", win, boost);
        else swprintf_s(buf, L"%s speed %d/20  (no boost)", win, d.level);
        Text(dc, g_fUi, C_DIM, LX + 160, 112 + DY, LW - 160, 52, buf, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
        Text(dc, g_fSmall, C_DIM, LX, 208 + DY, LW, 20, L"1\x2013" L"20 = Windows' own range      21\x2013" L"100 = Vagii boost (faster than Windows allows)");

        // accel block
        Text(dc, g_fLabel, C_DIM, LX, 252 + DY, 300, 18, L"ACCELERATION");
        if (HasCurve(dev)) swprintf_s(buf, L"Boost curve strength: %d / 20", d.accelStrength);
        else wcscpy_s(buf, L"Boost curve strength (only for the Vagii boost curve)");
        Text(dc, g_fSmall, HasCurve(dev) ? C_TEXT : C_DIM, LX, 318 + DY, LW, 20, buf);
        const wchar_t* hint;
        if (pad) hint = d.accelMode == 0 ? L"Windows touchpad curve: the normal precision-touchpad feel."
                                         : L"Boost curve on top: quick swipes cross the screen, slow drags stay precise.";
        else hint = d.accelMode == 0 ? L"Off: the same distance every time \x2014 best for aiming and muscle memory."
                  : d.accelMode == 1 ? L"Windows: \"Enhance pointer precision\" \x2014 fast moves go further, slow moves are finer."
                                     : L"Vagii boost curve: flicks travel much further, slow moves stay controllable.";
        Text(dc, g_fSmall, C_DIM, LX, 378 + DY, LW, 40, hint, DT_LEFT | DT_WORDBREAK);

        // right column
        if (!pad) {
            Text(dc, g_fLabel, C_DIM, RX, 96 + DY, RW, 18, L"EFFECTIVE DPI");
            double factor = TotalFactor(d.level);
            swprintf_s(buf, L"\x2248 %s", Commas(llround(S.mouseDpi * factor)).c_str());
            Text(dc, g_fBig, C_ACCENT, RX, 112 + DY, RW, 52, buf);
            Text(dc, g_fUi, C_TEXT, RX, 168 + DY, 160, 26, L"Your mouse's DPI");
            swprintf_s(buf, L"\x00D7%.2f vs Windows default%s", factor, d.accelMode ? L"  (+ accel)" : L"");
            Text(dc, g_fSmall, C_DIM, RX, 200 + DY, RW, 22, buf);
        } else {
            Text(dc, g_fLabel, C_DIM, RX, 96 + DY, RW, 18, L"SPEED VS WINDOWS MAX");
            swprintf_s(buf, L"\x00D7%.2f", boost);
            Text(dc, g_fBig, C_ACCENT, RX, 112 + DY, RW, 52, buf);
            Text(dc, g_fSmall, C_DIM, RX, 166 + DY, RW, 56,
                 L"Windows' touchpad slider stops at 20. Levels above that multiply on top of it.", DT_LEFT | DT_WORDBREAK);
        }
    }

    // status
    std::wstring st;
    bool any = NeedsHook(0) || NeedsHook(1);
    bool warn = false;
    if (!S.enabled && any) { st = L"Boost is OFF \x2014 press Ctrl+Alt+M or tick \"Boost enabled\" to turn it back on."; warn = true; }
    else if (any && g_paused.load()) st = L"Boost paused: a game has hidden or locked the cursor.";
    else if (any) st = L"Boost active \x2014 keep Vagii Mouse running (it lives in the tray).  Ctrl+Alt+M toggles it.";
    else st = L"Using Windows' own speeds. Changes apply instantly.";
    Text(dc, g_fSmall, warn ? C_WARN : C_DIM, LX, 454 + DY, RX + RW - LX, 26, st);
}

static void SwitchTab(int t) {
    if (t == S.tab) return;
    S.tab = t;
    SaveSettings();
    SyncControls();
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (WM_TASKBARCREATED && msg == WM_TASKBARCREATED) { TrayAdd(); return 0; }
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = h;
        g_dpiNow = GetDpiForWindow(h);
        MakeFonts();
        auto ctl = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
            return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 0, 0, 10, 10, h, (HMENU)(INT_PTR)id, nullptr, nullptr);
        };
        g_level = ctl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS, IDC_LEVEL);
        SendMessageW(g_level, TBM_SETRANGE, FALSE, MAKELPARAM(1, MAX_LEVEL));
        SendMessageW(g_level, TBM_SETTICFREQ, 10, 0);
        SendMessageW(g_level, TBM_SETPAGESIZE, 0, 5);
        g_minus = ctl(L"BUTTON", L"\x2212", BS_PUSHBUTTON, IDC_MINUS);
        g_plus = ctl(L"BUTTON", L"+", BS_PUSHBUTTON, IDC_PLUS);
        g_accel = ctl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST, IDC_ACCEL);
        g_strength = ctl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS, IDC_STRENGTH);
        SendMessageW(g_strength, TBM_SETRANGE, FALSE, MAKELPARAM(1, 20));
        g_dpi = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_CENTER, 0, 0, 10, 10, h, (HMENU)IDC_DPI, nullptr, nullptr);
        SendMessageW(g_dpi, EM_SETLIMITTEXT, 5, 0);
        g_enabled = ctl(L"BUTTON", L"Boost enabled  (Ctrl+Alt+M)", BS_AUTOCHECKBOX, IDC_ENABLED);
        g_gameSafeChk = ctl(L"BUTTON", L"Pause boost in games (locked cursor)", BS_AUTOCHECKBOX, IDC_GAMESAFE);
        g_startup = ctl(L"BUTTON", L"Start with Windows", BS_AUTOCHECKBOX, IDC_STARTUP);
        g_tray = ctl(L"BUTTON", L"Keep running in the tray when closed", BS_AUTOCHECKBOX, IDC_TRAY);
        g_reset = ctl(L"BUTTON", L"Reset to Windows default", BS_PUSHBUTTON, IDC_RESET);
        g_orig = ctl(L"BUTTON", L"Restore my original settings", BS_PUSHBUTTON, IDC_ORIGINAL);
        Layout();
        SyncControls();
        if (!g_test) RegisterHotKey(h, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'M');
        TrayAdd();
        SetTimer(h, TIMER_STATUS, 150, nullptr);
        return 0;
    }
    case WM_DPICHANGED: {
        g_dpiNow = HIWORD(wp);
        MakeFonts();
        auto* r = (RECT*)lp;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        for (int i = 0; i < 2; i++) { RECT r = TabRect(i); if (PtInRect(&r, pt)) SwitchTab(i); }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_TAB && (GetKeyState(VK_CONTROL) & 0x8000)) SwitchTab(1 - S.tab);
        return 0;
    case WM_HSCROLL: {
        if (g_updating) return 0;
        HWND src = (HWND)lp;
        int pos = (int)SendMessageW(src, TBM_GETPOS, 0, 0);
        if (src == g_level && pos != Cur().level) SetLevel(pos);
        else if (src == g_strength && pos != Cur().accelStrength) { Cur().accelStrength = pos; Changed(); }
        return 0;
    }
    case WM_COMMAND: {
        if (g_updating) return 0;
        int id = LOWORD(wp), code = HIWORD(wp);
        switch (id) {
        case IDC_MINUS: SetLevel(Cur().level - 1); break;
        case IDC_PLUS: SetLevel(Cur().level + 1); break;
        case IDC_ACCEL:
            if (code == CBN_SELCHANGE) { Cur().accelMode = ComboBox_GetCurSel(g_accel); Changed(false); }
            break;
        case IDC_DPI:
            if (code == EN_CHANGE) {
                wchar_t b[16]; GetWindowTextW(g_dpi, b, 16);
                int v = _wtoi(b);
                if (v >= 100 && v <= 64000) { S.mouseDpi = v; SaveSettings(); InvalidateRect(h, nullptr, FALSE); }
            }
            break;
        case IDC_ENABLED: S.enabled = Button_GetCheck(g_enabled) == BST_CHECKED; Changed(false); break;
        case IDC_GAMESAFE: S.gameSafe = Button_GetCheck(g_gameSafeChk) == BST_CHECKED; Changed(false); break;
        case IDC_STARTUP: S.startup = Button_GetCheck(g_startup) == BST_CHECKED; SetStartup(S.startup); SaveSettings(); break;
        case IDC_TRAY: S.closeToTray = Button_GetCheck(g_tray) == BST_CHECKED; SaveSettings(); break;
        case IDC_RESET: ResetDefault(S.tab); break;
        case IDM_RESET: ResetDefault(DEV_MOUSE); if (g_padPresent) ResetDefault(DEV_PAD); break;
        case IDC_ORIGINAL: RestoreOriginal(S.tab); break;
        case IDM_OPEN: ShowMain(); break;
        case IDM_TOGGLE: S.enabled = !S.enabled; Changed(false); break;
        case IDM_EXIT: DestroyWindow(h); break;
        }
        return 0;
    }
    case WM_HOTKEY:
        S.enabled = !S.enabled;
        Changed(false);
        MessageBeep(S.enabled ? MB_OK : MB_ICONASTERISK);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_PERSIST) { KillTimer(h, TIMER_PERSIST); ApplyAll(true); SaveSettings(); }
        else if (wp == TIMER_STATUS && IsWindowVisible(h)) {
            static int lastSig = -1;
            int sig = (g_paused.load() ? 1 : 0) | (DevLive(0) ? 2 : 0) | (DevLive(1) ? 4 : 0);
            if (sig != lastSig) { lastSig = sig; InvalidateRect(h, nullptr, FALSE); }
        }
        return 0;
    case WM_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK) ShowMain();
        else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, IDM_OPEN, L"Open Vagii Mouse");
            AppendMenuW(m, MF_STRING | (S.enabled ? MF_CHECKED : 0), IDM_TOGGLE, L"Boost enabled\tCtrl+Alt+M");
            AppendMenuW(m, MF_STRING, IDM_RESET, L"Reset everything to Windows default");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit");
            POINT pt; GetCursorPos(&pt);
            SetForegroundWindow(h);
            TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, nullptr);
            DestroyMenu(m);
        }
        return 0;
    case WM_SHOWME: ShowMain(); return 0;
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wp, C_BG);
        SetTextColor((HDC)wp, C_TEXT);
        return (LRESULT)g_bg;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ ob = SelectObject(mem, bmp);
        Paint(mem);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SHOWWINDOW:
        PostMessageW(h, WM_APP + 3, 0, 0);  // re-evaluate the hook after visibility settles
        break;
    case WM_APP + 3: UpdateHook(); return 0;
    case WM_CLOSE:
        if (S.closeToTray) { ShowWindow(h, SW_HIDE); return 0; }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        UnregisterHotKey(h, 1);
        if (KillTimer(h, TIMER_PERSIST)) ApplyAll(true);
        SaveSettings();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmd, int) {
    wchar_t ev[8] = L"";
    g_test = GetEnvironmentVariableW(L"VAGII_TEST", ev, 8) && ev[0] == L'1';
    const wchar_t* cls = g_test ? L"VagiiMouseWndTest" : L"VagiiMouseWnd";
    HANDLE mtx = CreateMutexW(nullptr, TRUE, g_test ? L"Local\\VagiiMouseTest" : L"Local\\VagiiMouseSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(cls, nullptr)) PostMessageW(other, WM_SHOWME, 0, 0);
        return 0;
    }
    bool startHidden = wcsstr(cmd, L"--tray") != nullptr;
    int forceTab = wcsstr(cmd, L"--tab=1") ? 1 : wcsstr(cmd, L"--tab=0") ? 0 : -1;  // for screenshots

    QueryPerformanceFrequency(&g_qpf);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    IniPath();
    ProbePad();
    LoadSettings();
    CaptureOriginal();
    if (forceTab >= 0) S.tab = forceTab;
    SaveSettings();

    g_hookReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE th = CreateThread(nullptr, 0, HookThread, nullptr, 0, &g_hookThreadId);
    WaitForSingleObject(g_hookReady, 5000);
    ApplyAll(false);
    if (S.startup) SetStartup(true);  // keep the Run entry pointing at this exe

    g_bg = CreateSolidBrush(C_BG);
    g_iconBig = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    g_iconSmall = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!g_iconBig) g_iconBig = MakeIcon(GetSystemMetrics(SM_CXICON));
    if (!g_iconSmall) g_iconSmall = MakeIcon(GetSystemMetrics(SM_CXSMICON));
    WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = g_iconBig;
    wc.hIconSm = g_iconSmall;
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);

    // Size from DIP client size at the monitor's DPI, then center in the work area.
    POINT cpt; GetCursorPos(&cpt);
    HMONITOR mon = MonitorFromPoint(cpt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(mon, &mi);
    UINT dpi = 96;
    {
        HDC sdc = GetDC(nullptr); dpi = GetDeviceCaps(sdc, LOGPIXELSX); ReleaseDC(nullptr, sdc);
        UINT dx, dy;
        using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
        if (HMODULE sh = LoadLibraryW(L"shcore.dll"))
            if (auto fn = (GetDpiForMonitorFn)GetProcAddress(sh, "GetDpiForMonitor"))
                if (SUCCEEDED(fn(mon, 0, &dx, &dy))) dpi = dx;
    }
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r{0, 0, MulDiv(CLIENT_W, dpi, 96), MulDiv(CLIENT_H, dpi, 96)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
    int w = r.right - r.left, hgt = r.bottom - r.top;
    RECT wa = mi.rcWork;
    int x = wa.left + ((wa.right - wa.left) - w) / 2, y = wa.top + ((wa.bottom - wa.top) - hgt) / 2;
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Vagii Mouse", style, x, y, w, hgt, nullptr, nullptr, inst, nullptr);
    FitOnScreen(hwnd);
    if (!startHidden) ShowWindow(hwnd, SW_SHOWNORMAL);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_TAB && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SwitchTab(1 - S.tab);
            continue;
        }
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
    WaitForSingleObject(th, 2000);
    CloseHandle(mtx);
    return 0;
}
