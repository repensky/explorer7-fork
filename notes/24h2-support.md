# Windows 11 24H2 support (build 26100)

Every OS side hook in the wrapper was checked against the 26100 binaries pulled
off the 24H2 VM into `..\exports\24H2`, with the host 19044 copies in
`..\exports\19044` as the control. The scripts that did the checking live in
`..\tools`:

| script | what it answers |
| --- | --- |
| `scan-patterns.ps1 [-Dir] [-Only]` | how many times each byte pattern matches, and at which RVA |
| `scan-guids.ps1` | which module carries each IID and CLSID the wrapper asks for |
| `check-imports.ps1` | whether every import of the Win7 explorer resolves, and which are SHUNIMPL stubs |
| `imports-of.ps1` | which import descriptor a module pulls a name through |

Function names come from the public PDBs through `cdb -z <dll>`, so each new
pattern below is anchored on a named function, not on a guess.

## What was broken on 26100

| area | before | 26100 finding | fix |
| --- | --- | --- | --- |
| build gate | `build > 20000` exits | 26100 | ceiling raised to 26200, 25H2 shares the binaries |
| `XamlLauncher::ShowStartView` | late NI pattern with a hardcoded cookie displacement | prologue grew a disp32 frame lea | new wildcarded pattern, `23F2A0`, matches once |
| `XamlLauncherState::ShowSearchFromOpenStart` | loose 22H2 shape | three hits, first one right by luck | tight pattern `2434A8`, matches once |
| `CortanaDesktopExperienceView::ShowInternal` | loose VB shape | first hit is `SearchAppDesktopExperienceView::ShowInternal` | tight pattern, both bodies patched |
| `TaskViewHost::Show` | 22H2 shape | miss | new pattern `27FFF0`, gaming host left alone |
| `XamlLauncher::OnShellHookMessage` | VB shape | miss, body now opens with a WIL feature gate | new pattern `23E0D0` |
| Win+X `ShowLauncherTipContextMenu` | three twinui.dll shapes | moved to twinui.pcshell as `xor eax,eax; ret` | skipped with a log line |
| shell32 `CanApplyOwnerDrawToMenu` | TH1 shape | `xor ebx,ebx` inserted | new pattern `582140` |
| shell32 pin gate in `CPinnedList::Modify` | `Feature_W32PTP` call site | renamed `Feature_W32PTU`, prologue reordered | new call site pattern `48B3CC` |
| shell32 class table for `CLSID_StartMenuPin` | two `DllGetClassObject` code shapes | neither present, entry layout rotated to `{CLSID*, CreateFunc, x, 1}` | entry found from the data, see below |
| timedate `IsolationAwareCreateWindowExW` | one shape | saves rbx and rsi now | new pattern `1447C` |
| timedate `CTrayClock::s_WndProc` | one shape | message kept in ebp | new pattern `207E0` |
| stobject `UpdateFlyoutUI` | one shape | push rbx and a padded import call | new pattern `23250` |
| user32 ordinal 2511 | hooked as `SetFallbackForeground` | now `SetShellSpecialWindow` | not hooked from 26100 |
| user32 ordinal 2542 | hooked as `RegisterShellPTPListener` | now `SetCoveredWindowStates` | not hooked from 26100 |
| user32 2628, 2629, 2631, 2632 | window group calls | not exported | not hooked from 26100 |
| `CNscTree` field offsets | 19041 layout hardcoded | every field past the tree window moved by eight | layout picked from ExplorerFrame's own code |
| `IsolationAwareCreateWindowExW` hook | nine arguments forwarded | the real function takes twelve | all twelve forwarded, this was a latent bug on every build |

## What was checked and needed nothing

