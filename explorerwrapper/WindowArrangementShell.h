#pragma once
#include "common.h"
#include "dbgprint.h"
#include "OSVersion.h"
#include "RegistryManager.h"
#include "MinHook.h"

// Side and corner snapping for the Windows 7 shell on Windows 11, corners only on 19041, see notes/window-arrangement-shell.md
// From 22000 the kernel asks the shell for every side snap rectangle and only maximizes by itself

//---Kernel layout------------------------------------------

// The block win32kfull sends the registered shell window, read from 26100.9168
struct ShellWindowManagementCalloutInfo
{
	HWND hwnd;           // the window being dragged
	DWORD kind;          // 0 asks for the arrangement rectangle
	DWORD reserved0C;
	POINT point;         // cursor in physical pixels
	DWORD inputFlags;
	DWORD inputType;     // copied from the dragging thread, 2 for a mouse drag, only logged
	DWORD result;        // 1 makes the kernel use the rectangle below
	RECT rect;           // visible frame in physical pixels
	DWORD regionValid;   // bit 0 makes the kernel ask again once the cursor crosses the region
	RECT region;
	DWORD shrinkWidth;
	DWORD reserved4C;
	HWND insertAfter;
	DWORD arrangeKind;   // twinui writes 5 when it has no windows to suggest
	DWORD reserved5C;
	BYTE reserved60[8];
};
static_assert(offsetof(ShellWindowManagementCalloutInfo, kind) == 0x08, "callout kind moved");
static_assert(offsetof(ShellWindowManagementCalloutInfo, point) == 0x10, "callout point moved");
static_assert(offsetof(ShellWindowManagementCalloutInfo, inputType) == 0x1C, "callout input type moved");
static_assert(offsetof(ShellWindowManagementCalloutInfo, result) == 0x20, "callout result moved");
static_assert(offsetof(ShellWindowManagementCalloutInfo, rect) == 0x24, "callout rect moved");
static_assert(offsetof(ShellWindowManagementCalloutInfo, regionValid) == 0x34, "callout region moved");
static_assert(offsetof(ShellWindowManagementCalloutInfo, region) == 0x38, "callout region rectangle moved");
static_assert(offsetof(ShellWindowManagementCalloutInfo, insertAfter) == 0x50, "callout insert after moved");
static_assert(offsetof(ShellWindowManagementCalloutInfo, arrangeKind) == 0x58, "callout kind of arrangement moved");
static_assert(sizeof(ShellWindowManagementCalloutInfo) == 0x68, "callout size moved");

// The shorter block win32kfull 19041 sends, read from CallShell::xxxArrangementRectangleHandler
struct ShellWindowManagementCalloutInfo19041
{
	HWND hwnd;           // the window being dragged
	DWORD kind;          // 0 asks for the arrangement rectangle
	POINT point;         // cursor in physical pixels
	DWORD inputFlags;
	DWORD inputType;
	DWORD reserved1C;
	RECT rect;           // becomes the window rectangle as it is, no margins are added
	RECT region;         // must hold the cursor, the kernel asks again once it leaves
};
static_assert(offsetof(ShellWindowManagementCalloutInfo19041, point) == 0x0C, "19041 callout point moved");
static_assert(offsetof(ShellWindowManagementCalloutInfo19041, rect) == 0x20, "19041 callout rect moved");
static_assert(offsetof(ShellWindowManagementCalloutInfo19041, region) == 0x30, "19041 callout region moved");
static_assert(sizeof(ShellWindowManagementCalloutInfo19041) == 0x40, "19041 callout size moved");

// _WINDOW_ACTION as win32kfull 26100.9168 ResolvePublicWindowAction reads it
struct ShellWindowAction
{
	DWORD fields;          // which members below count, 2 position and 4 size
	DWORD options;         // bit 1 treats the rectangle as a visible frame and widens it
	DWORD flag1Value;
	POINT position;        // physical pixels, converted for windows that are not per monitor aware
	SIZE size;
	DWORD reserved1C;
	HWND insertAfter;
	DWORD state;
	BYTE reserved2C[0x34];
};
static_assert(offsetof(ShellWindowAction, position) == 0x0C, "window action position moved");
static_assert(offsetof(ShellWindowAction, size) == 0x14, "window action size moved");
static_assert(offsetof(ShellWindowAction, insertAfter) == 0x20, "window action insert after moved");
static_assert(offsetof(ShellWindowAction, state) == 0x28, "window action state moved");
static_assert(sizeof(ShellWindowAction) == 0x60, "NtUserApplyWindowAction copies 0x60 bytes");
#define WINDOW_ACTION_POSITION 0x2
#define WINDOW_ACTION_SIZE 0x4

