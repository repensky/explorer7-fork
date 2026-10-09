#include "FlyoutFix.h"
#include "dbgprint.h"
#include "MinHook.h"
#include "OptionConfig.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <commctrl.h>

// The wrapper runs on minCRT, so wcscmp is not available here
// lstrcmpW would drag in locale rules, this is a plain ordinal compare
static int WideCompare(LPCWSTR a, LPCWSTR b) {
    while (*a && *a == *b) { ++a; ++b; }
    return (int)(*a) - (int)(*b);
}

#define WH_ORIG_WNDPROC L"WH_OrigWndProc"
#define WH_VAN_SETUP    L"WH_VanSetup"
#define CListUiBase_hWnd(pThis) *((HWND *)pThis + 2)

//---Diagnostics--------------------------------------------

#define WH_TRACE_COUNT   L"WH_FlyoutTrace"
#define FLYOUT_TRACE_MAX 24

// Bounded per window, a flyout that repositions constantly would flood the log
static BOOL FlyoutTraceWanted(HWND hwnd) {
    if (!hwnd) return FALSE;

    INT_PTR n = (INT_PTR)GetPropW(hwnd, WH_TRACE_COUNT);
    if (n >= FLYOUT_TRACE_MAX) return FALSE;

    SetPropW(hwnd, WH_TRACE_COUNT, (HANDLE)(n + 1));
    return TRUE;
}

// Each new window deserves its own budget, styling happens once per window
static void FlyoutTraceReset(HWND hwnd) {
    if (hwnd) RemovePropW(hwnd, WH_TRACE_COUNT);
}

// Nearly every line below wants the class, so it is read in one place
static void FlyoutClassOf(HWND hwnd, LPWSTR out, int cch) {
    out[0] = 0;
    if (hwnd && IsWindow(hwnd)) GetClassNameW(hwnd, out, cch);
}

//---Original function pointers-----------------------------

// All twelve CreateWindowExW arguments, the last three ride on the stack and were not forwarded before
static HWND (*IsolationAwareCreateWindowExW_orig)(__int64, LPCWSTR, __int64,
                                                  DWORD, int, int, int, int,
                                                  HWND, HMENU, HINSTANCE, LPVOID) = nullptr;
static WNDPROC g_clockWndProc_orig = nullptr;
static void (*UpdateFlyoutUI_orig)(void) = nullptr;
static void (*CListUiBase_PositionVanUI_orig)(void*, bool, LPRECT, int, UINT,
                                              LPPOINT) = nullptr;
static SUBCLASSPROC CVanUI_TopWindowSubclassProc_orig = nullptr;


//---Frame lock---------------------------------------------

LRESULT LockFrameHit(LRESULT hit) {
    if (hit == HTCLIENT || hit == HTTRANSPARENT || hit == HTNOWHERE) return hit;
    return HTBORDER;
}

bool IsFrameDragMessage(UINT msg, WPARAM wp) {
    if (msg == WM_NCLBUTTONDOWN || msg == WM_NCLBUTTONDBLCLK) return true;
    if (msg == WM_SYSCOMMAND) {
        WPARAM cmd = wp & 0xFFF0;
        return cmd == SC_MOVE || cmd == SC_SIZE;
    }
    return false;
}

// Live in util.h, which only dllmain includes, so they are declared here
bool IsClassicTheme(void);
bool IsCompositionManuallyDisabled(void);
bool ShouldDisableShellWindowTransparency(void);
void InvalidateThemeStateSnapshot(void);

// Dwmapi alone answers yes on Basic, a theme mod has to be present to lie
// The wrapper's own mode is the part that works with no mod in the process
static BOOL IsCompositionOn() {
    BOOL composed = FALSE;
    BOOL dwmOn = (DwmIsCompositionEnabled(&composed) >= 0) && composed;

    BOOL on = dwmOn && !ShouldDisableShellWindowTransparency() && IsCompositionActive();

    // Printed on a flip only, this runs on every flyout move
    static int last = -1;
    if (last != (int)on) {
        last = (int)on;
        dbgprintf(L"E7TRACE FlyoutComp on=%d dwm=%d classic=%d compAct=%d appThemed=%d manDis=%d",
                  (int)on, (int)dwmOn, (int)IsClassicTheme(), (int)IsCompositionActive(),
                  (int)IsAppThemed(), (int)IsCompositionManuallyDisabled());
    }
    return on;
}

