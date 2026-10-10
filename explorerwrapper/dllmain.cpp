#define INITGUID

#pragma warning(disable:4302)
#pragma warning(disable:4309)
#pragma warning(disable:4311)
#pragma warning(disable:4312)
#pragma warning(disable:4700) // this one in particular because it fires erroneously

#include "util.h"
#include "common.h"
#include "Explorer7Base.h"
#include "forwards.h"
#include "StartMenuResolver.h"
#include "TrayObject.h"
#include "dbgprint.h"
#include "ImmersiveShell.h"
#include "TrayNotify.h"
#include "UserTile.h"
#include "FlyoutFix.h"
#include "AuthUI.h"
#include "StartMenuPin.h"
#include "ImmersiveFactory.h"
#include "ProjectionFactory.h"
#include "OSVersion.h"
#include "PinnedList.h"
#include "DestinationList.h"
#include "resource.h"
#include "ThemeManager.h"
#include "MinHook.h"
#include "ShellTaskScheduler.h"
#include "RegistryManager.h"
#include "NscTree.h"
#include "RegTreeOptions.h"
#include "ExplorerLauncher.h"
#include "shellapi.h"
#include "AutoPlay.h"
#include "StartMenuItemFilter.h"
#include "shell32_wrappers.h"
#include "ShellURL.h"
#include "OptionConfig.h"
#include "AddressImports.h"
#include "PatternImports.h"
#include "MinhookImports.h"
#include "TypeDefinitions.h"
#include "WinXMenu.h"

static LRESULT ReloadInactiveThemeForTaskbar(HWND hwnd, WPARAM wParam, LPARAM lParam)
{
	// First, so the check below reads the live theme exactly as it did before the snapshot
	InvalidateThemeStateSnapshot();

	bool wasCompositionSuppressed = IsClassicTheme() || IsCompositionManuallyDisabled();

	RefreshThemeConfiguration();
	g_dwStartMenuThemeThreadId = 0;
	ThemeManagerInitialize();
	bool isCompositionSuppressed = IsClassicTheme() || IsCompositionManuallyDisabled();
	EnumWindows(RefreshExplorerFrameWindows, 0);
	EnumWindows(RefreshShellDialogWindows, 0);
	RefreshShellWindows(hwnd);

	if (wasCompositionSuppressed && !isCompositionSuppressed)
	{
		EnumWindows(RestoreExplorerComposition, 0);
		EnumWindows(RestoreShellDialogWindowsComposition, 0);
		RestoreShellWindowComposition(hwnd);
		RestoreShellWindowsComposition(hwnd);
	}

	if (wasCompositionSuppressed != isCompositionSuppressed)
	{
		EnumWindows(NotifyExplorerCompositionChanged, 0);
		EnumWindows(NotifyShellDialogCompositionChanged, 0);
		NotifyShellCompositionChanged(hwnd);
		NotifyShellWindowsCompositionChanged(hwnd);
	}
	LRESULT result = CallWindowProc(g_prevTrayProc, hwnd, WM_THEMECHANGED, wParam, lParam);
	RefreshWindowThemeChildren(hwnd);
	return result;
}

LRESULT CALLBACK NewTrayProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	if (uMsg == 0x56D) return 0;

	// Win+X is registered on this window, so it arrives on the tray thread
	if (uMsg == WM_HOTKEY && wParam == WINX_HOTKEY_ID)
	{
		OpenWinXMenuFromKeyboard(hwnd);
		return 0;
	}
	if (g_winXRegisterMsg && uMsg == g_winXRegisterMsg)
	{
		RegisterWinXHotkey(hwnd);
		return 0;
	}

	// High contrast arrives as a settings change, composition as its own message
	if (uMsg == WM_DWMCOMPOSITIONCHANGED || uMsg == WM_SETTINGCHANGE
		|| uMsg == WM_WININICHANGE || uMsg == WM_SYSCOLORCHANGE)
	{
		InvalidateThemeStateSnapshot();
	}
	if (uMsg == ThemeChangeMessage) //reinit thememanager on themechanged, so that inactive msstyles is updated
	{
		return ReloadInactiveThemeForTaskbar(hwnd, wParam, lParam);
	}

	if (uMsg == WM_DISPLAYCHANGE || uMsg == WM_WINDOWPOSCHANGED)
	{
		RemoveProp(hwnd, L"TaskbarMonitor");
		SetProp(hwnd, L"TaskbarMonitor", (HANDLE)MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY));
		//send displaychanged to desktop
		if (uMsg == WM_DISPLAYCHANGE) PostMessage(hwnd_desktop, 0x44B, 0, 0);
	}

	if (uMsg == 0x574) //handledelayboot
	{
		if (lParam == 3)
			return CallWindowProc(g_prevTrayProc, hwnd, 0x5B5, wParam, lParam); //fire ShellDesktopSwitch event
		if (lParam == 1)
			SetEvent(hEvent_DesktopVisible);
		return 0;
	}

	if (uMsg == WM_THEMECHANGED)
	{
		return ReloadInactiveThemeForTaskbar(hwnd, wParam, lParam);
	}

	// Which messages actually reach the tray, and which never arrive
	if (uMsg == WM_THEMECHANGED || uMsg == WM_DWMCOMPOSITIONCHANGED
		|| uMsg == WM_SETTINGCHANGE || uMsg == WM_WININICHANGE)
	{
		dbgprintf(L"E7TRACE TrayMsg hwnd=%p msg=%04X", hwnd, uMsg);
	}

	// A dwm restart drops every composition attribute this window was given
	// Nothing else re-arms them, the accent hook only sees explorer's own calls
	if (uMsg == WM_DWMCOMPOSITIONCHANGED && hwnd == GetTaskbarWnd())
	{
		LRESULT lr = CallWindowProc(g_prevTrayProc, hwnd, uMsg, wParam, lParam);

		// The applied prop makes the override one shot, so drop it first
		ClearShellAccentWorkaround(hwnd);
		if (ShellAccentOverrideActive())
			ApplyShellAccentWorkaround(hwnd);
		else
			UpdateShellWindowAccent(hwnd, false);
		return lr;
	}

	if (uMsg == WM_SETTINGCHANGE || uMsg == WM_ERASEBKGND || uMsg == WM_WININICHANGE) // Ittr: Fix taskbar colorization for non-legacy
	{
		if (hwnd == GetTaskbarWnd())
		{
			UpdateShellWindowAccent(hwnd, false);
		}
	}

	return CallWindowProc(g_prevTrayProc, hwnd, uMsg, wParam, lParam);
}

