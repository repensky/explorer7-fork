#pragma once

// The volume flyout lives in SndVol.exe, which is a fresh process for every open
// So explorer does the work from outside, see notes/accountscontrol-signin.md

#include "FlyoutFix.h"

//---The flyout, and not the mixer--------------------------
// CDlgSimpleVolumeHost is a popup dialog, the Volume Mixer is an ordinary window
static BOOL IsSndVolFlyout(HWND hwnd)
{
	if (GetWindow(hwnd, GW_OWNER))
		return FALSE;

	LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);

	if (!(style & WS_POPUP))
		return FALSE;

	// WS_CAPTION is two bits, the flyout carries WS_BORDER on its own
	if ((style & WS_CAPTION) == WS_CAPTION)
		return FALSE;

	WCHAR cls[64] = {};
	GetClassName(hwnd, cls, ARRAYSIZE(cls));

	return lstrcmp(cls, L"#32770") == 0;
}

//---Which process owns it----------------------------------
static HWINEVENTHOOK g_sndVolWatchHook;
static HWINEVENTHOOK g_sndVolMoveHook;
static HWND g_sndVolFlyout;

// Resolving the image name for every window shown anywhere would be wasteful
static BOOL IsProcessNamed(DWORD pid, LPCWSTR name)
{
	HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!proc)
		return FALSE;

	WCHAR path[MAX_PATH] = {};
	DWORD n = MAX_PATH;
	BOOL match = FALSE;

	if (QueryFullProcessImageName(proc, 0, path, &n))
	{
		PCWSTR leaf = path;
		for (PCWSTR p = path; *p; p++)
		{
			if (*p == L'\\')
				leaf = p + 1;
		}

		match = lstrcmpi(leaf, name) == 0;
	}

	CloseHandle(proc);
	return match;
}

//---Keeping it inside the gap------------------------------
// SndVol places the flyout itself, so it has to be put back after each move
static void CALLBACK SndVolMoveProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
	LONG idObject, LONG idChild, DWORD thread, DWORD time)
{
	if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF
		|| hwnd != g_sndVolFlyout)
	{
		return;
	}

	FlyoutFixReclampForeignWindow(hwnd);
}

//---Explorer side------------------------------------------
static void CALLBACK SndVolWatchProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
	LONG idObject, LONG idChild, DWORD thread, DWORD time)
{
	if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd)
		return;

	// Class first, it is the cheapest test and the flyout is a dialog
	WCHAR cls[64] = {};
	GetClassName(hwnd, cls, ARRAYSIZE(cls));

	if (lstrcmp(cls, L"#32770") != 0)
		return;

	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);

	if (!pid || pid == GetCurrentProcessId())
		return;

	if (!IsProcessNamed(pid, L"SndVol.exe"))
		return;

	BOOL flyout = IsSndVolFlyout(hwnd);

	dbgprintf(L"sndvol: %p [%s] style 0x%08X in pid %lu, treating it %d",
		hwnd, cls, (DWORD)GetWindowLongPtr(hwnd, GWL_STYLE), pid, flyout);

	if (!flyout)
		return;

	RECT before = {};
	GetWindowRect(hwnd, &before);

	FlyoutFixFloatForeignWindow(hwnd);
	g_sndVolFlyout = hwnd;

	RECT after = {};
	GetWindowRect(hwnd, &after);

	dbgprintf(L"sndvol: %p was %d,%d %dx%d now %d,%d %dx%d",
		hwnd, before.left, before.top,
		before.right - before.left, before.bottom - before.top,
		after.left, after.top,
		after.right - after.left, after.bottom - after.top);

	// Scoped to this process, a session wide move hook would fire on every mouse move
	if (g_sndVolMoveHook)
		UnhookWinEvent(g_sndVolMoveHook);

	g_sndVolMoveHook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE,
		EVENT_OBJECT_LOCATIONCHANGE, nullptr, SndVolMoveProc, pid, 0,
		WINEVENT_OUTOFCONTEXT);

	dbgprintf(L"sndvol: floated %p, move hook %p", hwnd, g_sndVolMoveHook);
}

// Watches the session for SndVol showing its flyout, then floats it from here
static void SndVolWatchInit()
{
	g_sndVolWatchHook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW,
		nullptr, SndVolWatchProc, 0, 0,
		WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

	dbgprintf(L"explorer7: watching for sndvol, hook %p", g_sndVolWatchHook);
}