// Windows never gave these windows a frame style, it had DWM draw the frame
// With composition off there is no glass, so the flyout stays frameless
BOOL ApplyFlyoutFrame(HWND hwnd) {
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    BOOL comp = IsCompositionOn();
    LONG_PTR want  = comp ? (style | WS_THICKFRAME)
                          : (style & ~(LONG_PTR)WS_THICKFRAME);

    if (want == style) {
        // Already correct, and a window that arrives with the frame lands here
        if (FlyoutTraceWanted(hwnd)) {
            dbgprintf(L"E7TRACE FlyoutFrame %p style 0x%08X comp %d, already right",
                      hwnd, (DWORD)style, (int)comp);
        }
        return FALSE;
    }

    SetWindowLongPtrW(hwnd, GWL_STYLE, want);

    LONG_PTR now = GetWindowLongPtrW(hwnd, GWL_STYLE);
    dbgprintf(L"E7TRACE FlyoutFrame %p style 0x%08X to 0x%08X now 0x%08X comp %d",
              hwnd, (DWORD)style, (DWORD)want, (DWORD)now, (int)comp);

    return TRUE;
}

//---Native gap---------------------------------------------

// The shell publishes how far a flyout floats off the taskbar
static const GUID GUID_ShellNotifyTaskbarOffset =
    { 0x964B6543, 0xBBAD, 0x44EE, { 0x84, 0x8A, 0x3A, 0x95, 0xD8, 0x59, 0x51, 0xEA } };

// Windows 7 published a real taskbar offset, this build answers zero
// Measured against a Windows 7 VM the flyouts want one more pixel
static const int LOST_SHELL_OFFSET = 1;

// Half a thick frame plus the shell taskbar offset
// SndVol, timedate and ActionCenter all build the number exactly this way
int NativeFlyoutGap() {
    // Win7 dropped composition with the classic theme and flyouts went flush
    if (!IsCompositionOn()) {
        static int said = 0;
        if (!said) {
            said = 1;
            dbgprintf(L"E7TRACE FlyoutGap 0, composition is off so flyouts sit flush");
        }
        return 0;
    }

    RECT frame = { 0, 0, 0, 0 };
    BOOL gotFrame = AdjustWindowRectEx(&frame, WS_THICKFRAME, FALSE, 0);
    int half = gotFrame ? (frame.right - frame.left) / 2 : 0;

    NOTIFYICONIDENTIFIER nid = {};
    nid.cbSize   = sizeof(nid);
    nid.guidItem = GUID_ShellNotifyTaskbarOffset;
    RECT off = {};
    int offset = 0;
    HRESULT hr = Shell_NotifyIconGetRect(&nid, &off);
    if (SUCCEEDED(hr)) offset = off.bottom - off.top;

    BOOL fellBack = (offset == 0);
    if (fellBack) offset = LOST_SHELL_OFFSET;

    // Printed on a change only, this is asked on every flyout move
    static int last = -1;
    if (last != half + offset) {
        last = half + offset;
        dbgprintf(L"E7TRACE FlyoutGap %d, half %d frameOk %d offset %d hr 0x%08X fellBack %d",
                  half + offset, half, (int)gotFrame, offset, (unsigned)hr, (int)fellBack);
    }

    return half + offset;
}

static int ClampInt(int v, int lo, int hi) {
    if (lo > hi) return lo;
    if (v < lo)  return lo;
    return (v > hi) ? hi : v;
}

POINT AdjustWindowPosForTaskbar(HWND hwnd, RECT rc) {
    HMONITOR hm = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    GetMonitorInfoW(hm, &mi);

    int gap = NativeFlyoutGap();
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    POINT pt = { ClampInt(rc.left, mi.rcWork.left + gap, mi.rcWork.right - w - gap),
                 ClampInt(rc.top,  mi.rcWork.top  + gap, mi.rcWork.bottom - h - gap) };

    // A window already inside the work area is left alone, which reads as no float
    if (FlyoutTraceWanted(hwnd)) {
        WCHAR cn[64] = {};
        FlyoutClassOf(hwnd, cn, ARRAYSIZE(cn));

        dbgprintf(L"E7TRACE FlyoutClamp %p [%s] gap %d in %d,%d %dx%d work %d,%d,%d,%d out %d,%d moved %d",
                  hwnd, cn, gap, rc.left, rc.top, w, h,
                  mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom,
                  pt.x, pt.y, (int)(pt.x != rc.left || pt.y != rc.top));
    }

    return pt;
}

//---Sizing helpers-----------------------------------------

typedef BOOL (WINAPI* AdjustWindowRectExForDpi_t)(LPRECT, DWORD, BOOL, DWORD, UINT);
typedef UINT (WINAPI* GetDpiForWindow_t)(HWND);