// Sent, not posted, the kernel waits up to two seconds for the answer
#define WM_SHELL_WINDOWMANAGEMENT_CALLOUT 0x341
#define ARRANGE_KIND_NO_SUGGESTIONS 5

typedef BOOL(WINAPI* NtUserAcquireIAMKey_t)(ULONGLONG* key);
typedef BOOL(WINAPI* NtUserEnableIAMAccess_t)(ULONGLONG* key, BOOL enable);
typedef BOOL(WINAPI* NtUserRegisterWindowArrangementCallout_t)(HWND hwnd, BOOL enable);
typedef DPI_AWARENESS_CONTEXT(WINAPI* SetThreadDpiAwarenessContext_t)(DPI_AWARENESS_CONTEXT context);
typedef HRESULT(WINAPI* DwmGetWindowAttribute_t)(HWND hwnd, DWORD attribute, PVOID value, DWORD size);
typedef BOOL(WINAPI* NtUserApplyWindowAction_t)(HWND hwnd, ShellWindowAction* action);

static NtUserAcquireIAMKey_t g_realAcquireIAMKey;
static NtUserEnableIAMAccess_t g_enableIAMAccess;
static NtUserRegisterWindowArrangementCallout_t g_realRegisterArrangement;

static ULONGLONG g_arrangeKey;
static bool g_arrangeKeyKnown;
static HWND g_arrangeWnd;
static bool g_arrangeWndRegistered;
static bool g_registeringOurs;
static HWND g_twinuiArrangeWnd;
static WNDPROC g_twinuiArrangeProc;
static DwmGetWindowAttribute_t g_getWindowAttribute;
static NtUserApplyWindowAction_t g_applyWindowAction;

// The last snap answered, the window lands one margin past the work area on its outer sides
struct LandingFix
{
	HWND hwnd;
	RECT landing;   // where the kernel puts the window
	RECT settled;   // the same rectangle with the outer margins taken back off
};
static LandingFix g_landingFix;
static HWINEVENTHOOK g_moveSizeEndHook;
static HWINEVENTHOOK g_landingHook;

// Registry Win10SideSnapFit, Win10SideSnapTrim and Win10CornerSnap, all on unless set to 0, read at start
static bool g_sideSnapFit = true;
static bool g_sideSnapTrim = true;
static bool g_cornerSnap = true;

// 19041 to 21999 send the shorter block and only ask about the left and right edges
static bool g_callout19041;

// The last fix, read back once to see whether it stuck
struct LandingCheck
{
	HWND hwnd;
	RECT want;
};
static LandingCheck g_landingCheck;
#define LANDING_CHECK_TIMER 0x5E8
#define LANDING_FIX_TIMER 0x5E7

//---Rectangle-------------------------------------------

// What a point snaps to, and the zone the kernel may stay in before it asks again
struct SnapTarget
{
	RECT rect;            // the half or quarter of the work area to fill
	RECT zone;            // physical pixels, the same as the callout point
	bool zoneValid;
	bool answer;          // false leaves the top edge to the kernel, which maximizes
	const wchar_t* name;
};

// Windows 10 splits in floats (DetermineDefaultSnapRegionRect), so the two halves of an odd size meet
static LONG SplitFromStart(LONG start, LONG end)
{
	return (LONG)((float)(end - start) * 0.5f + (float)start);
}

static LONG SplitFromEnd(LONG start, LONG end)
{
	return (LONG)((float)end - (float)(end - start) * 0.5f);
}

// The half for a side, with corners off the nearest screen edge decides as before
static bool SideSnapTarget(POINT pt, const MONITORINFO& mi, SnapTarget* out)
{
	// A tie goes to the side as the kernel's own hit test does
	LONG toLeft = pt.x - mi.rcMonitor.left;
	LONG toRight = mi.rcMonitor.right - 1 - pt.x;
	LONG toTop = pt.y - mi.rcMonitor.top;
	LONG toBottom = mi.rcMonitor.bottom - 1 - pt.y;
	LONG toSide = toLeft < toRight ? toLeft : toRight;
	LONG toEnd = toTop < toBottom ? toTop : toBottom;
	if (toSide > toEnd)
		return false;

	const RECT& w = mi.rcWork;
	out->rect = w;
	if (toLeft <= toRight)
		out->rect.right = SplitFromStart(w.left, w.right);
	else
		out->rect.left = SplitFromEnd(w.left, w.right);
	out->zoneValid = false;
	out->answer = true;
	out->name = toLeft <= toRight ? L"left half" : L"right half";
	return true;
}

