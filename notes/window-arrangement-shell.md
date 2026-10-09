# Side snapping on Windows 11 (window arrangement shell)

Symptom (26100 VM, 2026-10-05): with the Windows 11 explorer running, dragging a window to
the left or right edge snaps it to half the screen. Under Explorer7 only the top edge snaps
(maximize).

## Why (read from win32kfull 26100.9168 with cdb)

- `xxxGetArrangeRectFromHitTarget` asks the shell first (`xxxGetArrangeRectFromShell`).
  When the shell gives nothing it only has a fallback for the top edge, the monitor work
  rect clamped to the track size. Left, right and bottom return false, so no preview and no
  snap. Windows 10 (`xxxSizeRectFromHitTarget` in 19041) halved the work area itself.
- The shell is asked through `CallShell::xxxArrangementInfoHandler` ->
  `xxxCallIAMWindowManagementHandler`, a `SendMessageTimeout` of message 0x341 with a two
  second timeout to the window held at desktop +0x148. The answer counts when the message
  returns 1. A timeout on this kind cuts the shell connection.
- lParam is `_SHELL_WINDOWMANAGEMENT_CALLOUT_INFO`, 0x68 bytes, zeroed by the kernel:
  +0x00 dragged HWND, +0x08 kind (0 = arrangement rect), +0x10 cursor (physical),
  +0x18 input flags, +0x20 result (1 = rect below is the target), +0x24 RECT,
  +0x34 region valid, +0x38 region RECT, +0x48 shrink width, +0x50 insert after HWND,
  +0x58 copied to the move data (twinui writes 5, or 10 with window suggestions).
- With result 1 the rect must lie inside the monitor rect and touch two adjacent edges or
  two opposite ones (`GetFrameBoundsOverlapInfo`, `ArrangementStyleFromOverlap`), then the
  kernel converts it to logical pixels and adds the window margins. A work area half passes
  for every taskbar position.
- The info carries no edge. twinui hit tests the cursor itself
  (`WindowDragInputProvider::OnGetArrangementRects`), the kernel takes whatever rect comes
  back. The kernel only asks again when the edge changes or the cursor crosses the region.

## Registration (`_RegisterWindowArrangementCallout`)

Fails with 5 unless the caller is the shell process and THE IAM thread (desktop +0x120,
set by `NtUserAcquireIAMKey`) with IAM access switched on (`NtUserEnableIAMAccess`). Fails
with 87 unless the window is message only and per monitor DPI aware (window DPI context
low nibble 2). Fails with 0x4DA when a window is already registered.

The key can be taken once per desktop. Explorer7 starts twinui's immersive shell in process
(`CreateTwinUI_UWP`), so twinui already holds it there.

## What the wrapper does (`WindowArrangementShell.h`, build 22000 and up)

- Hooks win32u `NtUserAcquireIAMKey` before twinui loads. When twinui's call succeeds, the
  hook is on the IAM thread: it keeps the key, creates a message only per monitor aware
  window there, switches access on, registers it, and switches access off again.
- Its window answers kind 0 with the Windows 10 half of the work area when the cursor is
  nearer the left or right edge than the top or bottom. Top and bottom, and every other
  kind, return 0, which leaves the kernel's own behaviour exactly as unregistered.
- Hooks win32u `NtUserRegisterWindowArrangementCallout`. If twinui registers its own window
  later, ours steps aside, twinui's window is subclassed, and ours fills in a side only when
  twinui left the result empty. If twinui lets go, ours takes the slot back.
- Registry value `Win10SideSnap` = 0 under the Explorer7 key switches it off.

Other things registration routes to the shell, checked so the zero answer is safe:
ShowWindow policy is gated by `ShellWindowManagement::BehaviorEnabled` (flags the wrapper
never sets), Explorer7's own hotkeys keep going through `PostMessage`, and the 0x342 shell
notifications are `SendNotifyMessage`, ignored by `DefWindowProc`.

## Overlapping frames, 2026-10-05, done

Final state, confirmed on the VM: sides and the middle seam fit in the answer, the bottom is
trimmed on the window's own location event right after the drop. The brief overflow under the
taskbar is not visible, so the user accepted it as final.

