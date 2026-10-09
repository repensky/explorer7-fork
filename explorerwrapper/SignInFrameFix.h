#pragma once

// Microsoft account sign in dialogs, full account in notes/accountscontrol-signin.md
// Explorer7 is not an immersive process, so win32k refuses to register the frame

#include <commctrl.h>
#include "MinHook.h"

// Undocumented user32, both exported by ordinal with no name
#define ORD_ISSHELLMANAGEDWINDOW 2574
#define ORD_SETMODERNAPPWINDOW   2568
#define ORD_GETMODERNAPPWINDOW   2569

// Both live further down util.h, and this header is included above them
void ForceActiveWindowAppearance(HWND hwnd);
void ClearForcedActiveWindowAppearance(HWND hwnd);

typedef BOOL(WINAPI* IsShellManagedWindowAPI)(HWND);
typedef BOOL(WINAPI* SetModernAppWindowAPI)(HWND, HWND);
typedef HWND(WINAPI* GetModernAppWindowAPI)(HWND);

static IsShellManagedWindowAPI IsShellManagedWindowFn = nullptr;
static SetModernAppWindowAPI SetModernAppWindowOrig = nullptr;
static GetModernAppWindowAPI GetModernAppWindowFn = nullptr;

// Several of these dialogs are alive at once, and one can even own another
// Kept in a window property, since the foreground watcher asks from another thread
static HWND SignInAppOf(HWND frame)
{
	return (HWND)GetProp(frame, L"explorer7.SignInApp");
}

//---Find a shell managed owner-----------------------------
// The new frame inherits the shell managed bit from whoever owns it at creation
struct SignInOwnerSearch
{
	HWND best;
	HWND anyManaged;
};

static BOOL CALLBACK SignInOwnerProc(HWND hwnd, LPARAM lp)
{
	SignInOwnerSearch* s = (SignInOwnerSearch*)lp;

	if (!IsWindowVisible(hwnd) || !IsShellManagedWindowFn(hwnd))
		return TRUE;

	if (!s->anyManaged)
		s->anyManaged = hwnd;

	WCHAR cls[64] = {};
	GetClassName(hwnd, cls, ARRAYSIZE(cls));

	// A frame host window is the one that reliably carries the bit here
	if (lstrcmp(cls, L"ApplicationFrameWindow") == 0)
	{
		s->best = hwnd;
		return FALSE;
	}

	return TRUE;
}

static HWND SignInFindOwner()
{
	if (!IsShellManagedWindowFn)
		return nullptr;

	SignInOwnerSearch s = {};
	EnumWindows(SignInOwnerProc, (LPARAM)&s);

	return s.best ? s.best : s.anyManaged;
}

//---Carry the owned window---------------------------------
// An owner controls z order and cloaking, it does not move what it owns
static void SignInFollowFrame(HWND frame, HWND app)
{
	if (!app || !IsWindow(app) || !IsWindow(frame))
		return;

	RECT c = {};
	GetClientRect(frame, &c);

	POINT p = { c.left, c.top };
	ClientToScreen(frame, &p);

	SetWindowPos(app, nullptr, p.x, p.y,
		c.right - c.left, c.bottom - c.top, SWP_NOZORDER | SWP_NOACTIVATE);
}