// Grows a client rect into the window rect the current styles need
static void ClientRectToWindowRect(HWND hwnd, LPRECT prc) {
    static AdjustWindowRectExForDpi_t pAdjustForDpi =
        (AdjustWindowRectExForDpi_t)GetProcAddress(
            GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi");
    static GetDpiForWindow_t pGetDpiForWindow =
        (GetDpiForWindow_t)GetProcAddress(
            GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");

    DWORD style   = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
    DWORD exStyle = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    UINT  dpi     = pGetDpiForWindow ? pGetDpiForWindow(hwnd) : 0;

    if (pAdjustForDpi && dpi) {
        pAdjustForDpi(prc, style, FALSE, exStyle, dpi);
    } else {
        AdjustWindowRectEx(prc, style, FALSE, exStyle);
    }
}

//---Shared flyout window proc------------------------------

// Defined with the VAN code below, the shared proc needs it first
static BOOL SyncDwmFrame(HWND hwnd);

static LRESULT CALLBACK GenericFlyoutWndProc(HWND hwnd, UINT msg,
                                             WPARAM wp, LPARAM lp) {
    WNDPROC orig = (WNDPROC)GetPropW(hwnd, WH_ORIG_WNDPROC);
    if (!orig) {
        dbgprintf(L"E7TRACE FlyoutProc %p msg 0x%04X with no original, falling back", hwnd, msg);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    // The thick frame we add is the only drag surface, keep it dead
    if (msg == WM_NCHITTEST)
        return LockFrameHit(CallWindowProcW(orig, hwnd, msg, wp, lp));
    if (IsFrameDragMessage(msg, wp)) return 0;

    // Composition can be switched off under a window that already exists
    // Glass has to be dropped or re-added right here, not at next creation
    if (msg == WM_DWMCOMPOSITIONCHANGED || msg == WM_THEMECHANGED) {
        // This can be the first window to hear it, so the theme snapshot goes first
        InvalidateThemeStateSnapshot();
        dbgprintf(L"E7TRACE FlyoutRecheck %p msg 0x%04X", hwnd, msg);
        BOOL changed = ApplyFlyoutFrame(hwnd);

        // Only the VAN window ever had dwm frame drawing turned on for it
        if (GetPropW(hwnd, WH_VAN_SETUP) && SyncDwmFrame(hwnd)) changed = TRUE;

        if (changed) {
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_FRAMECHANGED | SWP_NOACTIVATE);
        }
    }

    if (msg == WM_WINDOWPOSCHANGING) {
        WINDOWPOS* wpos = (WINDOWPOS*)lp;

        // A move that never arrives is the quiet way for a flyout to stay put
        if ((wpos->flags & SWP_NOMOVE) && FlyoutTraceWanted(hwnd)) {
            dbgprintf(L"E7TRACE FlyoutNoMove %p flags 0x%08X, nothing to clamp",
                      hwnd, (DWORD)wpos->flags);
        }

        if (!(wpos->flags & SWP_NOMOVE)) {
            WCHAR cn[256] = {};
            GetClassNameW(hwnd, cn, 256);
            // NativeHWNDHost is fully repositioned by the VAN hook
            // Skipping it here avoids nudging the same window twice
            if (WideCompare(cn, L"NativeHWNDHost") != 0) {
                int cx = (wpos->flags & SWP_NOSIZE) ? 0 : wpos->cx;
                int cy = (wpos->flags & SWP_NOSIZE) ? 0 : wpos->cy;
                if (wpos->flags & SWP_NOSIZE) {
                    RECT wrc;
                    GetWindowRect(hwnd, &wrc);
                    cx = wrc.right - wrc.left;
                    cy = wrc.bottom - wrc.top;
                }
                RECT rc = { wpos->x, wpos->y, wpos->x + cx, wpos->y + cy };
                POINT pt = AdjustWindowPosForTaskbar(hwnd, rc);
                wpos->x = pt.x;
                wpos->y = pt.y;
            }
        }
    }
    return CallWindowProcW(orig, hwnd, msg, wp, lp);
}

// Frame and float for a flyout owned by another process, see SndVolFlyoutFix.h
// Styles and positions cross a process boundary, a subclass does not
void FlyoutFixFloatForeignWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;

    // Measured before the frame goes on, so the content keeps its size
    RECT client = {};
    GetClientRect(hwnd, &client);

    ApplyFlyoutFrame(hwnd);

    RECT want = { 0, 0, client.right, client.bottom };
    ClientRectToWindowRect(hwnd, &want);
    int w = want.right - want.left;
    int h = want.bottom - want.top;

    RECT now = {};
    GetWindowRect(hwnd, &now);
    RECT target = { now.left, now.top, now.left + w, now.top + h };
    POINT pt = AdjustWindowPosForTaskbar(hwnd, target);

    SetWindowPos(hwnd, nullptr, pt.x, pt.y, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

// Puts an already floated flyout back inside the gap after it moves itself
void FlyoutFixReclampForeignWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;

    RECT rc = {};
    GetWindowRect(hwnd, &rc);
    POINT pt = AdjustWindowPosForTaskbar(hwnd, rc);

    if (pt.x == rc.left && pt.y == rc.top) return;

    SetWindowPos(hwnd, nullptr, pt.x, pt.y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void ApplyStyleAndSubclass(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
        dbgprintf(L"E7TRACE FlyoutStyle %p refused, not a window", hwnd);
        return;
    }

    // Styling is once per window, so the trace budget starts over here
    FlyoutTraceReset(hwnd);

    WCHAR cn[64] = {};
    FlyoutClassOf(hwnd, cn, ARRAYSIZE(cn));

    RECT before = {};
    GetWindowRect(hwnd, &before);

    dbgprintf(L"E7TRACE FlyoutStyle %p [%s] style 0x%08X ex 0x%08X at %d,%d %dx%d visible %d",
              hwnd, cn,
              (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE),
              (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE),
              before.left, before.top,
              before.right - before.left, before.bottom - before.top,
              (int)IsWindowVisible(hwnd));

    ApplyFlyoutFrame(hwnd);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                      GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_TOOLWINDOW);

    // Without this swap nothing ever clamps the position, so the result is reported
    BOOL had = GetPropW(hwnd, WH_ORIG_WNDPROC) ? TRUE : FALSE;
    if (!had) {
        WNDPROC orig = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                                                  (LONG_PTR)GenericFlyoutWndProc);
        if (orig) SetPropW(hwnd, WH_ORIG_WNDPROC, (HANDLE)orig);

        dbgprintf(L"E7TRACE FlyoutSubclass %p orig %p err %lu",
                  hwnd, orig, orig ? 0 : GetLastError());
    }
    else {
        dbgprintf(L"E7TRACE FlyoutSubclass %p already done", hwnd);
    }

    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED |
                 SWP_NOACTIVATE);

    RECT after = {};
    GetWindowRect(hwnd, &after);

    dbgprintf(L"E7TRACE FlyoutStyled %p style 0x%08X ex 0x%08X at %d,%d %dx%d",
              hwnd,
              (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE),
              (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE),
              after.left, after.top,
              after.right - after.left, after.bottom - after.top);
}

