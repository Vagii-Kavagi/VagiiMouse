// Vagii Mouse - pointer speed booster that goes beyond Windows' 20 speed levels,
// with separate settings for the mouse and the trackpad. Per-user, no admin.
//  Mouse:    levels 1-20 = Windows pointer speed; 21-100 = Windows 20 + boost via a low-level hook.
//  Trackpad: levels 1-20 = Windows precision-touchpad cursor speed; 21-100 = 20 + boost.
// Trackpad moves are told apart from mouse moves by the touchpad's own raw HID reports.
// UI: dark "gaming dial" drawn with Direct2D (immediate-mode widgets). Updates come from
// the GitHub releases of APP_REPO.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>
#include <thread>
#include <appmodel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include "version.h"

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ================================================================ settings
static const int MAX_LEVEL = 100;
static const int WIN_MAX = 20;
static const double BOOST_STEP = 0.0625;  // each level above 20 adds 6.25% (level 100 = x6 of Windows max)
static const int MAX_STAGES = 6;
enum { DEV_MOUSE = 0, DEV_PAD = 1 };
enum { PAGE_MOUSE = 0, PAGE_PAD = 1, PAGE_SETTINGS = 2 };

struct DevSettings {
    int level = 10;
    int accelMode = 1;      // mouse: 0 off, 1 Windows EPP, 2 Vagii curve | trackpad: 0 Windows built-in, 1 Vagii curve on top
    int accelStrength = 6;  // 1..20
    int haveOrig = 0, origSpeed = 10, origAccel = 1;
    std::vector<int> stages{10, 20, 30, 45, 60};
};
struct Settings {
    DevSettings dev[2];
    int mouseDpi = 800;
    int enabled = 1;
    int gameSafe = 1;
    int startup = 1;
    int closeToTray = 1;
    int autoUpdate = 1;
    int page = 0;
    long long lastCheck = 0;
} S;

static wchar_t g_ini[MAX_PATH];
static bool g_test = false;  // VAGII_TEST=1: screenshot instance, own data, changes nothing on the system
// Microsoft Store (MSIX) install: the Store does updates and "Start with Windows" is a package
// StartupTask instead of the HKCU Run key. VAGII_FAKE_STORE=1 previews this mode unpackaged.
static bool g_packaged = false, g_fakeStore = false;
static bool IsPackaged() {
    UINT32 n = 0;
    return GetCurrentPackageFullName(&n, nullptr) == ERROR_INSUFFICIENT_BUFFER;
}
static bool StoreMode() { return g_packaged || g_fakeStore; }
// Started by the package's StartupTask at sign-in (stay in the tray, like --tray).
static bool LaunchedByStartupTask() {
    try {
        auto a = winrt::Windows::ApplicationModel::AppInstance::GetActivatedEventArgs();
        return a && a.Kind() == winrt::Windows::ApplicationModel::Activation::ActivationKind::StartupTask;
    } catch (...) { return false; }
}

static std::wstring DataDir() {
    wchar_t base[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, base))) GetTempPathW(MAX_PATH, base);
    return std::wstring(base) + (g_test ? L"\\VagiiMouseTest" : L"\\VagiiMouse");
}
static void IniPath() {
    std::wstring dir = DataDir();
    CreateDirectoryW(dir.c_str(), nullptr);
    wcscpy_s(g_ini, (dir + L"\\settings.ini").c_str());
}

#define SETTINGS_FIELDS(X) \
    X(L"level", dev[0].level) X(L"accelMode", dev[0].accelMode) X(L"accelStrength", dev[0].accelStrength) \
    X(L"haveOrig", dev[0].haveOrig) X(L"origSpeed", dev[0].origSpeed) X(L"origAccel", dev[0].origAccel) \
    X(L"tpLevel", dev[1].level) X(L"tpAccelMode", dev[1].accelMode) X(L"tpAccelStrength", dev[1].accelStrength) \
    X(L"tpHaveOrig", dev[1].haveOrig) X(L"tpOrigSpeed", dev[1].origSpeed) \
    X(L"mouseDpi", mouseDpi) X(L"enabled", enabled) X(L"gameSafe", gameSafe) X(L"startup", startup) \
    X(L"closeToTray", closeToTray) X(L"autoUpdate", autoUpdate) X(L"page", page)

static std::vector<int> ParseStages(const wchar_t* s) {
    std::vector<int> v;
    while (*s) {
        int n = _wtoi(s);
        if (n >= 1 && n <= MAX_LEVEL && (int)v.size() < MAX_STAGES) v.push_back(n);
        while (*s && *s != L',') s++;
        if (*s == L',') s++;
    }
    return v;
}
static std::wstring StagesStr(const std::vector<int>& v) {
    std::wstring s;
    for (size_t i = 0; i < v.size(); i++) s += (i ? L"," : L"") + std::to_wstring(v[i]);
    return s;
}

static void LoadSettings() {
    S.dev[1].accelMode = 0;
#define X(k, f) S.f = (int)GetPrivateProfileIntW(L"mouse", k, S.f, g_ini);
    SETTINGS_FIELDS(X)
#undef X
    // 1.1 stored the device tab as "tab"
    if (GetPrivateProfileIntW(L"mouse", L"page", -1, g_ini) < 0) S.page = (int)GetPrivateProfileIntW(L"mouse", L"tab", 0, g_ini);
    wchar_t buf[128];
    const wchar_t* keys[2] = {L"stages", L"tpStages"};
    for (int i = 0; i < 2; i++) {
        GetPrivateProfileStringW(L"mouse", keys[i], L"#", buf, 128, g_ini);
        if (wcscmp(buf, L"#") != 0) S.dev[i].stages = ParseStages(buf);
    }
    GetPrivateProfileStringW(L"mouse", L"lastCheck", L"0", buf, 128, g_ini);
    S.lastCheck = _wtoi64(buf);
    for (auto& d : S.dev) {
        d.level = std::clamp(d.level, 1, MAX_LEVEL);
        d.accelStrength = std::clamp(d.accelStrength, 1, 20);
    }
    S.dev[0].accelMode = std::clamp(S.dev[0].accelMode, 0, 2);
    S.dev[1].accelMode = std::clamp(S.dev[1].accelMode, 0, 1);
    S.mouseDpi = std::clamp(S.mouseDpi, 100, 64000);
    S.page = std::clamp(S.page, 0, 2);
}
static void SaveSettings() {
    wchar_t buf[32];
#define X(k, f) swprintf_s(buf, L"%d", S.f); WritePrivateProfileStringW(L"mouse", k, buf, g_ini);
    SETTINGS_FIELDS(X)
#undef X
    WritePrivateProfileStringW(L"mouse", L"stages", StagesStr(S.dev[0].stages).c_str(), g_ini);
    WritePrivateProfileStringW(L"mouse", L"tpStages", StagesStr(S.dev[1].stages).c_str(), g_ini);
    WritePrivateProfileStringW(L"mouse", L"lastCheck", std::to_wstring(S.lastCheck).c_str(), g_ini);
}

// Windows pointer-speed factor (Enhance pointer precision OFF), speeds 1..20.
static const double kWinFactor[21] = {0, 1.0/32, 1.0/16, 1.0/8, 2.0/8, 3.0/8, 4.0/8, 5.0/8, 6.0/8, 7.0/8, 1.0,
                                      1.25, 1.5, 1.75, 2.0, 2.25, 2.5, 2.75, 3.0, 3.25, 3.5};
static int WinSpeedFor(int level) { return std::min(level, WIN_MAX); }
static double BoostFor(int level) { return level <= WIN_MAX ? 1.0 : 1.0 + (level - WIN_MAX) * BOOST_STEP; }
static double TotalFactor(int level) { return kWinFactor[WinSpeedFor(level)] * BoostFor(level); }
static bool HasCurve(int dev) { return dev == DEV_MOUSE ? S.dev[0].accelMode == 2 : S.dev[1].accelMode == 1; }
static bool NeedsHook(int dev) { return BoostFor(S.dev[dev].level) > 1.0001 || HasCurve(dev); }
static long long EffDpi(int level) { return llround(S.mouseDpi * TotalFactor(level)); }

// ================================================================ touchpad (Windows 11 24H2+ API)
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

// ================================================================ hook (own thread)
struct DevHook {
    std::atomic<bool> on{false};
    std::atomic<double> mult{1.0};
    std::atomic<bool> curve{false};
    std::atomic<double> strength{0.6};
};
static DevHook g_dh[2];
static std::atomic<bool> g_gameSafe{true};
static std::atomic<bool> g_paused{false};
static std::atomic<long long> g_lastPadReport{0};
static std::atomic<long long> g_lastMove[2];
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
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    WNDCLASSW wc{};
    wc.lpfnWndProc = RawWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"VagiiMouseRaw";
    RegisterClassW(&wc);
    HWND raw = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    RAWINPUTDEVICE rid{0x0D, 0x05, RIDEV_INPUTSINK, raw};  // Digitizer / Touch Pad
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