// The bands 24H2 twinui tests (CustomDragout::DetermineSnapRegionFromPoint), a side band wins over the top and bottom ones
static bool CornerSnapTarget(POINT pt, const MONITORINFO& mi, SnapTarget* out)
{
	const RECT& m = mi.rcMonitor;
	const RECT& w = mi.rcWork;

	// A tenth of the screen each way, the size 24H2 gives a mouse
	LONG bandX = (LONG)((float)(m.right - m.left) * 0.1f);
	LONG bandY = (LONG)((float)(m.bottom - m.top) * 0.1f);

	// twinui's tests include the band's last pixel, so each zone ends one pixel past it
	bool left = pt.x <= w.left + bandX;
	bool right = !left && pt.x >= w.right - bandX;
	bool top = pt.y <= w.top + bandY;
	bool bottom = !top && pt.y >= w.bottom - bandY;

	out->zone.left = left ? m.left : right ? w.right - bandX : w.left + bandX + 1;
	out->zone.right = left ? w.left + bandX + 1 : right ? m.right : w.right - bandX;
	out->zone.top = top ? m.top : bottom ? w.bottom - bandY : w.top + bandY + 1;
	out->zone.bottom = top ? w.top + bandY + 1 : bottom ? m.bottom : w.bottom - bandY;
	out->zoneValid = true;
	out->answer = true;
	out->rect = w;

	// Away from both side bands only the top asks, and the kernel maximizes it itself
	if (!left && !right)
	{
		out->answer = false;
		out->name = L"top";
		return top;
	}

	if (left)
		out->rect.right = SplitFromStart(w.left, w.right);
	else
		out->rect.left = SplitFromEnd(w.left, w.right);
	if (top)
		out->rect.bottom = SplitFromStart(w.top, w.bottom);
	else if (bottom)
		out->rect.top = SplitFromEnd(w.top, w.bottom);

	out->name = top ? (left ? L"top left quarter" : L"top right quarter") :
		bottom ? (left ? L"bottom left quarter" : L"bottom right quarter") :
		(left ? L"left half" : L"right half");
	return true;
}

static bool FindSnapTarget(POINT pt, SnapTarget* out, RECT* work)
{
	HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
	MONITORINFO mi = { sizeof(mi) };
	if (!monitor || !GetMonitorInfoW(monitor, &mi))
		return false;
	*work = mi.rcWork;
	return g_cornerSnap ? CornerSnapTarget(pt, mi, out) : SideSnapTarget(pt, mi, out);
}

// The test xxxGetArrangeRectFromShell applies, inside the work area and on two adjacent or exactly two opposite edges
static bool KernelAcceptsSnapRect(const RECT& rc, const RECT& work)
{
	if (rc.left < work.left || rc.top < work.top || rc.right > work.right || rc.bottom > work.bottom ||
		rc.left >= rc.right || rc.top >= rc.bottom)
		return false;

	// Bits as GetFrameBoundsOverlapInfo sets them, the pairs as ArrangementStyleFromOverlap reads them
	UINT edges = (rc.left == work.left ? 1 : 0) | (rc.top == work.top ? 2 : 0) |
		(rc.right == work.right ? 4 : 0) | (rc.bottom == work.bottom ? 8 : 0);
	return (edges & 3) == 3 || (edges & 9) == 9 || (edges & 6) == 6 || (edges & 12) == 12 ||
		edges == 5 || edges == 10;
}