//---VAN network flyout-------------------------------------

// The tray icon VAN anchors the flyout to
static const GUID SCAID_Network =
    { 0x7820AE74, 0x23E3, 0x4229, { 0x82, 0xC1, 0xE4, 0x1C, 0xB6, 0x7D, 0x5B, 0x9C } };

// Same anchor lookup VAN does, the hidden icons button is the fallback
static BOOL GetNetworkAnchorRect(LPRECT prc) {
    NOTIFYICONIDENTIFIER nid = {};
    nid.cbSize   = sizeof(nid);
    nid.guidItem = SCAID_Network;

    RECT rc = {};
    if (SUCCEEDED(Shell_NotifyIconGetRect(&nid, &rc)) &&
        rc.left != rc.right && rc.top != rc.bottom) {
        *prc = rc;
        return TRUE;
    }

    HWND tray   = FindWindowW(L"Shell_TrayWnd", nullptr);
    HWND notify = tray ? FindWindowExW(tray, nullptr, L"TrayNotifyWnd", nullptr) : nullptr;
    HWND button = notify ? FindWindowExW(notify, nullptr, L"Button", nullptr) : nullptr;
    return (button && GetWindowRect(button, prc));
}

// VAN turns DWM frame drawing on from inside the call we no longer make
// Dwm draws that frame whatever the style says, so it has to flip back too
static BOOL SyncDwmFrame(HWND hwnd) {
    BOOL want = IsCompositionOn();

    int ncEnabled = 0;
    if (DwmGetWindowAttribute(hwnd, DWMWA_NCRENDERING_ENABLED,
                              &ncEnabled, sizeof(ncEnabled)) >= 0) {
        if ((ncEnabled != 0) == (want != 0)) return FALSE;
    }

    int policy = want ? DWMNCRP_ENABLED : DWMNCRP_DISABLED;
    DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
    return TRUE;
}