//---Clear topmost------------------------------------------
// A style write is ignored across processes, so the window manager is asked
static void SignInClearTopmost(HWND app)
{
	if (!app || !IsWindow(app))
		return;

	SetWindowPos(app, HWND_NOTOPMOST, 0, 0, 0, 0,
		SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

//---Keep the pair together---------------------------------
// An owner fixes the order between the two, it does not carry one when the other sinks
static void SignInStackOnFrame(HWND frame, HWND app)
{
	if (!app || !IsWindow(app) || !IsWindow(frame))
		return;

	// The app process can put the bit back, so it is checked every time
	if (GetWindowLongPtr(app, GWL_EXSTYLE) & WS_EX_TOPMOST)
		SignInClearTopmost(app);

	SetWindowPos(app, frame, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

//---Forced active look-------------------------------------
// DWM re-reads the attribute only after a frame change, so setting it alone does nothing
// Guarded on the current state, since the frame is sent this message in bursts
static void SignInSetActiveLook(HWND frame, BOOL on)
{
	BOOL was = GetProp(frame, L"explorer7.SignInForcedActive") ? TRUE : FALSE;

	if (was == on)
		return;

	// Set first, so the message below sees the new state and does not come back round
	if (on)
	{
		SetProp(frame, L"explorer7.SignInForcedActive", (HANDLE)1);
		ForceActiveWindowAppearance(frame);
	}
	else
	{
		RemoveProp(frame, L"explorer7.SignInForcedActive");
		ClearForcedActiveWindowAppearance(frame);
	}

	SetWindowPos(frame, nullptr, 0, 0, 0, 0,
		SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
		SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);

	// We have been telling the frame it is active, so it needs telling when that stops
	SendMessage(frame, WM_NCACTIVATE, on ? TRUE : FALSE, 0);
	RedrawWindow(frame, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW);

	dbgprintf(L"explorer7: frame %p active look now %d", frame, on);
}

//---Frames we are looking after----------------------------
// The frame is never the active window, so nothing ever deactivates it
// Only a foreground change anywhere tells us a real program has taken over
#define SIGNIN_MAX_FRAMES 8

static HWND g_signInFrames[SIGNIN_MAX_FRAMES];
static HWINEVENTHOOK g_signInForegroundHook;

static BOOL SignInIsOurs(HWND frame, HWND fg)
{
	// Nothing active, our own content active, or the frame itself, all count as ours
	return !fg || fg == frame || fg == SignInAppOf(frame);
}

static void SignInReviewLooks()
{
	HWND fg = GetForegroundWindow();

	for (int i = 0; i < SIGNIN_MAX_FRAMES; i++)
	{
		HWND frame = g_signInFrames[i];

		if (!frame)
			continue;

		if (!IsWindow(frame))
		{
			g_signInFrames[i] = nullptr;
			continue;
		}

		BOOL ours = SignInIsOurs(frame, fg);

		dbgprintf(L"explorer7: foreground %p, frame %p app %p, ours %d",
			fg, frame, SignInAppOf(frame), ours);

		SignInSetActiveLook(frame, ours);
	}
}

static void CALLBACK SignInForegroundProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
	LONG idObject, LONG idChild, DWORD thread, DWORD time)
{
	if (event == EVENT_SYSTEM_FOREGROUND && idObject == OBJID_WINDOW)
		SignInReviewLooks();
}

static void SignInWatchFrame(HWND frame)
{
	for (int i = 0; i < SIGNIN_MAX_FRAMES; i++)
	{
		if (g_signInFrames[i] == frame)
			return;
	}

	for (int i = 0; i < SIGNIN_MAX_FRAMES; i++)
	{
		if (!g_signInFrames[i] || !IsWindow(g_signInFrames[i]))
		{
			g_signInFrames[i] = frame;
			break;
		}
	}

	// Out of context, so it runs here in explorer and injects nothing
	if (!g_signInForegroundHook)
	{
		g_signInForegroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND,
			EVENT_SYSTEM_FOREGROUND, nullptr, SignInForegroundProc, 0, 0,
			WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNTHREAD);

		dbgprintf(L"explorer7: watching the foreground for sign in frames, hook %p",
			g_signInForegroundHook);
	}
}

static void SignInForgetFrame(HWND frame)
{
	int live = 0;

	for (int i = 0; i < SIGNIN_MAX_FRAMES; i++)
	{
		if (g_signInFrames[i] == frame)
			g_signInFrames[i] = nullptr;
		else if (g_signInFrames[i])
			live++;
	}

	if (!live && g_signInForegroundHook)
	{
		UnhookWinEvent(g_signInForegroundHook);
		g_signInForegroundHook = nullptr;
	}
}

//---Carry the title----------------------------------------
// The title normally arrives through the registration that is refused
static void SignInCopyTitle(HWND frame, HWND app)
{
	if (!app || !IsWindow(app) || !IsWindow(frame))
		return;

	WCHAR want[256] = {};
	WCHAR have[256] = {};

	GetWindowText(app, want, ARRAYSIZE(want));
	GetWindowText(frame, have, ARRAYSIZE(have));

	// A frame that named itself knows better, only a blank one gets filled in
	if (!want[0] || have[0])
		return;

	SetWindowText(frame, want);
	dbgprintf(L"explorer7: sign in frame %p title set to %s", frame, want);
}

//---Frame subclass-----------------------------------------
// Clicking the content leaves this frame inactive, so it is told otherwise
static LRESULT CALLBACK SignInFrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
	UINT_PTR id, DWORD_PTR ref)
{
	HWND app = SignInAppOf(hwnd);

	if (msg == WM_NCDESTROY)
	{
		SignInForgetFrame(hwnd);
		RemoveProp(hwnd, L"explorer7.SignInForcedActive");
		RemoveProp(hwnd, L"explorer7.SignInApp");
		RemoveProp(hwnd, L"explorer7.SignInNative");

		RemoveWindowSubclass(hwnd, SignInFrameProc, id);
		return DefSubclassProc(hwnd, msg, wp, lp);
	}

	// Answering this message does not move a DWM drawn caption, the composition attribute does
	// The start menu is kept lit the same way, see ShellAccentOverride
	// A registered host gets its caption from win32k, so the override stands down
	if (msg == WM_NCACTIVATE && !wp && !GetProp(hwnd, L"explorer7.SignInNative"))
	{
		HWND fg = GetForegroundWindow();

		// Ours while the seat is empty or our own content has it, theirs otherwise
		BOOL ours = !fg || fg == app;

		SignInSetActiveLook(hwnd, ours);

		if (ours)
			return DefSubclassProc(hwnd, msg, TRUE, lp);
	}

	LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);

	// Moving and sizing both land here, so one case covers the lot
	if (msg == WM_WINDOWPOSCHANGED)
	{
		SignInFollowFrame(hwnd, app);
		SignInStackOnFrame(hwnd, app);
	}

	// A program coming forward does not always move us, so activation is watched too
	if (msg == WM_ACTIVATE || msg == WM_ACTIVATEAPP)
	{
		SignInStackOnFrame(hwnd, app);
		SignInCopyTitle(hwnd, app);
	}

	return r;
}