// win32kfull widens the answer by the window's invisible margins (WindowMargins::ExtendRect), so they come off the answer first
// The kernel refuses an answer without two of its sides on the work area, so those sides keep their margins until the drop
static void FitSnapToFrame(HWND hwnd, const RECT& work, RECT* rc)
{
	if (!g_getWindowAttribute)
		g_getWindowAttribute = (DwmGetWindowAttribute_t)GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmGetWindowAttribute");
	auto getAttribute = g_getWindowAttribute;

	// On 26100 DWMWA_EXTENDED_FRAME_BOUNDS is answered by the kernel from the same margin record
	RECT window, frame;
	if (!hwnd || !getAttribute || !GetWindowRect(hwnd, &window) ||
		FAILED(getAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))))
	{
		dbgprintf(L"explorer7: snap margins unknown for %p, rectangle kept", hwnd);
		return;
	}
	LONG marginLeft = frame.left - window.left;
	LONG marginTop = frame.top - window.top;
	LONG marginRight = window.right - frame.right;
	LONG marginBottom = window.bottom - frame.bottom;

	// A half keeps top and bottom on the work area and loses both side margins
	// A quarter keeps its two outer sides and loses the margins of the two inner ones
	RECT fitted = *rc;
	bool half = rc->top == work.top && rc->bottom == work.bottom;
	if (half || rc->left != work.left)
		fitted.left += marginLeft;
	if (half || rc->right != work.right)
		fitted.right -= marginRight;
	if (!half && rc->top != work.top)
		fitted.top += marginTop;
	if (!half && rc->bottom != work.bottom)
		fitted.bottom -= marginBottom;

	bool accepted = g_sideSnapFit && KernelAcceptsSnapRect(fitted, work);
	g_landingFix.hwnd = NULL;
	if (accepted)
	{
		*rc = fitted;
		// Where the kernel will put the window, and that rectangle with the outer margins taken back off
		RECT landing = { fitted.left - marginLeft, fitted.top - marginTop, fitted.right + marginRight, fitted.bottom + marginBottom };
		RECT settled = landing;
		if (fitted.left == work.left)
			settled.left += marginLeft;
		if (fitted.right == work.right)
			settled.right -= marginRight;
		if (fitted.bottom == work.bottom)
			settled.bottom -= marginBottom;
		if (g_sideSnapTrim && !EqualRect(&landing, &settled))
		{
			g_landingFix.hwnd = hwnd;
			g_landingFix.landing = landing;
			g_landingFix.settled = settled;
		}
	}
	// The class and process say which app a snap belongs to
	WCHAR className[64] = {};
	GetClassNameW(hwnd, className, ARRAYSIZE(className));
	DWORD process = 0;
	GetWindowThreadProcessId(hwnd, &process);
	dbgprintf(L"explorer7: snap margins %d,%d,%d,%d, fitted %d, window %p class %s process %u",
		marginLeft, marginTop, marginRight, marginBottom, accepted, hwnd, className, process);
}

enum SnapReply { SNAP_NOT_OURS, SNAP_ANSWERED, SNAP_ZONE_ONLY };

// Fills the block with a half or a quarter, or only with the zone when the kernel keeps the top for itself
static SnapReply FillSnap(ShellWindowManagementCalloutInfo* info)
{
	// The block is in physical pixels, so every rectangle read here has to be too
	auto setContext = (SetThreadDpiAwarenessContext_t)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext");
	DPI_AWARENESS_CONTEXT previous = setContext ? setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) : NULL;

	SnapTarget target = {};
	RECT work;
	bool found = FindSnapTarget(info->point, &target, &work);
	RECT rc = target.rect;
	if (found && target.answer)
		FitSnapToFrame(info->hwnd, work, &rc);

	if (setContext && previous)
		setContext(previous);

	if (!found || !target.answer)
		g_landingFix.hwnd = NULL;
	if (!found)
		return SNAP_NOT_OURS;

	// Crossing the zone's edge brings the kernel back here, so a slide along an edge can turn a half into a quarter
	if (target.zoneValid)
	{
		info->regionValid = 1;
		info->region = target.zone;
	}
	if (!target.answer)
	{
		dbgprintf(L"explorer7: snap, point %d,%d input %u is the %s, kernel keeps it until it leaves %d,%d to %d,%d",
			info->point.x, info->point.y, info->inputType, target.name,
			target.zone.left, target.zone.top, target.zone.right, target.zone.bottom);
		return SNAP_ZONE_ONLY;
	}
	info->result = 1;
	info->rect = rc;
	info->arrangeKind = ARRANGE_KIND_NO_SUGGESTIONS;
	dbgprintf(L"explorer7: snap, point %d,%d input %u gives the %s %d,%d to %d,%d, zone %d,%d to %d,%d",
		info->point.x, info->point.y, info->inputType, target.name, rc.left, rc.top, rc.right, rc.bottom,
		target.zone.left, target.zone.top, target.zone.right, target.zone.bottom);
	return SNAP_ANSWERED;
}

// 19041 uses the rectangle as the window rectangle (TransformShellProvidedRectangles), so it goes out unfitted
// Its kernel refuses the answer unless the zone holds the cursor and the rectangle keeps to the work area
static bool FillSnap19041(ShellWindowManagementCalloutInfo19041* info)
{
	auto setContext = (SetThreadDpiAwarenessContext_t)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext");
	DPI_AWARENESS_CONTEXT previous = setContext ? setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) : NULL;
	SnapTarget target = {};
	RECT work;
	bool found = FindSnapTarget(info->point, &target, &work);
	if (setContext && previous)
		setContext(previous);

	if (!found || !target.answer || !target.zoneValid)
		return false;
	info->rect = target.rect;
	info->region = target.zone;
	dbgprintf(L"explorer7: snap 19041, point %d,%d input %u gives the %s %d,%d to %d,%d, zone %d,%d to %d,%d",
		info->point.x, info->point.y, info->inputType, target.name,
		target.rect.left, target.rect.top, target.rect.right, target.rect.bottom,
		target.zone.left, target.zone.top, target.zone.right, target.zone.bottom);
	return true;
}