| item | 26100 result |
| --- | --- |
| uxtheme `CVSUnpack::LoadAnimationDataMap` call site | matches once, call target confirmed `159B0` |
| uxtheme `GetClassIdForShellTarget` | missing on 19044 and 26100 alike, no symbol either |
| ExplorerFrame `CanApplyOwnerDrawToMenu` | both existing shapes match once |
| SndVolSSO `CAppWindow::LaunchSndVol` | VB shape matches once at `108AE` |
| shunimpl `DllMain` | matches once |
| shell32 ordinals 200, 201, 206, 241, 261 | `SHCreateDesktop`, `SHDesktopMessageLoop`, `SHCloseDesktopHandle`, `SHGetUserDisplayName`, `SHGetUserPicturePath` |
| shell32 ordinal 902 | forwarded to `SHUNIMPL.#473` on 19044 and 26100 alike, the import hook still lands |
| dwmapi 113, 127, 159 | `DwmpActivateLivePreview`, `DwmpGetColorizationParameters`, `DwmpUpdateAccentBlurRect` |
| uxtheme 7, 16, 92 | `GetThemeDefaults`, `OpenThemeDataFromFile`, `LoaderLoadTheme`, which reads fifteen arguments |
| uxtheme 74 | `GetThemeClass`, the wrapper names it GetThemeName and never calls it |
| user32 2513, 2514, 2537, 2563, 2564, 2566, 2567, 2568, 2569, 2573, 2574, 2579, 2581, 2585, 2627 | same functions as the comments claim |
| every Win7 explorer import, 7601 and 7850 | resolves, the SHUNIMPL stubs are the same three as on 19044 |
| every SHLWAPI and DUI70 forward in `forwards.h` | resolves |
| `CStartMenuPin` private vtable | 26 slots in the same order as 19044 |
| `CNscTree` interface subobjects | `INameSpaceTreeControl2` at `0xE0`, `IVisualProperties` at `0x100`, values private at `0x130`, same as 19044 |
| `CLSID_StartMenuCacheAndAppResolver` | served by `appresolver.dll`, which still carries `IStartMenuItemsCache10` and `IStartMenuAppItems8` |
| `CLSID_ImmersiveShellBuilder` | served by `twinui.pcshell.dll` |
| `IID_IShutdownChoices10` | served by `shutdownux.dll` |
| `IPinnedList3`, `IRegTreeOptions8`, `IShellURL10`, `IUserAssist10`, `IAutoDestList10`, `ICustomDestList10`, `TrayClock8` | present |
| `pnidui.dll`, `VAN.dll` | not shipped on 26100, the network flyout hooks have nothing to attach to |

## The pin class table

shell32 builds the objects it serves from a table of entries. On 19044 an entry
is `{ptr, count, CLSID*, CreateFunc}` and the wrapper found the table through a
`lea` inside `DllGetClassObject`. On 26100 the entry is
`{CLSID*, CreateFunc, ptr, 1}` and the `lea` is gone. Both layouts keep the
create function right after the CLSID pointer, so the wrapper now looks for the
one place in the image that holds a pointer to `CLSID_StartMenuPin` followed by
a pointer into code. On 26100 that is `5DCCE0`, pointing at
`CStartMenuPin_CreateInstance`, and it is the only such place.

## CNscTree offsets

Read out of `CNscTree::SetIndentValue`, `ScaleAndSetIndent`, `SetItemHeight`
and `ScaleAndSetRowHeight`, base relative:

| field | 19041 | 26100 |
| --- | --- | --- |
| indent store, from the values private subobject | `0xA0` | `0xA8` |
| height store, from the visual properties subobject | `0xC8` | `0xD0` |
| tree window | `0x178` | `0x180` |
| DPI window | `0x188` | `0x190` |
| indent read | `0x1D0` | `0x1D8` |
| height read | `0x1C8` | `0x1D0` |

The wrapper reads the store displacement out of the first instruction of both
methods at run time and picks the matching layout. A hooked method, which does
not open with that instruction, falls back to the build number. A store offset
that matches neither layout hands the call to ExplorerFrame's own methods.

## Pin verbs and taskbar pinning, found on the first VM run

Two things were missing on the VM with everything above in place: the
`Pin to Taskbar` and `Pin to Start Menu` verbs on Start menu items, and
`Pin this program to taskbar` from a jump list did nothing. The registry
handler registrations under `HKCR\*\shellex\ContextMenuHandlers` were identical
on both machines, so it was code, not setup.

`CPinnedList::QueryContextMenu` and `CTaskbandPin::v_AllowVerb` both ask
`IsProcessAnExplorer` and give up when it says no. On 19044 that is a local
shell32 function, on 26100 shell32 calls the windows.storage copy through an
import thunk. Both versions cache their answer and decide it by comparing the
process image path with `%SystemRoot%\explorer.exe` (26100 also accepts
`ppishell.exe` and a dpinit path). A wrapper loaded into the real explorer.exe
passes, a separate `Classic\Explorer\explorer.exe` never does. The wrapper only
faked the answer for explorer.exe's own import, so shell32's internal calls
still saw no. Both exports are now hooked process wide to answer yes.