//---Attach to the frame------------------------------------
// Subclassing only works from the thread that owns the window, so this runs at creation
static void SignInAttachFrame(HWND frame)
{
	DWORD_PTR ref = 0;

	if (!frame || GetWindowSubclass(frame, SignInFrameProc, 0, &ref))
		return;

	BOOL sub = SetWindowSubclass(frame, SignInFrameProc, 0, 0);

	dbgprintf(L"explorer7: sign in frame %p subclassed %d, our thread %lu, its thread %lu",
		frame, sub, GetCurrentThreadId(), GetWindowThreadProcessId(frame, nullptr));
}

//---Undo the wrapper cover---------------------------------
// The wrapper cloaks every frame at creation to kill a ghost window
// It also adds a tool window bit to every band window, which costs the caption buttons
static void SignInUncover(HWND frame)
{
	LONG_PTR ex = GetWindowLongPtr(frame, GWL_EXSTYLE);
	if (ex & WS_EX_TOOLWINDOW)
	{
		SetWindowLongPtr(frame, GWL_EXSTYLE, ex & ~(LONG_PTR)WS_EX_TOOLWINDOW);
		SetWindowPos(frame, nullptr, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
	}

	BOOL off = FALSE;
	DwmSetWindowAttribute(frame, DWMWA_CLOAK, &off, sizeof(off));
}

//---Adopt the content window-------------------------------
// Ownership rather than parenting, a child of ours never receives input
static void SignInAdopt(HWND frame, HWND app)
{
	// Normally already attached at creation, this catches a frame we did not create
	SignInAttachFrame(frame);

	// Remembered against this frame alone, several dialogs can be alive at once
	SetWindowSubclass(frame, SignInFrameProc, 0, (DWORD_PTR)app);
	SetProp(frame, L"explorer7.SignInApp", (HANDLE)app);

	SetWindowLongPtr(app, GWLP_HWNDPARENT, (LONG_PTR)frame);
	SignInFollowFrame(frame, app);

	SignInUncover(frame);

	SignInClearTopmost(app);
	SignInStackOnFrame(frame, app);
	SignInCopyTitle(frame, app);

	// The dialog starts out in front, and the watcher hands it back when a program takes over
	SignInWatchFrame(frame);
	SignInSetActiveLook(frame, TRUE);
}

//---Set modern app window----------------------------------
// The refusal is the only reason the dialog dies, so the refusal is covered
static BOOL WINAPI SetModernAppWindowNew(HWND host, HWND modern)
{
	SetLastError(0);
	BOOL ok = SetModernAppWindowOrig(host, modern);

	// The whole point of the type flag work, this line means win32k took it
	if (ok)
	{
		dbgprintf(L"explorer7: SetModernAppWindow accepted frame %p app %p, native", host, modern);

		// win32k owns the frame now, but our own cloak still hides it
		SignInUncover(host);

		// A registered host gets its caption from win32k, so the override stands down
		SetProp(host, L"explorer7.SignInNative", (HANDLE)1);
		return TRUE;
	}

	DWORD err = GetLastError();

	WCHAR cls[64] = {};
	if (host && IsWindow(host))
		GetClassName(host, cls, ARRAYSIZE(cls));

	// Only a refused registration of a real frame, anything else is left alone
	if (err != ERROR_INVALID_PARAMETER || lstrcmp(cls, L"ApplicationFrameWindow") != 0)
		return ok;

	// Three separate gates answer 0x57, and these values say which one it was
	// The gates are read out of win32kfull in notes/modern-app-window.md
	DWORD frameThread = GetWindowThreadProcessId(host, nullptr);

	// Null means two different things here, only the error tells them apart
	// 0x57 is not registered, anything else is registered with no app yet
	SetLastError(0);
	HWND registered = GetModernAppWindowFn ? GetModernAppWindowFn(host) : (HWND)-1;
	DWORD getErr = GetLastError();

	dbgprintf(L"explorer7: SetModernAppWindow refused for frame %p app %p, covering it",
		host, modern);

	dbgprintf(L"explorer7: gate check, calling thread %lu frame thread %lu registered %p getErr %lu managed %d",
		GetCurrentThreadId(), frameThread, registered, getErr,
		IsShellManagedWindowFn ? IsShellManagedWindowFn(host) : -1);

	if (modern && IsWindow(modern))
		SignInAdopt(host, modern);

	SetLastError(0);
	return TRUE;
}

//---Init---------------------------------------------------
static void SignInFrameFixInit()
{
	HMODULE user32 = GetModuleHandle(L"user32.dll");
	if (!user32)
		return;

	IsShellManagedWindowFn = (IsShellManagedWindowAPI)
		GetProcAddress(user32, (LPCSTR)ORD_ISSHELLMANAGEDWINDOW);

	SetModernAppWindowOrig = (SetModernAppWindowAPI)
		GetProcAddress(user32, (LPCSTR)ORD_SETMODERNAPPWINDOW);

	GetModernAppWindowFn = (GetModernAppWindowAPI)
		GetProcAddress(user32, (LPCSTR)ORD_GETMODERNAPPWINDOW);

	dbgprintf(L"explorer7: IsShellManagedWindow %p, SetModernAppWindow %p",
		IsShellManagedWindowFn, SetModernAppWindowOrig);

	if (SetModernAppWindowOrig)
	{
		MH_CreateHook(static_cast<LPVOID>(SetModernAppWindowOrig), SetModernAppWindowNew,
			reinterpret_cast<LPVOID*>(&SetModernAppWindowOrig));
	}
}
