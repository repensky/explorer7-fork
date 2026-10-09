#pragma once
#pragma warning(disable:4302)
#pragma warning(disable:4311)
#pragma warning(disable:4312)

#include "common.h"
#include "dbgprint.h"
#include "FlyoutFix.h"
#include "OptionConfig.h"
#include "OSVersion.h"
#include "TypeDefinitions.h"
#include "ThemeManager.h"
#include "RegistryManager.h"
#include "SignInFrameFix.h"
#include "SndVolFlyoutFix.h"
#include "StoreContentFix.h"

// Ittr: Code that doesn't relate to specific hooks resides here
// e.g. helper functions, HWND retrieval functions, error messages, non-descript registry changes

BOOL WINAPI RetTrue()
{
	return TRUE;
}

BOOL FileExists(LPCTSTR szPath)
{
	DWORD dwAttrib = GetFileAttributes(szPath);

	return (dwAttrib != INVALID_FILE_ATTRIBUTES &&
		!(dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
}

HMODULE GetCurrentModuleHandle() //use for internal resource calls... honestly i just wanted to show it could be done 
{
	HMODULE hMod = NULL;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&GetCurrentModuleHandle), &hMod);
	return hMod;
}

//---Theme state snapshot-----------------------------------
// Each of the four OS answers below is a win32k call, and the checks run per window message

#define THEME_SNAP_APPTHEMED    0x01
#define THEME_SNAP_THEMEACTIVE  0x02
#define THEME_SNAP_HIGHCONTRAST 0x04
#define THEME_SNAP_COMPOSITION  0x08
#define THEME_SNAP_VALID        0x80

// Bumped by anything that sees a theme, contrast or composition change
static volatile LONG g_themeSnapSerial = 0;

// Flags in the low byte, the serial above them, the tick it was taken in the high half
static volatile LONG64 g_themeSnap = 0;

// Kept for a second at most, a change that slips past every listener still lands
#define THEME_SNAP_MAX_AGE_MS 1000

void InvalidateThemeStateSnapshot(void)
{
	InterlockedIncrement(&g_themeSnapSerial);
}

static UINT ThemeStateSnapshot(void)
{
	ULONG now = (ULONG)GetTickCount64();
	ULONG serial = (ULONG)g_themeSnapSerial & 0xFFFFFF;
	LONG64 snap = g_themeSnap;

	if ((snap & THEME_SNAP_VALID)
		&& (((ULONG)((ULONG64)snap >> 8)) & 0xFFFFFF) == serial
		&& now - (ULONG)((ULONG64)snap >> 32) < THEME_SNAP_MAX_AGE_MS)
	{
		return (UINT)(snap & 0xFF);
	}

	UINT flags = THEME_SNAP_VALID;
	if (IsAppThemed())
		flags |= THEME_SNAP_APPTHEMED;
	if (IsThemeActive())
		flags |= THEME_SNAP_THEMEACTIVE;
	if (IsHighContrastEnabled())
		flags |= THEME_SNAP_HIGHCONTRAST;
	if (IsCompositionActive())
		flags |= THEME_SNAP_COMPOSITION;

	// Taken against the serial read first, so a change during the reads forces a retake
	InterlockedExchange64(&g_themeSnap, ((LONG64)now << 32) | ((LONG64)serial << 8) | flags);
	return flags;
}

bool IsCompositionActiveCached(void)
{
	return (ThemeStateSnapshot() & THEME_SNAP_COMPOSITION) != 0;
}

bool IsClassicTheme(void)
{
	UINT snap = ThemeStateSnapshot();
	return !(snap & THEME_SNAP_THEMEACTIVE) || s_ClassicTheme || (snap & THEME_SNAP_HIGHCONTRAST);
}

bool IsCompositionManuallyDisabled(void)
{
	return s_DisableComposition || (ThemeStateSnapshot() & THEME_SNAP_HIGHCONTRAST);
}
bool ShouldDisableAeroPeek(void)
{
	UINT snap = ThemeStateSnapshot();
	return !(snap & THEME_SNAP_APPTHEMED) || IsClassicTheme() || !(snap & THEME_SNAP_COMPOSITION) || IsCompositionManuallyDisabled();
}

// Basic leaves real DWM running, so faking frame DWM off there flickers the frame
bool ShouldForceExplorerFrameDwmOff(void)
{
	return !(ThemeStateSnapshot() & THEME_SNAP_APPTHEMED) || IsClassicTheme();
}

bool ShouldDisableShellWindowTransparency(void)
{
	return !(ThemeStateSnapshot() & THEME_SNAP_APPTHEMED) || IsClassicTheme() || IsCompositionManuallyDisabled();
}

bool ShouldApplyShellWindowAccent(void)
{
	return !ShouldDisableShellWindowTransparency() && IsCompositionActiveCached() && s_ColorizationOptions != 0;
}

bool AllowThemes(void)
{
	return !IsClassicTheme();
}

static HWND GetTaskbarWnd()
{
	if (!hwnd_taskbar)
		hwnd_taskbar = FindWindow(L"Shell_TrayWnd", NULL);
	return hwnd_taskbar;
}

static BOOL CALLBACK FindSMCallback(HWND hwnd, LPARAM lParam)
{
	if (GetClassWord(hwnd, GCW_ATOM) == (ATOM)lParam && (GetProp(hwnd, L"StartMenuTag")))
	{
		hwnd_startmenu = hwnd;
		return FALSE;
	}
	return TRUE;
}

static HWND GetStartMenuWnd()
{
	if (!hwnd_startmenu || !IsWindow(hwnd_startmenu))
	{
		WNDCLASS dummy = { 0 };
		ATOM dv2atom = GetClassInfo(GetModuleHandle(NULL), L"DV2ControlHost", &dummy);
		EnumThreadWindows(GetCurrentThreadId(), FindSMCallback, (LPARAM)dv2atom);
	}
	return hwnd_startmenu;
}

static HWND GetThumbnailWnd()
{
	if (!hwnd_taskthumb)
		hwnd_taskthumb = FindWindow(L"TaskListThumbnailWnd", NULL);
	return hwnd_taskthumb;
}
static bool IsExplorerFrameWindow(HWND hwnd)
{
	if (!hwnd)
		return false;

	HWND root = GetAncestor(hwnd, GA_ROOT);
	if (!root)
		root = hwnd;

	WCHAR className[64] = {};
	if (!GetClassNameW(root, className, ARRAYSIZE(className)))
		return false;

	return !StrCmpW(className, L"CabinetWClass") || !StrCmpW(className, L"ExploreWClass");
}

static bool ShouldTreatDwmAsDisabledForExplorerFrame(HWND hwnd)
{
	return ShouldForceExplorerFrameDwmOff() && IsExplorerFrameWindow(hwnd);
}

static void RefreshExplorerFrameNcArea(HWND hwnd);

static bool IsCoreWrapperWindow(HWND hwnd)
{
	if (!hwnd)
		return false;

	HWND taskbar = GetTaskbarWnd();
	if (taskbar && (hwnd == taskbar || IsChild(taskbar, hwnd)))
		return true;

	HWND startMenu = GetStartMenuWnd();
	if (startMenu && (hwnd == startMenu || IsChild(startMenu, hwnd)))
		return true;

	HWND thumbnail = GetThumbnailWnd();
	if (thumbnail && (hwnd == thumbnail || IsChild(thumbnail, hwnd)))
		return true;

	return IsExplorerFrameWindow(hwnd);
}

