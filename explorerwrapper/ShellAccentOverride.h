#pragma once
#include "util.h"
#include "MinHook.h"

//---ShellAccentOverride--------------------------------------
// Baked in from the w7-shell-ui-accent-overrider windhawk mod by ALTaleX
// Gives the shell real dwm blur behind instead of a win10 accent fill

// Marks a window we already woke up, so the poke only happens once per show
static const LPCWSTR SHELL_ACCENT_APPLIED_PROP = L"Explorer7ShellAccentApplied";

typedef LRESULT(WINAPI* CallWindowProcWAPI)(WNDPROC, HWND, UINT, WPARAM, LPARAM);
static CallWindowProcWAPI CallWindowProcWOrig;

static bool IsWindowOfClass(HWND hwnd, LPCWSTR pszClass)
{
	WCHAR className[64] = {};
	if (!hwnd || !GetClassNameW(hwnd, className, ARRAYSIZE(className)))
		return false;

	return !StrCmpW(className, pszClass);
}

// The three windows the override takes over, matched by class like the mod did
static bool IsShellAccentWindow(HWND hwnd)
{
	// One lookup, this sits on every accent write in the process
	WCHAR className[64] = {};
	if (!hwnd || !GetClassNameW(hwnd, className, ARRAYSIZE(className)))
		return false;

	return !StrCmpW(className, L"Shell_TrayWnd")
		|| !StrCmpW(className, L"Shell_SecondaryTrayWnd")
		|| !StrCmpW(className, L"TaskListThumbnailWnd");
}

static bool IsThumbnailAccentWindow(HWND hwnd)
{
	return IsWindowOfClass(hwnd, L"TaskListThumbnailWnd");
}

// The start menu and every jumplist, both want the forced active look
static bool IsDV2ShellHostWindow(HWND hwnd)
{
	return IsWindowOfClass(hwnd, L"DV2ControlHost")
		&& (GetWindowLongW(hwnd, GWL_STYLE) & WS_THICKFRAME);
}

// Explorer tags only the real start menu, so this leaves jumplists out
static bool IsStartMenuHostWindow(HWND hwnd)
{
	return hwnd && GetPropW(hwnd, L"StartMenuTag") && IsDV2ShellHostWindow(hwnd);
}

bool ShellAccentOverrideActive()
{
	return s_ShellUIAccentOverride
		&& !ShouldDisableShellWindowTransparency()
		&& IsCompositionActiveCached();
}

// Wakes the window up so dwm hands it colorization, once per show
void ApplyShellAccentWorkaround(HWND hwnd)
{
	if (!hwnd || !IsWindow(hwnd) || GetPropW(hwnd, SHELL_ACCENT_APPLIED_PROP))
		return;

	ForceActiveWindowAppearance(hwnd);

	// Dropping the layered bit and putting it back is what makes dwm re-read it
	LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
	SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style & ~WS_EX_LAYERED);
	SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style | WS_EX_LAYERED);

	SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
		SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
		SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);

	SetPropW(hwnd, SHELL_ACCENT_APPLIED_PROP, (HANDLE)1);
}

void ClearShellAccentWorkaround(HWND hwnd)
{
	if (hwnd)
		RemovePropW(hwnd, SHELL_ACCENT_APPLIED_PROP);
}

// The swap itself, dwm draws the glass and the shell gets a blank accent
static BOOL ApplyShellAccentPolicy(HWND hwnd, WINDOWCOMPOSITIONATTRIBDATA* pAttrData)
{
	ACCENT_POLICY* pAccentPolicy = (ACCENT_POLICY*)pAttrData->pvData;

	DWM_BLURBEHIND blurBehind = {};
	blurBehind.dwFlags = DWM_BB_ENABLE;

	// Gradient is an opaque fill, blur behind under it would only muddy it
	if (pAccentPolicy->AccentState == ACCENT_ENABLE_GRADIENT)
	{
		blurBehind.fEnable = FALSE;
		DwmEnableBlurBehindWindow(hwnd, &blurBehind);
		return SetWindowCompositionAttribute(hwnd, pAttrData);
	}

	blurBehind.fEnable = (pAccentPolicy->AccentState != ACCENT_DISABLED);
	DwmEnableBlurBehindWindow(hwnd, &blurBehind);

	ACCENT_POLICY blankPolicy = {};
	WINDOWCOMPOSITIONATTRIBDATA blankData;
	blankData.Attrib = WCA_ACCENT_POLICY;
	blankData.pvData = &blankPolicy;
	blankData.cbData = sizeof(blankPolicy);
	return SetWindowCompositionAttribute(hwnd, &blankData);
}

