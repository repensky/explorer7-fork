#include "OptionConfig.h"
#include "ThemeManager.h"

// Ittr: Migrated all configuration here to make things clearer in dllmain

// Individual option definitions
int s_BuildRuntime;
bool s_ClassicTheme;
bool s_DisableComposition;
int s_EnableImmersiveShellStack;
bool s_UseTaskbarPinning;
bool s_ShowStoreAppsOnTaskbar;
bool s_ShowStoreAppsInStart;
int s_AcrylicAlt;
int s_ColorizationOptions;
bool s_OverrideAlpha;
DWORD s_AlphaValue;
bool s_UseDCompFlyouts;
bool s_ShellUIAccentOverride;
int s_Win7DesktopIconRows;

static bool s_ClassicThemeSetting;
static bool s_DisableCompositionSetting;

void RefreshThemeConfiguration()
{
	bool forceClassicTheme = s_ClassicThemeSetting || !IsThemeActive() || IsHighContrastEnabled();

	s_DisableComposition = s_DisableCompositionSetting || forceClassicTheme;
	s_ClassicTheme = forceClassicTheme;
	if (forceClassicTheme)
	{
		dbgprintf(L"Disabling themes...");
	}
}

// Reads the version resource out of the already loaded explorer.exe image
// Not version.lib, walking the fixed info block keeps the link line as it was
static DWORD DetectBuildRuntime()
{
	HMODULE hExplorer = GetModuleHandle(NULL);

	HRSRC hRes = FindResource(hExplorer, MAKEINTRESOURCE(VS_VERSION_INFO), RT_VERSION);
	if (hRes)
	{
		HGLOBAL hMem = LoadResource(hExplorer, hRes);
		BYTE* pData = hMem ? (BYTE*)LockResource(hMem) : nullptr;
		DWORD cbData = hMem ? SizeofResource(hExplorer, hRes) : 0;

		// The fixed block sits after the version key, find it by signature
		// rather than trusting the padding to land where we expect
		for (DWORD i = 0; pData && i + sizeof(VS_FIXEDFILEINFO) <= cbData; i += 4)
		{
			VS_FIXEDFILEINFO* pInfo = (VS_FIXEDFILEINFO*)(pData + i);
			if (pInfo->dwSignature != 0xFEEF04BD)
				continue;

			WORD wMajor = HIWORD(pInfo->dwFileVersionMS);
			WORD wMinor = LOWORD(pInfo->dwFileVersionMS);
			WORD wBuild = HIWORD(pInfo->dwFileVersionLS);
			dbgprintf(L"explorer7: host explorer is %u.%u.%u", wMajor, wMinor, wBuild);

			// 7850 is the first build with the moved members, anything
			// newer would need its own offsets so treat it the same
			if (wBuild >= 7850)
				return BUILDRUNTIME_7850;
			return BUILDRUNTIME_WIN7;
		}
	}

	dbgprintf(L"explorer7: no version resource on the host, assuming Windows 7");
	return BUILDRUNTIME_WIN7;
}