static bool IsShellDialogWindow(HWND hwnd)
{
	if (!hwnd)
		return false;

	HWND root = GetAncestor(hwnd, GA_ROOT);
	if (!root)
		root = hwnd;

	WCHAR className[64] = {};
	if (!GetClassNameW(root, className, ARRAYSIZE(className)))
		return false;

	bool isDialogClass = !StrCmpW(className, L"#32770") || !StrCmpW(className, L"Shell_Dialog") || !StrCmpW(className, L"Shell_Dim") || !StrCmpW(className, L"NotifyIconOverflowWindow");
	if (!isDialogClass)
		return false;

	DWORD processId = 0;
	GetWindowThreadProcessId(root, &processId);
	return processId == GetCurrentProcessId();
}

static const LPCWSTR CLASSIC_DIALOG_PROP = L"Explorer7ClassicDialog";
static const LPCWSTR THEME_SUBAPP_PROP = (LPCWSTR)0xA911;
static const LPCWSTR THEME_SUBID_PROP = (LPCWSTR)0xA910;
static const LPCWSTR CLASSIC_FRAME_PROP = L"Explorer7ClassicFrame";
static const LPCWSTR CLASSIC_SUBAPP_PROP = L"Explorer7ClassicSubApp";
static const LPCWSTR CLASSIC_SUBID_PROP = L"Explorer7ClassicSubId";
static const LPCWSTR EXPLORER_FRAME_PREVPROC_PROP = L"Explorer7FramePrevProc";
static LRESULT CALLBACK ExplorerFrameProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
static void SyncExplorerFrameTheme(HWND hwnd);
void ClearForcedActiveWindowAppearance(HWND hwnd);
void ForceActiveWindowAppearance(HWND hwnd);
void DisableWindowNcRendering(HWND hwnd);
void RestoreWindowNcRendering(HWND hwnd);
void NotifyWindowCompositionChanged(HWND wnd);

// Lives in ShellAccentOverride.h, which needs everything below to be declared first
bool ShellAccentOverrideActive();
void ClearShellAccentWorkaround(HWND hwnd);
void ApplyShellAccentWorkaround(HWND hwnd);

static void ApplyDialogWindowTheme(HWND hwnd, LPCWSTR pszSubApp, LPCWSTR pszSubId)
{
	LPCWSTR themeArgs[2] = { pszSubApp, pszSubId };
	SetWindowTheme(hwnd, pszSubApp, pszSubId);
	EnumChildWindows(hwnd, [](HWND child, LPARAM lParam) -> BOOL
	{
		LPCWSTR* themeArgs = reinterpret_cast<LPCWSTR*>(lParam);
		SetWindowTheme(child, themeArgs[0], themeArgs[1]);
		return TRUE;
	}, (LPARAM)themeArgs);
}

static void RefreshShellDialogVisuals(HWND hwnd)
{
	RefreshExplorerFrameNcArea(hwnd);
	RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_FRAME | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

static void SyncShellDialogTheme(HWND hwnd)
{
	if (!IsShellDialogWindow(hwnd))
		return;

	if (IsClassicTheme())
	{
		if (!GetPropW(hwnd, CLASSIC_DIALOG_PROP))
		{
			SetPropW(hwnd, CLASSIC_DIALOG_PROP, (HANDLE)1);
			ClearForcedActiveWindowAppearance(hwnd);
			ApplyDialogWindowTheme(hwnd, L"", L"");
			RefreshShellDialogVisuals(hwnd);
		}
	}
	else if (GetPropW(hwnd, CLASSIC_DIALOG_PROP))
	{
		RemovePropW(hwnd, CLASSIC_DIALOG_PROP);
		ApplyDialogWindowTheme(hwnd, NULL, NULL);
		RefreshShellDialogVisuals(hwnd);
	}
}

static void CacheExplorerThemeAtom(HWND hwnd, LPCWSTR sourceProp, LPCWSTR cacheProp)
{
	ATOM atom = (ATOM)(ULONG_PTR)GetPropW(hwnd, sourceProp);
	if (!atom || GetPropW(hwnd, cacheProp))
		return;

	WCHAR buffer[260];
	UINT copied = GetAtomNameW(atom, buffer, ARRAYSIZE(buffer));
	if (!copied)
		return;

	ATOM cachedAtom = AddAtomW(buffer);
	if (cachedAtom)
	{
		SetPropW(hwnd, cacheProp, (HANDLE)(ULONG_PTR)cachedAtom);
	}
}

static void ReleaseExplorerThemeAtom(HWND hwnd, LPCWSTR cacheProp)
{
	ATOM atom = (ATOM)(ULONG_PTR)RemovePropW(hwnd, cacheProp);
	if (atom)
	{
		DeleteAtom(atom);
	}
}

static LPCWSTR RestoreExplorerThemeString(HWND hwnd, LPCWSTR cacheProp, WCHAR(&buffer)[260])
{
	ATOM atom = (ATOM)(ULONG_PTR)GetPropW(hwnd, cacheProp);
	if (!atom)
		return NULL;

	if (!GetAtomNameW(atom, buffer, ARRAYSIZE(buffer)))
		return NULL;

	return buffer;
}

static void EnsureExplorerFrameSubclass(HWND hwnd)
{
	if (!hwnd || !IsExplorerFrameWindow(hwnd) || GetPropW(hwnd, EXPLORER_FRAME_PREVPROC_PROP))
		return;

	WNDPROC prevProc = (WNDPROC)GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
	if (!prevProc)
		return;

	SetPropW(hwnd, EXPLORER_FRAME_PREVPROC_PROP, (HANDLE)prevProc);
	SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)ExplorerFrameProc);
}

static void RestoreExplorerFrameTheme(HWND hwnd)
{
	WCHAR subApp[260];
	WCHAR subId[260];
	LPCWSTR pszSubApp = RestoreExplorerThemeString(hwnd, CLASSIC_SUBAPP_PROP, subApp);
	LPCWSTR pszSubId = RestoreExplorerThemeString(hwnd, CLASSIC_SUBID_PROP, subId);
	SetWindowTheme(hwnd, pszSubApp, pszSubId);
}

static void ReleaseExplorerFrameState(HWND hwnd)
{
	ReleaseExplorerThemeAtom(hwnd, CLASSIC_SUBAPP_PROP);
	ReleaseExplorerThemeAtom(hwnd, CLASSIC_SUBID_PROP);

	WNDPROC prevProc = (WNDPROC)RemovePropW(hwnd, EXPLORER_FRAME_PREVPROC_PROP);
	if (prevProc)
	{
		SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)prevProc);
	}
}