With the verbs back, pressing one still did nothing, and neither did
`Pin this program to taskbar` from a jump list. That is a second, deeper
change in 26100. Three routes were read:

- `CPinnedList::Modify` with the `Feature_W32PTU` gate off, the route the
  wrapper forces, returns `S_OK` and does nothing for a null first pidl before
  it reaches `InternalModify`. Making that branch unconditional was tried and
  does not help, because `InternalModify` itself now returns `E_FAIL` for a
  null first pidl at `+0x156`. The whole legacy add, which on 19044 still lived
  at `InternalModify+0x7c7`, is gone.
- `CPinnedList::_TogglePinned`, which the verbs run, lost its gate too. On
  19044 it fell back to `Modify` when the gate said no. On 26100 a new pin
  always goes to the immersive shell's `SID_PinManager`, implemented in
  twinui.pcshell, whose `PinItemToTaskbarShim` is a two byte stub and whose
  `PinItemFromTrustedCaller` runs the Windows 11 pin dialog machinery.
- `CPinnedList::PinShellLink`, the route provisioning uses, still carries the
  complete legacy add on 26100: backup shortcut through
  `CBackupLocationManager::CreateBackupLnk`, `SetPinSourceForPinnedItem`,
  `_CacheAppIDInIDList`, `CPinList::Load`, `AppendPidl`, `Save` and
  `_DoNotifyPinListChange`. It needs a non null app id, the backup shortcut
  name is copied from it.

So on 26100 every new pin is sent through `PinShellLink` by the wrapper.
`CPinnedListWrapper::Modify` does it for a null first pidl, which is the jump
list, and a hook on `_TogglePinned` does it for the verbs, reading the pinned
flag at `+0x58`, the pidl at `+0x60` and the `IPinnedList3` subobject at
`+0x18` off the object, all read from the 26100 body. The shortcut handed over
is the item's own `.lnk` when it is one, otherwise a fresh link to the item,
and the app id comes from the app resolver with the item path as the fallback.
Unpinning already worked, it goes through `Modify` with a null second pidl.

The first `PinShellLink` argument is the backup shortcut's file name, read out
of `CBackupLocationManager::CreateBackupLnk`, which appends it to the backup
folder path and cleans it up as a file name. The first VM run passed the app
id there and pinned items showed up labelled `Microsoft.Windows.ControlPanel`.
The item's normal display name goes there now.

## The Windows key on 26100

With the shell hook route hooked and logging on every step, a press of the key
on the VM reached neither `XamlLauncher::OnShellHookMessage` nor the taskman
window as `HSHELL_TASKMAN`, code 7, while every other shell hook code did
arrive, and nothing in the process ever called `ShellRegisterHotKey`.

The whole route was then read out of the 26100 `win32kfull.sys` with symbols:

- `RegisterSystemHotkeys` owns the bare Windows key as hotkey id `-9`, modifier
  `0x1008`, no callback, and Ctrl+Esc as id `0xF130`. Both end in
  `xxxReportHotKey`.
- `xxxReportHotKey` looks up the desktop's shell window, the one set through
  `SetShellWindowEx`, and drops the key when there is none. Otherwise it calls
  `xxxReportWindowHotKey`.
- `xxxReportWindowHotKey` walks the hotkey's child list. Children are only ever
  added by `NtUserShellRegisterHotKey`, which demands `IsShellProcess`, so in
  the wrapper's process the list is empty. An empty list means
  `PostMessage(shellWindow, WM_SYSCOMMAND, SC_TASKLIST, 0)`. A child would go to
  `NotifyShell::ShellHotKey`, message `0x342` to the window management target
  window at desktop `+0x148`, the route the real Windows 11 shell uses.
- `xxxSysCommand` for `SC_TASKLIST` needs `ARW_HIDE` in the minimized metrics,
  then posts the `SHELLHOOK` message with `HSHELL_TASKMAN` to the taskman
  window set through `SetTaskmanWindow`. That is the classic route, and on
  19044 the wrapper's taskman window forwards it into the immersive shell.
