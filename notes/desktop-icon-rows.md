# Desktop icon rows spaced like Windows 7

`explorerwrapper/DesktopIconRows.h`, registry option `Win7DesktopIconRows`
(default 1). Ported 2026-10-04 from the Windhawk mod `win7-desktop-icon-rows`
(`Win11Restore\scripts\desktopiconrows.cpp`), where the user confirmed the rows
on the 24H2 VM. The full analysis is in
`Win11Restore\notes\desktop-icon-rows-win7.md`.

## What differs from Windows 7

shell32 `CListViewHost` builds the desktop row from the icon size and
`SM_CYICONSPACING` the same way on 7601, 19041 and 26100 (98 px for 48 px icons
and the stock 75 px spacing), and comctl32 lays the icons out identically on all
three. Only the stretch that fills the work area differs:

- Windows 7, `_UpdateThumbnailSpacing`: `row + (H % row) / (H / row)`
- 19041 `CalculateOptimalRowSpacing`, 26100 `CalculateOptimalSpacingForPrimaryMonitor`
  and `CalculateOptimalRowSpacing`: only the leftover beyond `(int)(0.3 * row)`
  is shared

## Hooks and patterns

Both hooks call the original and replace only the row with the Windows 7 rule.

| Build | Function | RVA | Callers |
|---|---|---|---|
| 19041 | `CalculateOptimalRowSpacing(int,int)` | 0xD5C44 | `UpdateIconSpacing`, `FindOptimalSpacing` |
| 26100 | `CalculateOptimalRowSpacing(int,int)` | 0x1EDC10 | `FindOptimalRowSpacing` (several monitors) |
| 26100 | `CalculateOptimalSpacingForPrimaryMonitor(work area *, SIZE, SIZE *)` | 0x195BD0 | `UpdateIconSpacing` (one monitor) |

The 26100 one monitor function takes the work area in `rcx` (rect at +0, dpi at
+0x10, pinned with static_assert), the cell SIZE packed in `rdx` and the output
in `r8`, read from its disassembly.

Each pattern was scanned against both shell32 copies: one hit in its own build,
none in the other. The copies are byte identical to what runs on the 19044 host
(`A2AF1196...`) and the 24H2 VM (`A7779772...`). The only wildcard is the
`mulss` displacement to the 0.3f constant. 26100 patterns are only tried on
26100 and later, the 19041 one below that, so a build in between simply logs a
pattern miss and keeps the stock rows.

## Why no relayout

The Windhawk mod has to nudge the desktop and repaint it, because it loads after
Explorer laid the icons out. explorer7 hooks inside `ChangeMinhookImports`, which
runs from `DllMain` before `SHCreateDesktop`, so the desktop's first layout
already uses the Windows 7 rows.

## Several displays

The first build changed nothing on the 19044 host, which has three displays:
main 3440x1440 (work area 1400 tall), Parsec 1920x720 and a portrait 1600x2560.
Read on the host: `explorer7.log` showed the row hook return 100 for the 1400
area, yet `LVM_GETITEMSPACING` on the desktop listview said 76 x 98 and the icons
sat every 98 px. With more than one work area both builds go through
`FindOptimalSpacing`, which caps the row at the tightest display. Under the
Windows 7 rule the portrait display (26 rows, 12 px left) only reaches 98, so
the cap was 98 and the hook's 100 for the main display was thrown away.

Windows 7 itself fitted the stretch to the work area holding listview point
(0,0), the top left of the whole virtual screen, which on this host is the
portrait display, so it would also have shown 98 here.

`Win7DesktopIconRows` is now a mode, the user chose 1 as the default:

- 0, stock rows
- 1, the row is fitted to the main display, like single monitor Windows 7. A
  taller display may fit one row fewer, the portrait one goes from 26 to 25
- 2, the row is fitted to the area at listview (0,0), the Windows 7 rule

A third hook, `long FindOptimalSpacing(work areas *, int count, SIZE, SIZE *)`,
runs the original (its column search and HRESULT stay) and replaces only the
row. 19041 RVA 0x44E940, 26100 RVA 0x1A540C, prologue patterns with the cookie
load wildcarded, each one hit in its own build and none in the other. Areas are
`{RECT, dpi}` with a 0x14 stride, read from both builds (`lea r8,[r8+14h]`).

Mode 1 finds the main display's centre in listview coordinates as the screen
centre minus `SM_XVIRTUALSCREEN` / `SM_YVIRTUALSCREEN`. That origin was measured
on the host, desktop icons at listview x 1614 sit at screen x 14 with the
virtual screen starting at -1600, -568. Not checked with mixed DPI displays.

## Known gaps

- Windows 7 skipped the stretch with Align icons to grid off, 10 and 11 stretch
  either way and the hooks cannot see the view flags
- Several work areas keep the 10 and 11 column search, only the row follows
  the chosen display
- The Windhawk mod `win7-desktop-icon-rows` has no multi display hook yet