static void CListUiBase_PositionVanUI_hook(void* pThis, bool bThickFrames,
                                           LPRECT lprc, int swpFlags,
                                           UINT uFlags, LPPOINT pAnchorPt) {
    HWND hwnd = bThickFrames ? CListUiBase_hWnd(pThis) : nullptr;
    WCHAR cn[256] = {};
    if (hwnd && IsWindow(hwnd)) GetClassNameW(hwnd, cn, 256);

    // The hover tooltip and anything unexpected keeps stock placement
    BOOL ours = hwnd && IsWindow(hwnd)
        && GetParent(hwnd) == nullptr && GetWindow(hwnd, GW_OWNER) == nullptr
        && !(GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD)
        && WideCompare(cn, L"NativeHWNDHost") == 0;

    RECT rcAnchor = {};
    if (ours && !GetNetworkAnchorRect(&rcAnchor)) ours = FALSE;

    if (!ours) {
        CListUiBase_PositionVanUI_orig(pThis, bThickFrames, lprc, swpFlags,
                                       uFlags, pAnchorPt);
        return;
    }

    RECT rcScreen;
    GetWindowRect(hwnd, &rcScreen);

    // Styling is once per window, VAN re-positions on every network scan
    BOOL firstPass = !GetPropW(hwnd, WH_VAN_SETUP);
    if (firstPass) {
        SetPropW(hwnd, WH_VAN_SETUP, (HANDLE)1);
        SetWindowRgn(hwnd, nullptr, FALSE);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                          GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_TOOLWINDOW);
        if (!GetPropW(hwnd, WH_ORIG_WNDPROC)) {
            WNDPROC prev = (WNDPROC)SetWindowLongPtrW(
                hwnd, GWLP_WNDPROC, (LONG_PTR)GenericFlyoutWndProc);
            if (prev) SetPropW(hwnd, WH_ORIG_WNDPROC, (HANDLE)prev);
        }
    }

    // This window outlives a theme change, so both frames get rechecked
    BOOL frameChanged = ApplyFlyoutFrame(hwnd);
    if (SyncDwmFrame(hwnd)) frameChanged = TRUE;

    // VAN hands us the client size the list was laid out for
    // Our thick frame sits outside that, so grow the window to keep it
    RECT c = { 0, 0, 0, 0 };
    if (lprc) {
        c.right  = lprc->right - lprc->left;
        c.bottom = lprc->bottom - lprc->top;
    } else {
        GetClientRect(hwnd, &c);
    }
    ClientRectToWindowRect(hwnd, &c);
    int w = c.right - c.left, h = c.bottom - c.top;

    // VAN's own formula, ask for a box gap px larger then sit back inside it
    int gap = NativeFlyoutGap();
    SIZE reserved = { w + 2 * gap, h + 2 * gap };
    POINT anchorPt = pAnchorPt ? *pAnchorPt
                               : POINT{ rcAnchor.left, rcAnchor.top };
    RECT rcPopup = {};
    int finalX = rcScreen.left, finalY = rcScreen.top;
    if (CalculatePopupWindowPosition(&anchorPt, &reserved, uFlags,
                                     &rcAnchor, &rcPopup)) {
        finalX = rcPopup.left + gap;
        finalY = rcPopup.top  + gap;
    }

    BOOL moved   = (finalX != rcScreen.left) || (finalY != rcScreen.top);
    BOOL resized = (w != rcScreen.right - rcScreen.left) ||
                   (h != rcScreen.bottom - rcScreen.top);

    // A repeat call with nothing to change would repaint the frame for free
    if (!firstPass && !frameChanged && (swpFlags & SWP_NOZORDER) &&
        !moved && !resized) return;

    UINT flags = swpFlags | SWP_NOCOPYBITS;
    if (firstPass || frameChanged) flags |= SWP_FRAMECHANGED;

    SetWindowPos(hwnd, HWND_TOPMOST, finalX, finalY, w, h, flags);
}

static LRESULT CALLBACK CVanUI_TopWindowSubclassProc_hook(
    HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    if (msg == WM_NCHITTEST) {
        return LockFrameHit(CVanUI_TopWindowSubclassProc_orig(
            hwnd, msg, wp, lp, uIdSubclass, dwRefData));
    }
    if (IsFrameDragMessage(msg, wp)) return 0;
    return CVanUI_TopWindowSubclassProc_orig(hwnd, msg, wp, lp,
                                             uIdSubclass, dwRefData);
}

//---Other explorer flyouts---------------------------------

static HWND IsolationAwareCreateWindowExW_hook(__int64 i1, LPCWSTR lpClassName,
                                               __int64 i2, DWORD dwStyle,
                                               int X, int Y, int nWidth,
                                               int nHeight, HWND hWndParent,
                                               HMENU hMenu, HINSTANCE hInstance,
                                               LPVOID lpParam) {
    HWND hwnd = IsolationAwareCreateWindowExW_orig(i1, lpClassName, i2, dwStyle,
                                                   X, Y, nWidth, nHeight,
                                                   hWndParent, hMenu, hInstance,
                                                   lpParam);
    if (hwnd && ((ULONG_PTR)lpClassName & ~0xffff) &&
        WideCompare(lpClassName, L"ClockFlyoutWindow") == 0) {
        ApplyStyleAndSubclass(hwnd);
    }
    return hwnd;
}