static LRESULT CALLBACK ExplorerFrameProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	WNDPROC prevProc = (WNDPROC)GetPropW(hwnd, EXPLORER_FRAME_PREVPROC_PROP);
	if (!prevProc)
	{
		return DefWindowProcW(hwnd, uMsg, wParam, lParam);
	}

	// Only frames still marked classic need the restore, and it runs once
	// Without the mark it re themed on every activation, which flashed the ribbon
	if (!IsClassicTheme() && GetPropW(hwnd, CLASSIC_FRAME_PROP)
		&& (uMsg == WM_NCACTIVATE || uMsg == WM_ACTIVATE || uMsg == WM_SETFOCUS))
	{
		SyncExplorerFrameTheme(hwnd);
	}

	if (uMsg == WM_NCDESTROY)
	{
		LRESULT ret = CallWindowProcW(prevProc, hwnd, uMsg, wParam, lParam);
		ReleaseExplorerFrameState(hwnd);
		return ret;
	}

	return CallWindowProcW(prevProc, hwnd, uMsg, wParam, lParam);
}

static void RefreshExplorerFrameNcArea(HWND hwnd)
{
	SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
		SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_NOSENDCHANGING | SWP_ASYNCWINDOWPOS);
	RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_FRAME);
}

static void SyncExplorerFrameTheme(HWND hwnd)
{
	if (!IsExplorerFrameWindow(hwnd))
		return;

	if (IsClassicTheme())
	{
		if (!GetPropW(hwnd, CLASSIC_FRAME_PROP))
		{
			CacheExplorerThemeAtom(hwnd, THEME_SUBAPP_PROP, CLASSIC_SUBAPP_PROP);
			CacheExplorerThemeAtom(hwnd, THEME_SUBID_PROP, CLASSIC_SUBID_PROP);
			EnsureExplorerFrameSubclass(hwnd);
			SetPropW(hwnd, CLASSIC_FRAME_PROP, (HANDLE)1);
			ClearForcedActiveWindowAppearance(hwnd);
			SetWindowTheme(hwnd, L"", L"");
			DisableWindowNcRendering(hwnd);
			RefreshExplorerFrameNcArea(hwnd);
		}
	}
	else if (GetPropW(hwnd, CLASSIC_FRAME_PROP))
	{
		RemovePropW(hwnd, CLASSIC_FRAME_PROP);
		RestoreExplorerFrameTheme(hwnd);
		RestoreWindowNcRendering(hwnd);
		RefreshExplorerFrameNcArea(hwnd);
	}
}

static bool IsWrapperManagedWindow(HWND hwnd)
{
	if (!hwnd)
		return false;

	return IsCoreWrapperWindow(hwnd) || IsShellDialogWindow(hwnd);
}

int g_fDPIAware = 0;
int g_nScreenDpi = 0;
int g_fForcedDpi = 0;
__int64 GetScreenDpi(void)
{
	int v0; // eax
	HDC DC; // rax
	HDC v3; // rbx

	if (!g_fForcedDpi)
	{
		v0 = IsProcessDPIAware();
		if (g_fDPIAware != v0 || !g_nScreenDpi)
		{
			g_fDPIAware = v0;
			g_nScreenDpi = 96;
			DC = GetDC(0LL);
			v3 = DC;
			if (DC)
			{
				g_nScreenDpi = GetDeviceCaps(DC, 88);
				ReleaseDC(0LL, v3);
			}
		}
	}
	return (unsigned int)g_nScreenDpi;
}


// Ittr: Forcing this change fixes colorization on aero.msstyles for 1809+ on taskbar and start menu ONLY.
void EnsureWindowColorization()
{
	if (g_osVersion.BuildNumber() >= 17763)
	{
		DWORD value = 0; // initialise in memory
		DWORD colorVal = 1; // doesn't work when reduced to a single string, annoying but atleast we can use it here
		RegGetDWORD(HKEY_CURRENT_USER, sz_DesktopWindowManagerKey, L"EnableWindowColorization", &value); // output the data from attributes key...

		if (value != colorVal) // basically if the attribute value doesn't exist or is the wrong value...
		{
			RegSetDWORD(HKEY_CURRENT_USER, sz_DesktopWindowManagerKey, L"EnableWindowColorization", &colorVal); // apply folder attributes, arguably the most important part
		}
	}
}

DWORD GetColorizationColor()
{
	DWMCOLORIZATIONPARAMS colors;
	CHAR buffer[0x28];
	memset(buffer, 0, 0x28);
	DwmGetColorizationParametersOrig(&buffer);
	memcpy(&colors, (PVOID)buffer, sizeof(DWMCOLORIZATIONPARAMS));

	int a = (colors.ColorizationColor >> 24) & 0xFF;
	int r = (colors.ColorizationColor >> 16) & 0xFF;
	int g = (colors.ColorizationColor >> 8) & 0xFF;
	int b = (colors.ColorizationColor) & 0xFF;

	// Automatic colorization can report alpha as 0 on Windows 10.
	if (a == 0x00 && (r != 0x00 || g != 0x00 || b != 0x00)) // only apply if it appears that the user is trying to set an actual colour - full transparency remains possible!
	{
		a = 0xC4; // we default to this as it's used by the majority of win10/11 default colours
	}

	// Approximate default Windows 8.1 translucency if user has regular 10/11 colours used and has not manually set to 0xC4
	if (a == 0xC4)
	{
		a = 0x74;
	}

	// mode 4 (gradient non-transparent is buggy) + current thumbnail edge case 
	if (s_ColorizationOptions == 4) 
	{
		a = 0xFF;
	}

	// Windows 10 and 11 users specifically without glass tools may struggle to adjust color opacity, this optional override fixes this
	if (s_OverrideAlpha && (s_ColorizationOptions == 1 || s_ColorizationOptions == 2))
	{
		a = (s_AlphaValue) & 0xFF;
	}

	if (s_ColorizationOptions == 3)
	{
		GetThemeName = (GetThemeName_t)GetProcAddress(LoadLibrary(L"uxtheme.dll"), (LPSTR)74);
		RefreshImmersiveColorPolicyState = (RefreshImmersiveColorPolicyState_t)GetProcAddress(LoadLibrary(L"uxtheme.dll"), (LPSTR)104);
		GetIsImmersiveColorUsingHighContrast = (GetIsImmersiveColorUsingHighContrast_t)GetProcAddress(LoadLibrary(L"uxtheme.dll"), (LPSTR)106);
		GetUserColorPreference = (GetUserColorPreference_t)GetProcAddress(LoadLibrary(L"uxtheme.dll"), (LPSTR)120);
		GetColorFromPreference = (GetColorFromPreference_t)GetProcAddress(LoadLibrary(L"uxtheme.dll"), (LPSTR)121);
	}

	IMMERSIVE_COLOR_TYPE imclr;

	switch (s_AcrylicAlt)
	{
		case 1:
			imclr = IMCLR_SystemAccentDark2;
			break;
		case 2:
			imclr = IMCLR_SystemAccentLight2;
			break;
		default:
			imclr = IMCLR_HardwareGutterRest;
			break;
	}

	DWORD color = (s_ColorizationOptions != 3 || s_AcrylicAlt == 3) ? ((a << 24) | (b << 16) | (g << 8) | r) : ((s_OverrideAlpha ? ((s_AlphaValue & 0xFF) << 24) : 0xCC000000) | (CImmersiveColor::GetColor(imclr) & 0xFFFFFF));
	return color;
}

