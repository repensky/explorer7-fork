# Win+X power user menu (`EnableWinXMenu`)

Replaces the Start button's right-click menu with a Windows 10 style power user
menu and makes Win+X open it. Off unless `Explorer\Advanced\EnableWinXMenu` is
nonzero, read once at startup. Code is `WinXMenu.cpp`, with small hooks in
`dllmain.cpp` (tray subclass, `ShimDesktop`, `HookAPIs`) and `util.h`
(`RegisterWindowHotkeyNew`). It replaces the `win7-winx-power-menu` Windhawk mod.

Read from `exports\win7\explorer7601.exe` and `explorer7850.exe` (both with PDBs),
`exports\19044\twinui.dll`, `exports\19044\shutdownux.dll`, and
`exports\24H2\twinui.pcshell.dll`, `twinui.dll`, `explorer.exe`, `win32kfull.sys`.

## The Start button menu

Identical logic in 7601 and 7850:

- `CStartButton::_StartButtonSubclassProc` takes `WM_CONTEXTMENU` and calls
  `CStartButton::OnContextMenu` (7601 `0x10003F75C`, 7850 `0x1000515B0`) unless
  `REST_NOTRAYCONTEXTMENU` is set
- The menu is built in code, no resource. Properties is command `0x7FF3`, Open
  Windows Explorer is `0x7FF1`
- Mouse: `TrackPopupMenu` through explorer's own import, flags `0x102`, owner is
  the Start button
- Keyboard (`lParam == -1`): `CStartButton::TrackMenu` calls `TrackPopupMenuEx`,
  flags `0x160`, placed on the button with a `TPMPARAMS` excluding its rect
- `TrackMenu` is also used for the tray system menu with the same owner, which
  is why the hook also tests for command `0x7FF3`
- The Start button is a `Button` popup owned by `Shell_TrayWnd`, with the
  property `StartButtonTag` set by `CTray::_CreateWindows`

The wrapper patches explorer's imports of both functions. A menu owned by a
window with `StartButtonTag` that holds `0x7FF3` is swapped for ours and
explorer gets 0 back.

Placement: explorer's keyboard route excludes the whole button rect, which left
a gap between the menu and the taskbar on the VM (screenshot, 2026-10-09).
`PlaceOnTaskbar` instead excludes the tray rect and anchors on the tray edge
beside the button, bottom aligned on a bottom taskbar, for both the right click
and Win+X. Windows 10 docks its menu the same way, at the work area corner
(19044 `CLauncherTipContextMenu::ShowLauncherTipContextMenu` with a null point).
Explorer's own place is kept if the tray cannot be measured.

## Win+X

Who holds the hotkey inside explorer7's process:

| Build | Registered by | Handler |
|---|---|---|
| 19044 | twinui `CImmersiveWindowMessageService::RequestHotkeys`, id `0xF`, mods `0x4008`, vk `X`, table entry 10 at RVA `0x5B5160` | `OnMessage` case `0xF` calls `_ShowLauncherTipContextMenu` (RVA `0x25E0E0`), which `DisableWinXMenu` patches |
| 26100 | twinui (not twinui.pcshell), same id and mods, row 7 at `0x1803E2178` | `OnMessage` case `0xF` calls `IsDesktopInputContext` and returns, the menu code is gone |

- twinui never checks the `RegisterHotKey` result on either build
- The Windows 7 tray registers no `X` (ids 500 to 565 in 7601, 500 to 566 in 7850)
- Stock 24H2 explorer registers Win+X in `CTray::_RegisterGlobalHotkeys` (id `0x24E`), not relevant under explorer7
- win32kfull reserves no plain Win+X, only Win+Ctrl+Shift+X

So `RegisterWindowHotkeyNew` drops twinui's Win+X while the option is on, and
the tray subclass registers Win+X on `Shell_TrayWnd` itself (id `0x7858`),
through the unhooked `RegisterHotKey` so a failure is logged. On the press it
posts `WM_CONTEXTMENU` with `lParam -1` to the Start button, so explorer's own
keyboard route places the menu and the import hook swaps it.

The 26100 Windows key watcher treats any other key during the hold as a chord,
so Win+X does not also open Start.

## Menu commands

`CTray::_Command`, same in both builds:

| Id | Action |
|---|---|
| 401 | `_RunDlg` |
| 402 | `LogoffWindowsDialog` |
| 407 | `_RaiseDesktop` toggle (show desktop) |
| 506 | `_DoExitWindows` with the choice in `lParam` |

The Windhawk mod used `0x19F`, which is 415, `_MinimizeAll`, and Run as hotkey
505, which is Win+F search. Run is 401 and the desktop toggle is 407.

## Shutdown submenu

The Start menu power button posts `WM_COMMAND` to the tray from
`CLogoffPane::PostTrayCommand`: 402 for log off, 506 with the choice for
shutdown, restart, sleep and hibernate. 506 reaches shell32
`ExitWindowsDialog`, which uses hybrid shutdown and adds `EWX_FORCE` only with
Ctrl held or choice bit `0x10000`. The Windhawk mod called `ExitWindowsEx` with
`EWX_FORCE`, which closed programs without asking to save.

The list comes from `IShutdownChoices` the way `CLogoffPane::AddShutdownOptions`
builds it:

- `CShutdownChoices` starts with mask `0x420056`, so log off is not listed and is added first by hand
- `GetChoiceEnumerator` yields a separator `0x400000`, sleep `0x10` and hibernate `0x40` when available, then `0x20002`/`2` and `0x20004`/`4`
- Bits `0xC0000` mark a choice the user cannot take, those are skipped
- `Refresh` is called first, shutdownux only rereads sleep and hibernate there
- `GetChoiceName(choice, 1, ...)` returns the menu form, en-US: `L&og off`, `&Sleep`, `&Hibernate`, `Sh&ut down`, `&Restart`, `Update and sh&ut down`, `Update and &restart`

## Command Prompt or PowerShell

19044 twinui `CLauncherTipContextMenu::_EnumerateAndBuildMenu` reads HKCU
`Explorer\Advanced\DontUsePowerShellOnWinX` on every build of the menu:

- Default is PowerShell when `IsWow64Process2` reports an x86 or x64 machine, otherwise Command Prompt
- A value that reads successfully overrides it, nonzero is Command Prompt
- The policy `POLID_ShowCommandPromptOnWinX` forces Command Prompt, not handled here
- Windows launches the WinX `.lnk` files with `runas` for the admin pair, the wrapper starts `cmd.exe` or `powershell.exe` from System32 directly

Labels and accelerators were read from 19044 `twinui.dll.mui` (ids `0x2A9F` to `0x2AB2`).

## Not verified

- The first build ran for the user on 2026-10-09 and showed the full menu with the PowerShell pair, which build and route were not stated
- The docked placement has not run yet
- Win+X with an elevated window in front, the hotkey should arrive since it is not the bare key, see `Win11Restore\notes\windows-key-elevated-24h2.md`
- That `SetForegroundWindow` on the Start button succeeds after the hotkey, the log line `foreground` says
- 24H2 `shutdownux` choice table, only 19044 was read
- 7785, which neither agent read

Log lines to look for: `Win+X menu on`, `Win+X hotkey on the tray ... registered 1`,
`RegisterHotKey id 15 Win+X left to the power user menu`, `Win+X pressed`,
`Win+X menu at ... picked`.