With OpenGlass restoring Windows 10 thick frames, two windows snapped side by side overlapped
by the frame width. Each frame sat 8 px over the other window's client area, where Windows 7
has the two dark outer lines touching.

- The answer is a visible frame. With `Feature_ApplyWindowActionConvergence2` on,
  `xxxGetArrangeRectFromShell+0x383` calls `WindowMargins::ExtendRect`, which grows it by
  `GetWindowMargins`. That reads the window's `CWindowMarginProp` (the sizing border for any DWM
  framed window), or 0 without the property. With the feature off it goes through
  `TransformShellProvidedRectangle` instead, not read.
- DWM cannot zero those margins. `DwmSetBorderMargins` (a `SetWindowCompositionAttribute`
  attribute) only accepts the window's own thread, and all zero margins clear the override.
- On 26100 `DwmGetWindowAttribute` calls only `GetWindowCompositionAttribute` and
  `GetDpiForWindow`. `NtUserGetWindowCompositionAttribute` answers the frame bounds with
  `WindowMargins::GetPhysicalFrameBounds`, which reads the same `CWindowMarginProp`.

PowerToys FancyZones uses the same `GetWindowRect` minus `DWMWA_EXTENDED_FRAME_BOUNDS` pair to
place windows on Windows 11.

### First attempt broke side snapping

Taking the margins off all four sides made every answer fail the kernel's check, so nothing
snapped. `xxxGetArrangeRectFromShell+0x29f` needs `IntersectRect(answer, work area)` to equal
the answer. The work area is monitor +0x2C, as `GetMonitorWorkRectForDpi` copies it, and the
monitor rect is at +0x1C. Then `GetFrameBoundsOverlapInfo` sets one bit per edge equal to the work
area (1 left, 2 top, 4 right, 8 bottom), and `ArrangementStyleFromOverlap` accepts two adjacent
edges or exactly 5 or 10. The fully shrunk half touched only the top.

`TransformShellProvidedRectangle`, the branch used with the feature off, also calls
`ExtendRect` and then clamps to the track size. Both branches widen, so an exact fit on all four
sides is impossible.

### What it does now

`FitSideSnapToFrame` takes off only the left and right margins, keeping top and bottom on the
work area (edges 10, accepted). The kernel adds them back, so the outer side frame sits on the
screen edge and the two snapped frames meet in the middle, as on Windows 7 (confirmed on the VM).
`KernelAcceptsSnapRect` mirrors the kernel's test and keeps the plain half whenever the fitted
one would fail. The log line is `side snap margins l,t,r,b, fitted 1`.

The bottom frame still lands one margin below the work area, under a bottom taskbar. Only result
1 returns a rectangle (0 and 2 just store the info), so the answer cannot fix that. It is
trimmed after the drop:

- `FitSideSnapToFrame` remembers the window and where the kernel will put it.
- A `WINEVENT_OUTOFCONTEXT` hook on `EVENT_SYSTEM_MOVESIZEEND`, on the IAM thread, starts a
  30 ms timer. Up to ten tries wait for the window to sit at that rectangle, within a pixel.
- Then `SetWindowPos(SWP_NOMOVE | SWP_ASYNCWINDOWPOS ...)` takes the bottom margin off.
  Log line `side snap bottom for <hwnd>, trimmed n, tries n`.
- The window stays snapped. `IsArranged` reads only a window flag (`[wnd+0x28]+0xE9` bit 4
  with the feature on). `MarkWindowAsArranged` sets it, `MarkWindowAsNotArranged` clears it,
  and only `xxxMinMaximizeEx` and `AddWFFULLSCREEN` call the latter, never `SetWindowPos`.
- Margins can't be zeroed at the source. With NC rendering on, `CalculateWindowMargins` works
  them out from the window borders, and the only override (`DwmSetBorderMargins`) is per window
  from its own thread.

### Why 19041 snaps the bottom exactly