ACCENT_STATE GetAccentState(bool isThumbnail)
{
	if (s_ColorizationOptions == 3) // acrylic (1803-)
		return ACCENT_ENABLE_ACRYLICBLURBEHIND;
	else if (s_ColorizationOptions == 2) // blurbehind (1507 until 11 21h2)
		return ACCENT_ENABLE_BLURBEHIND;

	if (isThumbnail) // run this block after the other ones, to ensure that pseudo-aero mode uses opaque thumbnail. using the option definition causes extreme visual bugs for some reason.
		return ACCENT_ENABLE_GRADIENT;

	// pseudo-aero & solid-color (all versions) - the replacements for option 0 & fallback for other values entered > 4
	return ACCENT_ENABLE_TRANSPARENTGRADIENT; // we use transparentgradient for both 1 and 4, as gradient has some weird hrgn side-effects on start menu

}

// The returned pvData used to point at a stack local that died on return, so
// DWM read whatever happened to be left there. Per thread storage outlives it.
__forceinline WINDOWCOMPOSITIONATTRIBDATA GetTrayAccentProperties(bool isThumbnail)
{
	static thread_local ACCENT_POLICY accentPolicy;
	WINDOWCOMPOSITIONATTRIBDATA attrData;

	accentPolicy = {};
	accentPolicy.AccentState = GetAccentState(isThumbnail);
	accentPolicy.AccentFlags = (isThumbnail) ? (0x1 | 0x2 | 0x200) : (0x13);
	accentPolicy.GradientColor = GetColorizationColor();

	attrData.Attrib = WCA_ACCENT_POLICY;
	attrData.pvData = &accentPolicy;
	attrData.cbData = sizeof(accentPolicy);
	return attrData;
}

__forceinline WINDOWCOMPOSITIONATTRIBDATA GetDisabledTrayAccentProperties()
{
	static thread_local ACCENT_POLICY accentPolicy;
	WINDOWCOMPOSITIONATTRIBDATA attrData;

	accentPolicy = {};
	accentPolicy.AccentState = ACCENT_DISABLED;

	attrData.Attrib = WCA_ACCENT_POLICY;
	attrData.pvData = &accentPolicy;
	attrData.cbData = sizeof(accentPolicy);
	return attrData;
}

void DisableShellWindowBlur(HWND hwnd)
{
	if (!hwnd || !IsWindow(hwnd))
		return;

	DWM_BLURBEHIND blurBehind = {};
	blurBehind.dwFlags = DWM_BB_ENABLE;
	blurBehind.fEnable = FALSE;
	DwmEnableBlurBehindWindow(hwnd, &blurBehind);
}

// Diagnostics only, set HKCU Explorer\Advanced LogAccentTrace to 1 to turn on
// This runs per taskbar repaint and every input below costs a dwm or theme call
static bool AccentTraceEnabled()
{
	static int cached = -1;
	if (cached < 0)
	{
		DWORD v = 0;
		RegGetDWORD(HKEY_CURRENT_USER, c_szSubkey, L"LogAccentTrace", &v);
		cached = v ? 1 : 0;
	}
	return cached != 0;
}

// Dumps every input to the accent decision, so the two builds can be diffed
void TraceAccentState(LPCWSTR where, HWND hwnd, LPCWSTR took)
{
	if (!AccentTraceEnabled())
		return;

	dbgprintf(
		L"E7TRACE %s hwnd=%p took=%s apply=%d disTrans=%d classic=%d "
		L"themeActive=%d appThemed=%d compActive=%d compManDis=%d "
		L"sClassic=%d sDisComp=%d colorOpts=%d grad=%08X",
		where, hwnd, took,
		(int)ShouldApplyShellWindowAccent(),
		(int)ShouldDisableShellWindowTransparency(),
		(int)IsClassicTheme(),
		(int)IsThemeActive(),
		(int)IsAppThemed(),
		(int)IsCompositionActive(),
		(int)IsCompositionManuallyDisabled(),
		(int)s_ClassicTheme,
		(int)s_DisableComposition,
		(int)s_ColorizationOptions,
		(unsigned)GetColorizationColor());
}

void UpdateShellWindowAccent(HWND hwnd, bool isThumbnail)
{
	// The override drives these windows from the accent hook, never from here
	// This runs on WM_ERASEBKGND, so re-arming blur here would blank the taskbar
	if (ShellAccentOverrideActive())
	{
		TraceAccentState(L"UpdateAccent", hwnd, L"OVERRIDE");
		return;
	}

	// Dropped so the override re-arms the window if it is switched back on
	ClearShellAccentWorkaround(hwnd);

	if (ShouldApplyShellWindowAccent())
	{
		TraceAccentState(L"UpdateAccent", hwnd, L"APPLY");
		SetWindowCompositionAttribute(hwnd, &GetTrayAccentProperties(isThumbnail));

		// The accent alone leaves the tray reading inactive, which flattens it
		// dwm drops this on a restart and only an explicit write brings it back
		ForceActiveWindowAppearance(hwnd);
		return;
	}

	// Leaving early is only safe while composition is on. With it off, an
	// accent that was armed earlier still tints the window and lets the
	// wallpaper through, because the classic paint writes alpha 0.
	if (!ShouldDisableShellWindowTransparency() && IsCompositionActive())
	{
		TraceAccentState(L"UpdateAccent", hwnd, L"SKIP");
		return;
	}

	TraceAccentState(L"UpdateAccent", hwnd, L"DISABLE");
	SetWindowCompositionAttribute(hwnd, &GetDisabledTrayAccentProperties());
	ClearForcedActiveWindowAppearance(hwnd);
	DisableShellWindowBlur(hwnd);
}

void ForceActiveWindowAppearance(HWND hwnd)
{
	BOOL bForceActiveWindowAppearance = true;
	WINDOWCOMPOSITIONATTRIBDATA attrData;
	attrData.Attrib = WCA_FORCE_ACTIVEWINDOW_APPEARANCE;
	attrData.pvData = &bForceActiveWindowAppearance;
	attrData.cbData = sizeof(bForceActiveWindowAppearance);
	SetWindowCompositionAttribute(hwnd, &attrData);
}
void ClearForcedActiveWindowAppearance(HWND hwnd)
{
	BOOL bForceActiveWindowAppearance = false;
	WINDOWCOMPOSITIONATTRIBDATA attrData;
	attrData.Attrib = WCA_FORCE_ACTIVEWINDOW_APPEARANCE;
	attrData.pvData = &bForceActiveWindowAppearance;
	attrData.cbData = sizeof(bForceActiveWindowAppearance);
	SetWindowCompositionAttribute(hwnd, &attrData);
}

