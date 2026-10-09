#pragma once
#include "common.h"
#include "dbgprint.h"
#include "OSVersion.h"
#include "MinHook.h"

//---Jumplist highlight under a tooltip---------------------
// 24H2 comctl32 keeps a corridor between a listview item and its shown tooltip where the highlight does not move
// The Win7 jump list opens its tooltip below the item, so sliding down there froze the highlight

// AccessibleToolTips::_IsPointInToolTipSafeZone past its setup, identical in comctl32 26100.1591, 8972 and 9278
static const char c_szToolTipSafeZoneBody[] = "48 8B F2 48 8B F9 48 8B 11 49 8B D8 48 8B CE 48 FF 15 ?? ?? ?? ?? 0F 1F 44 00 00 85 C0 74 22 B0 01";

// How far those bytes sit from the start, a hook over the start leaves them untouched
#define TOOLTIP_SAFE_ZONE_BODY_OFFSET 0x22

// Never inside the corridor, the tooltip still shows and hides as before
static bool ToolTipSafeZone_Hook(const POINT* point, const RECT* item, const RECT* tip)
{
	return false;
}

static void HookToolTipSafeZone()
{
	// 19041 comctl32 has no such corridor at all
	if (g_osVersion.BuildNumber() < 26100)
		return;

	// This process asks for comctl32 version 6 in its manifest, and only that copy has the corridor
	HMODULE comctl = LoadLibrary(L"comctl32.dll");
	char* body = comctl ? (char*)FindPattern((uintptr_t)comctl, c_szToolTipSafeZoneBody) : nullptr;
	void* target = body ? body - TOOLTIP_SAFE_ZONE_BODY_OFFSET : nullptr;
	MH_STATUS status = target ? MH_CreateHook(target, (LPVOID)ToolTipSafeZone_Hook, NULL) : MH_ERROR_FUNCTION_NOT_FOUND;

	WCHAR path[MAX_PATH] = L"";
	if (comctl)
		GetModuleFileNameW(comctl, path, MAX_PATH);
	dbgprintf(L"explorer7: tooltip safe zone off, check %p status %d in %s", target, status, path);
}