//---Frame after the drop-----------------------------------

// Within a pixel, the kernel turns the answer into logical pixels and back
static bool NearlySameRect(const RECT& a, const RECT& b)
{
	auto close = [](LONG x, LONG y) { return x - y <= 1 && y - x <= 1; };
	return close(a.left, b.left) && close(a.top, b.top) && close(a.right, b.right) && close(a.bottom, b.bottom);
}

// An elevated app's window, which ValidateHwnd refuses to SetWindowPos from here
// The shell's own path, NtUserApplyWindowAction, skips that check while this thread has IAM access
static void SettleThroughWindowAction(HWND hwnd, const RECT& rect)
{
	if (!g_applyWindowAction)
		g_applyWindowAction = (NtUserApplyWindowAction_t)GetProcAddress(GetModuleHandleW(L"win32u.dll"), "NtUserApplyWindowAction");
	if (!g_applyWindowAction || !g_enableIAMAccess || !g_arrangeKeyKnown)
	{
		dbgprintf(L"explorer7: snap landing for %p, no window action path", hwnd);
		return;
	}

	// Position and size only, option bit 1 would make the kernel widen the rectangle again
	ShellWindowAction action = {};
	action.fields = WINDOW_ACTION_POSITION | WINDOW_ACTION_SIZE;
	action.position = { rect.left, rect.top };
	action.size = { rect.right - rect.left, rect.bottom - rect.top };

	ULONGLONG key = g_arrangeKey;
	BOOL access = g_enableIAMAccess(&key, TRUE);
	SetLastError(0);
	BOOL applied = g_applyWindowAction(hwnd, &action);
	DWORD error = GetLastError();
	if (access)
		g_enableIAMAccess(&key, FALSE);
	dbgprintf(L"explorer7: snap landing for %p, window action access %d applied %d error %u",
		hwnd, access, applied, error);
}

// Logs where the settled window really ended up
static void CALLBACK LandingCheckProc(HWND hwnd, UINT, UINT_PTR id, DWORD)
{
	KillTimer(hwnd, id);
	HWND target = g_landingCheck.hwnd;
	g_landingCheck.hwnd = NULL;

	auto setContext = (SetThreadDpiAwarenessContext_t)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext");
	DPI_AWARENESS_CONTEXT previous = setContext ? setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) : NULL;
	RECT now = {};
	BOOL read = target && GetWindowRect(target, &now);
	if (setContext && previous)
		setContext(previous);

	const RECT& want = g_landingCheck.want;
	dbgprintf(L"explorer7: snap landing for %p, 300 ms later %d,%d to %d,%d, wanted %d,%d to %d,%d, read %d",
		target, now.left, now.top, now.right, now.bottom, want.left, want.top, want.right, want.bottom, read);
}

// Takes the outer margins off and forgets the window once it sits where the kernel put it, false while it has not landed
static bool SettleIfLanded(const wchar_t* when)
{
	HWND target = g_landingFix.hwnd;
	const RECT settled = g_landingFix.settled;
	auto setContext = (SetThreadDpiAwarenessContext_t)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext");
	DPI_AWARENESS_CONTEXT previous = setContext ? setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) : NULL;

	// Async so a hung app cannot stall this thread, the kernel answers its callouts here
	RECT now;
	bool landed = target && GetWindowRect(target, &now) && NearlySameRect(now, g_landingFix.landing);
	BOOL sent = FALSE;
	DWORD error = 0;
	if (landed)
	{
		SetLastError(0);
		sent = SetWindowPos(target, NULL, settled.left, settled.top, settled.right - settled.left, settled.bottom - settled.top,
			SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
		error = GetLastError();

		// Elevated apps refuse it with access denied, the shell's window action path is tried instead
		if (!sent && error == ERROR_ACCESS_DENIED)
			SettleThroughWindowAction(target, settled);
	}

	if (setContext && previous)
		setContext(previous);

	if (landed)
	{
		dbgprintf(L"explorer7: snap landing for %p, moved from %d,%d to %d,%d onto %d,%d to %d,%d %s, sent %d error %u",
			target, now.left, now.top, now.right, now.bottom, settled.left, settled.top, settled.right, settled.bottom,
			when, sent, error);
		// Read back later, a fix that was sent but undone is invisible in the line above
		g_landingCheck.hwnd = target;
		g_landingCheck.want = settled;
		if (g_arrangeWnd)
			SetTimer(g_arrangeWnd, LANDING_CHECK_TIMER, 300, LandingCheckProc);
		g_landingFix.hwnd = NULL;
	}
	return landed;
}