The 19041 host also runs Explorer7, so its kernel builds the rectangle itself.
`xxxSizeRectFromHitTarget` (win32kfull 19041.7058) halves the work area and also widens it, with
`ExtendRectByWindowMargin`. But its margins come from DWM. `GetWindowExtendedMargin` reads four
shorts from the window's composition info (`GetWindowCompositionInfo`), and
`xxxProcessUpdateFrameMargins` stores what DWM pushes there. OpenGlass's frames make DWM push
zero, so nothing is added and the window ends exactly on the work area.

24H2 removed `GetWindowExtendedMargin`, `ExtendRectByWindowMargin` and
`xxxProcessUpdateFrameMargins` (none in the IDA name list). The composition info itself survives
as `_GetWindowCompositionInfo` / `SetWindowCompositionInfo`. Its 26 readers are
`DwmWindowCreate`, `GetWindowCompositionCornerStyle`, the `SetWindowComposition*` attribute
setters and `NtUserGetWindowCompositionAttribute`. No `WindowMargins` or arrangement code reads it
(IDA xrefs). An earlier cdb lookup missed the leading underscore and called it removed. The only
frame margin function left, `DwmAsyncNotifyWindowFrameMarginsChange`, tells DWM afterwards. `CalculateContentRect` works
the margins out from styles and `GetResizeBorderWidthForDpiWithAppCompat2`, so DWM has no input.
The shell is asked only during the drag (`xxxUpdateArrangeDataForMove` ->
`xxxGetArrangeRectFromHitTarget` -> `xxxGetArrangeRectFromShell`, one caller each). Nothing is
asked at the drop, which is why the bottom can only be trimmed afterwards. Checked as a full xref
pass, not a call scan only: direct calls, RIP relative loads, 64 bit pointers and 32 bit RVAs
outside code (the CFG table among them). `xxxGetArrangeRectFromShell`,
`xxxArrangementInfoHandler`, `xxxGetArrangeRectFromHitTarget`, `xxxUpdateArrangeDataForMove`
(called only by `xxxSizeOrMoveRect`), `MarkWindowAsNotArranged`, `ExtendRect` and
`DwmSetBorderMargins` have no references beyond their direct calls and `.pdata`, so none is
reached through a pointer. The trim no longer
polls. At MOVESIZEEND it first hooks `EVENT_OBJECT_LOCATIONCHANGE` for the app's process (out of
context), then checks once. Whichever sees the window at its landing rectangle trims it, the
event firing the moment the kernel places the window. A 500 ms timer removes the hook if it never
lands. Log lines end in `at the drop`, `on its location event`, `at the timeout` or
`never landed`. A timer step could not go below `USER_TIMER_MINIMUM` (10 ms, about 15.6 ms in practice).

### Elevated apps

The trim failed only for windows of elevated processes: mmc and cmd (both high integrity, read
from their tokens) logged `sent 0 error 5`, while Explorer and Notepad (medium) trimmed. On 24H2
the integrity check is inside `ValidateHwnd`, so any `SetWindowPos` from medium Explorer, async
or not, is refused for a higher window.

The shell's own route is `NtUserApplyWindowAction(hwnd, _WINDOW_ACTION*)`, exported by win32u.
`WindowActions::xxxApplyAction` (feature on) and the old path both:
- need the calling thread per monitor aware, error 5023 otherwise
- call `IAMThreadAccessGranted`, true when `FindIAMThread` finds this thread, which
  `NtUserEnableIAMAccess` with the key adds. Then they build `CDisableILCheckAuto` before
  `ValidateHwnd`, which skips the integrity check
- without IAM access, refuse any window of another thread with error 5

`_WINDOW_ACTION` is 0x60 bytes (`ResolvePublicWindowAction`): +0 field mask (2 position, 4 size,
8 insert-after, 0x20 state at +0x28, 0x40 16 bytes at +0x2C), +4 options, +0xC POINT, +0x14 SIZE,
+0x20 HWND. Options bit 1 marks the rectangle as a visible frame, and `xxxApplyWindowAction` then
calls `ExtendRect` on it, so the trim leaves it clear and passes a window rectangle. Positions are
physical and converted for windows that are not per monitor aware.

