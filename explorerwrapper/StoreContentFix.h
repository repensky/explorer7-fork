#pragma once

//---Store content fix--------------------------------------
// The Store draws its content at a screen position it never updates on a move
// Only a frame size change re-anchors it, the whole finding is in notes/uwp-content-offset.md

static HWINEVENTHOOK g_storeMoveHook = nullptr;
static HWND g_storeFrame = nullptr;
static RECT g_storeRect = {};
static UINT_PTR g_storeTimer = 0;
static int g_storeStage = 0;

// Remembers the last window asked about, the location event is very chatty
static HWND g_storeAsked = nullptr;
static BOOL g_storeAskedIs = FALSE;

#define STORE_SETTLE_MS 120
#define STORE_GAP_MS 40

//---Is this the Store------------------------------------
// Settings and Camera do not have the fault, so nothing else may be touched
static BOOL StoreIsStoreFrame(HWND frame)
{
	if (frame == g_storeAsked)
		return g_storeAskedIs;

	g_storeAsked = frame;
	g_storeAskedIs = FALSE;

	WCHAR cls[64] = {};
	GetClassName(frame, cls, ARRAYSIZE(cls));

	if (lstrcmp(cls, L"ApplicationFrameWindow") != 0)
		return FALSE;

	// The frame belongs to the host, the content belongs to the app
	HWND content = FindWindowEx(frame, nullptr, L"Windows.UI.Core.CoreWindow", nullptr);

	if (!content)
		return FALSE;

	DWORD pid = 0;
	GetWindowThreadProcessId(content, &pid);

	g_storeAskedIs = IsProcessNamed(pid, L"WinStore.App.exe");
	return g_storeAskedIs;
}

//---The nudge----------------------------------------------
// Two stages so the explorer thread never sleeps between the two calls
static void CALLBACK StoreNudgeProc(HWND, UINT, UINT_PTR id, DWORD)
{
	KillTimer(nullptr, id);
	g_storeTimer = 0;

	if (!g_storeFrame || !IsWindow(g_storeFrame))
	{
		g_storeStage = 0;
		return;
	}

	RECT r = {};

	if (!GetWindowRect(g_storeFrame, &r))
	{
		g_storeStage = 0;
		return;
	}

	int w = r.right - r.left;
	int h = r.bottom - r.top;

	if (g_storeStage == 0)
	{
		SetWindowPos(g_storeFrame, nullptr, 0, 0, w, h - 1,
			SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

		g_storeStage = 1;
		g_storeTimer = SetTimer(nullptr, 0, STORE_GAP_MS, StoreNudgeProc);
		return;
	}

	SetWindowPos(g_storeFrame, nullptr, 0, 0, w, h + 1,
		SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

	g_storeStage = 0;
}

//---Watch for moves----------------------------------------
static void CALLBACK StoreMoveProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
	LONG idObject, LONG idChild, DWORD thread, DWORD time)
{
	if (event != EVENT_OBJECT_LOCATIONCHANGE || idObject != OBJID_WINDOW || idChild != CHILDID_SELF)
		return;

	if (!hwnd || !StoreIsStoreFrame(hwnd))
		return;

	RECT r = {};

	if (!GetWindowRect(hwnd, &r))
		return;

	BOOL sameSize = (r.right - r.left) == (g_storeRect.right - g_storeRect.left) &&
		(r.bottom - r.top) == (g_storeRect.bottom - g_storeRect.top);

	BOOL moved = r.left != g_storeRect.left || r.top != g_storeRect.top;

	g_storeFrame = hwnd;
	g_storeRect = r;

	// A resize re-anchors the content by itself, only a plain move needs the nudge
	if (!moved || !sameSize)
		return;

	if (g_storeTimer)
		KillTimer(nullptr, g_storeTimer);

	// Restarted on every move, so the nudge lands once after the window settles
	g_storeStage = 0;
	g_storeTimer = SetTimer(nullptr, 0, STORE_SETTLE_MS, StoreNudgeProc);
}

//---Init---------------------------------------------------
static void StoreContentFixInit()
{
	if (g_storeMoveHook)
		return;

	g_storeMoveHook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
		nullptr, StoreMoveProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

	dbgprintf(L"explorer7: watching store frames for moves, hook %p", g_storeMoveHook);
}
