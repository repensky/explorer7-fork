# Second explorer instance, opening a folder from the command line

`explorer.exe <path>` and `explorer.exe /factory,{CLSID}` run a second explorer
process. With the 7850 exe swapped into `C:\Windows` and no IFEO loader, that
second process is 7850 too, and on 24H2 it opened nothing.

The host machine never hit this. Its IFEO loader, `explorer7.dll`, reads the
process command line and returns before mapping 7850 when there is any argument,
so every launch with arguments there runs stock `explorer.exe`.

## What 7601 and 7850 do

Read from `exports\win7\explorer7601.exe.i64` and `explorer7850.exe.i64`.

`ShouldStartDesktopAndTray` gives mode 2 for a path and mode 4 for `/factory,{GUID}`.

| Mode | Call in `wWinMain` |
|---|---|
| 2, a path | `CoCreateInstance(CLSID_ExplorerLauncher, INPROC, IID)` then slot 3 `ShowWindow(&CLSID_SeparateMultipleProcessExplorerHost, pidl, flags, POINT {0,0}, nShow)` |
| 4, a factory | `CoCreateInstance(CLSID_ExplorerHostCreator, INPROC, {C4DE032A-...})` then slot 3 `CreateHost(&guid)` and slot 4 `RunHost()` |

| Build | Launcher IID asked for |
|---|---|
| 7601 | `{578E4660-E403-4E8F-9FD4-6D559F7A0EDC}` |
| 7850 | `{5AC8C8F7-1CC7-46CB-8D7D-3CF14B64868C}` |

- Both exes import `CoCreateInstance` from WRP64, so `Explorer_CoCreateInstance` sees every one of these calls
- The launcher is created only in `wWinMain`, in both exes
- `SHExplorerParseCmdLine` sets the same flag bits in 7601, 7850 and 26100, `/N` 1, `/SELECT` 4, `/E` 8, `/EXPAND` 0x10

## What 19041 and 21332 ExplorerFrame answer

Read from the PDB named databases of the host's `ExplorerFrame.dll` and the VM's.
The VM's file reports 10.0.26100.8972 but is byte identical to
`Win11Restore\exports\ExplorerFrame.dll` 10.0.21332.1000, the host's matches
`ExplorerFrame_10.dll`, both have databases there.

- `CExplorerLauncher` has one QueryInterface entry, `{9B25C299-03B6-4A14-827D-095485D0C022}`, so the 7601 and 7850 IIDs fail
- `CExplorerLauncher::ShowWindow(GUID const &, PCIDLIST_ABSOLUTE, LAUNCHEXPLORERFLAGS, POINT, int, HWND, IUnknown *, IBrowserThreadHandshake *)`, three more arguments than 7601 and 7850 pass
- Stock 26100 `explorer.exe` passes those three as null, read from its `wWinMain`
- `CExplorerHostCreator` still answers `{C4DE032A-...}`, `CreateHost` takes the class object of `CLSID_CommonExplorerHost` and registers it under the asked CLSID, `RunHost` pumps messages, nothing in either needs the exe
- `GetHostFromTarget` answers `CLSID_DesktopExplorerHost` unless `UseSeparateProcess(pidl)`, then `{5BD95610-...}` for Control Panel or `CLSID_SeparateSingleProcessExplorerHost`
- The 7850 shell registers `CLSID_DesktopExplorerHost` at startup through `CreateHost`

On the 24H2 VM the three hosts are `LocalServer32` launches of `%SystemRoot%\explorer.exe /factory,{CLSID}`:

| CLSID | Name |
|---|---|
| `{5BD95610-9434-43C2-886C-57852CC8A120}` | `CLSID_ControlPanelProcessExplorerHost` |
| `{75DFF2B7-6936-4C06-A8BB-676A7B00B24B}` | `CLSID_SeparateMultipleProcessExplorerHost` |
| `{682159D9-C321-47CA-B3F1-30E36B2EC8B9}` | `CLSID_DesktopExplorerHost` |

## The fix, `ExplorerLauncher.cpp`

- A launcher asked for by the 7601 or 7850 IID that fails is created with `{9B25C299-...}` and wrapped, the wrapper's slot 3 adds the three null arguments
- A frame that still answers the old IID is left alone
- In a `/factory` process the host creator is wrapped only to log `CreateHost` and `RunHost`
- Every launcher and host creator request is logged with the command line and the result, in `%TEMP%\explorer7.log` when that file exists

## Result on the VM

- Games Explorer, Tools, Hardware opens again, confirmed by the user
- The log showed the launcher shim working, `ShowWindow` hr 0, and the window served by a 7850 `/factory,{75DFF2B7-...}` process, `CreateHost` hr 0, so 7850 can serve a factory host
- The window went to `CLSID_SeparateMultipleProcessExplorerHost`, not the shell, so `ShowWindow` uses the host the caller asks for in that case
- Display Devices opened Settings, the cause was in Control Panel Reborn, see `RedirectionModern\notes\cpl-reborn-VERSION-HISTORY.md` 1.0.3

## Verified, inferred, unknown

Verified against the binaries: every IID, CLSID, slot, argument list and flag bit above.

Verified by the log: the shim's `ShowWindow` succeeds and a 7850 factory process hosts the window.

Unknown: why the Control Panel factory host `{5BD95610-...}` failed before, no launch since this build has asked for it.