static LRESULT CALLBACK CTrayClock_s_WndProc_hook(HWND hwnd, UINT msg,
                                                  WPARAM wp, LPARAM lp) {
    if (msg == WM_NCHITTEST) {
        return LockFrameHit(CallWindowProcW(g_clockWndProc_orig, hwnd, msg, wp, lp));
    }
    if (IsFrameDragMessage(msg, wp)) return 0;

    if (msg == WM_WINDOWPOSCHANGING) {
        WINDOWPOS* wpos = (WINDOWPOS*)lp;
        if (!(wpos->flags & SWP_NOMOVE)) {
            int cx = wpos->cx, cy = wpos->cy;
            if (wpos->flags & SWP_NOSIZE) {
                RECT wrc;
                GetWindowRect(hwnd, &wrc);
                cx = wrc.right - wrc.left;
                cy = wrc.bottom - wrc.top;
            }
            RECT rc = { wpos->x, wpos->y, wpos->x + cx, wpos->y + cy };
            POINT pt = AdjustWindowPosForTaskbar(hwnd, rc);
            wpos->x = pt.x;
            wpos->y = pt.y;
        }
    }
    return CallWindowProcW(g_clockWndProc_orig, hwnd, msg, wp, lp);
}

static void UpdateFlyoutUI_hook(void) {
    UpdateFlyoutUI_orig();
    HWND h = FindWindowW(L"BatMeterFlyout", nullptr);
    if (h) ApplyStyleAndSubclass(h);
}

// The battery and Action Center flyouts both arrive through
// CreateWindowInBand, which the wrapper already hooks in util.h
void FlyoutFixOnBandWindow(HWND hwnd, LPCWSTR className, LPCWSTR source) {
    if (!hwnd) {
        dbgprintf(L"E7TRACE FlyoutBand %s gave no window", source);
        return;
    }

    // An atom rather than a string, so there is no name to compare against
    if (!className || !((ULONG_PTR)className & ~0xffff)) {
        WCHAR cn[64] = {};
        FlyoutClassOf(hwnd, cn, ARRAYSIZE(cn));
        dbgprintf(L"E7TRACE FlyoutBand %s %p atom 0x%04X, real class [%s]",
                  source, hwnd, (unsigned)(ULONG_PTR)className, cn);
        return;
    }

    BOOL ours = WideCompare(className, L"BatMeterFlyout") == 0 ||
                WideCompare(className, L"WHCFlyoutWindow") == 0;

    // Every band window is named here, so an unexpected class cannot hide
    dbgprintf(L"E7TRACE FlyoutBand %s %p [%s] style 0x%08X ours %d",
              source, hwnd, className,
              (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), (int)ours);

    if (ours) ApplyStyleAndSubclass(hwnd);
}

//---Standalone band hook-----------------------------------

// Only used when the immersive stack is off, see the note in InstallFlyoutFix
typedef HWND (WINAPI* CreateWindowInBandFlyoutAPI)(DWORD, LPWSTR, PVOID, PVOID,
                                                   PVOID, PVOID, PVOID, PVOID,
                                                   PVOID, PVOID, PVOID, PVOID, DWORD);
static CreateWindowInBandFlyoutAPI CreateWindowInBandFlyoutOrig = nullptr;

static HWND WINAPI CreateWindowInBandFlyoutOnly(DWORD exStyle, LPWSTR className,
                                                PVOID windowName, PVOID style,
                                                PVOID x, PVOID y, PVOID w, PVOID h,
                                                PVOID parent, PVOID menu,
                                                PVOID inst, PVOID param, DWORD band) {
    HWND hwnd = CreateWindowInBandFlyoutOrig(exStyle, className, windowName, style,
                                             x, y, w, h, parent, menu, inst, param, band);
    FlyoutFixOnBandWindow(hwnd, className, L"BandStandalone");
    return hwnd;
}

//---Tray icon tooltips-------------------------------------
// Windows 7 put a tray icon tooltip against the notification area, later builds place it elsewhere
// Ported from the Legacy Tray Icon Tooltips mod by yanashubby (kieldbg)

// Balloons are positioned by the sender and must be left alone
#define TTS_BALLOON_STYLE 0x40

typedef BOOL (WINAPI* SetWindowPos_t)(HWND, HWND, int, int, int, int, UINT);
static SetWindowPos_t SetWindowPos_orig;

static bool IsDescendantOf(HWND child, HWND ancestor) {
    if (!child || !ancestor) return false;
    if (child == ancestor) return true;
    for (HWND h = child; h;) {
        h = GetParent(h);
        if (h == ancestor) return true;
    }
    return false;
}

static bool IsCursorOverNotifyArea(HWND notify) {
    if (!notify) return false;
    POINT pt;
    GetCursorPos(&pt);
    RECT rc;
    GetWindowRect(notify, &rc);
    return PtInRect(&rc, pt) != FALSE;
}