static void StopLandingWatch()
{
	if (g_landingHook)
	{
		UnhookWinEvent(g_landingHook);
		g_landingHook = NULL;
	}
	if (g_arrangeWnd)
		KillTimer(g_arrangeWnd, LANDING_FIX_TIMER);
}

// Fires the moment the kernel places the window, no timer step to wait for
static void CALLBACK LandingProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD)
{
	if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd || hwnd != g_landingFix.hwnd)
		return;
	if (SettleIfLanded(L"on its location event"))
		StopLandingWatch();
}

// Half a second after the drop the window never landed there, so it was no snap
static void CALLBACK LandingTimeoutProc(HWND, UINT, UINT_PTR, DWORD)
{
	if (!SettleIfLanded(L"at the timeout"))
	{
		dbgprintf(L"explorer7: snap landing for %p, never landed", g_landingFix.hwnd);
		g_landingFix.hwnd = NULL;
	}
	StopLandingWatch();
}

// One event per drag, only the window named by the last snap answer is looked at
static void CALLBACK MoveSizeEndProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG, DWORD, DWORD)
{
	if (idObject != OBJID_WINDOW || !hwnd || hwnd != g_landingFix.hwnd || !g_arrangeWnd)
		return;
	StopLandingWatch();

	// Watching first and checking second, so a landing between the two cannot be missed
	DWORD process = 0;
	GetWindowThreadProcessId(hwnd, &process);
	g_landingHook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, NULL,
		LandingProc, process, 0, WINEVENT_OUTOFCONTEXT);
	if (SettleIfLanded(L"at the drop"))
	{
		StopLandingWatch();
		return;
	}
	SetTimer(g_arrangeWnd, LANDING_FIX_TIMER, 500, LandingTimeoutProc);
}

//---Windows that answer the kernel--------------------------

// Every other kind asks about the kernel's own defaults, not answering keeps them
static LRESULT CALLBACK ArrangementWndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
	if (msg == WM_SHELL_WINDOWMANAGEMENT_CALLOUT && g_callout19041)
	{
		auto info = (ShellWindowManagementCalloutInfo19041*)l;
		return info && info->kind == 0 && FillSnap19041(info) ? 1 : 0;
	}
	if (msg == WM_SHELL_WINDOWMANAGEMENT_CALLOUT)
	{
		auto info = (ShellWindowManagementCalloutInfo*)l;
		if (!info || info->kind != 0)
			return 0;
		return FillSnap(info) != SNAP_NOT_OURS ? 1 : 0;
	}
	return DefWindowProcW(hwnd, msg, w, l);
}

// twinui answers first when it registered its own window, ours only fills in a side it left empty
static LRESULT CALLBACK TwinuiArrangementWndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
	WNDPROC original = g_twinuiArrangeProc;
	if (msg == WM_NCDESTROY && original)
	{
		SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)original);
		g_twinuiArrangeProc = nullptr;
		g_twinuiArrangeWnd = nullptr;
	}
	LRESULT result = original ? CallWindowProcW(original, hwnd, msg, w, l) : DefWindowProcW(hwnd, msg, w, l);
	// 19041 has no result field, twinui answered when its window returned 1
	if (msg == WM_SHELL_WINDOWMANAGEMENT_CALLOUT && l && g_callout19041)
	{
		auto info = (ShellWindowManagementCalloutInfo19041*)l;
		if (result != 1 && info->kind == 0 && FillSnap19041(info))
			return 1;
	}
	else if (msg == WM_SHELL_WINDOWMANAGEMENT_CALLOUT && l)
	{
		auto info = (ShellWindowManagementCalloutInfo*)l;
		if (info->kind == 0 && info->result != 1 && FillSnap(info) != SNAP_NOT_OURS)
			return 1;
	}
	return result;
}

// Message only and per monitor aware, the kernel refuses any other window with error 87
static HWND CreateArrangementWindow()
{
	WNDCLASSEXW cls = { sizeof(cls) };
	cls.lpfnWndProc = ArrangementWndProc;
	cls.hInstance = GetModuleHandleW(NULL);
	cls.lpszClassName = L"Explorer7ArrangementShell";
	if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
		return NULL;

	auto setContext = (SetThreadDpiAwarenessContext_t)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext");
	DPI_AWARENESS_CONTEXT previous = setContext ? setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) : NULL;
	HWND hwnd = CreateWindowExW(0, cls.lpszClassName, NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, cls.hInstance, NULL);
	if (setContext && previous)
		setContext(previous);
	return hwnd;
}

