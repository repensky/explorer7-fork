# Start menu power menu refreshing live

Turning hibernation on or off did not change the Start menu power menu until
Explorer restarted. Fixed in the wrapper, `AuthUI.h` and `AuthUI.cpp`, and
`FixAuthUI` removed from `PatternImports.cpp`. Built as `Release\shutdown-listener`.

Read from `exports\win7\explorer7601.exe`, `explorer7850.exe` (both with PDBs),
`exports\win7\authui7601.dll` 6.1.7601.17514 and `exports\19044\shutdownux.dll`.

## How Windows 7 kept it current

`IShutdownChoices::CreateListener` (slot 4) returns an `IShutdownChoiceListener`:

| Slot | Method |
|---|---|
| 3 | `SetNotifyWnd(HWND, UINT)` |
| 4 | `GetMessageWnd(HWND*)` |
| 5 | `ScanForPassiveChanges()` |
| 6 | `StartListening()` |
| 7 | `StopListening()` |

- `StartListening` makes a hidden top level window and registers it for `GUID_ACDC_POWER_SOURCE`, `GUID_HIBERNATE_FASTS4_POLICY` and `GUID_USERINTERFACEBUTTON_ACTION`
- On `WM_SETTINGCHANGE` or `PBT_POWERSETTINGCHANGE` it sends the pane `WM_NOTIFY` with `hwndFrom` set to that window
- `ScanForPassiveChanges` re-reads power, update and dock state and sends the same notify if anything moved

Explorer side, identical in 7601 and 7850:

| Pane field | Meaning |
|---|---|
| `+0x40` | pane HWND, passed to `SetNotifyWnd` |
| `+0x90` | `IShutdownChoices` |
| `+0x98` | listener |
| `+0xA0` | listener window from `GetMessageWnd` |

- 7601 sets these up in `CLogoffPane::_OnCreate`, 7850 in `CLogoffPane::_InitShutdownObjects`, same slots and order
- `_OnNotify` runs `_ApplyOptions` when `hwndFrom == +0xA0`, which calls `Refresh` and relabels the split button
- `AddShutdownOptions` builds the arrow menu from `GetChoiceEnumerator` and never calls `Refresh`
- `_DoSplitButtonContextMenu` and the Windows Update notify (`0xDD`) call `ScanForPassiveChanges` only when a pending flag is set
- Code `0xD5` also runs `_ApplyOptions`, it comes from `CDesktopHost::Exec` on the menuband refresh command, not on every open
- The destructor only Releases `+0x90` and `+0x98`, it never calls `StopListening`

## What was wrong

- `CAuthUIWrapper::CreateListener` returned `S_OK` without a listener
- `FixAuthUI` NOPed the three listener calls to stop the null calls crashing, old pattern in 7601 `_OnCreate`, new pattern in 7850, neither pattern exists in 7785
- So nothing ever told the pane to refresh, and Windows 10 `CShutdownChoices::GetChoiceEnumerator` lists only what the last `Refresh` cached

## The fix

- `CAuthUIWrapper::GetChoiceEnumerator` calls `Refresh` first, so the arrow menu is current every time it opens
- `CShutdownChoiceListener` rebuilds the Windows 7 listener with the same slot order, same window behaviour and same three power settings
- It also watches `HKLM\SYSTEM\CurrentControlSet\Control\Power` with `RegNotifyChangeKeyValue`, so a change there relabels the split button
- `FixAuthUI` is gone, the pane now calls the real listener
- The listener is declared `DECLSPEC_NOVTABLE`, otherwise the build needs `_purecall`, which minCRT does not provide

Checked in the built DLL against `wrp64.map`, the listener vtable is QueryInterface, AddRef, Release, SetNotifyWnd, GetMessageWnd, ScanForPassiveChanges, StartListening, StopListening.

## Side finding, the Power menu settings already reach this menu

Windows 10 `CShutdownPowerSettings::Refresh` reads live `SystemPowerCapabilities` and:

- lists Hibernate only when `ShowHibernateOption` is on and a full hiberfile exists
- lists Sleep only when `ShowSleepOption` is on

So the Power Options "Show in Power menu" boxes for Sleep and Hibernate drive this menu, and with the refresh they apply as soon as they are saved. Lock was not checked.

## Not yet confirmed

- That `powercfg /hibernate` writes under the `Control\Power` key, the arrow menu refresh covers hibernation either way
- 7785, which matched neither `FixAuthUI` pattern, was not examined