// The tooltip never says which icon it belongs to, so ownership and the cursor decide it
static bool IsTrayIconTooltip(HWND tooltip, HWND tray, HWND notify) {
    HWND owner = GetWindow(tooltip, GW_OWNER);
    if (owner && IsDescendantOf(owner, notify))
        return true;
    // The tray itself owns some of them, only the cursor tells those apart
    if (owner == tray)
        return IsCursorOverNotifyArea(notify);
    if (owner)
        return false;
    POINT pt;
    GetCursorPos(&pt);
    HWND hovered = WindowFromPoint(pt);
    if (hovered && IsDescendantOf(hovered, notify))
        return true;
    return IsCursorOverNotifyArea(notify);
}

static BOOL WINAPI SetWindowPos_hook(HWND hwnd, HWND insertAfter, int x, int y, int cx, int cy, UINT flags) {
    if (flags & SWP_NOMOVE)
        return SetWindowPos_orig(hwnd, insertAfter, x, y, cx, cy, flags);

    WCHAR cls[64] = L"";
    if (GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) <= 0 || wcscmp(cls, L"tooltips_class32") != 0)
        return SetWindowPos_orig(hwnd, insertAfter, x, y, cx, cy, flags);
    if (GetWindowLongPtrW(hwnd, GWL_STYLE) & TTS_BALLOON_STYLE)
        return SetWindowPos_orig(hwnd, insertAfter, x, y, cx, cy, flags);

    HWND tray = FindWindowW(L"Shell_TrayWnd", NULL);
    HWND notify = tray ? FindWindowExW(tray, NULL, L"TrayNotifyWnd", NULL) : NULL;
    if (!notify || !IsTrayIconTooltip(hwnd, tray, notify))
        return SetWindowPos_orig(hwnd, insertAfter, x, y, cx, cy, flags);

    // Anchor on the icon toolbar when it is there, the notify area is the fallback
    HWND pager = FindWindowExW(notify, NULL, L"SysPager", NULL);
    HWND toolbar = pager ? FindWindowExW(pager, NULL, L"ToolbarWindow32", NULL) : NULL;
    HWND anchor = toolbar ? toolbar : (pager ? pager : notify);

    RECT anchorRect = {}, trayRect = {};
    GetWindowRect(anchor, &anchorRect);
    GetWindowRect(tray, &trayRect);

    int width = cx;
    int height = cy;
    if (flags & SWP_NOSIZE) {
        RECT tip = {};
        GetWindowRect(hwnd, &tip);
        width = tip.right - tip.left;
        height = tip.bottom - tip.top;
    }

    // Which edge the taskbar is docked to decides which side the tooltip sits on
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(tray, MONITOR_DEFAULTTONEAREST), &mi);

    if ((trayRect.right - trayRect.left) > (trayRect.bottom - trayRect.top)) {
        LONG middle = mi.rcMonitor.top + (mi.rcMonitor.bottom - mi.rcMonitor.top) / 2;
        y = (trayRect.top < middle) ? anchorRect.bottom : anchorRect.top - height;
    }
    else {
        LONG middle = mi.rcMonitor.left + (mi.rcMonitor.right - mi.rcMonitor.left) / 2;
        x = (trayRect.left < middle) ? anchorRect.right : anchorRect.left - width;
    }

    // Topmost keeps a maximized window from clipping the tooltip
    return SetWindowPos_orig(hwnd, HWND_TOPMOST, x, y, cx, cy, flags & ~SWP_NOZORDER);
}

void InstallTrayTooltipFix() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    void* target = user32 ? (void*)GetProcAddress(user32, "SetWindowPos") : NULL;
    if (!target) {
        dbgprintf(L"TrayTooltipFix: SetWindowPos not found");
        return;
    }
    MH_STATUS st = MH_CreateHook(target, (LPVOID)SetWindowPos_hook,
                                 reinterpret_cast<LPVOID*>(&SetWindowPos_orig));
    dbgprintf(L"TrayTooltipFix: SetWindowPos=%p status %d", target, st);
}

//---Install------------------------------------------------