// ================================================================ applying
static HWND g_hwnd;
static void UpdateHook() {
    if (g_test) return;
    bool boost = false;
    for (int i = 0; i < 2; i++) {
        bool on = S.enabled && NeedsHook(i);
        g_dh[i].on = on;
        boost |= on;
    }
    // Also hook while the window is open so the device cards can show which one is moving.
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

#define WM_STARTUP_STATE (WM_APP + 4)
static void SetStartup(bool on) {
    if (g_test || g_fakeStore) return;
    if (g_packaged) {
        // StartupTask (declared in AppxManifest.xml). Off thread: its async calls must not block the UI.
        std::thread([on] {
            bool enabled = false;
            try {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                using namespace winrt::Windows::ApplicationModel;
                StartupTask t = StartupTask::GetAsync(L"VagiiMouseStartup").get();
                if (on) enabled = t.RequestEnableAsync().get() == StartupTaskState::Enabled;
                else t.Disable();
            } catch (...) {}
            // the user may have turned it off in Task Manager; show the real state
            if (on && !enabled && g_hwnd) PostMessageW(g_hwnd, WM_STARTUP_STATE, 0, 0);
        }).detach();
        return;
    }
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

// ================================================================ Windows mouse registry reset
// HKCU\Control Panel\Mouse only (per-user, no admin). Windows 10/11 defaults.
static const BYTE kCurveX[40] = {0,0,0,0,0,0,0,0, 0x15,0x6e,0,0,0,0,0,0, 0,0x40,0x01,0,0,0,0,0, 0x29,0xdc,0x03,0,0,0,0,0, 0,0,0x28,0,0,0,0,0};
static const BYTE kCurveY[40] = {0,0,0,0,0,0,0,0, 0xfd,0x11,0x01,0,0,0,0,0, 0,0x24,0x04,0,0,0,0,0, 0,0xfc,0x12,0,0,0,0,0, 0,0xc0,0xbb,0x01,0,0,0,0};
static const wchar_t* kMouseSz[][2] = {
    {L"ActiveWindowTracking", L"0"}, {L"Beep", L"No"}, {L"DoubleClickHeight", L"4"}, {L"DoubleClickSpeed", L"500"},
    {L"DoubleClickWidth", L"4"}, {L"ExtendedSounds", L"No"}, {L"MouseHoverHeight", L"4"}, {L"MouseHoverTime", L"400"},
    {L"MouseHoverWidth", L"4"}, {L"MouseSensitivity", L"10"}, {L"MouseSpeed", L"1"}, {L"MouseThreshold1", L"6"},
    {L"MouseThreshold2", L"10"}, {L"MouseTrails", L"0"}, {L"SnapToDefaultButton", L"0"}, {L"SwapMouseButtons", L"0"},
};
static std::wstring g_lastBackup;

// Writes the current key to a .reg file so the reset can be undone by double-clicking it.
static bool BackupMouseKey(std::wstring& path) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Mouse", 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    std::wstring out = L"Windows Registry Editor Version 5.00\r\n\r\n[HKEY_CURRENT_USER\\Control Panel\\Mouse]\r\n";
    for (DWORD i = 0;; i++) {
        wchar_t name[256]; DWORD nl = 256, type = 0, dl = 0;
        if (RegEnumValueW(k, i, name, &nl, nullptr, &type, nullptr, &dl) != ERROR_SUCCESS) break;
        std::vector<BYTE> data(dl + 2);
        nl = 256;
        if (RegEnumValueW(k, i, name, &nl, nullptr, &type, data.data(), &dl) != ERROR_SUCCESS) continue;
        std::wstring n = L"\"";
        for (wchar_t* c = name; *c; c++) { if (*c == L'\\' || *c == L'"') n += L'\\'; n += *c; }
        n += L"\"=";
        if (type == REG_SZ) {
            std::wstring v((wchar_t*)data.data(), wcsnlen((wchar_t*)data.data(), dl / 2));
            std::wstring e;
            for (wchar_t c : v) { if (c == L'\\' || c == L'"') e += L'\\'; e += c; }
            out += n + L"\"" + e + L"\"\r\n";
        } else if (type == REG_DWORD && dl == 4) {
            wchar_t b[16]; swprintf_s(b, L"dword:%08x", *(DWORD*)data.data());
            out += n + b + L"\r\n";
        } else {
            wchar_t b[8];
            std::wstring hx = type == REG_BINARY ? L"hex:" : L"hex(" + std::to_wstring(type) + L"):";
            for (DWORD j = 0; j < dl; j++) { swprintf_s(b, L"%s%02x", j ? L"," : L"", data[j]); hx += b; }
            out += n + hx + L"\r\n";
        }
    }
    RegCloseKey(k);
    time_t t = time(nullptr); tm lt; localtime_s(&lt, &t);
    wchar_t stamp[32]; wcsftime(stamp, 32, L"%Y%m%d-%H%M%S", &lt);
    path = DataDir() + L"\\mouse-registry-backup-" + stamp + L".reg";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wr; WORD bom = 0xFEFF;
    bool ok = WriteFile(f, &bom, 2, &wr, nullptr) && WriteFile(f, out.data(), DWORD(out.size() * 2), &wr, nullptr);
    CloseHandle(f);
    return ok;
}

static void ApplyMouseDefaultsLive();
static bool ResetMouseRegistry() {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Mouse", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    bool ok = true;
    for (auto& v : kMouseSz)
        ok &= RegSetValueExW(k, v[0], 0, REG_SZ, (const BYTE*)v[1], DWORD((wcslen(v[1]) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    ok &= RegSetValueExW(k, L"SmoothMouseXCurve", 0, REG_BINARY, kCurveX, sizeof(kCurveX)) == ERROR_SUCCESS;
    ok &= RegSetValueExW(k, L"SmoothMouseYCurve", 0, REG_BINARY, kCurveY, sizeof(kCurveY)) == ERROR_SUCCESS;
    RegCloseKey(k);
    ApplyMouseDefaultsLive();
    return ok;
}
// Apply what Windows can apply live (the acceleration curves need a sign-out).
static void ApplyMouseDefaultsLive() {
    const UINT fl = SPIF_UPDATEINIFILE | SPIF_SENDCHANGE;
    int acc[3] = {6, 10, 1};
    SystemParametersInfoW(SPI_SETMOUSE, 0, acc, fl);
    SystemParametersInfoW(SPI_SETMOUSESPEED, 0, (PVOID)10, fl);
    SystemParametersInfoW(SPI_SETDOUBLECLICKTIME, 500, nullptr, fl);
    SystemParametersInfoW(SPI_SETDOUBLECLKWIDTH, 4, nullptr, fl);
    SystemParametersInfoW(SPI_SETDOUBLECLKHEIGHT, 4, nullptr, fl);
    SystemParametersInfoW(SPI_SETMOUSEHOVERTIME, 400, nullptr, fl);
    SystemParametersInfoW(SPI_SETMOUSEHOVERWIDTH, 4, nullptr, fl);
    SystemParametersInfoW(SPI_SETMOUSEHOVERHEIGHT, 4, nullptr, fl);
    SystemParametersInfoW(SPI_SETMOUSETRAILS, 0, nullptr, fl);
    SystemParametersInfoW(SPI_SETSNAPTODEFBUTTON, FALSE, nullptr, fl);
    SystemParametersInfoW(SPI_SETACTIVEWINDOWTRACKING, 0, (PVOID)FALSE, fl);
    SystemParametersInfoW(SPI_SETMOUSEBUTTONSWAP, FALSE, nullptr, fl);
}

// ================================================================ updater (GitHub releases)
enum { U_IDLE, U_CHECKING, U_UPTODATE, U_AVAILABLE, U_DOWNLOADING, U_READY, U_ERROR };
static const UINT WM_UPD = WM_APP + 5;
struct Updater {
    std::atomic<int> state{U_IDLE};
    std::atomic<int> progress{0};  // 0..1000
    std::atomic<bool> busy{false};
    std::mutex mx;
    std::wstring latest, url, digest, error, file;
} U;

static std::wstring Widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// Reads the string value of "key" at or after `from`; returns npos-like empty on failure.
static std::string JsonStr(const std::string& s, size_t from, const char* key, size_t* at = nullptr, size_t limit = std::string::npos) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = s.find(k, from);
    if (p == std::string::npos || p >= limit) return "";
    p += k.size();
    while (p < s.size() && (s[p] == ' ' || s[p] == ':' || s[p] == '\n' || s[p] == '\r' || s[p] == '\t')) p++;
    if (p >= s.size() || s[p] != '"') return "";
    std::string v;
    for (p++; p < s.size() && s[p] != '"'; p++) {
        if (s[p] == '\\' && p + 1 < s.size()) p++;
        v += s[p];
    }
    if (at) *at = p;
    return v;
}

static bool HttpGet(const std::wstring& url, std::string& out, bool progress) {
    URL_COMPONENTS uc{sizeof(uc)};
    wchar_t host[256], path[2048];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    wchar_t extra[2048] = L"";
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) return false;
    std::wstring full = std::wstring(path) + extra;
    bool ok = false;
    HINTERNET ses = WinHttpOpen(L"VagiiMouse-Updater/" APP_VER_WSTR, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    HINTERNET con = ses ? WinHttpConnect(ses, host, uc.nPort, 0) : nullptr;
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", full.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (req) {
        WinHttpSetTimeouts(req, 10000, 10000, 15000, 30000);
        const wchar_t* hdr = L"Accept: application/vnd.github+json\r\n";
        if (WinHttpSendRequest(req, hdr, (DWORD)-1L, nullptr, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
            DWORD status = 0, sz = sizeof(status);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &sz, nullptr);
            DWORD total = 0; sz = sizeof(total);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &total, &sz, nullptr);
            if (status == 200) {
                out.clear();
                char buf[16384];
                DWORD got = 0;
                ok = true;
                while (true) {
                    if (!WinHttpReadData(req, buf, sizeof(buf), &got)) { ok = false; break; }
                    if (!got) break;
                    out.append(buf, got);
                    if (progress && total) {
                        U.progress = (int)std::min<long long>(1000, (long long)out.size() * 1000 / total);
                        PostMessageW(g_hwnd, WM_UPD, 0, 0);
                    }
                }
            }
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return ok;
}

static bool ParseVer(std::wstring v, int out[3]) {
    if (!v.empty() && (v[0] == L'v' || v[0] == L'V')) v.erase(0, 1);
    out[0] = out[1] = out[2] = 0;
    return swscanf_s(v.c_str(), L"%d.%d.%d", &out[0], &out[1], &out[2]) >= 2;
}
static bool NewerThanMe(const std::wstring& tag) {
    int v[3];
    if (!ParseVer(tag, v)) return false;
    int me[3] = {APP_VER_MAJOR, APP_VER_MINOR, APP_VER_PATCH};
    for (int i = 0; i < 3; i++) if (v[i] != me[i]) return v[i] > me[i];
    return false;
}

static std::string Sha256Hex(const std::string& data) {
    unsigned char h[32];
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)data.data(), (ULONG)data.size(), h, 32) != 0) return "";
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (unsigned char c : h) { s += hex[c >> 4]; s += hex[c & 15]; }
    return s;
}

static std::wstring ExePath() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}
static std::wstring ExeSibling(const wchar_t* name) {
    std::wstring p = ExePath();
    return p.substr(0, p.find_last_of(L'\\') + 1) + name;
}

static void SetUpd(int st, const std::wstring& err = L"") {
    { std::lock_guard<std::mutex> g(U.mx); if (!err.empty()) U.error = err; }
    U.state = st;
    PostMessageW(g_hwnd, WM_UPD, 1, 0);
}

static DWORD WINAPI CheckThread(LPVOID p) {
    bool quiet = p != nullptr;
    SetUpd(U_CHECKING);
    std::string body;
    if (!HttpGet(L"https://api.github.com/repos/" APP_REPO L"/releases/latest", body, false)) {
        SetUpd(quiet ? U_IDLE : U_ERROR, L"Couldn't reach GitHub. Check your connection and try again.");
        U.busy = false;
        return 0;
    }
    std::wstring tag = Widen(JsonStr(body, 0, "tag_name"));
    std::string url, digest;
    size_t pos = body.find("\"assets\"");
    while (pos != std::string::npos) {
        size_t at = 0;
        std::string name = JsonStr(body, pos, "name", &at);
        if (name.empty()) break;
        // The plain app exe: updating swaps it in place, so no installer has to run (Smart App Control
        // blocks unsigned installers launched by another app).
        if (name == "VagiiMouse.exe") {
            size_t urlAt = 0;
            url = JsonStr(body, at, "browser_download_url", &urlAt);
            digest = JsonStr(body, at, "digest", nullptr, urlAt);
            break;
        }
        pos = at;
    }
    {
        std::lock_guard<std::mutex> g(U.mx);
        U.latest = tag;
        U.url = Widen(url);
        U.digest = Widen(digest);
    }
    S.lastCheck = (long long)time(nullptr);
    if (tag.empty()) SetUpd(quiet ? U_IDLE : U_ERROR, L"GitHub's answer didn't include a version.");
    else if (NewerThanMe(tag) && !url.empty()) SetUpd(U_AVAILABLE);
    else SetUpd(U_UPTODATE);
    U.busy = false;
    return 0;
}