// 19041 hands the registered window an Alt+F4 hotkey (id SC_CLOSE), which ours would swallow
// Dropping it keeps Alt+F4 as it was with no shell window registered
static void DropAltF4Hotkey19041()
{
	if (!g_callout19041 || !g_arrangeWnd)
		return;
	SetLastError(0);
	BOOL dropped = UnregisterHotKey(g_arrangeWnd, SC_CLOSE);
	dbgprintf(L"explorer7: side snap 19041 Alt+F4 hotkey dropped %d, error %u", dropped, GetLastError());
}

// Only the thread holding the key may register, and only while its access is switched on
static void RegisterArrangementWindow()
{
	if (!g_arrangeWnd)
		g_arrangeWnd = CreateArrangementWindow();
	if (!g_arrangeWnd)
	{
		dbgprintf(L"explorer7: side snap window not created, error %u", GetLastError());
		return;
	}

	// Out of context, so it lands on this thread's message loop next to the callouts
	if (!g_moveSizeEndHook)
		g_moveSizeEndHook = SetWinEventHook(EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND, NULL,
			MoveSizeEndProc, 0, 0, WINEVENT_OUTOFCONTEXT);

	ULONGLONG key = g_arrangeKey;
	BOOL access = g_enableIAMAccess(&key, TRUE);
	g_registeringOurs = true;
	SetLastError(0);
	BOOL registered = g_realRegisterArrangement(g_arrangeWnd, TRUE);
	DWORD error = GetLastError();
	g_registeringOurs = false;
	if (access)
		g_enableIAMAccess(&key, FALSE);

	g_arrangeWndRegistered = registered != FALSE;
	dbgprintf(L"explorer7: side snap window %p on thread %u, access %d, registered %d, error %u, build %u",
		g_arrangeWnd, GetCurrentThreadId(), access, registered, error, g_osVersion.BuildNumber());
	if (g_arrangeWndRegistered)
		DropAltF4Hotkey19041();
}

//---Hooks--------------------------------------------------

// twinui takes the key on its immersive thread, the one thread allowed to answer the kernel
static BOOL WINAPI AcquireIAMKey_Hook(ULONGLONG* key)
{
	BOOL ok = g_realAcquireIAMKey(key);
	dbgprintf(L"explorer7: IAM key taken on thread %u, ok %d", GetCurrentThreadId(), ok);
	if (ok && key && !g_arrangeKeyKnown)
	{
		g_arrangeKey = *key;
		g_arrangeKeyKnown = true;
		RegisterArrangementWindow();
	}
	return ok;
}

// When twinui brings its own window, it gets the slot and ours fills in behind it
static BOOL WINAPI RegisterArrangement_Hook(HWND hwnd, BOOL enable)
{
	if (g_registeringOurs || hwnd == g_arrangeWnd)
		return g_realRegisterArrangement(hwnd, enable);

	if (enable && g_arrangeWndRegistered)
	{
		g_realRegisterArrangement(g_arrangeWnd, FALSE);
		g_arrangeWndRegistered = false;
	}

	BOOL ok = g_realRegisterArrangement(hwnd, enable);
	DWORD error = GetLastError();
	if (ok && enable && !g_twinuiArrangeProc)
	{
		g_twinuiArrangeWnd = hwnd;
		g_twinuiArrangeProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)TwinuiArrangementWndProc);
	}
	else if (ok && !enable && g_arrangeWnd && GetWindowThreadProcessId(g_arrangeWnd, NULL) == GetCurrentThreadId())
	{
		// twinui let go of the slot, its access is still on here so ours can take it back
		g_arrangeWndRegistered = g_realRegisterArrangement(g_arrangeWnd, TRUE) != FALSE;
		if (g_arrangeWndRegistered)
			DropAltF4Hotkey19041();
	}
	dbgprintf(L"explorer7: twinui arrangement window %p enable %d, ok %d, error %u, ours registered %d",
		hwnd, enable, ok, error, g_arrangeWndRegistered);
	SetLastError(error);
	return ok;
}

// 19041 registers through user32 ordinal 2564, a stub that hands 0x67 to NtUserCallHwndParam
typedef BOOL(WINAPI* NtUserCallHwndParam_t)(HWND hwnd, ULONG_PTR param, DWORD procedure);
static NtUserCallHwndParam_t g_callHwndParam;
#define REGISTER_ARRANGEMENT_PROCEDURE_19041 0x67

// The stub's own call, made directly because MinhookImports turns ordinal 2564 into a no-op for twinui
static BOOL WINAPI RegisterArrangement19041(HWND hwnd, BOOL enable)
{
	return g_callHwndParam(hwnd, (ULONG_PTR)(LONG_PTR)enable, REGISTER_ARRANGEMENT_PROCEDURE_19041);
}