// Thumbnail glass, rounded off to match the win7 flyout corners
HRESULT UpdateThumbnailAccentBlurRect(HWND hwnd, LPRECT prc)
{
	if (!ShellAccentOverrideActive() || !IsThumbnailAccentWindow(hwnd))
		return DwmpUpdateAccentBlurRect ? DwmpUpdateAccentBlurRect(hwnd, prc) : E_FAIL;

	// The caller reads this rect off hardcoded offsets, so it can come back bad
	// A null region means blur the whole window to dwm, which shows straight through
	HRGN hRgnBlur = NULL;
	if (prc && prc->right > prc->left && prc->bottom > prc->top)
		hRgnBlur = CreateRoundRectRgn(prc->left, prc->top, prc->right, prc->bottom, 4, 4);

	if (!hRgnBlur)
		return DwmpUpdateAccentBlurRect ? DwmpUpdateAccentBlurRect(hwnd, prc) : E_FAIL;

	ApplyShellAccentWorkaround(hwnd);

	ACCENT_POLICY blankPolicy = {};
	WINDOWCOMPOSITIONATTRIBDATA blankData;
	blankData.Attrib = WCA_ACCENT_POLICY;
	blankData.pvData = &blankPolicy;
	blankData.cbData = sizeof(blankPolicy);
	SetWindowCompositionAttribute(hwnd, &blankData);

	DWM_BLURBEHIND blurBehind = {};
	blurBehind.dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION;
	blurBehind.fEnable = TRUE;
	blurBehind.hRgnBlur = hRgnBlur;

	HRESULT hr = DwmEnableBlurBehindWindow(hwnd, &blurBehind);
	DeleteObject(hRgnBlur);
	return hr;
}

// The start menu has to keep its active glass for as long as it is up
LRESULT WINAPI CallWindowProcWNEW(WNDPROC lpPrevWndFunc, HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	// Every subclassed window passes here, so it hears a theme change before anything asks
	if (uMsg == WM_THEMECHANGED || uMsg == WM_DWMCOMPOSITIONCHANGED
		|| uMsg == WM_SETTINGCHANGE || uMsg == WM_SYSCOLORCHANGE)
	{
		InvalidateThemeStateSnapshot();
	}

	// Cheap message test first, this hook sees every subclassed window in explorer
	if ((uMsg != WM_WINDOWPOSCHANGING && uMsg != WM_NCACTIVATE)
		|| !ShellAccentOverrideActive()
		|| !IsDV2ShellHostWindow(hwnd))
	{
		return CallWindowProcWOrig(lpPrevWndFunc, hwnd, uMsg, wParam, lParam);
	}

	// Only the start menu takes the layered poke, it blanks a jumplist
	if (uMsg == WM_WINDOWPOSCHANGING && lParam && IsStartMenuHostWindow(hwnd))
	{
		WINDOWPOS* pos = (WINDOWPOS*)lParam;
		if (pos->flags & SWP_SHOWWINDOW)
			ApplyShellAccentWorkaround(hwnd);
		else if (pos->flags & SWP_HIDEWINDOW)
			ClearShellAccentWorkaround(hwnd);
	}

	// Both keep the active appearance, which is what carries the shadow
	if (uMsg == WM_NCACTIVATE)
		return CallWindowProcWOrig(lpPrevWndFunc, hwnd, uMsg, TRUE, lParam);

	return CallWindowProcWOrig(lpPrevWndFunc, hwnd, uMsg, wParam, lParam);
}