static DWORD WINAPI DownloadThread(LPVOID) {
    std::wstring url, digest, tag;
    { std::lock_guard<std::mutex> g(U.mx); url = U.url; digest = U.digest; tag = U.latest; }
    U.progress = 0;
    SetUpd(U_DOWNLOADING);
    std::string data;
    if (!HttpGet(url, data, true) || data.size() < 100000 || data[0] != 'M' || data[1] != 'Z') {
        SetUpd(U_ERROR, L"The download failed. Try again.");
        U.busy = false;
        return 0;
    }
    if (digest.rfind(L"sha256:", 0) == 0) {
        std::wstring want = digest.substr(7);
        if (Widen(Sha256Hex(data)) != want) {
            SetUpd(U_ERROR, L"The download didn't match GitHub's checksum, so it wasn't installed.");
            U.busy = false;
            return 0;
        }
    }
    // Saved next to the running exe so the swap is a rename on the same drive.
    std::wstring file = ExeSibling(L"VagiiMouse.new.exe");
    HANDLE f = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD wr = 0;
    bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, data.data(), (DWORD)data.size(), &wr, nullptr) && wr == data.size();
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (!ok) { DeleteFileW(file.c_str()); SetUpd(U_ERROR, L"Couldn't save the update next to the app (folder not writable)."); U.busy = false; return 0; }
    { std::lock_guard<std::mutex> g(U.mx); U.file = file; }
    SetUpd(U_READY);
    U.busy = false;
    return 0;
}

static void StartCheck(bool quiet) {
    if (U.busy.exchange(true)) return;
    CloseHandle(CreateThread(nullptr, 0, CheckThread, quiet ? (LPVOID)1 : nullptr, 0, nullptr));
}
static void StartDownload() {
    if (U.busy.exchange(true)) return;
    CloseHandle(CreateThread(nullptr, 0, DownloadThread, nullptr, 0, nullptr));
}

// ================================================================ drawing
static ID2D1Factory* g_d2d;
static IDWriteFactory* g_dw;
static ID2D1HwndRenderTarget* g_rt;
static ID2D1SolidColorBrush* g_br;
static IDWriteTextFormat *fTitle, *fH1, *fBody, *fBodyB, *fSmall, *fLabel, *fBig, *fDial, *fIcon, *fIconLg;
static float g_scale = 1.0f;

enum : uint32_t {
    C_BG = 0x15161B, C_SIDE = 0x0F1014, C_CARD = 0x1D1F27, C_CARD2 = 0x262935, C_CARD3 = 0x30344A, C_TEXT = 0xE8E9EF,
    C_DIM = 0x8B8FA3, C_FAINT = 0x5B5F72, C_MOUSE = 0x3B82F6, C_MOUSE_T = 0x60A5FA, C_PAD = 0x8B5CF6, C_PAD_T = 0xA78BFA,
    C_LIVE = 0x22C55E, C_WARN = 0xF59E0B, C_WHITE = 0xFFFFFF, C_DANGER = 0xF87171
};
static uint32_t AccentOf(int dev) { return dev == DEV_PAD ? C_PAD : C_MOUSE; }
static uint32_t AccentTOf(int dev) { return dev == DEV_PAD ? C_PAD_T : C_MOUSE_T; }

typedef D2D1_RECT_F RF;
static RF R(float x, float y, float w, float h) { return D2D1::RectF(x, y, x + w, y + h); }
static ID2D1SolidColorBrush* B(uint32_t c, float a = 1.f) { g_br->SetColor(D2D1::ColorF(c, a)); return g_br; }
static void Fill(RF r, float rad, uint32_t c, float a = 1.f) { g_rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), B(c, a)); }
static void Stroke(RF r, float rad, uint32_t c, float w = 1.f, float a = 1.f) { g_rt->DrawRoundedRectangle(D2D1::RoundedRect(r, rad, rad), B(c, a), w); }
static void Txt(const std::wstring& s, RF r, IDWriteTextFormat* f, uint32_t c,
                DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT pa = DWRITE_PARAGRAPH_ALIGNMENT_CENTER, float a = 1.f) {
    f->SetTextAlignment(al);
    f->SetParagraphAlignment(pa);
    g_rt->DrawTextW(s.c_str(), (UINT32)s.size(), f, r, B(c, a), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
static void Circle(float cx, float cy, float r, uint32_t c, float a = 1.f) { g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), B(c, a)); }

