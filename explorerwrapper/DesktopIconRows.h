#pragma once
#include "common.h"
#include "dbgprint.h"
#include "OptionConfig.h"
#include "OSVersion.h"
#include "MinHook.h"

// Desktop icon rows spaced the Windows 7 way, see notes/desktop-icon-rows.md
// Win7, 10 and 11 build the same row height, only the stretch to fill the screen differs

// Win7 shared all leftover height between the rows, 10 and 11 hold back 30 percent of a row first
static int Win7DesktopRowSpacing(int cyArea, int cyRow)
{
	if (cyRow <= 0 || cyArea <= 0)
		return cyRow;

	int cRows = cyArea / cyRow;
	if (cRows <= 0)
		cRows = 1;
	return cyRow + (cyArea % cyRow) / cRows;
}

// Every work area on 19041, several monitors on 26100
static int (*CalculateOptimalRowSpacing_Orig)(int, int) = nullptr;
static int CalculateOptimalRowSpacing_Hook(int cyArea, int cyRow)
{
	int cyStock = CalculateOptimalRowSpacing_Orig(cyArea, cyRow);
	int cyWin7 = Win7DesktopRowSpacing(cyArea, cyRow);
	dbgprintf(L"explorer7: desktop rows, area %d, row %d, stock %d, Windows 7 %d", cyArea, cyRow, cyStock, cyWin7);
	return cyWin7;
}

// One work area as shell32 hands it over, an array of these steps 0x14 on 19041 and 26100
// The rect is in desktop listview coordinates, whose origin is the virtual screen's top left
struct DesktopWorkAreaWithDpi
{
	RECT rc;
	UINT dpi;
};
static_assert(offsetof(DesktopWorkAreaWithDpi, rc) == 0x00, "work area rect moved");
static_assert(offsetof(DesktopWorkAreaWithDpi, dpi) == 0x10, "work area dpi moved");
static_assert(sizeof(DesktopWorkAreaWithDpi) == 0x14, "work area stride moved");

// The area holding a listview point, the first one when none does
static int DesktopAreaHoldingPoint(const DesktopWorkAreaWithDpi* pAreas, int cAreas, POINT pt)
{
	for (int i = 0; i < cAreas; i++)
	{
		if (PtInRect(&pAreas[i].rc, pt))
			return i;
	}
	return 0;
}