// Signatures were lifted from the databases in exports and each one was
// checked to match exactly once, see notes/flyout-frame-signatures.md
void InstallFlyoutFix() {
    HMODULE timedate = LoadLibraryW(L"timedate.cpl");
    if (timedate) {
        void* createWnd = (void*)FindPattern((uintptr_t)timedate,
            "48 8B C4 48 89 70 08 48 89 78 10 4C 89 60 18 4C 89 68 20 41 56 48 83 EC 70 41 8B F1");
        // 24H2 saves rbx and rsi instead, then zeroes two of the home slots
        if (!createWnd) createWnd = (void*)FindPattern((uintptr_t)timedate,
            "48 8B C4 48 89 58 08 48 89 70 10 4C 89 40 18 57 48 83 EC 70 41 8B D9 48 8B FA 48 8B 35 ?? ?? ?? ?? 48 83 60 E8 00 48 83 60 18 00");
        if (createWnd) {
            MH_CreateHook(createWnd, (LPVOID)IsolationAwareCreateWindowExW_hook,
                          reinterpret_cast<LPVOID*>(&IsolationAwareCreateWindowExW_orig));
        }
        dbgprintf(L"FlyoutFix: timedate IsolationAwareCreateWindowExW=%p", createWnd);

        void* clockProc = (void*)FindPattern((uintptr_t)timedate,
            "48 89 6C 24 08 48 89 74 24 10 48 89 7C 24 18 41 54 48 83 EC 20 44 8B E2 BA EB FF FF FF");
        // 24H2 keeps the message in ebp and the lparam in rsi
        if (!clockProc) clockProc = (void*)FindPattern((uintptr_t)timedate,
            "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 8B EA 49 8B F1 BA EB FF FF FF");
        if (clockProc) {
            MH_CreateHook(clockProc, (LPVOID)CTrayClock_s_WndProc_hook,
                          reinterpret_cast<LPVOID*>(&g_clockWndProc_orig));
        }
        dbgprintf(L"FlyoutFix: timedate CTrayClock::s_WndProc=%p", clockProc);
    }

    HMODULE stobject = LoadLibraryW(L"stobject.dll");
    if (stobject) {
        void* updateUI = (void*)FindPattern((uintptr_t)stobject,
            "48 89 5C 24 20 55 56 57 48 8B EC 48 83 EC 30 33 D2 8D 4A 13 FF 15 ?? ?? ?? ?? 33 DB");
        // 24H2 pushes rbx instead of saving it and pads the import call with a nop
        if (!updateUI) updateUI = (void*)FindPattern((uintptr_t)stobject,
            "40 55 53 57 48 8B EC 48 83 EC 30 33 D2 8D 4A 13 48 FF 15 ?? ?? ?? ?? 0F 1F 44 00 00 8B D8 85 C0");
        if (updateUI) {
            MH_CreateHook(updateUI, (LPVOID)UpdateFlyoutUI_hook,
                          reinterpret_cast<LPVOID*>(&UpdateFlyoutUI_orig));
        }
        dbgprintf(L"FlyoutFix: stobject UpdateFlyoutUI=%p", updateUI);
    }

    // 24H2 ships neither VAN.dll nor pnidui.dll, so the network flyout has nothing to frame there
    HMODULE van = LoadLibraryW(L"VAN.dll");
    if (!van) dbgprintf(L"FlyoutFix: VAN.dll is not on this build, network flyout left alone");
    if (van) {
        void* positionVan = (void*)FindPattern((uintptr_t)van,
            "48 8B C4 53 55 56 57 41 54 48 81 EC 80 00 00 00 33 DB 45 8B E1 49 8B F8 48 8B E9");
        if (positionVan) {
            MH_CreateHook(positionVan, (LPVOID)CListUiBase_PositionVanUI_hook,
                          reinterpret_cast<LPVOID*>(&CListUiBase_PositionVanUI_orig));
        }
        dbgprintf(L"FlyoutFix: VAN CListUiBase::PositionVanUI=%p", positionVan);

        void* topSubclass = (void*)FindPattern((uintptr_t)van,
            "40 53 55 57 41 55 41 56 41 57 48 83 EC 78 48 8B 05 ?? ?? ?? ?? 48 33 C4");
        if (topSubclass) {
            MH_CreateHook(topSubclass, (LPVOID)CVanUI_TopWindowSubclassProc_hook,
                          reinterpret_cast<LPVOID*>(&CVanUI_TopWindowSubclassProc_orig));
        }
        dbgprintf(L"FlyoutFix: VAN CVanUI::TopWindowSubclassProc=%p", topSubclass);
    }

    // ActionCenter reaches CreateWindowInBand directly, it has no
    // IsolationAwareCreateWindowExW, but load it so the module is present
    HMODULE actionCenter = LoadLibraryW(L"ActionCenter.dll");
    dbgprintf(L"FlyoutFix: ActionCenter.dll=%p", actionCenter);

    // ChangeMinhookImports only hooks CreateWindowInBand with UWP turned on
    // With it off nothing claims that target, so the flyouts need it here
    if (s_EnableImmersiveShellStack != 1) {
        LPVOID band = (LPVOID)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                             "CreateWindowInBand");
        MH_STATUS st = MH_UNKNOWN;
        if (band) {
            st = MH_CreateHook(band, (LPVOID)CreateWindowInBandFlyoutOnly,
                               reinterpret_cast<LPVOID*>(&CreateWindowInBandFlyoutOrig));
        }
        dbgprintf(L"FlyoutFix: standalone band hook target=%p status=%d", band, (int)st);
    }
}