- 26100 `shell32!CDesktopBrowser::_OnCreate` calls `SetShellWindow` on the
  Progman window unconditionally, 19044 called `SetShellWindowEx`.

So the key was expected to land on the Progman window as `SC_TASKLIST`. A
Progman subclass in the wrapper and a Windhawk probe
(`Win11Restore\scripts\winkeyprobe.cpp`) then showed on the VM:

- the kernel holds Progman as the shell window and the wrapper's window as
  the taskman window, so the delivery side is complete
- Progman never receives `SC_TASKLIST`, the kernel drops the key before
  `xxxReportHotKey`
- the two gates in `xxxDoHotKeyStuff` that could be read from user mode are
  clear: the lock screen active field (private SPI `0xAA` reads it, `0xAB`
  writes it and only the LogonUI process may) is zero, and the only raw input
  registration in the process is the tray's consumer control sink without
  `RIDEV_NOHOTKEYS`
- no module loaded in the process references `ShellRegisterHotKey`, so no
  child hotkey diverts the key either
- a low level keyboard hook still sees every press and release

What remains unread is a session flag at `+0x3968` tested for every non-SAS
key and the raw input process flags at `ppi+0x340`. Rather than chase those,
the wrapper installs a `WH_KEYBOARD_LL` hook on its own thread when the build
is 26100 or later. A Windows key press with no other key before its release,
and no foreground change in between, posts tray message `0x504`, which is
what the `OnShellHookMessage` hook posts on 19044. The Progman subclass keeps
logging `SC_TASKLIST` and swallows it there so a tap can never toggle twice.
The probe confirmed the hook route opens and closes the Start menu on the VM.

With that in place the key still did nothing while another process owned
the foreground. The tray opens Start with `SetForegroundWindow`, and
`win32kfull!CanForceForeground` on 26100 grants that only to the process
named by the last woken thread bookkeeping, the current foreground process,
or the process that last injected input. The kernel's own hotkey route
calls `SetLastWokenThread` for the shell thread before it posts, the hook
route has no such grant. A first fix injected a lone `VK_MENU` release
through `SendInput` right before posting `0x504`, which made the shell
process the last input provider and worked for ordinary windows.

The wrapper now posts `WM_SYSCOMMAND` `SC_TASKLIST` to its own taskman
window instead. `DefWindowProc` carries that into `xxxSysCommand`, whose
`SC_TASKLIST` case on 26100 calls `SetLastWokenThread` for the taskman
window's thread and then posts `SHELLHOOK` code 7 to it, read at
`xxxSysCommand+0x5c9` and `+0x60f`. The taskman window handler answers code 7
by posting `0x504` to the tray, so the grant comes from the kernel and no
input is injected. The `SendInput` route stays as a fallback for the case
where the taskman window does not exist yet.

With an elevated window in the foreground the key still did nothing. The
log prints the foreground owner's pid, integrity level and exe on every
tap, and a run with Task Manager in front logged no tap at all, so the
kernel never shows the medium integrity sink a key headed for a higher
integrity window. The relay for that case is the Windhawk mod
`Win11Restore\scripts\winkeyelevated.cpp`, hosted in ctfmon.exe whose
UIAccess right lets a low level hook see those keys, it posts the same
`SC_TASKLIST` to the taskman window, see
`Win11Restore\notes\windows-key-elevated-24h2.md`.
`NtUserRegisterHotKey` rejects modifier bit `0x1000`, the bit that marks the
kernel's own bare Windows key entry, so a user mode registration of the bare
key is not an option either.

## Fixed on 19044 along the way

The loose 22H2 search pattern matched `XamlSnapAssistViewController::InvokeSwitchItem`
first on 19044, so the host had been patching snap assist instead of
`ShowCortanaFromOpenStart`. The search patterns are now chosen by build.

## First logon crash on 26100, found on the second VM

Every new account crash looped explorer at first logon, admin or standard,
while existing accounts were fine.