// Ittr: Subclass the thumbnail so we can update its colorization as needed
LRESULT CALLBACK NewThumbnailProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	// The thumbnail loses its attributes to a dwm restart the same way
	if (uMsg == WM_DWMCOMPOSITIONCHANGED && hwnd == GetThumbnailWnd())
	{
		InvalidateThemeStateSnapshot();

		LRESULT lr = CallWindowProc(g_prevThumbnailProc, hwnd, uMsg, wParam, lParam);

		ClearShellAccentWorkaround(hwnd);
		if (ShellAccentOverrideActive())
			ApplyShellAccentWorkaround(hwnd);
		else
			UpdateShellWindowAccent(hwnd, true);
		return lr;
	}

	if (uMsg == WM_SETTINGCHANGE || uMsg == WM_ERASEBKGND || uMsg == WM_WININICHANGE) // Ittr: Fix thumbnail colorization for non-legacy
	{
		if (hwnd == GetThumbnailWnd())
		{
			UpdateShellWindowAccent(hwnd, true);
		}
	}

	return CallWindowProc(g_prevThumbnailProc, hwnd, uMsg, wParam, lParam);
}

// A bare Windows key would arrive here as SC_TASKLIST if the 26100 kernel ever reported it
// The low level hook owns the key there, so the command is logged and swallowed to avoid a double toggle
LRESULT CALLBACK NewDesktopProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	if (uMsg == WM_SYSCOMMAND && (wParam & 0xFFF0) == SC_TASKLIST)
	{
		dbgprintf(L"desktop SC_TASKLIST lparam %p", (void*)lParam);
		if (g_osVersion.BuildNumber() >= 26100)
			return 0;
	}
	return CallWindowProc(g_prevDesktopProc, hwnd, uMsg, wParam, lParam);
}

// 26100 shell32 registers the desktop as the shell window while creating it, a miss means the kernel refused
static void EnsureShellWindow(HWND hwndDesktop)
{
	if (GetShellWindow())
		return;

	typedef BOOL(WINAPI* SetShellWindowExApi)(HWND, HWND);
	auto setShell = (SetShellWindowExApi)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetShellWindowEx");
	HWND view = FindWindowExW(hwndDesktop, NULL, L"SHELLDLL_DefView", NULL);
	BOOL ok = setShell ? setShell(hwndDesktop, view ? view : hwndDesktop) : FALSE;
	dbgprintf(L"SetShellWindowEx retry %p view %p ok %d error %u", hwndDesktop, view, ok, GetLastError());
}

// Gate the swap fixes to Win11 build 22000+ under the IFEO loader
// A raw file swap has no loader and behaves like the direct identity
bool IsSwapShell()
{
	static int cached = -1;
	if (cached < 0)
	{
		DWORD build = *(volatile DWORD*)0x7FFE0260;
		WCHAR self[MAX_PATH] = {};
		WCHAR shell[MAX_PATH] = {};
		GetModuleFileNameW(NULL, self, ARRAYSIZE(self));
		GetWindowsDirectoryW(shell, ARRAYSIZE(shell));
		StringCchCatW(shell, ARRAYSIZE(shell), L"\\explorer.exe");
		// C:\Windows\explorer.exe is the swap target, Classic path is direct
		bool swapTarget = (lstrcmpiW(self, shell) == 0);
		// The IFEO loader injects explorer7.dll, a raw file swap does not
		bool loaderPresent = (GetModuleHandleW(L"explorer7.dll") != NULL);
		cached = (build >= 22000 && swapTarget && loaderPresent) ? 1 : 0;
	}
	return cached != 0;
}