static void Arc(float cx, float cy, float r, float a0, float a1, uint32_t c, float w) {  // degrees, clockwise, 0 = east
    if (a1 - a0 < 0.5f) return;
    ID2D1PathGeometry* g = nullptr;
    if (FAILED(g_d2d->CreatePathGeometry(&g))) return;
    ID2D1GeometrySink* s = nullptr;
    g->Open(&s);
    auto P = [&](float a) { float t = a * 3.14159265f / 180.f; return D2D1::Point2F(cx + r * cosf(t), cy + r * sinf(t)); };
    s->BeginFigure(P(a0), D2D1_FIGURE_BEGIN_HOLLOW);
    s->AddArc(D2D1::ArcSegment(P(a1), D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, (a1 - a0) > 180 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    s->EndFigure(D2D1_FIGURE_END_OPEN);
    s->Close();
    ID2D1StrokeStyle* ss = nullptr;
    D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND);
    g_d2d->CreateStrokeStyle(sp, nullptr, 0, &ss);
    g_rt->DrawGeometry(g, B(c), w, ss);
    SafeRelease(ss);
    SafeRelease(s);
    SafeRelease(g);
}

static bool CreateTextFormats() {
    auto mk = [](const wchar_t* fam, float size, DWRITE_FONT_WEIGHT w, IDWriteTextFormat** out) {
        g_dw->CreateTextFormat(fam, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", out);
        if (*out) (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    };
    mk(L"Segoe UI Variable Display", 17, DWRITE_FONT_WEIGHT_SEMI_BOLD, &fTitle);
    mk(L"Segoe UI Variable Display", 24, DWRITE_FONT_WEIGHT_SEMI_BOLD, &fH1);
    mk(L"Segoe UI Variable Text", 13, DWRITE_FONT_WEIGHT_NORMAL, &fBody);
    mk(L"Segoe UI Variable Text", 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, &fBodyB);
    mk(L"Segoe UI Variable Text", 11.5f, DWRITE_FONT_WEIGHT_NORMAL, &fSmall);
    mk(L"Segoe UI Variable Text", 10.5f, DWRITE_FONT_WEIGHT_BOLD, &fLabel);
    mk(L"Segoe UI Variable Display", 26, DWRITE_FONT_WEIGHT_SEMI_BOLD, &fBig);
    mk(L"Segoe UI Variable Display", 54, DWRITE_FONT_WEIGHT_SEMI_BOLD, &fDial);
    mk(L"Segoe Fluent Icons", 16, DWRITE_FONT_WEIGHT_NORMAL, &fIcon);
    mk(L"Segoe Fluent Icons", 20, DWRITE_FONT_WEIGHT_NORMAL, &fIconLg);
    return fBody != nullptr;
}

// Icon glyphs (Segoe Fluent Icons)
#define IC_MOUSE L"\xE962"
#define IC_PAD L"\xEFA5"
#define IC_SETTINGS L"\xE713"
#define IC_DOWNLOAD L"\xE896"
#define IC_ADD L"\xE710"
#define IC_REMOVE L"\xE738"
#define IC_REFRESH L"\xE72C"
#define IC_CHECK L"\xE73E"
#define IC_LEFT L"\xE76B"
#define IC_RIGHT L"\xE76C"
#define IC_POWER L"\xE7E8"

// ================================================================ immediate-mode input
struct UIState {
    float mx = -100, my = -100;
    bool down = false, pressed = false, released = false, rpressed = false;
    int wheel = 0;
    int hot = 0, newHot = 0, active = 0;
} ui;
static int g_menuStage = -1;  // deferred right-click menu on a stage pill
static bool g_updateNow = false;  // --update-now
static bool g_justUpdated = false;
static bool g_pendingInstall = false;  // "Install now" clicked; run after painting finishes
static bool g_pendingRegReset = false;  // deferred confirmation dialog
static std::wstring g_regMsg;
static bool g_regOk = false;

static bool Over(RF r) { return ui.mx >= r.left && ui.mx < r.right && ui.my >= r.top && ui.my < r.bottom; }
static bool Click(int id, RF r, bool* hover = nullptr) {
    bool h = Over(r);
    if (h) ui.newHot = id;
    if (h && ui.pressed) ui.active = id;
    if (hover) *hover = h;
    return ui.released && ui.active == id && h;
}

// ================================================================ app actions
static const UINT WM_TRAY = WM_APP + 1, WM_SHOWME = WM_APP + 2;
static const UINT TIMER_PERSIST = 1, TIMER_STATUS = 2, TIMER_AUTOCHECK = 3;
static UINT WM_TASKBARCREATED = 0;
static HICON g_iconBig, g_iconSmall;
static NOTIFYICONDATAW g_nid{};

static void Redraw() { if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE); }
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
static void TrayUpdate() { if (g_test) return; TrayTip(); Shell_NotifyIconW(NIM_MODIFY, &g_nid); }

// Live change: apply now without broadcasting, persist shortly after the user stops.
static void Changed(bool live = true) {
    ApplyAll(!live);
    if (live) SetTimer(g_hwnd, TIMER_PERSIST, 400, nullptr);
    else SaveSettings();
    TrayUpdate();
    Redraw();
}
static int CurDev() { return S.page == PAGE_PAD ? DEV_PAD : DEV_MOUSE; }
static void SetLevel(int dev, int lv) {
    lv = std::clamp(lv, 1, MAX_LEVEL);
    if (S.dev[dev].level == lv) return;
    S.dev[dev].level = lv;
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
static void ToggleBoost() { S.enabled = !S.enabled; Changed(false); }
static bool DevLive(int dev) { long long t = g_lastMove[dev].load(); return t && MsSince(t) < 350; }

// Ctrl+Alt+S: next stage of whichever device moved last.
static void CycleStage() {
    int dev = (g_padPresent && g_lastMove[1].load() > g_lastMove[0].load()) ? DEV_PAD : DEV_MOUSE;
    auto& st = S.dev[dev].stages;
    if (st.empty()) return;
    int cur = S.dev[dev].level, next = st[0];
    for (size_t i = 0; i < st.size(); i++)
        if (st[i] == cur) { next = st[(i + 1) % st.size()]; break; }
    S.dev[dev].level = next;
    Changed(false);
    MessageBeep(MB_OK);
}

static void ShowMain() {
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_hwnd);
    UpdateHook();
}

static bool g_updBlocked = false;  // last install attempt was stopped by Windows app control
#define RELEASES_URL L"https://github.com/" APP_REPO L"/releases/latest"

// Swap the running exe for the downloaded one: rename ours to .old (allowed while running),
// move the new one into place and start it. The new copy waits for us to exit before it
// takes over. If Windows refuses to start it (e.g. Smart App Control), put everything back.
static void InstallUpdate() {
    std::wstring file, tag;
    { std::lock_guard<std::mutex> g(U.mx); file = U.file; tag = U.latest; }
    if (file.empty() || g_test) return;
    std::wstring cur = ExePath(), old = ExeSibling(L"VagiiMouse.old.exe");
    g_updBlocked = false;
    DeleteFileW(old.c_str());
    if (!MoveFileExW(cur.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        SetUpd(U_ERROR, L"Couldn't replace the app file (error " + std::to_wstring(GetLastError()) + L").");
        return;
    }
    if (!MoveFileExW(file.c_str(), cur.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DWORD e = GetLastError();
        MoveFileExW(old.c_str(), cur.c_str(), MOVEFILE_REPLACE_EXISTING);
        SetUpd(U_ERROR, L"Couldn't put the new version in place (error " + std::to_wstring(e) + L").");
        return;
    }
    std::wstring cmd = L"\"" + cur + L"\" --updated --wait-pid=" + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    wchar_t fake[4] = L"";  // VAGII_FAKE_BLOCK=1 simulates Smart App Control refusing the new exe (testing)
    bool fakeBlock = GetEnvironmentVariableW(L"VAGII_FAKE_BLOCK", fake, 4) && fake[0] == L'1';
    if (!fakeBlock && CreateProcessW(cur.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        DestroyWindow(g_hwnd);  // the new version takes over once we're gone
        return;
    }
    DWORD e = fakeBlock ? 4551 : GetLastError();
    // Roll back: the new file goes away, the old one comes back.
    MoveFileExW(cur.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
    MoveFileExW(old.c_str(), cur.c_str(), MOVEFILE_REPLACE_EXISTING);
    DeleteFileW(file.c_str());
    { std::lock_guard<std::mutex> g(U.mx); U.file.clear(); }
    if (e >= 4550 && e <= 4599) {
        g_updBlocked = true;
        SetUpd(U_ERROR, L"Windows Smart App Control blocked " + tag + L" because it isn't signed yet. You're still on " APP_VER_WSTR
                        L" and nothing changed. Get the new version from the download page instead.");
    } else {
        SetUpd(U_ERROR, L"Windows didn't start the new version (error " + std::to_wstring(e) + L"). You're still on " APP_VER_WSTR L".");
    }
}

// After an update: remove leftovers and keep the installer's "Apps" entry on the right version.
static void PostUpdateCleanup() {
    for (int i = 0; i < 20 && !DeleteFileW(ExeSibling(L"VagiiMouse.old.exe").c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND; i++) Sleep(250);
    DeleteFileW(ExeSibling(L"VagiiMouse.new.exe").c_str());
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"VagiiMouseUpdate";
    WIN32_FIND_DATAW fd;
    HANDLE fh = FindFirstFileW((dir + L"\\*.exe").c_str(), &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do DeleteFileW((dir + L"\\" + fd.cFileName).c_str()); while (FindNextFileW(fh, &fd));
        FindClose(fh);
    }
    RemoveDirectoryW(dir.c_str());
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{8C3F1B27-6A94-4E5D-A0B8-2F7D91C4E6A3}_is1",
                      0, KEY_QUERY_VALUE | KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        const wchar_t* v = APP_VER_WSTR;
        RegSetValueExW(k, L"DisplayVersion", 0, REG_SZ, (const BYTE*)v, DWORD((wcslen(v) + 1) * sizeof(wchar_t)));
        std::wstring name = L"Vagii Mouse " APP_VER_WSTR;
        RegSetValueExW(k, L"DisplayName", 0, REG_SZ, (const BYTE*)name.c_str(), DWORD((name.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }
}

// ================================================================ widgets
static void Toggle(int id, RF r, bool on, uint32_t acc, bool* clicked) {
    bool hov;
    *clicked = Click(id, r, &hov);
    Fill(r, (r.bottom - r.top) / 2, on ? acc : C_CARD3);
    float k = (r.bottom - r.top) - 6, x = on ? r.right - 3 - k : r.left + 3;
    Circle(x + k / 2, r.top + 3 + k / 2, k / 2, C_WHITE, hov ? 1.f : 0.92f);
}
static bool Button(int id, RF r, const std::wstring& label, uint32_t bg = C_CARD2, uint32_t fg = C_TEXT, const wchar_t* icon = nullptr) {
    bool hov;
    bool c = Click(id, r, &hov);
    Fill(r, 8, bg, 1.f);
    if (hov) Fill(r, 8, C_WHITE, ui.active == id ? 0.10f : 0.06f);
    if (icon) {
        float tw = (float)label.size() * 6.6f;
        float x0 = (r.left + r.right) / 2 - (tw + 22) / 2;
        Txt(icon, R(x0, r.top, 18, r.bottom - r.top), fIcon, fg);
        Txt(label, R(x0 + 22, r.top, r.right - x0 - 22, r.bottom - r.top), fBodyB, fg);
    } else {
        Txt(label, r, fBodyB, fg, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    return c;
}
static bool IconButton(int id, RF r, const wchar_t* icon, uint32_t bg = C_CARD2) {
    bool hov;
    bool c = Click(id, r, &hov);
    Fill(r, (r.right - r.left) / 2, bg);
    if (hov) Fill(r, (r.right - r.left) / 2, C_WHITE, ui.active == id ? 0.12f : 0.07f);
    Txt(icon, r, fIcon, C_TEXT, DWRITE_TEXT_ALIGNMENT_CENTER);
    return c;
}
static int Segmented(int id, RF r, const std::vector<std::wstring>& items, int sel, uint32_t acc) {
    Fill(r, 8, C_BG);
    float w = (r.right - r.left - 4) / items.size();
    int res = -1;
    for (size_t i = 0; i < items.size(); i++) {
        RF s = R(r.left + 2 + i * w, r.top + 2, w, r.bottom - r.top - 4);
        bool hov;
        if (Click(id + (int)i, s, &hov)) res = (int)i;
        if ((int)i == sel) Fill(s, 6, acc);
        else if (hov) Fill(s, 6, C_WHITE, 0.06f);
        Txt(items[i], s, fBodyB, (int)i == sel ? C_WHITE : C_DIM, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    return res;
}
static int Slider(int id, RF r, int v, int lo, int hi, uint32_t acc) {
    bool hov;
    Click(id, R(r.left - 8, r.top - 8, r.right - r.left + 16, r.bottom - r.top + 16), &hov);
    float cy = (r.top + r.bottom) / 2;
    if (ui.active == id && ui.down) {
        float t = std::clamp((ui.mx - r.left) / (r.right - r.left), 0.f, 1.f);
        v = lo + (int)lroundf(t * (hi - lo));
    }
    if (hov && ui.wheel) v = std::clamp(v + (ui.wheel > 0 ? 1 : -1), lo, hi);
    float t = float(v - lo) / (hi - lo), x = r.left + t * (r.right - r.left);
    Fill(R(r.left, cy - 2, r.right - r.left, 4), 2, C_CARD3);
    Fill(R(r.left, cy - 2, x - r.left, 4), 2, acc);
    Circle(x, cy, (hov || ui.active == id) ? 8.f : 7.f, C_WHITE);
    return v;
}

// ================================================================ pages
static const float W = 960, H = 600, SIDE = 196;

static void DrawSidebar() {
    g_rt->FillRectangle(R(0, 0, SIDE, H), B(C_SIDE));
    Txt(L"Vagii Mouse", R(20, 18, 160, 26), fTitle, C_TEXT);
    Txt(L"v" APP_VER_WSTR, R(20, 42, 160, 16), fSmall, C_FAINT);

    for (int i = 0; i < 2; i++) {
        RF r = R(12, 72 + i * 74.f, SIDE - 24, 64);
        bool act = S.page == i, hov;
        if (Click(100 + i, r, &hov)) { S.page = i; SaveSettings(); }
        uint32_t acc = AccentOf(i);
        Fill(r, 12, act ? acc : C_CARD);
        if (hov && !act) Fill(r, 12, C_WHITE, 0.05f);
        Txt(i ? IC_PAD : IC_MOUSE, R(r.left + 14, r.top, 24, 64), fIconLg, act ? C_WHITE : C_DIM);
        Txt(i ? L"Trackpad" : L"Mouse", R(r.left + 48, r.top + 12, 100, 20), fBodyB, act ? C_WHITE : C_TEXT);
        std::wstring sub = (i == DEV_PAD && !g_padPresent) ? L"Not found" : L"Level " + std::to_wstring(S.dev[i].level);
        Txt(sub, R(r.left + 48, r.top + 32, 100, 18), fSmall, act ? 0xDCE4FF : C_DIM);
        if (DevLive(i)) Circle(r.right - 16, r.top + 32, 4.5f, C_LIVE);
    }

    // boost state chip
    {
        RF r = R(12, 230, SIDE - 24, 40);
        bool hov;
        if (Click(110, r, &hov)) ToggleBoost();
        Fill(r, 10, C_CARD);
        if (hov) Fill(r, 10, C_WHITE, 0.05f);
        Txt(IC_POWER, R(r.left + 14, r.top, 20, 40), fIcon, S.enabled ? C_LIVE : C_WARN);
        Txt(S.enabled ? L"Boost on" : L"Boost off", R(r.left + 42, r.top, 90, 40), fBodyB, C_TEXT);
        Txt(L"Ctrl+Alt+M", R(r.left, r.top, r.right - r.left - 12, 40), fSmall, C_FAINT, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    float y = H - 60;
    int st = U.state.load();
    if (st == U_AVAILABLE || st == U_DOWNLOADING || st == U_READY) {
        RF r = R(12, y - 52, SIDE - 24, 42);
        bool hov;
        if (Click(111, r, &hov)) { S.page = PAGE_SETTINGS; SaveSettings(); }
        Fill(r, 10, C_LIVE, hov ? 0.28f : 0.18f);
        Txt(IC_DOWNLOAD, R(r.left + 14, r.top, 20, 42), fIcon, C_LIVE);
        std::wstring lv;
        { std::lock_guard<std::mutex> g(U.mx); lv = U.latest; }
        Txt(L"Update " + lv, R(r.left + 42, r.top, 130, 42), fBodyB, 0xBBF7D0);
    }
    RF r = R(12, y, SIDE - 24, 44);
    bool act = S.page == PAGE_SETTINGS, hov;
    if (Click(112, r, &hov)) { S.page = PAGE_SETTINGS; SaveSettings(); }
    if (act) Fill(r, 10, C_CARD2);
    else if (hov) Fill(r, 10, C_WHITE, 0.05f);
    Txt(IC_SETTINGS, R(r.left + 14, r.top, 20, 44), fIcon, act ? C_TEXT : C_DIM);
    Txt(L"Settings", R(r.left + 42, r.top, 100, 44), fBodyB, act ? C_TEXT : C_DIM);
}

// Approximate gain (x multiplier) at pointer speed v (px per ms) for the graph.
static double CurveAt(int dev, double v) {
    const DevSettings& d = S.dev[dev];
    double b = BoostFor(d.level);
    double winCurve = 0.45 + 1.15 * (1 - exp(-v / 0.9));  // shape of Windows' own acceleration
    if (dev == DEV_MOUSE) {
        if (d.accelMode == 0) return b;
        if (d.accelMode == 1) return b * winCurve;
        return b * (1 + d.accelStrength * 0.1 * std::min(v / 1.5, 2.5));
    }
    double base = b * winCurve;
    return d.accelMode == 1 ? base * (1 + d.accelStrength * 0.1 * std::min(v / 1.5, 2.5)) : base;
}

static void DrawDevicePage(int dev) {
    DevSettings& d = S.dev[dev];
    uint32_t acc = AccentOf(dev), accT = AccentTOf(dev);
    const float X = SIDE + 28;
    Txt(dev ? L"Trackpad" : L"Mouse", R(X, 18, 400, 34), fH1, C_TEXT);
    Txt(dev ? L"Touchpad speed \x00B7 1\x2013" L"20 Windows range, 21\x2013" L"100 Vagii boost"
            : L"Pointer speed \x00B7 1\x2013" L"20 Windows range, 21\x2013" L"100 Vagii boost",
        R(X, 50, 500, 18), fSmall, C_DIM);

    if (dev == DEV_PAD && !g_padPresent) {
        RF r = R(X, 96, W - X - 28, 120);
        Fill(r, 14, C_CARD);
        Txt(g_padApi ? L"No precision touchpad found on this PC." : L"This Windows version has no touchpad speed setting.", R(r.left + 20, r.top + 24, 600, 24), fBodyB, C_TEXT);
        Txt(L"Trackpad settings need a Windows precision touchpad (Windows 11 24H2 or newer).", R(r.left + 20, r.top + 54, 600, 20), fSmall, C_DIM);
        Txt(L"Your mouse settings still work \x2014 use the Mouse card on the left.", R(r.left + 20, r.top + 76, 600, 20), fSmall, C_DIM);
        return;
    }

    // ---- dial
    const float cx = X + 180, cy = 232, rad = 112;
    const float A0 = 135, SWEEP = 270;
    {
        RF hit = R(cx - rad - 22, cy - rad - 22, (rad + 22) * 2, (rad + 22) * 2);
        // Only the ring itself grabs the dial; the number and the -/+ buttons inside it don't.
        float pdx = ui.mx - cx, pdy = ui.my - cy;
        RF minusR = R(cx - 52, cy + 70, 34, 34), plusR = R(cx + 18, cy + 70, 34, 34);
        bool onRing = Over(hit) && !Over(minusR) && !Over(plusR) && fabsf(sqrtf(pdx * pdx + pdy * pdy) - rad) <= 20;
        bool hov = onRing;
        if (onRing) ui.newHot = 200;
        if (onRing && ui.pressed) ui.active = 200;
        if (ui.active == 200 && ui.down) {
            float dx = ui.mx - cx, dy = ui.my - cy;
            if (dx * dx + dy * dy > 16 * 16) {
                float a = atan2f(dy, dx) * 180.f / 3.14159265f;  // -180..180, 0 = east, clockwise positive
                float rel = a - A0;
                while (rel < -45) rel += 360;
                while (rel >= 315) rel -= 360;
                rel = std::clamp(rel, 0.f, SWEEP);
                SetLevel(dev, 1 + (int)lroundf(rel / SWEEP * (MAX_LEVEL - 1)));
            }
        }
        if (Over(hit) && ui.wheel) SetLevel(dev, d.level + (ui.wheel > 0 ? 1 : -1));
        Arc(cx, cy, rad, A0, A0 + SWEEP, C_CARD2, 14);
        // Windows range marker (1..20) in a dimmer tone under the fill
        float winEnd = A0 + SWEEP * (WIN_MAX - 1) / float(MAX_LEVEL - 1);
        Arc(cx, cy, rad, A0, winEnd, C_CARD3, 14);
        float t = (d.level - 1) / float(MAX_LEVEL - 1);
        Arc(cx, cy, rad, A0, A0 + SWEEP * t, acc, 14);
        float ka = (A0 + SWEEP * t) * 3.14159265f / 180.f;
        Circle(cx + rad * cosf(ka), cy + rad * sinf(ka), (hov || ui.active == 200) ? 11.f : 9.f, C_WHITE);
        // tick for the Windows max
        float wa = winEnd * 3.14159265f / 180.f;
        g_rt->DrawLine(D2D1::Point2F(cx + (rad + 12) * cosf(wa), cy + (rad + 12) * sinf(wa)),
                       D2D1::Point2F(cx + (rad + 20) * cosf(wa), cy + (rad + 20) * sinf(wa)), B(C_DIM), 2);
        Txt(L"20", R(cx + (rad + 30) * cosf(wa) - 12, cy + (rad + 30) * sinf(wa) - 9, 24, 18), fSmall, C_DIM, DWRITE_TEXT_ALIGNMENT_CENTER);

        Txt(std::to_wstring(d.level), R(cx - 90, cy - 44, 180, 70), fDial, d.level > WIN_MAX ? accT : C_TEXT, DWRITE_TEXT_ALIGNMENT_CENTER);
        Txt(L"of 100", R(cx - 60, cy + 22, 120, 20), fSmall, C_DIM, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (IconButton(201, R(cx - 52, cy + 70, 34, 34), IC_REMOVE)) SetLevel(dev, d.level - 1);
        if (IconButton(202, R(cx + 18, cy + 70, 34, 34), IC_ADD)) SetLevel(dev, d.level + 1);
    }
    wchar_t buf[200];
    const wchar_t* win = dev ? L"Windows touchpad" : L"Windows";
    if (d.level > WIN_MAX) swprintf_s(buf, L"%s 20/20  +  boost \x00D7%.2f", win, BoostFor(d.level));
    else swprintf_s(buf, L"%s speed %d/20 \x00B7 no boost", win, d.level);
    Txt(buf, R(cx - 180, cy + 124, 360, 20), fBody, C_DIM, DWRITE_TEXT_ALIGNMENT_CENTER);

    // ---- stages
    {
        float y = 396;
        Txt(dev ? L"SPEED STAGES" : L"DPI STAGES", R(X, y, 200, 16), fLabel, C_DIM);
        auto& st = d.stages;
        float x = X, pw = 54, ph = 34, gap = 7;
        for (size_t i = 0; i < st.size(); i++) {
            RF r = R(x, y + 24, pw, ph);
            bool hov;
            if (Click(210 + (int)i, r, &hov)) { d.level = st[i]; Changed(false); }
            if (hov && ui.rpressed) g_menuStage = (int)i;
            bool on = st[i] == d.level;
            Fill(r, 9, on ? acc : C_CARD2);
            if (hov && !on) Fill(r, 9, C_WHITE, 0.06f);
            std::wstring lab = dev ? L"Lv " + std::to_wstring(st[i]) : std::to_wstring(EffDpi(st[i]));
            Txt(lab, r, fBodyB, on ? C_WHITE : C_TEXT, DWRITE_TEXT_ALIGNMENT_CENTER);
            x += pw + gap;
        }
        if ((int)st.size() < MAX_STAGES) {
            RF r = R(x, y + 24, 34, ph);
            bool hov;
            if (Click(220, r, &hov) && std::find(st.begin(), st.end(), d.level) == st.end()) {
                st.push_back(d.level);
                std::sort(st.begin(), st.end());
                SaveSettings();
            }
            Stroke(R(r.left + 0.5f, r.top + 0.5f, 33, ph - 1), 9, hov ? C_DIM : C_CARD3, 1);
            Txt(IC_ADD, r, fIcon, C_DIM, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        Txt(L"Click to use \x00B7 + adds the current level \x00B7 right-click to save or remove", R(X, y + 66, 380, 16), fSmall, C_FAINT);
        Txt(L"Ctrl+Alt+S cycles stages from anywhere", R(X, y + 84, 380, 16), fSmall, C_FAINT);
    }

    // ---- right column
    const float RX = 586, RW = W - RX - 24;
    {
        RF r = R(RX, 24, RW, 92);
        Fill(r, 14, C_CARD);
        if (dev == DEV_MOUSE) {
            Txt(L"EFFECTIVE DPI", R(r.left + 18, r.top + 14, 200, 16), fLabel, C_DIM);
            long long e = EffDpi(d.level);
            std::wstring s = std::to_wstring(e);
            for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert(i, L",");
            Txt(L"\x2248 " + s, R(r.left + 18, r.top + 32, 220, 36), fBig, accT);
            swprintf_s(buf, L"\x00D7%.2f vs Windows default \x00B7 your mouse %d DPI%s", TotalFactor(d.level), S.mouseDpi, d.accelMode ? L" + accel" : L"");
            Txt(buf, R(r.left + 18, r.top + 68, RW - 30, 16), fSmall, C_DIM);
        } else {
            Txt(L"SPEED VS WINDOWS MAX", R(r.left + 18, r.top + 14, 200, 16), fLabel, C_DIM);
            swprintf_s(buf, L"\x00D7%.2f", BoostFor(d.level));
            Txt(buf, R(r.left + 18, r.top + 32, 220, 36), fBig, accT);
            Txt(L"Windows stops at 20; higher levels multiply on top.", R(r.left + 18, r.top + 68, RW - 30, 16), fSmall, C_DIM);
        }
    }
    {
        RF r = R(RX, 128, RW, 250);
        Fill(r, 14, C_CARD);
        Txt(L"ACCELERATION", R(r.left + 18, r.top + 14, 200, 16), fLabel, C_DIM);
        std::vector<std::wstring> items = dev ? std::vector<std::wstring>{L"Windows curve", L"+ Boost curve"}
                                              : std::vector<std::wstring>{L"Off", L"Windows", L"Boost curve"};
        int s = Segmented(230, R(r.left + 14, r.top + 38, RW - 28, 32), items, d.accelMode, acc);
        if (s >= 0 && s != d.accelMode) { d.accelMode = s; Changed(false); }

        // graph: multiplier vs pointer speed
        RF gr = R(r.left + 18, r.top + 86, RW - 36, 92);
        const int N = 64;
        double vmax = 4.0, ymax = 0;
        for (int i = 0; i <= N; i++) ymax = std::max(ymax, CurveAt(dev, vmax * i / N));
        ymax = std::max(ymax * 1.4, 1.0);
        for (int k = 1; k <= 3; k++) {
            float gy = gr.bottom - (gr.bottom - gr.top) * k / 3.f;
            g_rt->DrawLine(D2D1::Point2F(gr.left, gy), D2D1::Point2F(gr.right, gy), B(C_CARD3, 0.6f), 1);
        }
        g_rt->DrawLine(D2D1::Point2F(gr.left, gr.bottom), D2D1::Point2F(gr.right, gr.bottom), B(C_CARD3), 1);
        D2D1_POINT_2F prev{};
        for (int i = 0; i <= N; i++) {
            double v = vmax * i / N;
            D2D1_POINT_2F p = D2D1::Point2F(gr.left + (gr.right - gr.left) * i / N, gr.bottom - float(CurveAt(dev, v) / ymax) * (gr.bottom - gr.top));
            if (i) g_rt->DrawLine(prev, p, B(acc), 2.5f);
            prev = p;
        }
        swprintf_s(buf, L"\x00D7%.1f", ymax);
        Txt(buf, R(gr.right - 60, gr.top - 2, 60, 14), fSmall, C_FAINT, DWRITE_TEXT_ALIGNMENT_TRAILING);
        Txt(L"slow", R(gr.left, gr.bottom + 2, 60, 14), fSmall, C_FAINT);
        Txt(L"fast", R(gr.right - 60, gr.bottom + 2, 60, 14), fSmall, C_FAINT, DWRITE_TEXT_ALIGNMENT_TRAILING);

        if (HasCurve(dev)) {
            swprintf_s(buf, L"Strength  %d / 20", d.accelStrength);
            Txt(buf, R(r.left + 18, r.top + 200, 140, 18), fBody, C_TEXT);
            int nv = Slider(240, R(r.left + 150, r.top + 209, RW - 172, 1), d.accelStrength, 1, 20, acc);
            if (nv != d.accelStrength) { d.accelStrength = nv; Changed(); }
        } else {
            const wchar_t* hint = dev ? L"The normal precision-touchpad feel."
                                : d.accelMode == 0 ? L"Same distance every time \x2014 best for aiming."
                                                   : L"Windows' \"Enhance pointer precision\".";
            Txt(hint, R(r.left + 18, r.top + 200, RW - 36, 18), fSmall, C_DIM);
        }
    }
    {
        RF r = R(RX, 390, RW, 54);
        Fill(r, 14, C_CARD);
        Txt(L"Boost", R(r.left + 18, r.top, 120, 54), fBodyB, C_TEXT);
        Txt(S.enabled ? L"on \x00B7 Ctrl+Alt+M" : L"off \x00B7 Ctrl+Alt+M", R(r.left + 70, r.top, 160, 54), fSmall, C_DIM);
        bool c;
        Toggle(250, R(r.right - 62, r.top + 15, 44, 24), S.enabled != 0, acc, &c);
        if (c) ToggleBoost();
    }
    float bw = (RW - 10) / 2;
    if (Button(251, R(RX, 456, bw, 38), L"Reset to default")) ResetDefault(dev);
    if (Button(252, R(RX + bw + 10, 456, bw, 38), L"Restore original")) RestoreOriginal(dev);
}

static const int kDpiSteps[] = {400, 600, 800, 1000, 1200, 1600, 2000, 2400, 3200, 4000, 4800, 6400, 8000, 12000, 16000, 20000, 26000, 32000};

static void DrawSettingsPage() {
    const float X = SIDE + 28;
    Txt(L"Settings", R(X, 18, 400, 34), fH1, C_TEXT);
    Txt(L"Apply to both the mouse and the trackpad", R(X, 50, 500, 18), fSmall, C_DIM);
    const float CW = 352;
    // left column
    {
        RF r = R(X, 86, CW, 70);
        Fill(r, 14, C_CARD);
        Txt(L"Your mouse's DPI", R(r.left + 18, r.top + 14, 200, 20), fBodyB, C_TEXT);
        Txt(L"See the box; 800 if unsure", R(r.left + 18, r.top + 36, 230, 18), fSmall, C_DIM);
        RF v = R(r.right - 140, r.top + 18, 122, 34);
        Fill(v, 9, C_BG);
        Txt(std::to_wstring(S.mouseDpi), v, fBodyB, C_TEXT, DWRITE_TEXT_ALIGNMENT_CENTER);
        int idx = 0;
        for (int i = 0; i < (int)std::size(kDpiSteps); i++) if (kDpiSteps[i] <= S.mouseDpi) idx = i;
        bool hov;
        if (Click(300, R(v.left, v.top, 34, 34), &hov) && S.mouseDpi > kDpiSteps[0]) {
            S.mouseDpi = kDpiSteps[kDpiSteps[idx] == S.mouseDpi ? std::max(idx - 1, 0) : idx]; SaveSettings();
        }
        Txt(IC_LEFT, R(v.left, v.top, 34, 34), fIcon, hov ? C_TEXT : C_DIM, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (Click(301, R(v.right - 34, v.top, 34, 34), &hov) && idx + 1 < (int)std::size(kDpiSteps)) {
            S.mouseDpi = kDpiSteps[idx + 1]; SaveSettings();
        }
        Txt(IC_RIGHT, R(v.right - 34, v.top, 34, 34), fIcon, hov ? C_TEXT : C_DIM, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (Over(v) && ui.wheel) {
            int ni = std::clamp(idx + (ui.wheel > 0 ? 1 : -1), 0, (int)std::size(kDpiSteps) - 1);
            S.mouseDpi = kDpiSteps[ni]; SaveSettings();
        }
    }
    struct Row { const wchar_t* t; const wchar_t* s; int* v; };
    Row rows[] = {
        {L"Pause boost in games", L"When a game hides or locks the cursor", &S.gameSafe},
        {L"Start with Windows", L"Runs quietly in the tray", &S.startup},
        {L"Keep running when closed", L"Closing the window hides it to the tray", &S.closeToTray},
        {L"Check for updates automatically", L"Looks on GitHub a few times a day", &S.autoUpdate},
    };
    const int nRows = StoreMode() ? 3 : 4;  // the Store handles updates
    RF box = R(X, 168, CW, nRows * 62.f);
    Fill(box, 14, C_CARD);
    for (int i = 0; i < nRows; i++) {
        float y = box.top + i * 62;
        if (i) g_rt->DrawLine(D2D1::Point2F(box.left + 18, y), D2D1::Point2F(box.right - 18, y), B(C_CARD2), 1);
        Txt(rows[i].t, R(box.left + 18, y + 12, 260, 20), fBodyB, C_TEXT);
        Txt(rows[i].s, R(box.left + 18, y + 32, 260, 18), fSmall, C_DIM);
        bool c;
        Toggle(310 + i, R(box.right - 62, y + 19, 44, 24), *rows[i].v != 0, C_MOUSE, &c);
        if (c) {
            *rows[i].v = !*rows[i].v;
            if (rows[i].v == &S.startup) SetStartup(S.startup);
            Changed(false);
        }
    }

    {
        RF r = R(X, 168 + nRows * 62.f + 12, CW, 76);
        Fill(r, 14, C_CARD);
        Txt(L"Windows mouse registry", R(r.left + 18, r.top + 14, 200, 20), fBodyB, C_TEXT);
        Txt(g_regMsg.empty() ? L"Control Panel\\Mouse to defaults" : g_regMsg, R(r.left + 18, r.top + 36, 214, 30),
            fSmall, g_regMsg.empty() ? C_DIM : (g_regOk ? C_LIVE : C_DANGER), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        if (Button(330, R(r.right - 118, r.top + 20, 100, 36), L"Reset\x2026")) g_pendingRegReset = true;
    }

    // right column: updates + hotkeys
    const float RX = X + CW + 20, RW = W - RX - 24;
    {
        RF r = R(RX, 86, RW, 214);
        Fill(r, 14, C_CARD);
        Txt(L"UPDATES", R(r.left + 18, r.top + 14, 200, 16), fLabel, C_DIM);
        Txt(L"Vagii Mouse " APP_VER_WSTR, R(r.left + 18, r.top + 36, 300, 24), fBodyB, C_TEXT);
        if (StoreMode()) {
            fSmall->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            Txt(L"Installed from the Microsoft Store, which keeps Vagii Mouse up to date automatically.",
                R(r.left + 18, r.top + 62, RW - 36, 62), fSmall, C_DIM, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            fSmall->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            if (Button(325, R(r.left + 18, r.top + 150, RW - 36, 40), L"Open Store updates", C_MOUSE, C_WHITE, IC_REFRESH))
                ShellExecuteW(nullptr, L"open", L"ms-windows-store://downloadsandupdates", nullptr, nullptr, SW_SHOWNORMAL);
        } else {
        int st = U.state.load();
        std::wstring latest, err;
        { std::lock_guard<std::mutex> g(U.mx); latest = U.latest; err = U.error; }
        std::wstring msg;
        uint32_t mc = C_DIM;
        switch (st) {
        case U_IDLE: msg = S.lastCheck ? L"Not checked since the app started." : L"Never checked yet."; break;
        case U_CHECKING: msg = L"Checking GitHub\x2026"; break;
        case U_UPTODATE: msg = g_justUpdated ? L"Updated to " APP_VER_WSTR L". You're on the latest version." : L"You're on the latest version."; mc = C_LIVE; break;
        case U_AVAILABLE: msg = L"Version " + latest + L" is available."; mc = C_LIVE; break;
        case U_DOWNLOADING: msg = L"Downloading " + latest + L"\x2026"; break;
        case U_READY: msg = L"Downloaded and checked. Install restarts Vagii Mouse."; mc = C_LIVE; break;
        case U_ERROR: msg = err; mc = C_DANGER; break;
        }
        RF mr = R(r.left + 18, r.top + 62, RW - 36, 62);
        fSmall->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        Txt(msg, mr, fSmall, mc, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        fSmall->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        if (S.lastCheck) {
            time_t t = (time_t)S.lastCheck;
            tm lt; localtime_s(&lt, &t);
            wchar_t tb[64];
            wcsftime(tb, 64, L"Last checked %d %b, %H:%M", &lt);
            Txt(tb, R(r.left + 18, r.top + 126, RW - 36, 16), fSmall, C_FAINT);
        }
        RF br = R(r.left + 18, r.top + 150, RW - 36, 40);
        if (st == U_DOWNLOADING) {
            Fill(R(br.left, br.top + 16, br.right - br.left, 8), 4, C_CARD3);
            Fill(R(br.left, br.top + 16, (br.right - br.left) * U.progress.load() / 1000.f, 8), 4, C_LIVE);
        } else if (st == U_AVAILABLE) {
            if (Button(320, br, L"Download and install " + latest, C_LIVE, 0x052E16, IC_DOWNLOAD)) StartDownload();
        } else if (st == U_READY) {
            if (Button(321, br, L"Install now", C_LIVE, 0x052E16, IC_CHECK)) g_pendingInstall = true;  // after EndDraw
        } else if (st == U_ERROR && g_updBlocked) {
            if (Button(324, br, L"Open download page", C_MOUSE, C_WHITE, IC_DOWNLOAD))
                ShellExecuteW(nullptr, L"open", RELEASES_URL, nullptr, nullptr, SW_SHOWNORMAL);
        } else if (st == U_CHECKING) {
            Button(322, br, L"Checking\x2026", C_CARD2, C_DIM);
        } else {
            if (Button(323, br, L"Check for updates", C_MOUSE, C_WHITE, IC_REFRESH)) StartCheck(false);
        }
        }  // !StoreMode()
    }
    {
        RF r = R(RX, 312, RW, 104);
        Fill(r, 14, C_CARD);
        Txt(L"SHORTCUTS", R(r.left + 18, r.top + 14, 200, 16), fLabel, C_DIM);
        Txt(L"Turn the boost on or off", R(r.left + 18, r.top + 38, 220, 20), fBody, C_TEXT);
        Txt(L"Ctrl+Alt+M", R(r.left, r.top + 38, RW - 18, 20), fBodyB, C_DIM, DWRITE_TEXT_ALIGNMENT_TRAILING);
        Txt(L"Next stage (last-used device)", R(r.left + 18, r.top + 66, 220, 20), fBody, C_TEXT);
        Txt(L"Ctrl+Alt+S", R(r.left, r.top + 66, RW - 18, 20), fBodyB, C_DIM, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
}

static void DrawStatus() {
    std::wstring st;
    uint32_t c = C_FAINT;
    bool any = NeedsHook(0) || NeedsHook(1);
    if (!S.enabled && any) { st = L"Boost is off \x2014 press Ctrl+Alt+M or the Boost switch to turn it back on."; c = C_WARN; }
    else if (any && g_paused.load()) st = L"Boost paused: a game has hidden or locked the cursor.";
    else if (any) st = L"Boost active \x2014 Vagii Mouse keeps running in the tray.";
    else st = L"Using Windows' own speeds. Changes apply instantly.";
    g_rt->DrawLine(D2D1::Point2F(SIDE + 28, H - 46), D2D1::Point2F(W - 24, H - 46), B(C_CARD2), 1);
    Txt(st, R(SIDE + 28, H - 40, W - SIDE - 52, 30), fSmall, c);
}

static bool CreateTarget() {
    if (g_rt) return true;
    RECT rc; GetClientRect(g_hwnd, &rc);
    D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties();
    UINT dpi = GetDpiForWindow(g_hwnd);
    rp.dpiX = rp.dpiY = (float)dpi;
    g_scale = dpi / 96.f;
    if (FAILED(g_d2d->CreateHwndRenderTarget(rp, D2D1::HwndRenderTargetProperties(g_hwnd, D2D1::SizeU(rc.right, rc.bottom)), &g_rt))) return false;
    g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(C_TEXT), &g_br);
    return true;
}

static void Render() {
    if (!CreateTarget()) return;
    ui.newHot = 0;
    g_rt->BeginDraw();
    g_rt->Clear(D2D1::ColorF(C_BG));
    DrawSidebar();
    if (S.page == PAGE_SETTINGS) DrawSettingsPage();
    else DrawDevicePage(CurDev());
    DrawStatus();
    if (g_rt->EndDraw() == D2DERR_RECREATE_TARGET) { SafeRelease(g_br); SafeRelease(g_rt); }
    ui.hot = ui.newHot;
    ui.pressed = ui.rpressed = false;
    ui.wheel = 0;
    if (ui.released) { ui.released = false; ui.active = 0; }
}

static void StageMenu(int idx) {
    int dev = CurDev();
    auto& st = S.dev[dev].stages;
    if (idx < 0 || idx >= (int)st.size()) return;
    HMENU m = CreatePopupMenu();
    wchar_t buf[96];
    swprintf_s(buf, L"Save current level (%d) here", S.dev[dev].level);
    AppendMenuW(m, MF_STRING, 1, buf);
    AppendMenuW(m, MF_STRING | (st.size() <= 1 ? MF_GRAYED : 0), 2, L"Remove this stage");
    POINT pt; GetCursorPos(&pt);
    int cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(m);
    if (cmd == 1) { st[idx] = S.dev[dev].level; std::sort(st.begin(), st.end()); st.erase(std::unique(st.begin(), st.end()), st.end()); }
    if (cmd == 2 && st.size() > 1) st.erase(st.begin() + idx);
    SaveSettings();
    Redraw();
}

// Store installs: Windows redirects a packaged app's own HKCU writes to a private copy that
// Windows never reads (verified with tools\probe-msix.ps1), so there the reset is done by
// Windows' own reg.exe started outside the package. Desktop-app breakaway policy: a process
// created with BREAKAWAY_ENABLE_PROCESS_TREE still runs in the package, but its children
// (that aren't part of the package) run outside it, hence cmd.exe -> reg.exe.
#ifndef PROC_THREAD_ATTRIBUTE_DESKTOP_APP_POLICY
#define PROC_THREAD_ATTRIBUTE_DESKTOP_APP_POLICY ProcThreadAttributeValue(18, FALSE, TRUE, FALSE)
#define PROCESS_CREATION_DESKTOP_APP_BREAKAWAY_ENABLE_PROCESS_TREE 0x01
#endif
static std::wstring System32(const wchar_t* exe) {
    wchar_t d[MAX_PATH];
    GetSystemDirectoryW(d, MAX_PATH);
    return std::wstring(d) + L"\\" + exe;
}
// Runs a command line hidden and waits; returns its exit code (0xFFFFFFFF if it didn't start).
static DWORD RunHidden(std::wstring cmd, bool childrenBreakAway) {
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    SIZE_T sz = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &sz);
    std::vector<BYTE> buf(sz);
    auto list = (LPPROC_THREAD_ATTRIBUTE_LIST)buf.data();
    DWORD policy = PROCESS_CREATION_DESKTOP_APP_BREAKAWAY_ENABLE_PROCESS_TREE;
    bool attr = childrenBreakAway && InitializeProcThreadAttributeList(list, 1, 0, &sz);
    if (attr && !UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_DESKTOP_APP_POLICY, &policy, sizeof(policy), nullptr, nullptr)) {
        DeleteProcThreadAttributeList(list);
        attr = false;
    }
    if (attr) si.lpAttributeList = list;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | (attr ? EXTENDED_STARTUPINFO_PRESENT : 0),
                             nullptr, nullptr, &si.StartupInfo, &pi);
    if (attr) DeleteProcThreadAttributeList(list);
    if (!ok) return 0xFFFFFFFF;
    DWORD code = 0xFFFFFFFF;
    if (WaitForSingleObject(pi.hProcess, 30000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}
// cmd.exe (inside the package, breakaway policy) running reg.exe commands (outside it).
static DWORD RunRegOutsidePackage(const std::wstring& regCommands) {
    return RunHidden(L"\"" + System32(L"cmd.exe") + L"\" /d /c \"" + regCommands + L"\"", true);
}
static std::wstring RegExe() { return L"\"" + System32(L"reg.exe") + L"\""; }
static std::wstring HexStr(const BYTE* p, size_t n) {
    std::wstring s;
    wchar_t b[4];
    for (size_t i = 0; i < n; i++) { swprintf_s(b, L"%02x", p[i]); s += b; }
    return s;
}
// Packaged reset: backup with "reg export", then "reg add" every default; returns RR_*.
enum { RR_OK = 0, RR_BACKUP = 2, RR_WRITE = 3 };
static int ResetMouseRegistryOutsidePackage(const std::wstring& bak, const wchar_t* regKey = L"HKCU\\Control Panel\\Mouse") {
    const std::wstring key = L" \"" + std::wstring(regKey) + L"\"";
    RunRegOutsidePackage(RegExe() + L" export" + key + L" \"" + bak + L"\" /y");
    if (GetFileAttributesW(bak.c_str()) == INVALID_FILE_ATTRIBUTES) return RR_BACKUP;
    std::wstring cmds;
    auto add = [&](const std::wstring& name, const wchar_t* type, const std::wstring& data) {
        if (!cmds.empty()) cmds += L" && ";
        cmds += RegExe() + L" add" + key + L" /v " + name + L" /t " + type + L" /d \"" + data + L"\" /f";
    };
    for (auto& v : kMouseSz) add(v[0], L"REG_SZ", v[1]);
    add(L"SmoothMouseXCurve", L"REG_BINARY", HexStr(kCurveX, sizeof(kCurveX)));
    add(L"SmoothMouseYCurve", L"REG_BINARY", HexStr(kCurveY, sizeof(kCurveY)));
    return RunRegOutsidePackage(cmds) == 0 ? RR_OK : RR_WRITE;
}
static void RegResetFlow() {
    int a = MessageBoxW(g_hwnd,
        L"Reset HKEY_CURRENT_USER\\Control Panel\\Mouse to Windows' default values?\n\n"
        L"This covers pointer speed and acceleration (including the SmoothMouse curves), double-click speed, "
        L"hover time, pointer trails, snap-to and swapped buttons. Nothing outside that key is touched.\n\n"
        L"Your current values are saved to a .reg backup first; double-click it to undo.\n"
        L"The acceleration curves take effect after you sign out and back in.",
        L"Vagii Mouse", MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
    if (a != IDOK) return;
    if (g_test) { g_regOk = true; g_regMsg = L"Test mode: nothing changed."; Redraw(); return; }
    std::wstring bak;
    if (g_packaged) {
        // Documents, not AppData: a packaged app's new AppData files are redirected too, and
        // reg.exe runs outside the package, so the backup must live where both see the same file.
        wchar_t docs[MAX_PATH];
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docs))) { g_regOk = false; g_regMsg = L"Couldn't find your Documents folder."; Redraw(); return; }
        std::wstring dir = std::wstring(docs) + L"\\Vagii Mouse";
        CreateDirectoryW(dir.c_str(), nullptr);
        time_t t = time(nullptr); tm lt; localtime_s(&lt, &t);
        wchar_t stamp[32]; wcsftime(stamp, 32, L"%Y%m%d-%H%M%S", &lt);
        bak = dir + L"\\mouse-registry-backup-" + stamp + L".reg";
        int r = ResetMouseRegistryOutsidePackage(bak);
        if (r == RR_BACKUP) { g_regOk = false; g_regMsg = L"Couldn't save the backup, so nothing was changed."; Redraw(); return; }
        g_regOk = r == RR_OK;
        if (g_regOk) ApplyMouseDefaultsLive();
    } else {
        if (!BackupMouseKey(bak)) { g_regOk = false; g_regMsg = L"Couldn't save the backup, so nothing was changed."; Redraw(); return; }
        g_regOk = ResetMouseRegistry();
    }
    g_lastBackup = bak;
    g_regMsg = g_regOk ? L"Done. Backup saved; sign out to apply the curves." : L"Some values couldn't be written.";
    S.dev[DEV_MOUSE].level = 10;
    S.dev[DEV_MOUSE].accelMode = 1;
    Changed(false);
    if (g_regOk && MessageBoxW(g_hwnd, (L"Done. Backup saved to:\n" + bak + L"\n\nShow it in File Explorer?").c_str(), L"Vagii Mouse", MB_YESNO | MB_ICONINFORMATION) == IDYES)
        ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + bak + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
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

static void SetMouse(LPARAM lp) { ui.mx = GET_X_LPARAM(lp) / g_scale; ui.my = GET_Y_LPARAM(lp) / g_scale; }

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (WM_TASKBARCREATED && msg == WM_TASKBARCREATED) { TrayAdd(); return 0; }
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = h;
        BOOL dark = TRUE;
        DwmSetWindowAttribute(h, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
        COLORREF cap = RGB(0x0F, 0x10, 0x14);
        DwmSetWindowAttribute(h, 35 /*DWMWA_CAPTION_COLOR*/, &cap, sizeof(cap));
        if (!g_test) {
            RegisterHotKey(h, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'M');
            RegisterHotKey(h, 2, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'S');
        }
        TrayAdd();
        SetTimer(h, TIMER_STATUS, 150, nullptr);
        if (!g_test && !StoreMode() && S.autoUpdate) SetTimer(h, TIMER_AUTOCHECK, 8000, nullptr);
        return 0;
    }
    case WM_DPICHANGED: {
        SafeRelease(g_br); SafeRelease(g_rt);
        auto* r = (RECT*)lp;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Redraw();
        return 0;
    }
    case WM_SIZE:
        if (g_rt) g_rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        return 0;
    case WM_MOUSEMOVE: {
        SetMouse(lp);
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
        TrackMouseEvent(&tme);
        Redraw();
        return 0;
    }
    case WM_MOUSELEAVE: if (!ui.down) { ui.mx = ui.my = -100; } Redraw(); return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: SetMouse(lp); ui.down = ui.pressed = true; SetCapture(h); Redraw(); return 0;
    case WM_LBUTTONUP: SetMouse(lp); ui.down = false; ui.released = true; ReleaseCapture(); Redraw(); return 0;
    case WM_RBUTTONDOWN: SetMouse(lp); ui.rpressed = true; Redraw(); return 0;
    case WM_MOUSEWHEEL: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(h, &p);
        ui.mx = p.x / g_scale; ui.my = p.y / g_scale;
        ui.wheel += GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1;
        Redraw();
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursorW(nullptr, ui.hot ? IDC_HAND : IDC_ARROW)); return TRUE; }
        break;
    case WM_KEYDOWN: {
        int dev = CurDev();
        if (wp == VK_TAB && (GetKeyState(VK_CONTROL) & 0x8000)) { S.page = (S.page + 1) % 3; SaveSettings(); Redraw(); }
        else if (S.page != PAGE_SETTINGS) {
            if (wp == VK_RIGHT || wp == VK_UP) SetLevel(dev, S.dev[dev].level + 1);
            else if (wp == VK_LEFT || wp == VK_DOWN) SetLevel(dev, S.dev[dev].level - 1);
            else if (wp == VK_PRIOR) SetLevel(dev, S.dev[dev].level + 5);
            else if (wp == VK_NEXT) SetLevel(dev, S.dev[dev].level - 5);
        }
        return 0;
    }
    case WM_HOTKEY:
        if (wp == 1) { ToggleBoost(); MessageBeep(S.enabled ? MB_OK : MB_ICONASTERISK); }
        else if (wp == 2) CycleStage();
        return 0;
    case WM_TIMER:
        if (wp == TIMER_PERSIST) { KillTimer(h, TIMER_PERSIST); ApplyAll(true); SaveSettings(); }
        else if (wp == TIMER_STATUS && IsWindowVisible(h)) {
            static int lastSig = -1;
            int sig = (g_paused.load() ? 1 : 0) | (DevLive(0) ? 2 : 0) | (DevLive(1) ? 4 : 0);
            if (sig != lastSig) { lastSig = sig; Redraw(); }
        } else if (wp == TIMER_AUTOCHECK) {
            SetTimer(h, TIMER_AUTOCHECK, 60 * 60 * 1000, nullptr);  // re-evaluate hourly
            if (S.autoUpdate && (long long)time(nullptr) - S.lastCheck > 6 * 3600 && U.state.load() != U_READY) StartCheck(true);
        }
        return 0;
    case WM_UPD:
        if (wp == 1) SaveSettings();  // lastCheck
        if (g_updateNow && wp == 1) {  // --update-now: check, download and install without clicks
            int st = U.state.load();
            if (st == U_AVAILABLE) StartDownload();
            else if (st == U_READY) InstallUpdate();
            else if (st == U_UPTODATE || st == U_ERROR) g_updateNow = false;
        }
        Redraw();
        return 0;
    case WM_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK) ShowMain();
        else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, 1, L"Open Vagii Mouse");
            AppendMenuW(m, MF_STRING | (S.enabled ? MF_CHECKED : 0), 2, L"Boost enabled\tCtrl+Alt+M");
            AppendMenuW(m, MF_STRING, 3, L"Next stage\tCtrl+Alt+S");
            AppendMenuW(m, MF_STRING, 4, L"Reset everything to Windows default");
            if (U.state.load() == U_AVAILABLE || U.state.load() == U_READY) AppendMenuW(m, MF_STRING, 6, L"Update available\x2026");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, 5, L"Exit");
            POINT pt; GetCursorPos(&pt);
            SetForegroundWindow(h);
            int cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, h, nullptr);
            DestroyMenu(m);
            switch (cmd) {
            case 1: ShowMain(); break;
            case 2: ToggleBoost(); break;
            case 3: CycleStage(); break;
            case 4: ResetDefault(DEV_MOUSE); if (g_padPresent) ResetDefault(DEV_PAD); break;
            case 5: DestroyWindow(h); break;
            case 6: S.page = PAGE_SETTINGS; ShowMain(); Redraw(); break;
            }
        }
        return 0;
    case WM_SHOWME: ShowMain(); return 0;
    case WM_STARTUP_STATE: S.startup = 0; SaveSettings(); Redraw(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        Render();
        EndPaint(h, &ps);
        if (g_menuStage >= 0) { int i = g_menuStage; g_menuStage = -1; StageMenu(i); }
        if (g_pendingRegReset) { g_pendingRegReset = false; RegResetFlow(); }
        if (g_pendingInstall) { g_pendingInstall = false; InstallUpdate(); }
        return 0;
    }
    case WM_SHOWWINDOW:
        PostMessageW(h, WM_APP + 3, 0, 0);  // re-evaluate the hook after visibility settles
        break;
    case WM_APP + 3: UpdateHook(); return 0;
    case WM_CLOSE:
        if (S.closeToTray && !g_test) { ShowWindow(h, SW_HIDE); return 0; }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        if (!g_test) Shell_NotifyIconW(NIM_DELETE, &g_nid);
        UnregisterHotKey(h, 1);
        UnregisterHotKey(h, 2);
        if (KillTimer(h, TIMER_PERSIST)) ApplyAll(true);
        SaveSettings();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Dark context menus (uxtheme ordinals 135/136, Windows 10 1903+).
static void DarkMenus() {
    if (HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        using SetMode = int(WINAPI*)(int);
        using Flush = void(WINAPI*)();
        if (auto f = (SetMode)GetProcAddress(ux, MAKEINTRESOURCEA(135))) f(2 /*ForceDark*/);
        if (auto f = (Flush)GetProcAddress(ux, MAKEINTRESOURCEA(136))) f();
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmd, int) {
    wchar_t ev[8] = L"";
    g_test = GetEnvironmentVariableW(L"VAGII_TEST", ev, 8) && ev[0] == L'1';
    g_packaged = IsPackaged();
    g_fakeStore = !g_packaged && GetEnvironmentVariableW(L"VAGII_FAKE_STORE", ev, 8) && ev[0] == L'1';
    // tools\probe-msix.ps1: can reg.exe started from the package write the real HKCU?
    // Only touches the throwaway key HKCU\Software\VagiiMsixProbe.
    if (wcsstr(GetCommandLineW(), L"--probe-breakaway")) {
        std::wstring k = L" HKCU\\Software\\VagiiMsixProbe";
        RunHidden(RegExe() + L" add" + k + L" /v direct /d 1 /f", true);   // reg.exe as the direct child
        RunRegOutsidePackage(RegExe() + L" add" + k + L" /v viacmd /d 1 /f");  // cmd.exe -> reg.exe
        // the real reset commands, aimed at a throwaway key; arg = where to put the test backup
        wchar_t* out = wcsstr(GetCommandLineW(), L"--probe-breakaway") + 17;
        while (*out == L' ' || *out == L'"') out++;
        std::wstring bak = out;
        while (!bak.empty() && (bak.back() == L'"' || bak.back() == L' ')) bak.pop_back();
        if (!bak.empty()) {
            RunRegOutsidePackage(RegExe() + L" add HKCU\\Software\\VagiiMsixProbe\\Mouse /v Seed /d 1 /f");  // something to back up
            return ResetMouseRegistryOutsidePackage(bak, L"HKCU\\Software\\VagiiMsixProbe\\Mouse");
        }
        return 0;
    }
    const wchar_t* cls = g_test ? L"VagiiMouseWndTest" : L"VagiiMouseWnd";
    // Started by the updater: wait for the previous version to exit before taking over.
    if (const wchar_t* wp = wcsstr(cmd, L"--wait-pid=")) {
        if (HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)_wtoi(wp + 11))) {
            WaitForSingleObject(old, 15000);
            CloseHandle(old);
        }
    }
    HANDLE mtx = CreateMutexW(nullptr, TRUE, g_test ? L"Local\\VagiiMouseTest" : L"Local\\VagiiMouseSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(cls, nullptr)) PostMessageW(other, WM_SHOWME, 0, 0);
        return 0;
    }
    bool startHidden = wcsstr(cmd, L"--tray") != nullptr;
    const wchar_t* pg = wcsstr(cmd, L"--page=");  // for screenshots
    const wchar_t* us = wcsstr(cmd, L"--upd=");   // fake updater state for screenshots (test mode only)

    QueryPerformanceFrequency(&g_qpf);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (g_packaged && LaunchedByStartupTask()) startHidden = true;
    if (StoreMode()) S.startup = 0;  // Store installs start with Windows only once the user turns it on
    IniPath();
    ProbePad();
    LoadSettings();
    CaptureOriginal();
    if (pg) S.page = std::clamp(_wtoi(pg + 7), 0, 2);
    if (us && g_test) { U.state = _wtoi(us + 6); U.latest = L"v1.3.0"; U.progress = 420; U.error = L"Couldn't reach GitHub. Check your connection and try again."; }
    SaveSettings();
    DarkMenus();
    if (!g_test && !StoreMode()) PostUpdateCleanup();
    if (wcsstr(cmd, L"--updated")) { S.page = PAGE_SETTINGS; U.state = U_UPTODATE; g_justUpdated = true; }

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d)) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_dw)) || !CreateTextFormats()) {
        MessageBoxW(nullptr, L"Direct2D isn't available on this PC.", L"Vagii Mouse", MB_ICONERROR);
        return 1;
    }

    g_hookReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE th = CreateThread(nullptr, 0, HookThread, nullptr, 0, &g_hookThreadId);
    WaitForSingleObject(g_hookReady, 5000);
    ApplyAll(false);
    if (S.startup) SetStartup(true);  // keep the Run entry pointing at this exe

    g_iconBig = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    g_iconSmall = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = g_iconBig;
    wc.hIconSm = g_iconSmall;
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);

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
    RECT r{0, 0, MulDiv((int)W, dpi, 96), MulDiv((int)H, dpi, 96)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
    int w = r.right - r.left, hgt = r.bottom - r.top;
    RECT wa = mi.rcWork;
    int x = wa.left + ((wa.right - wa.left) - w) / 2, y = wa.top + ((wa.bottom - wa.top) - hgt) / 2;
    HWND hwnd = CreateWindowExW(0, cls, L"Vagii Mouse", style, x, y, w, hgt, nullptr, nullptr, inst, nullptr);
    FitOnScreen(hwnd);
    if (!startHidden) ShowWindow(hwnd, SW_SHOWNORMAL);
    if (!StoreMode() && wcsstr(cmd, L"--update-now")) { g_updateNow = true; S.page = PAGE_SETTINGS; StartCheck(false); }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
    WaitForSingleObject(th, 2000);
    SafeRelease(g_br); SafeRelease(g_rt); SafeRelease(g_dw); SafeRelease(g_d2d);
    CloseHandle(mtx);
    return 0;
}