- Stack from a WER dump: `CTray::_SyncThreadProc` -> `HandleFirstTime` -> `PinInitialItems+0xfb` -> address `0x10B`
- `PinInitialItems` calls slot 14 (`call [rbx+70h]`) on the `CLSID_TaskbandPin` object, which is our `CPinnedListWrapper`
- The wrapper's vtable ended at slot 13, the next data in wrp64 was a GUID whose first qword is `0x10B`
- Slot 14 is `GetPinnedItemForAppID(L"{F38BF404-...}\\explorer.exe", &pidl)`, the result is freed with CoTaskMemFree
- 26100 `shell32!CPinnedList` vtable, read with symbols from the dump: slot 7 `LegacyModify`, 14 `PinShellLink`, 15 `GetPinnedItemForAppID`, 16 `Modify`
- The wrapper now has slot 14 and forwards it to `IPinnedList3` slot 15, verified in the built DLL
- Of the seven 7850 functions that create the pinned list, `PinInitialItems` is the only one that calls past slot 13, checked in `exports\win7\explorer7850.exe.i64`
- 19041 never hit it because its default profile ships `Explorer\Advanced\StartMenuInit = 0xd`, which skips the whole first logon block, 26100's default profile has no value
- An account that crashed in the block never writes `StartMenuInit = 5`, so every restart runs it again

## First logon seeding as Windows 7 Ultimate, `InitialMFU.cpp`

Explorer's first logon block picks its Start menu, taskbar pins and Explorer jump list
by edition. 24H2 reports SKU 0xBF, which none of the tables carry, so nothing was seeded.

- `GetProductInfo` (KERNEL32 import) has three callers in 7850, `CreateInitialMFU`, `PinInitialItems` and `AddExplorerDests`, nothing else in explorer calls it
- The wrapper points that import at `GetProductInfoNEW`, which forwards the call and then answers `PRODUCT_ULTIMATE`, on both the 7601 and the 7850 runtime
- Tables, 264 byte records `{product, touch, 16 x {KNOWNFOLDERID*, path}}`: MFU at `0x100014570` (70 rows), taskbar pins at `0x100018DA0` (64), Explorer jump list at `0x10001CFA0` (58)
- Ultimate pins are IE, `Accessories\Windows Explorer.lnk` and Windows Media Player, the jump list gets the Documents, Pictures, Music and Videos libraries
- `MSMFUEnumerator::Next` parses each path under its folder and skips one that fails, so a shortcut missing on 24H2 is dropped, not pinned broken
- The pin loop moves items with `Modify(pidl, (PCIDLIST)n)`, 26100's `CPinnedList::InternalModify` checks `< 0x10000` for that form, read from the dump
- 7850's Ultimate MFU row is 7601's without Welcome Center, so on the 7850 runtime only, the first call rewrites both Ultimate rows (touch 0 and 1) in place
- New row: Getting Started, Media Center, Calculator, Sticky Notes, Snipping Tool, Paint, Remote Desktop Connection, Magnify, then 7850's own Solitaire slot
- Each item tries the Win7 path first, then where 24H2 restorations keep it, `Accessories\Getting Started.lnk`, `Accessories\Windows Media Center.lnk`, `Programs\Accessibility\Magnify.lnk`
- 7850 has no `.rdata`, the table and its strings sit inside `.text`, so the finder scans every readable section and writes with `PAGE_EXECUTE_READWRITE`
- Tested with a harness that maps the real 7850 explorer.exe and runs `InitialMFU.cpp` against it: 2 rows found, both rewritten, product 0x1C untouched
- That harness caught a pointer overflow in the section check, a near top of memory value wrapped `p + len` past the end test and faulted in the string compare
- Getting Started's jump list (7601 `GettingStarted_AddLinksToList`, through `CLSID_UserOobe`) is not ported, 7850 has none of that code

### Why only five of the nine showed on the first VM2 run

Seeding wrote all nine items to both UserAssist stores, read back as Test4. The list is built by
the wrapper's `CStartMenuResolver::GetStartMenuMFUList`, which only read usage of each app's best shortcut.