// A layered Shell_TrayWnd with no populated layer shows only glass, not its content
// Force it non-layered and visible so it composites normally on the swapped shell
static DWORD WINAPI TrayFixupThread(LPVOID)
{
	for (int i = 0; i < 30; ++i)
	{
		Sleep(500);
		HWND tray = FindWindowW(L"Shell_TrayWnd", NULL);
		if (!tray)
			continue;
		LONG ex = GetWindowLongW(tray, GWL_EXSTYLE);
		BOOL vis = IsWindowVisible(tray);
		if (!(ex & WS_EX_LAYERED) && vis)
			continue;
		if (ex & WS_EX_LAYERED)
			SetWindowLongW(tray, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
		if (!vis)
			ShowWindow(tray, SW_SHOW);
		SetWindowPos(tray, NULL, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
		RedrawWindow(tray, NULL, NULL,
			RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);
		dbgprintf(L"tray fixup applied, ex %08X vis %d", (unsigned)GetWindowLongW(tray, GWL_EXSTYLE), IsWindowVisible(tray));
	}
	return 0;
}

// The swapped shell cloaks UWP frames and never un-cloaks them, clear it ourselves
// Via the immersive shell view manager, GetViewForHwnd then IApplicationView SetCloak none
static DWORD WINAPI UwpUncloakThread(LPVOID)
{
	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	GUID clsidImmersiveShell = { 0xc2f03a33, 0x21f5, 0x47fa, { 0xb4,0xbb,0x15,0x63,0x62,0xa2,0xf2,0x39 } };
	GUID iidServiceProvider = { 0x6d5140c1, 0x7436, 0x11ce, { 0x80,0x34,0x00,0xaa,0x00,0x60,0x09,0xfa } };
	GUID iidViewCollection = { 0x1841c6d7, 0x4f9d, 0x42c0, { 0xaf,0x41,0x87,0x47,0x53,0x8f,0x10,0xe5 } };

	void* isp = NULL;
	if (FAILED(CoCreateInstance(clsidImmersiveShell, NULL, 0x404, iidServiceProvider, &isp)) || !isp)
	{
		dbgprintf(L"uncloak: no immersive shell");
		return 0;
	}
	typedef HRESULT(STDMETHODCALLTYPE* QueryService_t)(void*, REFGUID, REFIID, void**);
	void* viewColl = NULL;
	HRESULT qh = ((QueryService_t)(*(void***)isp)[3])(isp, iidViewCollection, iidViewCollection, &viewColl);
	dbgprintf(L"uncloak: view collection %p hr %x", viewColl, qh);
	if (!viewColl)
		return 0;

	typedef HRESULT(WINAPI* DwmGet_t)(HWND, DWORD, PVOID, DWORD);
	auto pDwmGet = (DwmGet_t)GetProcAddress(GetModuleHandleW(L"dwmapi.dll"), "DwmGetWindowAttribute");
	typedef HRESULT(STDMETHODCALLTYPE* GetViewForHwnd_t)(void*, HWND, void**);
	typedef HRESULT(STDMETHODCALLTYPE* SetCloak_t)(void*, int, int);
	typedef ULONG(STDMETHODCALLTYPE* Release_t)(void*);

	for (;;)
	{
		Sleep(1000);
		HWND w = NULL;
		while ((w = FindWindowExW(NULL, w, L"ApplicationFrameWindow", NULL)) != NULL)
		{
			DWORD ck = 0;
			if (pDwmGet) pDwmGet(w, 14, &ck, sizeof(ck)); // 14 = DWMWA_CLOAKED
			if (!(ck & 2)) // 2 = DWM_CLOAKED_SHELL
				continue;
			void* view = NULL;
			HRESULT gh = ((GetViewForHwnd_t)(*(void***)viewColl)[6])(viewColl, w, &view);
			if (SUCCEEDED(gh) && view)
			{
				void** vvt = *(void***)view;
				HRESULT sh = ((SetCloak_t)vvt[12])(view, 1, 0); // 1 = AVCT_DEFAULT, show
				ShowWindowAsync(w, SW_SHOW);
				DWORD ck2 = 0;
				if (pDwmGet) pDwmGet(w, 14, &ck2, sizeof(ck2));
				dbgprintf(L"uncloak %p getview %x setcloak %x cloak %u to %u", w, gh, sh, ck, ck2);
				((Release_t)vvt[2])(view);
			}
			else
				dbgprintf(L"uncloak %p getview failed %x", w, gh);
		}
	}
}

void ShimDesktop()
{
	static int InitOnce = FALSE;
	if (InitOnce) return;
	hwnd_desktop = FindWindow(L"Progman", L"Program Manager");
	HWND hwndTray = GetTaskbarWnd();
	HWND hwndThumbnail = GetThumbnailWnd(); // thumbnail hwnd not being present should not stop the shim
	if (!hwnd_desktop || !hwndTray) return;
	InitOnce = TRUE;
	//hook tray
	g_prevTrayProc = (WNDPROC)GetWindowLongPtr(hwndTray, GWLP_WNDPROC);
	g_prevThumbnailProc = (WNDPROC)GetWindowLongPtr(hwndThumbnail, GWLP_WNDPROC);
	SetWindowLongPtr(hwndTray, GWLP_WNDPROC, (LONG_PTR)NewTrayProc);
	SetWindowLongPtr(hwndThumbnail, GWLP_WNDPROC, (LONG_PTR)NewThumbnailProc);
	//hook desktop, the Windows key arrives here as a system command
	g_prevDesktopProc = (WNDPROC)GetWindowLongPtr(hwnd_desktop, GWLP_WNDPROC);
	SetWindowLongPtr(hwnd_desktop, GWLP_WNDPROC, (LONG_PTR)NewDesktopProc);
	dbgprintf(L"desktop %p shell window %p tray %p", hwnd_desktop, GetShellWindow(), hwndTray);
	// Set only while the power user menu is on, the subclass above registers Win+X on the tray thread
	if (g_winXRegisterMsg)
		PostMessage(hwndTray, g_winXRegisterMsg, 0, 0);
	if (IsSwapShell())
	{
		EnsureShellWindow(hwnd_desktop);
		// This process owns the shell window now so the Win11 immersive shell Start passes
		if (s_EnableImmersiveShellStack == 1)
			CreateTwinUI_UWP();
		HANDLE fixup = CreateThread(NULL, 0, TrayFixupThread, NULL, 0, NULL);
		if (fixup) CloseHandle(fixup);
		HANDLE ufd = CreateThread(NULL, 0, UwpUncloakThread, NULL, 0, NULL);
		if (ufd) CloseHandle(ufd);
	}

	// The kernel drops the bare Windows key on 26100 whatever the shell identity is
	// This self gates on the build, so the swap gate must not decide it
	StartWinKeyHook();
	//set monitor (doh!)
	SetProp(hwndTray, L"TaskbarMonitor", (HANDLE)MonitorFromWindow(hwndTray, MONITOR_DEFAULTTOPRIMARY));
	//init desktop	
	PostMessage(hwnd_desktop, 0x45C, 1, 1); //wallpaper
	PostMessage(hwnd_desktop, 0x45E, 0, 2); //wallpaper host
	PostMessage(hwnd_desktop, 0x45C, 2, 3); //wallpaper & icons
	PostMessage(hwnd_desktop, 0x45B, 0, 0); //final init
	PostMessage(hwnd_desktop, 0x40B, 0, 0); //pins
	//catch the shell windows that were already up
	ArmShellAccentOverride();
}

PVOID WINAPI SHCreateDesktopNEW(PVOID p1)
{
	PVOID ret = SHCreateDesktopOrig(p1);
	ShimDesktop();
	return ret;
}

PVOID WINAPI SHDesktopMessageLoopNEW(PVOID p1)
{
	PVOID ret = SHDesktopMessageLoop(p1);
	SHPtrParamAPI SHCloseDesktopHandle;
	SHCloseDesktopHandle = (SHPtrParamAPI)GetProcAddress(GetModuleHandle(L"shell32.dll"),(LPSTR)206);
	SHCloseDesktopHandle(p1);
	return ret;
}

DWORD WINAPI CTray__SyncThreadProc_hook(LPVOID lpParameter)
{
	if (!g_dwTrayThreadId)
	{
		g_dwTrayThreadId = GetCurrentThreadId();
		dbgprintf(L"set g_dwTrayThreadId to %u", g_dwTrayThreadId);
	}

	return CTray__SyncThreadProc_orig(lpParameter);
}

void HookTrayThread(void)
{
	CTray__SyncThreadProc_orig = (LPTHREAD_START_ROUTINE)FindPattern(
		(uintptr_t)GetModuleHandle(NULL),
		"48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 54 41 55 41 56 41 57 48 81 EC 00 03 00 00 48 8B"
	);

	// Ensure File Explorer behaves correctly where certain explorer versions are concerned
	if (!CTray__SyncThreadProc_orig)
	{
		CTray__SyncThreadProc_orig = (LPTHREAD_START_ROUTINE)FindPattern(
			(uintptr_t)GetModuleHandle(NULL),
			"48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 55 41 54 41 55 41 56 41 57 48 8D A8 ?? ?? ?? ?? 48 81 EC 00 03 00 00"
		);
	}

	// 7850, distinguished from CDesktopHost::_ReapplyRegion by its frame size
	if (!CTray__SyncThreadProc_orig)
	{
		CTray__SyncThreadProc_orig = (LPTHREAD_START_ROUTINE)FindPattern(
			(uintptr_t)GetModuleHandle(NULL),
			"48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 48 89 48 08 55 41 54 41 55 41 56 41 57 48 8D A8 58 FE FF FF 48 81 EC 80 02 00 00"
		);
	}

	if (CTray__SyncThreadProc_orig)
	{
		MH_CreateHook(
			(void*)CTray__SyncThreadProc_orig,
			(void*)CTray__SyncThreadProc_hook,
			(void**)&CTray__SyncThreadProc_orig
		);
	}
}

// Win7 shell init runs 8 bytes off the x64 stack alignment on one path
// 24H2 movaps stores fault there, realign the entry, no-op below 26100
static void* g_realShellInit = nullptr;
static BYTE* g_shellInitThunk = nullptr;

// push rbp, force rsp to 16, forward four stack args, call real, unwind
static const BYTE k_shellInitThunk[] = {
	0x55,
	0x48, 0x89, 0xE5,
	0x48, 0x83, 0xE4, 0xF0,
	0x48, 0x83, 0xEC, 0x40,
	0x48, 0x8B, 0x45, 0x30,
	0x48, 0x89, 0x44, 0x24, 0x20,
	0x48, 0x8B, 0x45, 0x38,
	0x48, 0x89, 0x44, 0x24, 0x28,
	0x48, 0x8B, 0x45, 0x40,
	0x48, 0x89, 0x44, 0x24, 0x30,
	0x48, 0x8B, 0x45, 0x48,
	0x48, 0x89, 0x44, 0x24, 0x38,
	0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,
	0x48, 0x8B, 0x00,
	0xFF, 0xD0,
	0x48, 0x89, 0xEC,
	0x5D,
	0xC3,
};

void HookShellInitRealign()
{
	// Only the swapped Win11 shell hits the movaps fault, direct and Win10 are fine
	if (!IsSwapShell()) return;

	void* target = (void*)FindPattern((uintptr_t)GetModuleHandle(NULL),
		"48 8B C4 44 89 48 20 4C 89 40 18 48 89 50 10 48 89 48 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ??");
	if (!target)
	{
		dbgprintf(L"shell init realign: pattern not found");
		return;
	}

	g_shellInitThunk = (BYTE*)VirtualAlloc(NULL, sizeof(k_shellInitThunk),
		MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!g_shellInitThunk) return;

	for (SIZE_T i = 0; i < sizeof(k_shellInitThunk); ++i)
		g_shellInitThunk[i] = k_shellInitThunk[i];
	*(void**)(g_shellInitThunk + 50) = &g_realShellInit;
	FlushInstructionCache(GetCurrentProcess(), g_shellInitThunk, sizeof(k_shellInitThunk));

	MH_CreateHook(target, (void*)g_shellInitThunk, &g_realShellInit);
	dbgprintf(L"shell init realign armed at %p, thunk %p", target, g_shellInitThunk);
}

// Win7 shell mutex deadlocks on 24H2, the tray thread waits on it while the main thread owns it
// Give the tray thread a private mutex so its acquire never blocks on the real one
typedef HANDLE(WINAPI* CreateMutexW_t)(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR);
static CreateMutexW_t g_realCreateMutexW;

HANDLE WINAPI CreateMutexW_hook(LPSECURITY_ATTRIBUTES sa, BOOL owner, LPCWSTR name)
{
	if (name && g_dwTrayThreadId && GetCurrentThreadId() == g_dwTrayThreadId
		&& lstrcmpiW(name, L"Local\\ExplorerIsShellMutex") == 0)
	{
		dbgprintf(L"tray shell mutex made private to break 24H2 deadlock");
		return g_realCreateMutexW(sa, owner, NULL);
	}
	return g_realCreateMutexW(sa, owner, name);
}

void HookShellMutex()
{
	if (!IsSwapShell()) return;
	void* p = (void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateMutexW");
	if (p)
	{
		MH_CreateHook(p, (void*)CreateMutexW_hook, (void**)&g_realCreateMutexW);
		dbgprintf(L"tray shell mutex guard armed");
	}
}

// 24H2 immersive component 8 throws and twinui fail-fasts the whole shell
// Return its own benign declined code so the create loop skips it instead
typedef HRESULT(*ImmComponentDispatch_t)(void*, unsigned int, void*, void**);
static ImmComponentDispatch_t g_realImmDispatch;

HRESULT ImmComponentDispatch_hook(void* self, unsigned int index, void* arg3, void** out)
{
	if (index == 8)
	{
		// Component 8 UwpWindowLifecycleManager hosts UWP windows, run it so UWP works
		// Registry VetoImmersiveComponent8 skips it as a safety if it ever fail-faults
		static int veto = -1;
		if (veto < 0)
		{
			DWORD v = 0;
			g_registry.QueryValue(L"VetoImmersiveComponent8", (LPBYTE)&v, sizeof(v));
			veto = v ? 1 : 0;
		}
		if (veto)
		{
			dbgprintf(L"vetoing immersive component %u", index);
			if (out) *out = NULL;
			return 0x8027FFFF;
		}
	}
	HRESULT hr = g_realImmDispatch(self, index, arg3, out);
	if (hr != 0 || index == 8)
		dbgprintf(L"immersive component %u dispatch %x", index, hr);
	return hr;
}

// Installed from CreateTwinUI_UWP once twinui.pcshell is loaded, before Start
void InstallImmersiveComponentVeto()
{
	if (!IsSwapShell()) return;
	if (g_realImmDispatch) return;
	HMODULE tw = GetModuleHandleW(L"twinui.pcshell.dll");
	if (!tw)
	{
		dbgprintf(L"immersive veto: twinui.pcshell not loaded");
		return;
	}
	void* target = (void*)FindPattern((uintptr_t)tw,
		"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 49 83 21 00 49 8B F1 83 79 20 00");
	if (!target)
	{
		dbgprintf(L"immersive veto: dispatcher pattern not found");
		return;
	}
	if (MH_CreateHook(target, (void*)ImmComponentDispatch_hook, (void**)&g_realImmDispatch) == MH_OK
		&& MH_EnableHook(target) == MH_OK)
		dbgprintf(L"immersive component veto armed at %p", target);
	else
		dbgprintf(L"immersive component veto hook failed");
}

void GetOrbDPIAndPos(LPWSTR fName)
{
	APPBARDATA abd;
	abd.cbSize = sizeof(APPBARDATA);
	SHAppBarMessage(ABM_GETTASKBARPOS, &abd);

	HDC screen = GetDC(NULL);
	double hPixelsPerInch = GetDeviceCaps(screen, LOGPIXELSX);
	double vPixelsPerInch = GetDeviceCaps(screen, LOGPIXELSY);
	ReleaseDC(NULL, screen);
	double dpi = (hPixelsPerInch + vPixelsPerInch) * 0.5;

	if (dpi >= 120)
	{
		if (dpi >= 144)
		{
			if (dpi >= 192)
			{
				if (abd.uEdge == ABE_LEFT || abd.uEdge == ABE_RIGHT) StringCchCopyW(fName, MAX_PATH, L"6808");
				else if (abd.uEdge == ABE_TOP) StringCchCopyW(fName, MAX_PATH, L"6812");
				else StringCchCopyW(fName, MAX_PATH, L"6804");
			}
			else
			{
				if (abd.uEdge == ABE_LEFT || abd.uEdge == ABE_RIGHT) StringCchCopyW(fName, MAX_PATH, L"6807");
				else if (abd.uEdge == ABE_TOP) StringCchCopyW(fName, MAX_PATH, L"6811");
				else StringCchCopyW(fName, MAX_PATH, L"6803");
			}
		}
		else
		{
			if (abd.uEdge == ABE_LEFT || abd.uEdge == ABE_RIGHT) StringCchCopyW(fName, MAX_PATH, L"6806");
			else if (abd.uEdge == ABE_TOP) StringCchCopyW(fName, MAX_PATH, L"6810");
			else StringCchCopyW(fName, MAX_PATH, L"6802");
		}
	}
	else
	{
		if (abd.uEdge == ABE_LEFT || abd.uEdge == ABE_RIGHT) StringCchCopyW(fName, MAX_PATH, L"6805");
		else if (abd.uEdge == ABE_TOP) StringCchCopyW(fName, MAX_PATH, L"6809");
		else StringCchCopyW(fName, MAX_PATH, L"6801");
	}
}

static HMODULE GetCustomOrbResourceModule(LPCWSTR szExeDir, LPCWSTR szOrbPath)
{
	static HMODULE s_hOrbModule = NULL;
	static WCHAR s_szOrbModulePath[MAX_PATH * 3] = {};

	WCHAR szResolvedOrbPath[MAX_PATH * 3] = {};
	if (PathIsRelativeW(szOrbPath))
	{
		wsprintfW(szResolvedOrbPath, L"%s\\%s", szExeDir, szOrbPath);
	}
	else
	{
		StringCchCopyW(szResolvedOrbPath, ARRAYSIZE(szResolvedOrbPath), szOrbPath);
	}

	if (FileExists(szResolvedOrbPath) == FALSE)
		return NULL;

	if (s_hOrbModule && lstrcmpiW(s_szOrbModulePath, szResolvedOrbPath) == 0)
		return s_hOrbModule;

	if (s_hOrbModule)
	{
		FreeLibrary(s_hOrbModule);
		s_hOrbModule = NULL;
	}

	s_hOrbModule = LoadLibraryExW(szResolvedOrbPath, NULL, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
	if (s_hOrbModule)
	{
		StringCchCopyW(s_szOrbModulePath, ARRAYSIZE(s_szOrbModulePath), szResolvedOrbPath);
	}
	else
	{
		s_szOrbModulePath[0] = L'\0';
	}

	return s_hOrbModule;
}

HANDLE __stdcall LoadImageW_CallHook(HINSTANCE hInst, LPCWSTR name, UINT type, int cx, int cy, UINT fuLoad)
{
	WCHAR szExeDir[MAX_PATH];
	GetExplorer7BaseDir(szExeDir, MAX_PATH);

	WCHAR szOrbDir[MAX_PATH] = {};
	LSTATUS res = g_registry.QueryValue(L"OrbDirectory", (LPBYTE)szOrbDir, sizeof(szOrbDir));
	if (ERROR_SUCCESS != res || !*szOrbDir)
	{
		StringCchCopyW(szOrbDir, ARRAYSIZE(szOrbDir), L"orbs\\aero.orb");
	}

	if (lstrcmpiW(PathFindExtensionW(szOrbDir), L".orb") == 0)
	{
		HMODULE hOrbModule = GetCustomOrbResourceModule(szExeDir, szOrbDir);
		if (hOrbModule)
		{
			HANDLE hOrbImage = LoadImageW(hOrbModule, name, type, cx, cy, fuLoad);
			if (hOrbImage)
				return hOrbImage;
		}

		return LoadImageW(hInst, name, type, cx, cy, fuLoad);
	}

	WCHAR szOrbFile[MAX_PATH];
	GetOrbDPIAndPos(szOrbFile);

	WCHAR szOrbPath[MAX_PATH * 3];
	wsprintfW(
		szOrbPath,
		L"%s\\orbs\\%s\\%s.bmp",
		szExeDir,
		szOrbDir,
		szOrbFile
	);

	if (FileExists(szOrbPath) == FALSE)
		return LoadImageW(hInst, name, type, cx, cy, fuLoad);
	else
		return LoadImageW(NULL, szOrbPath, IMAGE_BITMAP, 0, 0, fuLoad | LR_LOADFROMFILE);
}

void HookLoadImageForSizeAndFont()
{
	// 7601 builds the R8 argument with a 5 byte LEA, 7785 uses a 4 byte one
	auto callLoadImage = (uintptr_t)FindPattern((uintptr_t)GetModuleHandle(0), "FF 15 ?? ?? ?? ?? 48 89 43 ?? 48 85 C0 74 ?? 4C 8D ?? ?? ?? BA ?? ?? ?? ?? 48 8B C8 FF 15");
	if (!callLoadImage)
		callLoadImage = (uintptr_t)FindPattern((uintptr_t)GetModuleHandle(0), "FF 15 ?? ?? ?? ?? 48 89 43 ?? 48 85 C0 74 ?? 4C 8D ?? ?? BA ?? ?? ?? ?? 48 8B C8 FF 15");

	if (callLoadImage)
	{
		//write a nop
		DWORD old;
		VirtualProtect((void*)callLoadImage, 1, PAGE_EXECUTE_READWRITE, &old);
		*reinterpret_cast<char*>(callLoadImage) = 0x90;
		VirtualProtect((void*)callLoadImage, 1, old, 0);

		callLoadImage += 1;

		// write a call to our function
		DetourCall((void*)callLoadImage, LoadImageW_CallHook);
	}

	char* callDrawExtended = (char*)FindPattern((uintptr_t)GetModuleHandle(0), "48 89 5C 24 08 57 48 83 EC 30 33 DB 48 8B F9 48 39 59 40");
	if (!callDrawExtended)
		callDrawExtended = (char*)FindPattern((uintptr_t)GetModuleHandle(0), "4C 8B DC 49 89 5B 10 49 89 4B 08 57 48 83 EC 40 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24");
	if (!callDrawExtended) return;

	if (callDrawExtended)
	{
		unsigned char bytes[] = { 0xB0,0x01,0xC3 };
		ChangeImportedPattern(callDrawExtended, bytes, sizeof(bytes));
	}
}

void ModifyDesktopHwnd()
{
	// Must start at the JZ right before the CMP that reads v_hwndDesktop
	uintptr_t desktopHwnd = FindPattern((uintptr_t)GetModuleHandle(0), "74 ?? 48 3B 3D ?? ?? ?? ?? 8D 43 01 0F 45 D8");
	if (!desktopHwnd)
		desktopHwnd = FindPattern((uintptr_t)GetModuleHandle(0), "74 ?? 48 3B 3D ?? ?? ?? ?? 74 ?? 8D 56 E0");

	if (desktopHwnd)
	{
		desktopHwnd += 2;
		v_hwndDesktop = (HWND*)(desktopHwnd + 7 + *reinterpret_cast<signed int*>(desktopHwnd + 3));
	}
}

void HookShell32();
void HookAPIs() // largely a legacy function now
{

	// Change and fix core desktop components
	hEvent_DesktopVisible = CreateEvent(NULL, TRUE, FALSE, L"ShellDesktopVisibleEvent");
	SHCreateDesktopOrig = (SHCreateDesktopAPI)GetProcAddress(GetModuleHandle(L"shell32.dll"), (LPSTR)200);
	ChangeImportedAddress(GetModuleHandle(NULL), "shell32.dll", SHCreateDesktopOrig, SHCreateDesktopNEW);
	SHDesktopMessageLoop = (SHCreateDesktopAPI)GetProcAddress(GetModuleHandle(L"shell32.dll"), (LPSTR)201);
	ChangeImportedAddress(GetModuleHandle(NULL), "shell32.dll", SHDesktopMessageLoop, SHDesktopMessageLoopNEW);

	// ???
	ModifyDesktopHwnd();

	// We run the Minhook patches here
	ChangeMinhookImports();
	PatchAdvapi32();

	// Prevent theme overrides applying to file explorer *VERY IMPORTANT*
	HookTrayThread();

	// 1. shell32.dll - hack created startmenupin instance
	// 2. shell32.dll - patch delayload stuff
	StartMenuPin_PatchShell32();
	HookShell32();

	// Handle custom start orb feature
	HookLoadImageForSizeAndFont();

	// Account picture, tooltip and flyout for the tray user tile, 7850 only
	HookUserTile();

	// Win+X and the Start button right click show the power user menu when EnableWinXMenu is 1
	InstallWinXMenu();

	// Glass frame and native float gap for the legacy system flyouts
	InstallFlyoutFix();

	// Tray icon tooltips sit against the notification area again
	InstallTrayTooltipFix();

	// The volume flyout is in SndVol, so the same fix has to be carried into it
	SndVolWatchInit();

	// Keep the swapped Win7 shell init from faulting on 24H2 aligned SSE stores
	HookShellInitRealign();

	// Break the 24H2 tray-thread deadlock on the Win7 shell arbitration mutex
	HookShellMutex();

	// Enable MinHook hooks at the end
	MH_EnableHook(MH_ALL_HOOKS);
}

BOOL WINAPI GetUserObjectInformationNew(HANDLE hObj, int nIndex, PVOID pvInfo, DWORD nLength, LPDWORD lpnLengthNeeded)
{
	lstrcpy(LPWSTR(pvInfo), L"Winlogon");
	return TRUE;
}

BOOL WINAPI GetWindowBandNew(HWND hwnd, DWORD* out)
{
	BOOL ret = GetWindowBandOrig(hwnd, out);
	DWORD origband = (DWORD)GetProp(GetAncestor(hwnd, GA_ROOTOWNER), L"explorer7.WindowBand");
	//dbgprintf(L"GetWindowBand %p %p %p",hwnd,*out,origband);
	if (origband && out) *out = origband;
	return ret;
}

UINT_PTR WINAPI SetTimer_WUI(HWND hWnd, UINT_PTR nIDEvent, UINT uElapse, TIMERPROC lpTimerFunc)
{
	if (nIDEvent == 0x2252CE37)
		ShowWindow(hWnd, SW_HIDE);
	return SetTimer(hWnd, nIDEvent, uElapse, lpTimerFunc);
}

// Used even when immersive UI is not active in some cases..?
void HookImmersive()
{
	HMODULE immersiveui = LoadLibrary(L"Windows.UI.Immersive.dll");
	HMODULE hUser32 = GetModuleHandle(L"user32.dll");

	// Import tables hold the raw export, the Orig globals hold MinHook trampolines
	// Assigning raw addresses to those globals threw the trampolines away
	CreateWindowInBandAPI rawBand =
		(CreateWindowInBandAPI)GetProcAddress(hUser32, "CreateWindowInBand");

	GetWindowBandOrig = (GetWindowBandAPI)GetProcAddress(hUser32, "GetWindowBand");
	ChangeImportedAddress(immersiveui, "user32.dll", rawBand, CreateWindowInBandNew);
	ChangeImportedAddress(immersiveui, "user32.dll", GetWindowBandOrig, GetWindowBandNew);
	ChangeImportedAddress(immersiveui, "user32.dll", GetUserObjectInformation, GetUserObjectInformationNew);
	ChangeImportedAddress(immersiveui, "user32.dll", SetTimer, SetTimer_WUI);

	if (!s_EnableImmersiveShellStack) // Ittr: If user *either* has UWP disabled, or they are NOT on Windows 10, run legacy window band code
	{
		//bugbug!!!
		ChangeImportedAddress(GetModuleHandle(L"twinui.dll"), "user32.dll", rawBand, CreateWindowInBandNew);
		ChangeImportedAddress(GetModuleHandle(L"authui.dll"), "user32.dll", rawBand, CreateWindowInBandNew);
		ChangeImportedAddress(GetModuleHandle(L"shell32.dll"), "user32.dll", rawBand, CreateWindowInBandNew);

		ChangeImportedAddress(GetModuleHandle(L"twinapi.dll"), "user32.dll", rawBand, CreateWindowInBandNew);
		ChangeImportedAddress(GetModuleHandle(L"Windows.UI.dll"), "user32.dll", rawBand, CreateWindowInBandNew);
	}
}

// Basically this allows explorer to actually work on builds >9200
void PatchShunimpl()
{
	uintptr_t shunImpl = (uintptr_t)GetModuleHandle(L"shunimpl.dll");
	if (!shunImpl) return;
	char* dllmainSHUNIMPL = (char*)FindPattern(shunImpl, "48 83 EC 28 83 FA 01");

	if (dllmainSHUNIMPL)
	{
		unsigned char bytes[] = { 0xB0,0x01,0xC3 };
		ChangeImportedPattern(dllmainSHUNIMPL, bytes, sizeof(bytes));
	}
}

// Starts the Win+. panel that replaces TextInputHost
// Runs off the loader lock, skipped when it is not installed
DWORD WINAPI LaunchTextInputPanel(LPVOID)
{
	HANDLE running = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\TextInputPanel.Single");
	if (running)
	{
		CloseHandle(running);
		return 0;
	}

	WCHAR szPanel[MAX_PATH] = L"";
	if (!GetSystemDirectoryW(szPanel, MAX_PATH)) return 0;
	lstrcatW(szPanel, L"\\TextInputPanel.exe");
	if (GetFileAttributesW(szPanel) == INVALID_FILE_ATTRIBUTES) return 0;

	STARTUPINFOW si = { sizeof(si) };
	PROCESS_INFORMATION pi = { 0 };
	if (CreateProcessW(szPanel, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
	{
		dbgprintf(L"Started %s\n", szPanel);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
	}
	return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule,
	DWORD  ul_reason_for_call,
	LPVOID lpReserved)
{

	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
	{
		g_hInstance = hModule;

		// Nothing has patched code yet, so a pattern miss found from here on can be cached
		PatternCacheBeginAttach();

		// Before any MH_EnableHook, MinHook's freeze then lists this process instead of the machine
		InstallOwnThreadSnapshot();

		UnsupportedBuildWarningAndExit();

		PatchShunimpl();

		CreateShellFolder(); // Fix shell folder for 1607+
		EnsureWindowColorization(); // Correct colorization enablement setting for Windows 10
		FirstRunPrereleaseWarning(); // Warn users if this is a pre-release build that this is the case on first run ONLY

		dbgprintf(L"Dll Attach\n");

		// Ittr: Load user configuration from the registry, important that we do this first before applying API hooks
		InitializeConfiguration();

		// Ittr: Handle pattern byte replacement patches, usually for disabling or fixing features
		ChangePatternImports();

		// Ittr: Handle address import changes, usually for rewriting or modifying results from API
		ChangeAddressImports();

		g_hInstance = hModule;
		if (GetModuleHandle(L"DisplaySwitch.exe"))
		{
			dbgprintf(L"loaded into displayswitch %p %s!", GetCurrentProcessId(), GetCommandLine());
			HookImmersive();
		}
		else
		{
			HookAPIs();

			// Only the shell itself brings the panel up
			WCHAR szHost[MAX_PATH] = L"";
			GetModuleFileNameW(NULL, szHost, MAX_PATH);
			PCWSTR pszName = szHost;
			for (PCWSTR p = szHost; *p; p++)
			{
				if (*p == L'\\') pszName = p + 1;
			}
			if (lstrcmpiW(pszName, L"explorer.exe") == 0)
			{
				HANDLE hStart = CreateThread(NULL, 0, LaunchTextInputPanel, NULL, 0, NULL);
				if (hStart) CloseHandle(hStart);
			}
		}

		// Every lookup attach made goes to the cache file in one write
		PatternCacheEndAttach();
	}
	break;
	case DLL_THREAD_ATTACH:
	{
		if (!g_alttabhooked && GetModuleHandle(L"alttab.dll"))
		{
			// A local again, the global here is the trampoline the band hook calls
			CreateWindowInBandAPI rawBand =
				(CreateWindowInBandAPI)GetProcAddress(GetModuleHandle(L"user32.dll"), "CreateWindowInBand");

			ChangeImportedAddress(GetModuleHandle(L"alttab.dll"), "user32.dll", rawBand, CreateWindowInBandNew);
			g_alttabhooked = TRUE;
		}

	}
	break;
	case DLL_THREAD_DETACH:
		break;
	case DLL_PROCESS_DETACH:
		ThemeManagerUninitialize();
		break;
	}
	return TRUE;
}

extern "C" HRESULT WINAPI Explorer_CoCreateInstance(
	__in   REFCLSID rclsid,
	__in   LPUNKNOWN pUnkOuter,
	__in   DWORD dwClsContext,
	__in   REFIID riid,
	__out  LPVOID* ppv
)
{
	HRESULT result;
	result = CoCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, ppv);

	if (rclsid == CLSID_PersonalStartMenu && riid == IID_IShellItemFilter && result != S_OK) //Ittr: as far as im aware doesnt cause crashing on 1507/11. needs further checking when im awake
	{
		auto shellItemFilter = new CStartMenuItemFilter();
		result = shellItemFilter->QueryInterface(riid, ppv);
	}

	if (rclsid == CLSID_SysTray) //create Metro before tray
	{
		dbgprintf(L"create Metro before tray\n");
		HookImmersive();

		// The swapped Win11 Start needs the shell window owned first, defer Metro to ShimDesktop
		if (s_EnableImmersiveShellStack == 1 && !IsSwapShell())
			CreateTwinUI_UWP();

	}
	if (rclsid == CLSID_RegTreeOptions && riid == IID_IRegTreeOptions7) //upgrading RegTreeOptions interface
	{
		result = CoCreateInstance(rclsid, pUnkOuter, dwClsContext, IID_IRegTreeOptions8, ppv);
		*ppv = new CRegTreeOptionsWrapper((IRegTreeOptions8*)*ppv);
	}

	// A second explorer opening a folder or serving a factory, see ExplorerLauncher.h
	result = WrapExplorerLauncher(rclsid, pUnkOuter, dwClsContext, riid, ppv, result);

	if (riid == IID_IAuthUILogonSound7 && result != S_OK)
	{
		dbgprintf(L"Wrap authuilogonsound7\n");
		result = CoCreateInstance(rclsid, pUnkOuter, dwClsContext, IID_IAuthUILogonSound10, ppv);
	}

	if (rclsid == CLSID_UserAssist && result != S_OK)
	{
		if (riid == IID_IUserAssist7)
			result = CoCreateInstance(rclsid, pUnkOuter, dwClsContext, IID_IUserAssist10, ppv);
		else if (riid == IID_IUserAssist72)
			result = CoCreateInstance(rclsid, pUnkOuter, dwClsContext, IID_IUserAssist102, ppv);
		else
		{
			dbgprintf(L"Warning, unknown useraassist riid!!!!!");
			dbgprintf(L"Warning, unknown useraassist riid!!!!!");
		}
	}

	if (rclsid == CLSID_StartMenuCacheAndAppResolver && result != S_OK)
	{
		if (riid == IID_IAppResolver7)
		{
			//dbgprintf(L"Explorer_CoCreateInstance: Resolver7 using iappresolver8\n");
			PVOID rslvr8 = NULL;
			CoCreateInstance(rclsid, pUnkOuter, dwClsContext, IID_IAppResolver8, &rslvr8);
			//create our object

			CStartMenuResolver* resolver7 = new CStartMenuResolver((IAppResolver8*)rslvr8);
			result = resolver7->QueryInterface(riid, ppv);
			//if (result == S_OK)
				//dbgprintf(L"Explorer_CoCreateInstance: Resolver7 using iappresolver8 IS OK!!\n");
		}
		else if (riid == IID_IStartMenuItemsCache7)
		{
			int build = g_osVersion.BuildNumber();
			IID iid = IID_IStartMenuItemsCache8;
			if (build >= 14393)
				iid = IID_IStartMenuItemsCache10;

			void* newcache = nullptr;
			CoCreateInstance(rclsid, pUnkOuter, dwClsContext, iid, &newcache);

			CStartMenuResolver* resolver7 = nullptr;
			if (build >= 14393)
				resolver7 = new CStartMenuResolver((IStartMenuItemsCache10*)newcache);
			else
				resolver7 = new CStartMenuResolver((IStartMenuItemsCache8*)newcache);

			result = resolver7->QueryInterface(riid, ppv);
		}
	}
	if ((rclsid == CLSID_StartMenuPin || rclsid == CLSID_TaskbarPin) /* && riid == IID_IPinnedList2*/ && result != S_OK)
	{
		int build = g_osVersion.BuildNumber();
		IID id = IID_IPinnedList25;

		if (build >= 14393 && build < 17763)
		{
			id = IID_IFlexibleTaskbarPinnedList;
		}
		else if (build >= 17763)
		{
			id = IID_IPinnedList3;
		}

		result = CoCreateInstance(rclsid, pUnkOuter, dwClsContext, id, ppv);

		if (SUCCEEDED(result))
		{
			*ppv = new CPinnedListWrapper((IUnknown*)*ppv, build);
		}

	}

	if (riid == IID_AutoDestList && result != S_OK)
	{
		result = CoCreateInstance(rclsid, pUnkOuter, dwClsContext, IID_AutoDestList10, ppv);
		*ppv = new CAutoDestWrapper((IAutoDestinationList10*)*ppv);
	}
	if (riid == IID_CustomDestList && result != S_OK)
	{
		result = CoCreateInstance(rclsid, pUnkOuter, dwClsContext, IID_CustomDestList10, ppv);
		if (result != S_OK || !*ppv)
		{
			result = CoCreateInstance(rclsid, pUnkOuter, dwClsContext, IID_CustomDestList1507, ppv);
			*ppv = new CCustomDestWrapper((IInternalCustomDestList1507*)*ppv);
		}
		else
			*ppv = new CCustomDestWrapper((IInternalCustomDestList10*)*ppv);
	}
	if (riid == IID_IShellTaskScheduler7)
	{
		result = CoCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, ppv);
		*ppv = new CShellTaskSchedulerWrapper((IShellTaskScheduler7*)*ppv);
	}
	if (result == S_OK && rclsid == CLSID_SysTray) //wrap stobject
	{
		dbgprintf(L"wrap stobject\n");
		*ppv = new CSysTrayWrapper((IOleCommandTarget*)*ppv);
	}
	if (rclsid == CLSID_AuthUIShutdownChoices && result != S_OK) //wrap authui
	{
		if (*ppv)
		{
			dbgprintf(L"good\n");
			*ppv = new CAuthUIWrapper((IUnknown*)*ppv);
		}
		else
		{
			result = CoCreateInstance(rclsid, pUnkOuter, dwClsContext, IID_IShutdownChoices10, ppv);
			if (*ppv)
			{
				*ppv = new CAuthUIWrapper((IUnknown*)*ppv);
			}
		}
	}
	if (riid == IID_TrayClock7 && result != S_OK)
		result = CoCreateInstance(rclsid, pUnkOuter, dwClsContext, IID_TrayClock8, ppv);

	return result;
}

extern "C" HRESULT WINAPI Explorer_CoRegisterClassObject(
	REFCLSID rclsid,     //Class identifier (CLSID) to be registered
	IUnknown* pUnk,     //Pointer to the class object
	DWORD dwClsContext,  //Context for running executable code
	DWORD flags,         //How to connect to the class object
	LPDWORD  lpdwRegister
)
{
	if (rclsid == CLSID_TrayNotify)
	{
		pUnk = new CTrayNotifyFactory((IClassFactory*)pUnk);
		if (s_EnableImmersiveShellStack == 2) // Ittr: gate fakeimmersive to 8.1 due to functional issues (e.g. hanging) with 10 - restoring this on 10 is now seemingly unnecessary
		{
			//register immersive shell fake too
			RegisterFakeImmersive();
			//and projection
			RegisterProjection();
		}
	}

	HRESULT rslt = CoRegisterClassObject(rclsid, pUnk, dwClsContext, flags, lpdwRegister);

	if (rclsid == CLSID_TrayNotify)
		dwRegisterNotify = *lpdwRegister;

	return rslt;
}

extern "C" HRESULT WINAPI Explorer_CoRevokeClassObject(DWORD dwRegister)
{
	if (dwRegister == dwRegisterNotify)
	{
		if (s_EnableImmersiveShellStack == 2) // Ittr: gate fakeimmersive to 8.1 due to functional issues (e.g. hanging) with 10
		{
			UnregisterFakeImmersive();
			UnregisterProjection();
		}
	}
	return CoRevokeClassObject(dwRegister);
}

//---Jump list name for immersive apps----------------------

DEFINE_GUID(SID_SM_DESTLIST, 0x0851942B, 0xA1D0, 0x4E2E, 0xBE, 0x3A, 0x68, 0x5D, 0xD6, 0xA2, 0x5D, 0x1C); // 0851942b-a1d0-4e2e-be3a-685dd6a25d1c
DEFINE_GUID(GUID_39d63fd3_04c3_400d_b394_f7d99c0efe61, 0x39D63FD3, 0x04C3, 0x400D, 0xB3, 0x94, 0xF7, 0xD9, 0x9C, 0x0E, 0xFE, 0x61); // 39d63fd3-04c3-400d-b394-f7d99c0efe61

// windows 7
struct SMDESTINFO7
{
	int iImage;
	WCHAR* pszExeName;
	WCHAR* pszAppId;
	WCHAR* pszLauncherName;
	BOOL bPinned;
	BOOL bPinnable;
	BOOL bLaunchable;
	BOOL bActive;
	HWND hwndTask;
	ITEMIDLIST_ABSOLUTE* pidlApp;
	ITEMIDLIST_ABSOLUTE* pidlShortcut;
};

HRESULT GetDisplayNameFromAUMID(LPWSTR aumid, LPWSTR* out)
{
	IShellItem* ish;
	HRESULT hr = SHCreateItemInKnownFolder(FOLDERID_AppsFolder, KF_FLAG_DONT_VERIFY, aumid, IID_PPV_ARGS(&ish));
	if (SUCCEEDED(hr))
	{
		hr = ish->GetDisplayName(SIGDN_PARENTRELATIVE, out);
		ish->Release();
	}
	return hr;
}

// Immersive windows are hosted by ApplicationFrameHost, so the taskbar names them after the host
// Swap in the real name from the app id before the destination list is built
extern "C" HRESULT WINAPI IUnknown_QueryServiceExecNEW(
	IUnknown* punk,
	REFGUID guidService,
	const GUID* guid,
	DWORD cmdID,
	DWORD cmdParam,
	VARIANT* pvarargIn,
	VARIANT* pvarargOut)
{
	if (IsEqualGUID(guidService, SID_SM_DESTLIST)
		&& IsEqualGUID(*guid, GUID_39d63fd3_04c3_400d_b394_f7d99c0efe61)
		&& cmdID == 332)
	{
		if (pvarargIn)
		{
			SMDESTINFO7* info = (SMDESTINFO7*)pvarargIn->byref;
			if (IsShellManagedWindow(info->hwndTask))
			{
				GetDisplayNameFromAUMID(info->pszAppId, &info->pszLauncherName);

				// The taskbar points pidlShortcut at ApplicationFrameHost, whose default icon is the generic glyph
				// Point it at the AppsFolder item instead so the app's own icon is used
				IShellItem* ish;
				if (SUCCEEDED(SHCreateItemInKnownFolder(FOLDERID_AppsFolder, KF_FLAG_DONT_VERIFY, info->pszAppId, IID_PPV_ARGS(&ish))))
				{
					PIDLIST_ABSOLUTE pidl;
					if (SUCCEEDED(SHGetIDListFromObject(ish, &pidl)))
					{
						info->pidlShortcut = pidl;
					}
					ish->Release();
				}
			}
		}
	}

	IOleCommandTarget* poct;
	HRESULT hr = IUnknown_QueryService(punk, guidService, IID_PPV_ARGS(&poct));
	if (SUCCEEDED(hr))
	{
		hr = poct->Exec(guid, cmdID, cmdParam, pvarargIn, pvarargOut);
		poct->Release();
	}
	return hr;
}