// True when user32 still holds the 19041 stub, with or without that no-op's jump over its first bytes
static bool Check19041RegistrationStub(HMODULE win32u)
{
	HMODULE user32 = GetModuleHandleW(L"user32.dll");
	BYTE* stub = user32 ? (BYTE*)GetProcAddress(user32, MAKEINTRESOURCEA(2564)) : nullptr;
	g_callHwndParam = win32u ? (NtUserCallHwndParam_t)GetProcAddress(win32u, "NtUserCallHwndParam") : nullptr;

	// The procedure number and the jump into win32u sit past the five bytes a hook overwrites
	static const BYTE clean[] = { 0x48, 0x63, 0xD2, 0x41, 0xB8 };
	static const BYTE tail[] = { 0x67, 0x00, 0x00, 0x00, 0x48, 0xFF, 0x25 };
	bool start = stub && (memcmp(stub, clean, sizeof(clean)) == 0 || stub[0] == 0xE9);
	bool found = start && memcmp(stub + 5, tail, sizeof(tail)) == 0 && g_callHwndParam;
	dbgprintf(L"explorer7: side snap 19041 registration stub %p, first byte %02X, found %d",
		stub, stub ? stub[0] : 0, found);
	return found;
}

// Armed before twinui loads, so the key it takes on its immersive thread is seen
static void InstallWindowArrangementShell()
{
	static bool installed = false;
	if (installed)
		return;
	installed = true;

	DWORD build = g_osVersion.BuildNumber();
	if (build < 19041)
		return;
	g_callout19041 = build < 22000;

	DWORD enabled = 1;
	g_registry.QueryValue(L"Win10SideSnap", (LPBYTE)&enabled, sizeof(enabled));
	if (!enabled)
	{
		dbgprintf(L"explorer7: side snap switched off");
		return;
	}

	// Switches for the frame fit, the fix after the drop and quarter snapping in the corners, all on by default
	DWORD fit = 1, trim = 1, corners = 1;
	g_registry.QueryValue(L"Win10SideSnapFit", (LPBYTE)&fit, sizeof(fit));
	g_registry.QueryValue(L"Win10SideSnapTrim", (LPBYTE)&trim, sizeof(trim));
	g_registry.QueryValue(L"Win10CornerSnap", (LPBYTE)&corners, sizeof(corners));
	g_sideSnapFit = fit != 0;
	g_sideSnapTrim = trim != 0;
	g_cornerSnap = corners != 0;
	dbgprintf(L"explorer7: side snap fit %d trim %d corners %d, build %u", g_sideSnapFit, g_sideSnapTrim, g_cornerSnap, build);

	// 19041 snaps halves by itself, so it only needs the shell for corners
	if (g_callout19041 && !g_cornerSnap)
		return;

	HMODULE win32u = GetModuleHandleW(L"win32u.dll");
	if (!win32u)
		win32u = LoadLibraryW(L"win32u.dll");
	void* acquire = win32u ? (void*)GetProcAddress(win32u, "NtUserAcquireIAMKey") : nullptr;
	g_enableIAMAccess = win32u ? (NtUserEnableIAMAccess_t)GetProcAddress(win32u, "NtUserEnableIAMAccess") : nullptr;

	// 19041 registers ours with a direct call, twinui's own attempt already goes nowhere so there is nothing to hook
	void* registerArrangement = nullptr;
	bool registerReady = false;
	if (g_callout19041)
	{
		registerReady = Check19041RegistrationStub(win32u);
		if (registerReady)
			g_realRegisterArrangement = RegisterArrangement19041;
	}
	else
	{
		registerArrangement = win32u ? (void*)GetProcAddress(win32u, "NtUserRegisterWindowArrangementCallout") : nullptr;
		registerReady = registerArrangement != nullptr;
	}
	if (!acquire || !registerReady || !g_enableIAMAccess)
	{
		dbgprintf(L"explorer7: side snap needs the arrangement callout, this build has none");
		return;
	}

	MH_Initialize();
	bool hooked =
		MH_CreateHook(acquire, (void*)AcquireIAMKey_Hook, (void**)&g_realAcquireIAMKey) == MH_OK &&
		MH_EnableHook(acquire) == MH_OK;
	if (hooked && registerArrangement)
		hooked = MH_CreateHook(registerArrangement, (void*)RegisterArrangement_Hook, (void**)&g_realRegisterArrangement) == MH_OK &&
			MH_EnableHook(registerArrangement) == MH_OK;
	dbgprintf(L"explorer7: side snap hooks %d, build %u", hooked, g_osVersion.BuildNumber());
}