`TrimThroughWindowAction` runs when `SetWindowPos` fails with `ERROR_ACCESS_DENIED`. It turns IAM
access on with the key, applies position plus size, then turns it off again. Log line:
`window action access 1 applied 1 error 0`. Confirmed on the VM 2026-10-05 with an elevated mmc:
`sent 0 error 5`, then `window action access 1 applied 1 error 0`, and the read back 300 ms later
showed the bottom at the work area (1249).

### Dragging a snapped window out lands the cursor on the menu bar

19041 holds a snapped window for a while, then puts the restored window back so the cursor sits
at its original grab point. On 24H2 the cursor stays as far below the title bar as it had been
pulled. Confirmed with a drag log: grabbed at y 15 to 27, the window let go at cursor y 53 to 65,
and the restored window stayed at top 0. The bottom trim and side fit were ruled out by switching
both off.

- The kernel decides it, in `xxxSizeOrMoveRect`. 24H2 places the restored rectangle (window
  property, atom at session +41494) with `GetRestoreAroundCursorOffset(currentSize,
  cursorOffsetNow, restoreSize)`, which returns the current offset while it is under half the
  restore height. 19041 used the offsets stored at the grab (MOVESIZEDATA dwords 73 and 74).
  Nothing there calls the shell.
- The hold is the mouse side-move threshold, SPI 0x88/0x89, 50 px from the top of the screen.
  For an arranged window moved by its caption, `xxxInitializeMoveSizeData` starts the selector at
  4 (0x2000 in bits 11 to 13 of +0xC8), and `HitTargetAndMonitorFromPoint` keeps the window
  snapped while the cursor is inside that top band.
- The threshold table is session +63584, six selectors of four bytes each (top, left, right,
  bottom): mouse dock 0x7F, pen dock 0x81, mouse drag-out 0x85, pen drag-out 0x87, mouse
  side-move 0x89, pen side-move 0x8B. The GET codes return only the left byte. A SET takes its
  value in **uiParam**, not pvParam (`xxxSystemParametersInfoWorker(action, uiParam, pvParam,
  winIni)`). The value must be at least the mouse dock value and at most the pen value of the same
  kind, and a SET outside that range is silently ignored.
- twinui `CShellSnapComponent::SetDockThreshold` pushes 1/30/24/30/50/50 (dock, pen dock,
  drag-out, pen drag-out, side-move, pen side-move) with the Settings checkbox off, and 64/138/64
  with it on, through its `SystemParametersInfoW` import.

Tried and removed: a `SystemParametersInfoW` hook forcing 0x89 to 1 made the window let go on the
first movement with the title bar under the cursor (confirmed by hand on the VM), but it drops the
19041 hold, so the user chose to keep Windows' value (50, the same as 19041). Left as a 24H2 kernel
limitation. At let-go the restored window keeps its top where the snapped window was. With a 50 px
band the cursor is always at least 50 px below that top, below any title bar. No shell callout
or user mode state reaches the offset `RecomputeMouseOffset` stores for the rest of the drag.

The window's own `WM_MOVING` reply does reach the rectangle, which is the route now taken
(Windhawk mod `snap-drag-out-grab-point`, `Win11Restore\scripts\snapdragout.cpp`, built
2026-10-05, not yet run on the VM). Read from the 26100 corpus:

- `xxxTM_MoveDragRect` sends `WM_MOVING` with a copy of the new rect, then
  `xxxDrawDragRectEx(a1, &copy, ..., a1+24)` stores whatever the window returned as the current
  drag rect (`*a4 = *a2`).
- The next move starts from that rect, but `MoveDragRect` places it absolutely from the cursor and
  the stored offset, and with `Feature_ApplyWindowActionConvergence2` on the offset is not rebased
  to the reply. So the reply holds for one move only and has to be given on every `WM_MOVING`.
- At button up `xxxMS_TrackMove` draws with a null rect (keeps +24) and `xxxCommitMoveSize`
  commits +24, so the last reply is where the window lands.
- A caption drag reaches the window as `WM_SYSCOMMAND` 0xF012 (`xxxHandleNCMouseGuys`, hit 2 |
  `SC_MOVE`), and the move loop runs inside `DefWindowProc` handling it.
- user32 `IsWindowArranged` reads WND byte +0xE9 bit 0x10, the same bit the kernel's
  `IsArranged` reads when the convergence feature is on.