void DisableWindowNcRendering(HWND hwnd)
{
	if (!hwnd || !IsWindow(hwnd))
		return;

	const MARGINS margins = { 0 };
	if (DwmExtendFrameIntoClientAreaOrig)
	{
		DwmExtendFrameIntoClientAreaOrig(hwnd, &margins);
	}
	else
	{
		static auto fn = reinterpret_cast<DwmExtendFrameIntoClientAreaAPI>(GetProcAddress(GetModuleHandleW(L"dwmapi.dll"), "DwmExtendFrameIntoClientArea"));
		if (fn)
			fn(hwnd, &margins);
	}
	int bNCRenderingPolicy = DWMNCRP_DISABLED;
	WINDOWCOMPOSITIONATTRIBDATA attrData;
	attrData.Attrib = WCA_NCRENDERING_POLICY;
	attrData.pvData = &bNCRenderingPolicy;
	attrData.cbData = sizeof(bNCRenderingPolicy);
	SetWindowCompositionAttribute(hwnd, &attrData);
	DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY, &bNCRenderingPolicy, sizeof(bNCRenderingPolicy));
}
void RestoreWindowNcRendering(HWND hwnd)
{
	if (!hwnd || !IsWindow(hwnd))
		return;

	int bNCRenderingPolicy = DWMNCRP_USEWINDOWSTYLE;
	WINDOWCOMPOSITIONATTRIBDATA attrData;
	attrData.Attrib = WCA_NCRENDERING_POLICY;
	attrData.pvData = &bNCRenderingPolicy;
	attrData.cbData = sizeof(bNCRenderingPolicy);
	SetWindowCompositionAttribute(hwnd, &attrData);
	DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY, &bNCRenderingPolicy, sizeof(bNCRenderingPolicy));
}

void RestoreManagedWindowComposition(HWND hwnd)
{
	if (IsExplorerFrameWindow(hwnd) && GetPropW(hwnd, CLASSIC_FRAME_PROP))
	{
		SyncExplorerFrameTheme(hwnd);
	}

	RestoreWindowNcRendering(hwnd);
	if (IsExplorerFrameWindow(hwnd))
	{
		SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_NOSENDCHANGING);
		RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW);
	}
	PostMessage(hwnd, WM_DWMCOMPOSITIONCHANGED, 0, 0);
}

BOOL CALLBACK RestoreExplorerComposition(HWND wnd, LPARAM prm)
{
	UNREFERENCED_PARAMETER(prm);
	if (IsExplorerFrameWindow(wnd))
	{
		RestoreManagedWindowComposition(wnd);
	}
	return TRUE;
}

void RestoreShellWindowComposition(HWND wnd)
{
	if (!wnd || !IsWindow(wnd))
		return;

	RestoreManagedWindowComposition(wnd);
}

void RestoreShellWindowsComposition(HWND excludeWnd)
{
	HWND taskbar = GetTaskbarWnd();
	if (taskbar && taskbar != excludeWnd)
	{
		RestoreShellWindowComposition(taskbar);
	}

	HWND startMenu = GetStartMenuWnd();
	if (startMenu && startMenu != excludeWnd)
	{
		RestoreShellWindowComposition(startMenu);
	}

	HWND thumbnail = GetThumbnailWnd();
	if (thumbnail && thumbnail != excludeWnd)
	{
		RestoreShellWindowComposition(thumbnail);
	}
}

void RestoreShellDialogComposition(HWND wnd)
{
	if (!wnd || !IsWindow(wnd) || !IsShellDialogWindow(wnd))
		return;

	SyncShellDialogTheme(wnd);
	RestoreWindowNcRendering(wnd);
	RefreshShellDialogVisuals(wnd);
	NotifyWindowCompositionChanged(wnd);
}

BOOL CALLBACK RestoreShellDialogWindowsComposition(HWND wnd, LPARAM prm)
{
	UNREFERENCED_PARAMETER(prm);
	RestoreShellDialogComposition(wnd);
	return TRUE;
}

const UINT ThemeChangeMessage = WM_USER + 69420;