- Media Center: in the app cache, but its best shortcut is not the seeded one, and `ehshell.exe` is missing on VM2 so it shows a blank icon once listed
- Paint and Snipping Tool: seeded under `Accessories\`, but both also have a shortcut at the Programs root, so the best shortcut can be one that was never used
- Solitaire: seeded as the Games folder item `::{ED228FDF-...}\{00D8862B-...}`, which the app cache never holds
- Windows 7 ranks by app id alone: shell32 7601 `s_CombineUAInfoCB` reads `UAIID_APPLICATIONS` for each app's id, `CombineUAInfo` stores its R, `s_SortAppInfoCB` sorts on it
- A first try ranked by shortcut usage with an app id fallback, the two stores score on different scales and Notepad with 2 runs beat Getting Started with 15
- Fix, `UAQueryMFUUsage`: every item is ranked by its app id entry, the shortcut's own entry is used only when there is no app id
- Desktop items keep their shortcut usage as the gate but rank by app id, so all sources share one scale
- 7850's Remove from this list deletes the shortcut entry and fires event 4 on the app id, 26100's `CUserAssist::FireEvent` maps 4 to `CUADBLog::DeleteEntry2`, so removed items stay removed
- Fix, `AddUsedGames`: Games folder entries are read from the SHORTCUTS store, parsed through the Games folder the way the seeding made them, and ranked by the game's app id
- `UAQueryApp` was run on the 19044 host's own UserAssist through the wrapper's code, it returns scores, and an unknown app id gives R 0
- Side effect, as in Windows 7: an app run from anywhere, Run box included, counts toward the list when it has a Start menu shortcut
- `%TEMP%\explorer7.log` is opt in, a new account has none, so its first logon lines are lost unless the file is made in the Default profile

### Getting Started jump list at first logon

7601's `CreateInitialMFU` opens with `CLSID_DestinationList`, `SetAppID("Microsoft.Windows.GettingStarted")`, `BeginList`,
then `GettingStarted_AddLinksToList` at `100088DB4` and `CommitList`. 7850 has none of it.

- 7601 enumerates `CLSID_UserOobe` with `SHCONTF_NONFOLDERS` and reads title `{D376374E-...},2` and tooltip `{E1229318-...},2` with `GetDetailsEx`
- Its links run `GettingStarted.exe "<{E55FC3B0-...},100> <{ABCB1176-...},2>"`, the port instead uses the tile's id list like the OobeFldr mod, so the exe's unclear two argument mode is not needed
- The folder is bound from its Control Panel path so the tiles have absolute id lists, the mod's notes record that a bare `CoCreateInstance` folder cannot make them
- Runs on the first `GetProductInfo` call, inside `CreateInitialMFU` where COM is up, on the 7850 runtime only
- Wrapped in `__try`, a fault in the first logon block would repeat on every logon, and the wrapper links no C runtime so `__C_specific_handler` is forwarded to ntdll's export by `/alternatename`
- The forwarder was proven in a CRT free test exe, which caught an access violation through it
- The tasks themselves are an `oobetasks` xml inside OobeFldr, each with a destTitle, an icon and a command such as `control.exe /name Microsoft.Personalization`

### Taskbar pins at first logon

7850's `PinInitialItems` runs right after `CreateInitialMFU` and reads a second table at `0x100018DA0`, same 264 byte rows.

- Ultimate is rows 0 and 1, touch and not, each `FOLDERID_Programs` + `Internet Explorer.lnk`, `FOLDERID_Programs` + `Accessories\Windows Explorer.lnk`, `FOLDERID_CommonPrograms` + `Windows Media Player.lnk`
- It first unpins Explorer by app id, then for each item that resolves it unpins it, pins it, and moves it with `Modify(pidl, ++n)`
- So the items that resolve take slots 1, 2, 3 in table order and anything already pinned, a list copied into Default included, follows them
- On VM1 none of the three paths exist, 26100 keeps `Programs\File Explorer.lnk` per user and `Accessories\Windows Media Player Legacy.lnk` common, and no IE shortcut at all
- The host has IE and Windows Explorer in the common Programs folder where the row asks for the user's, so only WMP resolved there
- `RewriteUltimatePinRows` gives each item fallbacks the same way the MFU rows got them, Windows 7 path first
- A drag sends `Modify(pidl, slot + 1)` and 26100's `CPinnedList::Modify` takes that form, VM1 saved such a move with IE ahead of File Explorer

### Customize Start Menu list empty on 24H2

7850's `CCustomizeStartMenuDlg::AdvancedTabInit` fills the list with `IRegTreeOptions::InitTree(HKLM, "...\StartMenu\StartPanel")`,
and the wrapper's `CRegTreeOptionsWrapper::InitTree` swaps that path for `...\StartMenu\StartPanel7`.

- `StartPanel7` comes only from Explorer7's `Import Me.reg`, which the DLLLoader installer imports, the host has 112 subkeys and VM1 has none
- The VMs got Explorer7 by a manual swap, so nothing from that .reg reached them
- Do not import the whole .reg on a VM, its Help and Support class `{2559a1f1-...}` points at `shell32legacy.dll` where VM1's own uses `shell7.dll`
- `StartPanel7.reg` beside this tree holds the `StartPanel7` subtree alone, all 34 of its texts resolve through 26100's shell32 on VM1
- 26100's `CRegTreeOptions::ShouldIncludeViewOption` filters only eight Folder Options names, none of them a Start menu key

### Start menu search empty on VM2, working on VM1

Not a wrapper bug. The user found the cause: VM2's Windhawk Resource Redirect entry that sends
`ExplorerFrame.dll` to `C:\Windows\Classic\System Files\Resources\Explorerframe.dll.mun`.

- The searchpaneprobe mod showed `CDefView::_OnCreate` returning -1 right after `CLSID_ItemsView` was created
- It breaks even with a stock .mun, so the redirect itself is the trigger, which resource it misses is still unknown
- The `{865e5e76-...}` class difference between the VMs was a red herring, putting it back changed nothing
- `RegSetSZ` and `RegSetExpandSZ` passed two and four times the string's bytes, reading past the literal, both now pass the text plus terminator

### Internet Explorer shortcut deleted on 26100

The user's `Internet Explorer.lnk` points at msedge.exe with iexplore's icon. 26100's `ie4uinit.exe`
(11.00.26100.8117) deletes it by file name, 19044's (11.00.19041.5915) cannot.

- `UserConfigIE` calls `SBEUtil::RemoveIEShortcuts` when `SBEUtil::IsSBEEnabled`
- On 26100 that gate is always true, `InitOnceIsSingleBrowserExperienceEnabled` reads `Feature_IE_SBE` only to report usage, then sets the flag
- `RemoveIEShortcuts` walks a 3 row table, a known folder plus a relative path, 536 bytes a row
- The rows are `FOLDERID_Programs` + `Accessories\Internet Explorer.lnk`, `FOLDERID_UserPinned` + `TaskBar\Internet Explorer.lnk`, `FOLDERID_Desktop` + `Internet Explorer.lnk`
- Each one found is unpinned through `CLSID_StartMenuPin` and deleted, the target is never read
- Switches that reach `UserConfigIE`: `-show`, `-hide`, `-reinstall`, `-apply`, `-UserConfig`, `-UserIconConfig`
- `-ClearIconCache` and `-BaseSettings` do not
- Active Setup `{89820200-ECBD-11cf-8B85-00AA005B4383}` runs `ie4uinit.exe -UserConfig`, so every new account's first logon deletes it too
- 19044's `UserConfigIE` has no `RemoveIEShortcuts`, its `FindAndDeleteIcons` deletes only links whose target is iexplore or the IE namespace items
- 19044's ie4uinit loads on 26100: its 45 iertutil and 4 msIso ordinals match by PDB name, and the 3 names 26100's copy lacks resolve on VM1
- urlmon ordinal 410 was `LogSqmBits` and is now a folded `return 0` stub, its only caller is telemetry

## Not verified

- 22000 to 22631 were not checked against their binaries, they keep the paths they had
- The slot 14 fix has not run a first logon yet, the rest of `HandleFirstTime` after `PinInitialItems` is unproven on 26100
- The Ultimate seeding ran on VM2 and the list matches 7601, `AddExplorerDests` with Ultimate is still unproven on 26100
- The Getting Started jump list port has not run a first logon yet, OobeFldr would not load in a plain process on the host so the enumeration was never run outside Explorer
- Nothing here has run on the VM yet, the pattern hits are file scans and the layouts are symbol reads
- Win+X does nothing on 26100 under the wrapper, the stub that replaced the menu is left alone
- The pin row rewrite (`pins14`) has not run a first logon yet
- 26100's `CPinnedList::Modify` sends moves to the immersive shell's `SID_PinManager` when `Feature_W32PTU` is on, which way VM1 took is not known, only that the move saved