The mod hooks the `DefWindowProcW/A` and `DefDlgProcW/A` exports (forwarded to ntdll stubs on
26100). On `SC_MOVE` | `HTCAPTION` for an arranged window it records the grab offset from the
window top and runs the loop. On each `WM_MOVING` after the rect leaves the snapped place or size,
it sets top to cursor y minus that offset. x is left to the kernel.

### Every way to zero the margins, checked 2026-10-05

`CalculateWindowMargins` returns zero margins in exactly three cases:

- NC rendering is off for the window, so DWM draws no frame at all. Not usable.
- Session +0x52C4 bit 0 is set. That is the High Contrast flag (`SetHighContrastWorker`
  writes it, `HighContrastHotKey` clears it), so it can't be set on its own.
- An override set through `SetWindowCompositionAttribute` attribute 0x26 (38), routed to
  `DwmSetBorderMargins`. Four shorts, the caller must be the window's own thread
  (`pwnd->pti == PtiCurrent`, else 0xC0000022). Negative values fail, and all zero clears the
  override instead of setting it, so the smallest override still leaves 1 px on one side.

The margins also feed `DeferWindowPos`, `xxxApplyWindowAction`, `xxxModifyActionForArrangement`,
`xxxVerticalMaximize`, `xxxRestoreToPosAndState`, monitor migration, `xxxDrawDragRectEx` and
`AdjustFinalDragRectToKeepCaptionOnScreen`. So an override would line up all of window
management with the visible thick frame, not just side snapping. The cost is a mod injected
into every GUI process, setting it on each top level window from its owning thread.

## Corner (quarter) snapping, 2026-10-06, confirmed on the VM

Registry `Win10CornerSnap` (DWORD, HKCU then HKLM `...\Explorer\Advanced`, default 1, read at
start). 0 keeps the old nearest edge halves. Read from the binaries:

- **The kernel never decides a corner.** `HitTargetAndMonitorFromPoint` tests the left and
  right threshold first and the top second, so a corner reaches the shell as a left or right
  hit. `xxxGetArrangeRectFromHitTarget` asks the shell for every direction, the top included,
  and only maximizes by itself when nobody answers.
- **Re-asking along an edge is the region in the answer.** Block +0x34 bit 0 and the RECT at
  +0x38 go to MOVESIZEDATA +372/+356 (`SHData_StoreShellArrangeInfo`), with +373 holding
  whether the cursor was inside. `SHData_NeedsArrangementCallout` asks again when that flips.
  `xxxUpdateArrangeDataForMove` clears the region (`SHData_ResetRuntimeState`) before every
  ask, so no stale region survives. 24H2 twinui (`WindowDragInputProvider::OnGetArrangementRects`)
  always fills it as x, y, x+w, y+h in physical pixels.
- **A handled reply with result 0 still stores the region.** The kernel counts the reply only
  when the window procedure returns 1 (`xxxCallIAMWindowManagementHandler`). Result 0 or 2 go to
  the store and return false. So the middle of the top edge answers zone only, the kernel
  maximizes, and sliding into a corner band brings it back.
- **The bands**, 24H2 `CustomDragout::DetermineSnapRegionFromPoint` fed by
  `CShellSnapComponent::GetSnapRegionAndSafetyRectForPoint`: a mouse gets 10 percent of the
  monitor rect each way, anything else 138 px. The left band (x <= work.left + band) wins, then
  the right band, then top (y <= work.top + band) and bottom (y >= work.bottom - band). Left plus
  top is the top left quarter, left alone is the left half, the middle of the top is maximize.
  19041 twinui (`ComputeSnapTargetDataForRegionHelper`, `s_DefaultSnapTargetMetrics` 25 px or
  0.1 per side) uses the same 10 percent zones for input type 4 or 8.
- **The input type** is block +0x1C, which the kernel copies from a THREADINFO dword. 24H2 twinui
  never reads it. A mouse drag on the VM sends **2**, so the first build, which guessed 2 meant
  touch, gave the mouse 138 px zones. Every input now gets the 10 percent zones and the value is
  only logged as `input`.