// Mode 1 follows the main display, mode 2 the area at the listview origin as Win7 did
static int DesktopRowArea(const DesktopWorkAreaWithDpi* pAreas, int cAreas)
{
	POINT pt = {};
	if (s_Win7DesktopIconRows == 1)
	{
		// The main display's centre, moved from screen to listview coordinates
		MONITORINFO mi = { sizeof(mi) };
		HMONITOR hmon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
		if (hmon && GetMonitorInfo(hmon, &mi))
		{
			pt.x = (mi.rcMonitor.left + mi.rcMonitor.right) / 2 - GetSystemMetrics(SM_XVIRTUALSCREEN);
			pt.y = (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2 - GetSystemMetrics(SM_YVIRTUALSCREEN);
		}
	}
	return DesktopAreaHoldingPoint(pAreas, cAreas, pt);
}

// Several displays on 19041 and 26100, Windows caps the row at the tightest display
// Its column search and result stay, only the row is taken from the chosen display
static HRESULT (*FindOptimalSpacing_Orig)(const DesktopWorkAreaWithDpi*, int, SIZE, SIZE*) = nullptr;
static HRESULT FindOptimalSpacing_Hook(const DesktopWorkAreaWithDpi* pAreas, int cAreas, SIZE sizeCell, SIZE* pOut)
{
	HRESULT hr = FindOptimalSpacing_Orig(pAreas, cAreas, sizeCell, pOut);
	if (FAILED(hr) || !pAreas || cAreas <= 0 || !pOut)
		return hr;

	int iArea = DesktopRowArea(pAreas, cAreas);
	const DesktopWorkAreaWithDpi* pArea = &pAreas[iArea];
	if (pArea->dpi == 0)
		return hr;

	// The same 96 dpi height the single monitor path divides into rows
	int cyArea = 96 * (pArea->rc.bottom - pArea->rc.top) / (int)pArea->dpi;
	int cyWin7 = Win7DesktopRowSpacing(cyArea, sizeCell.cy);
	dbgprintf(L"explorer7: desktop rows, %d displays, mode %d picked area %d of height %d, row %d, stock %d, Windows 7 %d",
		cAreas, s_Win7DesktopIconRows, iArea, cyArea, sizeCell.cy, pOut->cy, cyWin7);
	pOut->cy = cyWin7;
	return hr;
}

// One monitor on 26100, its column rule already matches Win7 so only the row changes
static void (*CalculateOptimalSpacingForPrimaryMonitor_Orig)(const DesktopWorkAreaWithDpi*, SIZE, SIZE*) = nullptr;
static void CalculateOptimalSpacingForPrimaryMonitor_Hook(const DesktopWorkAreaWithDpi* pArea, SIZE sizeCell, SIZE* pOut)
{
	CalculateOptimalSpacingForPrimaryMonitor_Orig(pArea, sizeCell, pOut);
	if (!pArea || !pOut || pArea->dpi == 0)
		return;

	// The same 96 dpi height the original divides into rows
	int cyArea = 96 * (pArea->rc.bottom - pArea->rc.top) / (int)pArea->dpi;
	int cyWin7 = Win7DesktopRowSpacing(cyArea, sizeCell.cy);
	dbgprintf(L"explorer7: desktop rows, primary monitor area %d, row %d, stock %d, Windows 7 %d",
		cyArea, sizeCell.cy, pOut->cy, cyWin7);
	pOut->cy = cyWin7;
}

// Patterns read from shell32 19041 and 26100, each one hits once in its own build only
// The mulss displacement to the 0.3f constant is the only wildcard
static const char* c_szRowSpacing26100 =
	"44 8B C2 8B C1 99 41 F7 F8 66 41 0F 6E C0 0F 5B C0 8B C8 33 C0 F3 0F 59 05 ?? ?? ?? ?? "
	"F3 44 0F 2C C8 41 3B D1 7E 12 41 2B D1 44 8D 48 01 85 C9 8B C2 99 41 0F 4E C9 F7 F9 41 03 C0 C3";
static const char* c_szRowSpacing19041 =
	"44 8B C2 8B C1 99 45 33 C9 41 F7 F8 66 41 0F 6E C0 0F 5B C0 44 8B D2 F3 0F 59 05 ?? ?? ?? ?? "
	"F3 0F 2C C0 3B D0 7E 1D 44 2B D0 8B C1 99 41 F7 F8 8B C8 41 8D 41 01 85 C9 0F 4E C8 41 8B C2 99 F7 F9 44 8B C8 43 8D 04 01 C3";
static const char* c_szPrimaryMonitor26100 =
	"48 89 5C 24 08 48 89 7C 24 10 8B 41 08 4C 8B CA 2B 01 4C 8B D1 49 8B D8 BF 01 00 00 00 "
	"8D 04 40 C1 E0 05 99 F7 79 10 8B 49 0C 41 2B 4A 04 44 8B D8 8D 04 49 C1 E0 05 99 41 F7 7A 10";

// FindOptimalSpacing prologues, the security cookie load is the only wildcard
static const char* c_szFindOptimal19041 =
	"48 89 5C 24 08 55 56 41 54 41 56 41 57 48 83 EC 40 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 38 "
	"4C 63 FA 4C 8B F1 49 8B D7 4C 89 44 24 20 48 8D 4C 24 28 4D 89 01 4D 8B E1 49 8B F0 49 8B EF E8";
static const char* c_szFindOptimal26100 =
	"40 55 56 41 55 41 56 41 57 48 83 EC 40 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 38 "
	"4C 63 F2 4C 8B E9 49 8B D6 4C 89 44 24 20 48 8D 4C 24 30 4D 89 01 4D 8B F9 49 8B F0 49 8B EE E8";

// Hooked before the desktop is built, so its very first layout already uses the Win7 rows
static void HookDesktopIconRows()
{
	if (s_Win7DesktopIconRows == 0)
	{
		dbgprintf(L"explorer7: Windows 7 desktop icon rows switched off");
		return;
	}

	HMODULE shell32 = GetModuleHandle(L"shell32.dll");
	if (!shell32)
		shell32 = LoadLibrary(L"shell32.dll");
	if (!shell32)
		return;

	// 24H2 first, the 19041 shape is only tried below that
	ULONG build = g_osVersion.BuildNumber();
	void* rowSpacing = nullptr;
	void* primaryMonitor = nullptr;
	void* findOptimal = nullptr;
	if (build >= 26100)
	{
		rowSpacing = (void*)FindPattern((uintptr_t)shell32, c_szRowSpacing26100);
		primaryMonitor = (void*)FindPattern((uintptr_t)shell32, c_szPrimaryMonitor26100);
		findOptimal = (void*)FindPattern((uintptr_t)shell32, c_szFindOptimal26100);
	}
	else
	{
		rowSpacing = (void*)FindPattern((uintptr_t)shell32, c_szRowSpacing19041);
		findOptimal = (void*)FindPattern((uintptr_t)shell32, c_szFindOptimal19041);
	}

	if (rowSpacing)
		MH_CreateHook(rowSpacing, CalculateOptimalRowSpacing_Hook, reinterpret_cast<LPVOID*>(&CalculateOptimalRowSpacing_Orig));
	if (primaryMonitor)
		MH_CreateHook(primaryMonitor, CalculateOptimalSpacingForPrimaryMonitor_Hook, reinterpret_cast<LPVOID*>(&CalculateOptimalSpacingForPrimaryMonitor_Orig));
	if (findOptimal)
		MH_CreateHook(findOptimal, FindOptimalSpacing_Hook, reinterpret_cast<LPVOID*>(&FindOptimalSpacing_Orig));

	// One display uses the first two, several displays go through the third
	dbgprintf(L"explorer7: desktop icon rows mode %d, row spacing %p, primary monitor %p, several displays %p, build %u",
		s_Win7DesktopIconRows, rowSpacing, primaryMonitor, findOptimal, build);
}