static HWINEVENTHOOK g_hShellAccentShowHook;

// A new jumplist misses its first activation, nothing has subclassed it yet
// This fires on the show itself, so it does not need the message to route
static void CALLBACK ShellAccentShowProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
	LONG idObject, LONG idChild, DWORD, DWORD)
{
	if (event != EVENT_OBJECT_SHOW || idObject != OBJID_WINDOW || idChild != CHILDID_SELF)
		return;

	// No frame style test here, a brand new window may not carry it yet
	if (!ShellAccentOverrideActive() || !IsWindowOfClass(hwnd, L"DV2ControlHost"))
		return;

	ForceActiveWindowAppearance(hwnd);

	// The same poke opens two onward get, posted so it lands after the show
	PostMessageW(hwnd, WM_NCACTIVATE, TRUE, 0);
}

static BOOL CALLBACK ArmStartMenuHostWindow(HWND hwnd, LPARAM)
{
	DWORD processId = 0;
	GetWindowThreadProcessId(hwnd, &processId);

	// Other processes host DV2 windows too, only ours may be poked
	if (processId == GetCurrentProcessId() && IsDV2ShellHostWindow(hwnd))
		SendMessageW(hwnd, WM_NCACTIVATE, TRUE, 0);

	return TRUE;
}

static DWORD WINAPI ShellAccentArmThread(LPVOID)
{
	// A bare sleep races the shell on slow hardware, so wait for its own signal
	if (hEvent_DesktopVisible)
		WaitForSingleObject(hEvent_DesktopVisible, 30000);

	// Settle time for the tray and thumbnail to finish their first layout
	Sleep(500);

	if (!ShellAccentOverrideActive())
		return 0;

	// The secondary taskbar is the one window the mod left unpoked here
	struct { LPCWSTR pszClass; bool needsWorkaround; } targets[] =
	{
		{ L"Shell_TrayWnd", true },
		{ L"Shell_SecondaryTrayWnd", false },
		{ L"TaskListThumbnailWnd", true },
	};

	ACCENT_POLICY accentPolicy = {};
	accentPolicy.AccentState = ACCENT_ENABLE_BLURBEHIND;

	WINDOWCOMPOSITIONATTRIBDATA attrData;
	attrData.Attrib = WCA_ACCENT_POLICY;
	attrData.pvData = &accentPolicy;
	attrData.cbData = sizeof(accentPolicy);

	for (int i = 0; i < ARRAYSIZE(targets); i++)
	{
		HWND hwnd = FindWindowW(targets[i].pszClass, NULL);
		if (!hwnd)
			continue;

		ApplyShellAccentPolicy(hwnd, &attrData);
		if (targets[i].needsWorkaround)
			ApplyShellAccentWorkaround(hwnd);
	}

	EnumWindows(ArmStartMenuHostWindow, 0);
	return 0;
}

// Arms the shell windows that were already up before we hooked anything
void ArmShellAccentOverride()
{
	if (!s_ShellUIAccentOverride)
		return;

	// Installed here rather than at attach, SetWinEventHook under loader lock is unsafe
	if (!g_hShellAccentShowHook)
	{
		g_hShellAccentShowHook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW,
			GetCurrentModuleHandle(), ShellAccentShowProc,
			GetCurrentProcessId(), 0, WINEVENT_INCONTEXT);
	}

	HANDLE hThread = CreateThread(NULL, 0, ShellAccentArmThread, NULL, 0, NULL);
	if (hThread)
		CloseHandle(hThread);
}

void SetUpShellAccentOverrideHooks()
{
	if (!s_ShellUIAccentOverride)
		return;

	MH_CreateHookApi(L"user32.dll", "CallWindowProcW", CallWindowProcWNEW,
		reinterpret_cast<LPVOID*>(&CallWindowProcWOrig));
}