void RefreshWindowTheme(HWND wnd)
{
	PostMessage(wnd, WM_THEMECHANGED, 0, 0);
	dbgprintf(L"themechanged sent to %i", wnd);
}
void RefreshWindowThemeChildren(HWND wnd)
{
	if (!wnd || !IsWindow(wnd))
		return;

	EnumChildWindows(wnd, [](HWND child, LPARAM) -> BOOL
	{
		RedrawWindow(child, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
		return TRUE;
	}, 0);

	RedrawWindow(wnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void NotifyWindowCompositionChanged(HWND wnd)
{
	PostMessage(wnd, WM_DWMCOMPOSITIONCHANGED, 0, 0);
}

void RefreshExplorerFrameTheme(HWND wnd)
{
	if (!wnd || !IsWindow(wnd) || !IsExplorerFrameWindow(wnd))
		return;

	SyncExplorerFrameTheme(wnd);
	RefreshWindowTheme(wnd);
}

BOOL CALLBACK RefreshExplorerFrameWindows(HWND wnd, LPARAM prm)
{
	UNREFERENCED_PARAMETER(prm);
	RefreshExplorerFrameTheme(wnd);
	return TRUE;
}

void RefreshShellDialogTheme(HWND wnd)
{
	if (!wnd || !IsWindow(wnd) || !IsShellDialogWindow(wnd))
		return;

	SyncShellDialogTheme(wnd);
	RefreshWindowTheme(wnd);
}

BOOL CALLBACK RefreshShellDialogWindows(HWND wnd, LPARAM prm)
{
	UNREFERENCED_PARAMETER(prm);
	RefreshShellDialogTheme(wnd);
	return TRUE;
}

BOOL CALLBACK NotifyShellDialogCompositionChanged(HWND wnd, LPARAM prm)
{
	UNREFERENCED_PARAMETER(prm);
	if (IsShellDialogWindow(wnd))
	{
		NotifyWindowCompositionChanged(wnd);
	}
	return TRUE;
}

BOOL CALLBACK NotifyExplorerCompositionChanged(HWND wnd, LPARAM prm)
{
	UNREFERENCED_PARAMETER(prm);
	if (IsExplorerFrameWindow(wnd))
	{
		NotifyWindowCompositionChanged(wnd);
	}
	return TRUE;
}

void RefreshShellWindow(HWND wnd)
{
	if (!wnd || !IsWindow(wnd))
		return;

	RefreshWindowTheme(wnd);
}

void NotifyShellCompositionChanged(HWND wnd)
{
	if (!wnd || !IsWindow(wnd))
		return;

	NotifyWindowCompositionChanged(wnd);
}

void RefreshShellWindows(HWND excludeWnd)
{
	HWND taskbar = GetTaskbarWnd();
	if (taskbar && taskbar != excludeWnd)
	{
		RefreshShellWindow(taskbar);
	}

	HWND startMenu = GetStartMenuWnd();
	if (startMenu && startMenu != excludeWnd)
	{
		RefreshShellWindow(startMenu);
	}

	HWND thumbnail = GetThumbnailWnd();
	if (thumbnail && thumbnail != excludeWnd)
	{
		RefreshShellWindow(thumbnail);
	}
}

void NotifyShellWindowsCompositionChanged(HWND excludeWnd)
{
	HWND taskbar = GetTaskbarWnd();
	if (taskbar && taskbar != excludeWnd)
	{
		NotifyShellCompositionChanged(taskbar);
	}

	HWND startMenu = GetStartMenuWnd();
	if (startMenu && startMenu != excludeWnd)
	{
		NotifyShellCompositionChanged(startMenu);
	}

	HWND thumbnail = GetThumbnailWnd();
	if (thumbnail && thumbnail != excludeWnd)
	{
		NotifyShellCompositionChanged(thumbnail);
	}
}

BOOL WINAPI GetWindowBandNew(HWND hwnd, DWORD* out);

BOOL __stdcall GetWindowBandHelper(HWND hwnd, ZBID* out)
{
	if (GetWindowBandOrig)
	{
		return GetWindowBandNew(hwnd, (DWORD*)out);
	}

	static BOOL(__stdcall * fn)(HWND, ZBID*) = nullptr;
	if (!fn)
	{
		HMODULE h = GetModuleHandleW(L"user32.dll");
		if (h)
			fn = (decltype(fn))GetProcAddress(h, "GetWindowBand");
		//FAIL_FAST_IF_NULL(fn);
		if (!fn)
			return 0;
	}
	return fn(hwnd, out);
}

BOOL IsShellManagedWindow(HWND hwnd)
{
	static IsShellManagedWindow_t fn = nullptr;
	if (!fn)
	{
		HMODULE h = GetModuleHandleW(L"user32.dll");
		if (h)
			fn = (IsShellManagedWindow_t)GetProcAddress(h, MAKEINTRESOURCEA(2574));
		//FAIL_FAST_IF_NULL(fn);
		if (!fn)
			return 0;
	}
	return fn(hwnd);
}

bool ShouldExcludeFromTaskbar(HWND hwnd)
{
	wchar_t text[256];
	text[0] = 0;

	// InternalGetWindowText reads the stored caption with no WM_GETTEXT send
	// A blocking GetWindowText here stalls the tray thread on a busy frame
	static int (WINAPI* fnInternal)(HWND, LPWSTR, int) = nullptr;
	static bool resolved = false;
	if (!resolved)
	{
		HMODULE u = GetModuleHandleW(L"user32.dll");
		if (u)
			fnInternal = (decltype(fnInternal))GetProcAddress(u, "InternalGetWindowText");
		resolved = true;
	}

	if (fnInternal)
		fnInternal(hwnd, text, 255);
	else
		GetWindowTextW(hwnd, text, 255);

	if (!StrCmpW(text, L"Microsoft Text Input Application") || !StrCmpW(text, L"Windows Shell Experience Host") || !StrCmpW(text, L"Start") || !StrCmpW(text, L"Search"))
		return true;

	return false;
}

bool IsValidDesktopZOrderBand(HWND hwnd, BOOL bCheckShellManagedWindow)
{
	bool bValid = false;

	ZBID band;
	if (GetWindowBandHelper(hwnd, &band))
	{
		bValid = s_bandInclusionData[band].bInclude;

		//if (Feature_WindowTabHost && (HWND)GetPropW(hwnd, (LPCWSTR)0xA920))
		//	bValid = true;

		if (bValid && bCheckShellManagedWindow)
			bValid = !IsShellManagedWindow(hwnd) || ShellManagedWindowHelper::ShouldTreatShellManagedWindowAsNotShellManaged(hwnd);
	}

	if (bValid)
		bValid = !ShouldExcludeFromTaskbar(hwnd);

	return bValid;
}

bool IsWindowNotDesktopOrTray(HWND hwnd)
{
	if (!IsWindow(hwnd) || !IsValidDesktopZOrderBand(hwnd, TRUE) || hwnd == hwnd_taskbar || (v_hwndDesktop && hwnd == *v_hwndDesktop))
		return false;

	return true;
}

// CDesktopHost owns this class, it hosts both the start menu and every jumplist
static bool IsDesktopHostWindow(HWND hwnd)
{
	WCHAR cls[32];
	return GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) && lstrcmpiW(cls, L"DV2ControlHost") == 0;
}

// The costly half of the visibility hook, bands, dwm, window text and themes
// The tray sweeps every window on each destroy so this is kept out of that path
static BOOL IsWindowVisibleClassify(HWND hWnd)
{
	// CDesktopHost::_OnDismiss hides the start menu only when this says it is visible
	// Taskbar filtering must not answer that, a no leaves the menu stuck open
	if (IsDesktopHostWindow(hWnd))
		return TRUE;

	if (!IsValidDesktopZOrderBand(hWnd, TRUE))
		return FALSE;

	// A failed query never writes the flag, so an uninitialised read cloaked at random
	BOOL bCloaked = FALSE;
	if (SUCCEEDED(DwmGetWindowAttribute(hWnd, DWMWA_CLOAKED, &bCloaked, sizeof(bCloaked))) && bCloaked)
		return FALSE;

	if ((ShouldForceExplorerFrameDwmOff() || GetPropW(hWnd, CLASSIC_FRAME_PROP)) && IsExplorerFrameWindow(hWnd))
	{
		SyncExplorerFrameTheme(hWnd);
	}
	else if (IsShellDialogWindow(hWnd))
	{
		SyncShellDialogTheme(hWnd);
	}

	if (IsShellFrameWindow && GhostWindowFromHungWindow)
	{
		if (IsShellFrameWindow(hWnd) && !GhostWindowFromHungWindow(hWnd))
			return TRUE;
	}

	if (IsShellManagedWindow(hWnd) && GetPropW(hWnd, L"Microsoft.Windows.ShellManagedWindowAsNormalWindow") == NULL)
		return FALSE;

	return TRUE;
}

BOOL WINAPI IsWindowVisibleNEW(HWND hWnd)
{
	// Real visibility is cheap and changes often so it always runs live
	if (!IsWindowVisible(hWnd))
		return FALSE;

	// CTaskBand enumerates every window on each destroy, which repeats this hard
	// A short lived per window answer keeps one sweep from redoing the costly half
	struct Entry { HWND hwnd; ULONGLONG tick; BOOL result; };
	static Entry cache[256];

	ULONGLONG now = GetTickCount64();
	unsigned idx = (unsigned)(((ULONG_PTR)hWnd >> 2) & 0xFF);

	if (cache[idx].hwnd == hWnd && now - cache[idx].tick < 250)
		return cache[idx].result;

	BOOL result = IsWindowVisibleClassify(hWnd);

	cache[idx].hwnd = hWnd;
	cache[idx].tick = now;
	cache[idx].result = result;
	return result;
}

__int64 ShouldAddWindowToTray(HWND hwnd)
{
	BOOL ret = IsWindowNotDesktopOrTray(hwnd) && IsWindowVisibleNEW(hwnd) && ShouldAddWindowToTrayHelper(hwnd);
	//dbgprintf(L"ShouldAddWindowToTray %i", (int)ret);
	return ret;
}

//---Control Panel host split-------------------------------
// 24H2 sends Control Panel to a separate host process launched as explorer.exe
// The swapped 7850 exe cannot serve that factory, so the launch retries forever
void NeuterExplorerHostSplit()
{
	if (g_osVersion.BuildNumber() < 22000)
		return;

	DWORD keep = 0;
	RegGetDWORD(HKEY_CURRENT_USER, c_szSubkey, L"KeepExplorerHostSplit", &keep);
	if (keep)
		return;

	HMODULE ef = LoadLibraryW(L"ExplorerFrame.dll");
	if (!ef)
		return;

	static const char* c_sepProcSites[] =
	{
		// UseSeparateProcess(IShellItem *), read by CExplorerLauncher::ShowWindow
		"4C 8B DC 49 89 5B 10 49 89 73 18 57 48 83 EC 60 48 8B 05 ?? ?? ?? ?? 48 33 C4",
		// UseSeparateProcess(PCIDLIST_ABSOLUTE), read by GetHostFromTarget
		"48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8B EC 48 83 EC 70",
	};

	for (int i = 0; i < ARRAYSIZE(c_sepProcSites); i++)
	{
		uintptr_t fn = FindPattern((uintptr_t)ef, c_sepProcSites[i]);
		if (!fn)
		{
			dbgprintf(L"explorer7: UseSeparateProcess site %d not found", i);
			continue;
		}

		// Answering no keeps the window in this process, the Windows 10 behaviour
		DWORD old = 0;
		VirtualProtect((void*)fn, 3, PAGE_EXECUTE_READWRITE, &old);
		((uint8_t*)fn)[0] = 0x33;
		((uint8_t*)fn)[1] = 0xC0;
		((uint8_t*)fn)[2] = 0xC3;
		VirtualProtect((void*)fn, 3, old, &old);

		dbgprintf(L"explorer7: UseSeparateProcess site %d answered no at %p", i, (void*)fn);
	}
}

// Create all programs shellfolder on 1607+ where it doesn't already exist
void CreateShellFolder()
{
	//addendum: using the regular HKLM location is not viable for non-administrator users so we store in HKCU, which causes it to turn up in HKEY_USERS somewhere. 
	//this shouldn't work, but it does :P
	if (g_osVersion.BuildNumber() >= 14393) // Ittr: byebye shellfolder.reg
	{
		DWORD value = 0; // initialise in memory
		DWORD attrVal = 0x28100000; // doesn't work when reduced to a single string, annoying but atleast we can use it here
		LRESULT read = RegGetDWORD(HKEY_CURRENT_USER, sz_ShellFolder3, L"Attributes", &value); // output the data from attributes key...

		// A class without its server key is as good as missing, so both are checked
		WCHAR server[MAX_PATH] = L"";
		DWORD cbServer = sizeof(server);
		LRESULT serverRead = SHRegGetValueW(HKEY_CURRENT_USER, sz_ShellFolder2, NULL, SRRF_RT_REG_SZ | SRRF_RT_REG_EXPAND_SZ | SRRF_NOEXPAND, NULL, server, &cbServer);

		if (value != attrVal || serverRead != ERROR_SUCCESS) // basically if the attribute value doesn't exist or is the wrong value...
		{
			// we create all the relevant values. issue solved for new users - program list works out of the box now
			LRESULT name = RegSetSZ(HKEY_CURRENT_USER, sz_ShellFolder, NULL, (DWORD*)L"Programs Folder and Fast Items"); // create clsid name
			LRESULT path = RegSetExpandSZ(HKEY_CURRENT_USER, sz_ShellFolder2, NULL, (DWORD*)L"%SystemRoot%\\system32\\shell32.dll"); // point it to shell32
			LRESULT model = RegSetSZ(HKEY_CURRENT_USER, sz_ShellFolder2, L"ThreadingModel", (DWORD*)L"Apartment"); // regular threading model criteria...
			LRESULT attrs = RegSetDWORD(HKEY_CURRENT_USER, sz_ShellFolder3, L"Attributes", &attrVal); // apply folder attributes, arguably the most important part
			dbgprintf(L"explorer7: Programs folder class written, read %d %d, writes %d %d %d %d", (int)read, (int)serverRead, (int)name, (int)path, (int)model, (int)attrs);
		}
	}
}


// Warn and exit on unsupported OS builds
// 26100 is checked against its binaries in notes/24h2-support.md, 26200 shares them
void UnsupportedBuildWarningAndExit()
{
	ULONG build = g_osVersion.BuildNumber();
	if (build < 9999 || build > 26200)
	{
		MessageBoxW(NULL, L"This build of Windows is not supported.", L"explorer7", MB_ICONEXCLAMATION);
		ExitProcess(0);
	}
}

// One-off warning for pre-release version
void FirstRunPrereleaseWarning()
{
#ifdef PRERELEASE_COPY // do nothing if this isn't defined
	DWORD value = 0;
	RegGetDWORD(HKEY_CURRENT_USER, c_szSubkey, L"FirstRunPrereleaseCheck", &value);
	if (value != 1)
	{
		MessageBoxW(NULL, L"Evaluation copy.\nFor testing purposes only.", L"explorer7", MB_ICONEXCLAMATION);
		DWORD newValue = 1;
		RegSetDWORD(HKEY_CURRENT_USER, c_szSubkey, L"FirstRunPrereleaseCheck", &newValue);
	}
#endif
}

// Ittr: The following 3 functions are here rather than any specific imports header because they are used by 2 different patch types
HWND WINAPI CreateWindowInBandNew(DWORD dwExStyle,
	LPCWSTR lpClassName,
	LPCWSTR lpWindowName,
	DWORD dwStyle,
	int x,
	int y,
	int nWidth,
	int nHeight,
	HWND hwndParent,
	HMENU hMenu,
	HINSTANCE hInstance,
	LPVOID lpParam,
	DWORD dwBand)
{
	if (s_EnableImmersiveShellStack == 1) // immersive enabled
	{
		dwExStyle = dwExStyle | WS_EX_TOOLWINDOW; // TODO is this needed?
		HWND ret = CreateWindowExW(dwExStyle, lpClassName, lpWindowName, dwStyle, x, y, nWidth, nHeight, hwndParent, hMenu, hInstance, lpParam);

		// Ittr: Emulate always-on-top behaviour for Windows 10 toasts
		BOOL excludeFromPeek = true;
		WCHAR className[MAX_PATH];
		GetClassName(ret, className, ARRAYSIZE(className));
		if (lstrcmp(className, L"Windows.UI.Core.CoreWindow") == 0 || lstrcmp(className, L"Shell_Dialog") == 0 || lstrcmp(className, L"Shell_Dim") == 0 || lstrcmp(className, L"NotifyIconOverflowWindow") == 0)
		{
			SetWindowPos(ret, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOREPOSITION);
		}

		// We do this to eliminate the ghost window
		BOOL shouldCloak = true;
		WCHAR titleBuffer[MAX_PATH];
		GetClassName(ret, titleBuffer, ARRAYSIZE(titleBuffer));
		if (lstrcmp(titleBuffer, L"ApplicationFrameWindow") == 0)
		{
			DwmSetWindowAttribute(ret, DWMWA_CLOAK, &shouldCloak, sizeof(shouldCloak));
		}

		// This path drops dwTypeFlags, so it matters who reaches it and from where
		if (ret && lpClassName && !IS_INTRESOURCE(lpClassName) &&
			lstrcmp(lpClassName, L"ApplicationFrameWindow") == 0)
		{
			void* bandCaller = _ReturnAddress();
			HMODULE bandMod = nullptr;
			WCHAR bandName[MAX_PATH] = {};

			if (GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)bandCaller, &bandMod))
			{
				GetModuleFileName(bandMod, bandName, ARRAYSIZE(bandName));
			}

			dbgprintf(L"explorer7: band frame %p came from %s rva 0x%IX, plain orig %p",
				ret, bandName[0] ? bandName : L"unknown",
				bandMod ? (ULONG_PTR)bandCaller - (ULONG_PTR)bandMod : 0,
				CreateWindowInBandOrig);
		}

		if (ret)
		{
			SetProp(ret, L"UIA_WindowVisibilityOverriden", (HANDLE)2);
			SetProp(ret, L"explorer7.WindowBand", (HANDLE)dwBand);
		}

		// Battery and Action Center flyouts get the glass frame here
		FlyoutFixOnBandWindow(ret, lpClassName, L"Band");
		return ret;
	}
	else // Preserve legacy codepath for Windows 8.1 and non-immersive users
	{
		dwStyle = dwStyle | WS_EX_TOOLWINDOW;
		HWND ret = CreateWindowInBandOrig(dwExStyle, (LPWSTR)lpClassName, (PVOID)lpWindowName, (PVOID)dwStyle, (PVOID)x, (PVOID)y, (PVOID)nWidth, (PVOID)nHeight, hwndParent, hMenu, hInstance, lpParam, dwBand & 1);
		SetProp(ret, L"explorer7.WindowBand", (HANDLE)dwBand);
		FlyoutFixOnBandWindow(ret, lpClassName, L"BandLegacy");
		return ret;
	}
}