// Each setting starts from a DWORD default, then the registry may replace it
// A missing registry value leaves the default in place
void InitializeConfiguration()
{
	// Which explorer build we are patching, taken from the host exe
	// Selects the hardcoded struct offsets and the user tile patches
	DWORD dwBuildRuntime = DetectBuildRuntime();

	DWORD dwOverride = 0;
	if (g_registry.QueryValue(L"BuildRuntime", (LPBYTE)&dwOverride, sizeof(DWORD)) == ERROR_SUCCESS
		&& (dwOverride == BUILDRUNTIME_WIN7 || dwOverride == BUILDRUNTIME_7850))
	{
		if (dwOverride != dwBuildRuntime)
			dbgprintf(L"explorer7: BuildRuntime overridden to %d, detected %d",
				dwOverride, dwBuildRuntime);
		dwBuildRuntime = dwOverride;
	}

	s_BuildRuntime = dwBuildRuntime;
	dbgprintf(L"explorer7: BuildRuntime = %d", s_BuildRuntime);

	// The OS build picks the byte patterns, so it belongs next to the explorer build in the log
	dbgprintf(L"explorer7: Windows build %u.%u.%u", g_osVersion.MajorVersion(), g_osVersion.MinorVersion(), g_osVersion.BuildNumber());

	// Immersive shell stack for modern apps (e.g. PC settings)
	// - Defaults to enabled (1)
	DWORD dwEnableUWP = 1;
	g_registry.QueryValue(L"EnableImmersive", (LPBYTE)&dwEnableUWP, sizeof(DWORD));
#ifndef PRERELEASE_COPY
	if (dwEnableUWP == 2) // mode 2 is for debugging only, not release builds!
	{
		dwEnableUWP = 0; // change to fully disabled state as though 2 doesn't exist
	}
#endif
	s_EnableImmersiveShellStack = dwEnableUWP;

	// Taskbar pinning, defaults to enabled (1)
	// Off gives Vista behaviour and hides the jumplist actions that would break
	DWORD dwTaskbarPinning = 1;
	g_registry.QueryValue(L"UseTaskbarPinning", (LPBYTE)&dwTaskbarPinning, sizeof(DWORD));
	s_UseTaskbarPinning = dwTaskbarPinning;

	// Store apps on taskbar, defaults to whatever the immersive stack is set to
	// Always off when UWP is disabled
	DWORD dwStoreAppsOnTaskbar = s_EnableImmersiveShellStack;
	g_registry.QueryValue(L"StoreAppsOnTaskbar", (LPBYTE)&dwStoreAppsOnTaskbar, sizeof(DWORD));
	s_ShowStoreAppsOnTaskbar = dwStoreAppsOnTaskbar;
	
	// Modern apps in the start menu programs list, defaults to enabled (1)
	// RS1 onwards only, TH2 and earlier use the native program list
	DWORD dwStoreAppsInStart = 1;
	g_registry.QueryValue(L"StoreAppsInStart", (LPBYTE)&dwStoreAppsInStart, sizeof(DWORD));
	s_ShowStoreAppsInStart = dwStoreAppsInStart;

	// Disable composition effects (e.g. Aero glass)
	// - Defaults to disabled (0)
	DWORD dwDisableComposition = 0;
	g_registry.QueryValue(L"DisableComposition", (LPBYTE)&dwDisableComposition, sizeof(DWORD));
	s_DisableCompositionSetting = (dwDisableComposition != 0);

	// Disable themes (e.g. aero.msstyles)
	// - Defaults to disabled (0)
	DWORD dwClassicTheme = 0;
	g_registry.QueryValue(L"ClassicTheme", (LPBYTE)&dwClassicTheme, sizeof(DWORD));
	s_ClassicThemeSetting = (dwClassicTheme != 0);
	RefreshThemeConfiguration();

	// Colorization configuration
	// - Defaults to regular (0)
	DWORD dwColorizationOptions = 0;
	g_registry.QueryValue(L"ColorizationOptions", (LPBYTE)&dwColorizationOptions, sizeof(DWORD));
	if (dwColorizationOptions < 6)
	{
		// Acrylic has no Win32 api until RS4, so it falls back to translucent
		if (dwColorizationOptions == 3 && g_osVersion.BuildNumber() < 17134)
		{
			s_ColorizationOptions = 1;
		}
		else
		{
			s_ColorizationOptions = dwColorizationOptions;
		}
	}

	// Composited colorization alpha override
	// - Defaults to disabled (0)
	DWORD dwOverrideAlpha = 0;
	g_registry.QueryValue(L"OverrideAlpha", (LPBYTE)&dwOverrideAlpha, sizeof(DWORD));
	s_OverrideAlpha = dwOverrideAlpha;

	// Alpha override value, defaults to the Windows 7 value (0x6B)
	// Only used when OverrideAlpha is 1
	DWORD dwAlphaValue = 0x6B;
	g_registry.QueryValue(L"AlphaValue", (LPBYTE)&dwAlphaValue, sizeof(DWORD));
	s_AlphaValue = dwAlphaValue;
	
	// Which acrylic style to use, defaults to regular (0)
	// Only used when ColorizationOptions is 3
	DWORD dwAcrylicAlt = 0;
	g_registry.QueryValue(L"AcrylicColorization", (LPBYTE)&dwAcrylicAlt, sizeof(DWORD));
	s_AcrylicAlt = dwAcrylicAlt;

	// DComp flyouts, defaults to whatever the immersive stack is set to
	// Always off when UWP is disabled
	DWORD dwUseDCompFlyouts = s_EnableImmersiveShellStack;
	g_registry.QueryValue(L"UseDCompFlyouts", (LPBYTE)&dwUseDCompFlyouts, sizeof(DWORD));
	s_UseDCompFlyouts = dwUseDCompFlyouts;

	// Shell UI accent override, defaults to enabled (1)
	// Takes the three shell windows away from ColorizationOptions while it is on
	DWORD dwShellUIAccentOverride = 1;
	g_registry.QueryValue(L"ShellUIAccentOverride", (LPBYTE)&dwShellUIAccentOverride, sizeof(DWORD));
	s_ShellUIAccentOverride = dwShellUIAccentOverride;

	// Windows 7 desktop icon row spacing, 0 off, 1 follow the main display, 2 as Win7 did
	// Read once here, the hooks go in before the desktop exists
	DWORD dwWin7DesktopIconRows = 1;
	g_registry.QueryValue(L"Win7DesktopIconRows", (LPBYTE)&dwWin7DesktopIconRows, sizeof(DWORD));
	s_Win7DesktopIconRows = (dwWin7DesktopIconRows <= 2) ? (int)dwWin7DesktopIconRows : 1;
}
