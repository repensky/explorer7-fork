# Tray icon tooltip placement

Windows 7 put the tooltip for a notification area icon hard against the
notification area itself. Later builds let the tooltip control place it near
the cursor, which reads wrong under the Windows 7 shell. The wrapper puts it
back, in `FlyoutFix.cpp`, installed from `HookAPIs` as
`InstallTrayTooltipFix`.

Ported on 2026-09-16 from the Windhawk mod **Legacy Tray Icon Tooltips** by
yanashubby (kieldbg), `Win11Restore\scripts\legacy-tray-tooltipsfix.cpp`.
The logic is that mod's, restructured into early returns. Credit belongs
there, and the license should be confirmed before this ships, the mod
declares none and Explorer7 is GPLv3.

## How it works

`user32!SetWindowPos` is detoured with MinHook, which catches the tooltip
control wherever it lives. A move that carries `SWP_NOMOVE`, a window whose
class is not `tooltips_class32`, or a tooltip with the balloon style is passed
straight through, so the cost on the common path is one flag test and one
class name read.

A tooltip does not say which icon it belongs to, so three tests in order decide
whether it is a tray one:

1. its owner is a descendant of `TrayNotifyWnd`
2. its owner is `Shell_TrayWnd` itself and the cursor is over the notify area,
   which is the only way to tell those apart
3. it has no owner, and either the window under the cursor is inside the notify
   area or the cursor is simply within its rectangle

It is then anchored on `ToolbarWindow32` inside `SysPager`, falling back to the
pager and then to the notify area. The taskbar's own rectangle gives the
orientation, and its position against the monitor midpoint gives the docked
edge, so all four edges place the tooltip on the correct side. `SWP_NOSIZE`
means the size has to be read back off the window before the offset can be
worked out. The call is finally reissued as `HWND_TOPMOST` with `SWP_NOZORDER`
cleared, otherwise a maximized window clips the tooltip.

## Not gated by build

There is no version check. The Windows 7 placement is what the shell should
have on every build the wrapper supports, and the hook only acts on tooltips
that belong to the notification area, so there is nothing for it to break on
8.1 or 10. Add a gate only if a build turns up where the stock placement is
already correct.