HWND WINAPI CreateWindowInBandExNew(DWORD exStyle, LPWSTR szClassName, PVOID p3, PVOID p4, PVOID p5, PVOID p6, PVOID p7, PVOID p8, PVOID p9, PVOID p10, PVOID p11, PVOID p12, DWORD p13, DWORD dwTypeFlags)
{
	// Native registration is out of reach for this process, see notes/modern-app-window.md
	// The flag stays because the sign in frame still needs its own logging
	BOOL wantsNative = szClassName && !IS_INTRESOURCE(szClassName) &&
		lstrcmp(szClassName, L"ApplicationFrameWindow") == 0;

	DWORD exStyleIn = exStyle;
	exStyle = exStyle | WS_EX_TOOLWINDOW;

	// A sign in frame is created with no owner, and StartModal then refuses it
	// It inherits the shell managed bit from an owner, so lend it one for the call
	BOOL borrowedOwner = FALSE;
	if (!p9 && szClassName && !IS_INTRESOURCE(szClassName) &&
		lstrcmp(szClassName, L"ApplicationFrameWindow") == 0)
	{
		HWND owner = SignInFindOwner();
		if (owner)
		{
			p9 = (PVOID)owner;
			borrowedOwner = TRUE;
			dbgprintf(L"explorer7: lending the sign in frame owner %p", owner);
		}
	}

	BOOL isAppFrame = wantsNative;

	HWND ret = CreateWindowInBandExOrig(exStyle, szClassName, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13 & 1, dwTypeFlags);

	// Type flag bits 0 and 1 need an imrsiv section in the process image
	// An unpatched 7850 explorer has none, so keep the old path for those boxes
	if (!ret)
	{
		DWORD exErr = GetLastError();

		ret = CreateWindowInBandNew(exStyle, szClassName, (LPCWSTR)p3, (DWORD)(DWORD_PTR)p4,
			(int)(DWORD_PTR)p5, (int)(DWORD_PTR)p6, (int)(DWORD_PTR)p7, (int)(DWORD_PTR)p8,
			(HWND)p9, (HMENU)p10, (HINSTANCE)p11, p12, p13 & 1);

		dbgprintf(L"explorer7: ex export refused with %lu, plain path gave %p", exErr, ret);
	}

	if (isAppFrame)
	{
		// hInstance names the module owning the class, which need not be the caller
		void* caller = _ReturnAddress();
		HMODULE callerMod = nullptr;
		WCHAR callerName[MAX_PATH] = {};

		if (GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)caller, &callerMod))
		{
			GetModuleFileName(callerMod, callerName, ARRAYSIZE(callerName));
		}

		dbgprintf(L"explorer7: frame exStyle in 0x%08X asked 0x%08X typeFlags 0x%08X got %p err %lu thread %lu",
			exStyleIn, exStyle, dwTypeFlags, ret, GetLastError(), GetCurrentThreadId());

		dbgprintf(L"explorer7: frame made by %s rva 0x%IX",
			callerName[0] ? callerName : L"unknown",
			callerMod ? (ULONG_PTR)caller - (ULONG_PTR)callerMod : 0);
	}

	// Windows 10 leaves this frame ownerless and lets StartModal set one
	if (ret && borrowedOwner)
	{
		SetWindowLongPtr(ret, GWLP_HWNDPARENT, 0);

		// This thread owns the new window, which is the only place subclassing works
		SignInAttachFrame(ret);
	}

	// Ittr: Emulate always-on-top behaviour for Windows 10 toasts
	BOOL excludeFromPeek = true;
	WCHAR className[MAX_PATH];
	GetClassName(ret, className, ARRAYSIZE(className));
	if (lstrcmp(className, L"Windows.UI.Core.CoreWindow") == 0 || lstrcmp(className, L"Shell_Dialog") == 0 || lstrcmp(className, L"Shell_Dim") == 0 || lstrcmp(className, L"NotifyIconOverflowWindow") == 0)
	{
		SetWindowPos(ret, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOREPOSITION);
	}

	// We do this to eliminate the ghost window
	BOOL shouldCloak = true;
	WCHAR titleBuffer[MAX_PATH];
	GetClassName(ret, titleBuffer, ARRAYSIZE(titleBuffer));
	if (lstrcmp(titleBuffer, L"ApplicationFrameWindow") == 0)
	{
		DwmSetWindowAttribute(ret, DWMWA_CLOAK, &shouldCloak, sizeof(shouldCloak));
	}

	SetProp(ret, L"UIA_WindowVisibilityOverriden", (HANDLE)2);
	SetProp(ret, L"explorer7.WindowBand", (HANDLE)p13);

	// Some builds create the flyouts through this variant rather than the plain one
	FlyoutFixOnBandWindow(ret, szClassName, L"BandEx");
	return ret;
}

BOOL WINAPI SetWindowBandNew(HWND hwnd, HWND hwndInsertAfter, DWORD flags)
{
	// Ittr: Emulate always-on-top behaviour for Windows 10 toasts
	BOOL excludeFromPeek = true;
	WCHAR className[MAX_PATH];
	GetClassName(hwnd, className, ARRAYSIZE(className));
	if (lstrcmp(className, L"Windows.UI.Core.CoreWindow") == 0 || lstrcmp(className, L"Shell_Dialog") == 0 || lstrcmp(className, L"Shell_Dim") == 0 || lstrcmp(className, L"NotifyIconOverflowWindow") == 0)
	{
		SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOREPOSITION);
	}

	SetProp(hwnd, L"explorer7.WindowBand", (HANDLE)flags);
	return TRUE;
}

BOOL WINAPI RegisterWindowHotkeyNew(HWND hwnd, int id, UINT mod, UINT vk)
{
	if (!RegisterHotKeyApiOrg(hwnd, id, mod, vk))
		dbgprintf(L"RegisterHotKey id %d mod %X vk %X failed", id, mod, vk);
	return TRUE;
}