- **The split** is Windows 10's `DetermineDefaultSnapRegionRect`: 0.5 floats truncated,
  `start + size * 0.5` and `end - size * 0.5`. On the VM's 3063 wide work area both halves now
  meet at 1531, the old integer math left a 1 px gap at 1531.
- **Fitting a quarter.** The kernel needs two adjacent work area sides, so a quarter keeps its
  two outer sides and only the inner ones lose their margins. The kernel's widening then pushes
  the outer side 7 px off screen (left or right) and the bottom quarters 7 px under the taskbar.
  The drop fix now sets the whole settled rectangle (`SettleIfLanded`, position and size) instead
  of trimming the bottom. Halves land exactly as before.

## Corners on 19041, 2026-10-06, built, not yet run

Read from win32kfull 19041.7058 (the host's own copy, hash checked) and the 19044 user32:

- **The same callout exists.** `xxxSizeRectFromHitTarget` calls `xxxGetSizeRectFromShell` first and
  only falls back to halves (`ExtendRectByWindowMargin`, DWM pushed margins) when nobody answers.
  It asks for the left and right hits only (direction 1 or 2), never the top, so 19041 corners come
  from the side edges and the top edge always maximizes, as stock Windows 10 does.
- **The block is shorter**, 0x40 bytes (`CallShell::xxxArrangementRectangleHandler`): hwnd +0,
  kind +8, point +0x0C, flags +0x14, input type +0x18, rect +0x20, zone +0x30, no result field.
  The reply counts when the window procedure returns 1.
- **The kernel checks the reply**: the zone must hold the cursor (`PtInRect`), the rect must sit
  inside the work area, and `ArrangementStyleFromOverlap` must give 55553 (0xD901) for a left hit
  or 55554 for a right one. Left plus top and left plus bottom both give 55553, so left quarters
  pass a left hit, right quarters a right one.
- **No margins are added.** `TransformShellProvidedRectangles` converts the rect into the dragged
  window's coordinates, clips it to the work area, pins the side to the work area edge and clamps
  to the min and max track size. The answer is the window rectangle, so no fit and no fix after
  the drop.
- **Re-asking is the zone**, `ComputeMoveOutcome` asks again once the cursor leaves MOVESIZEDATA
  +0x108 on the same monitor and side.
- **Registration** is user32 ordinal 2564 (no name), `movsxd rdx,edx; mov r8d,67h; jmp
  [__imp_NtUserCallHwndParam]`, bytes `48 63 D2 41 B8 67 00 00 00 48 FF 25`.
  `NtUserAcquireIAMKey` and `NtUserEnableIAMAccess` are win32u exports with the same arguments.
- **Explorer7 already patches ordinal 2564 to return TRUE** (`MinhookImports.h`, the
  `c_retTrueOrdinals` list, every build). That is why twinui never registers inside Explorer7 and
  why the host had no quarters. The first 19041 build hooked the stub, found `E9` over its first
  five bytes in explorer (the no-op's jump) and gave up. Ours now checks the stub with or without
  that jump (bytes 5 to 11 hold `67 00 00 00 48 FF 25`) and calls win32u
  `NtUserCallHwndParam(hwnd, enable, 0x67)` itself, hooking nothing for registration.
- **Registering binds Alt+F4 to the window** on 19041 (`_RegisterHotKey` id 0xF060, modifiers
  0x7001, key 0x73). 24H2 dropped that. Ours unregisters it right after registering so Alt+F4
  works as it does with no shell window.

Only installed when `Win10CornerSnap` is on (halves need nothing on 19041) and the immersive shell
stack runs (`CreateTwinUI_UWP`), since that is where twinui takes the key. Log lines start
`snap 19041`.

## Not verified

- Only 26100 was read. 22000 to 23H2 use the same win32u names, the block layout there is
  assumed.
- Whether twinui's own drag provider registers inside Explorer7. The log line
  `twinui arrangement window` appears only if it does.
- Corner quarters from the side edges are confirmed in the log (top left and bottom right
  landed and settled exactly). The zone only reply on the top edge has not shown in a log yet.

Log lines to look for: `IAM key taken on thread`, `side snap window ... registered 1`, then
one `snap, point` line per edge hit and a `snap landing` line after each drop.
