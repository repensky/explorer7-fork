#pragma once
#include "common.h"
#include "dbgprint.h"
#include "OptionConfig.h"
#include "RegistryManager.h"

//---Field offsets, 7850 CUserTileTrayButton-----------------

// Confirmed against the 7850 public PDB. Only valid when the shell is the
// 7850 build, so every user of these is gated on s_BuildRuntime
#define UT_TILE_HWND    0x08   // the tile button window
#define UT_TOOLINFO     0x38   // live TOOLINFOW, reused by every track message
#define UT_STATE_ID     0x34   // 1 normal, 2 hot, what part 2 is drawn with
#define UT_TOOLTIP_HWND 0x80   // tooltips_class32 window, null until we build it
#define UT_THEME        0x88   // TrayNotify::UserTile handle, null on Win7 styles
#define UT_NAME_PTR     0x90   // LPWSTR the tool info text points at
#define UT_RECT_LEFT    0x98
#define UT_RECT_TOP     0x9C
#define UT_RECT_RIGHT   0xA0
#define UT_RECT_BOTTOM  0xA4
#define UT_FRAME_RECT   0xA8   // rect part 1 draws, the picture rect plus 1px
#define UT_PIC_SOURCE   0xC0   // source picture, sized by GetObjectW
#define UT_PIC_BLIT     0xC8   // what _Paint selects and blits
#define UT_HOT_FLAG     0xD5   // set while the pointer is over the tile
#define UT_LOADED_FLAG  0xD7
#define UT_TIP_PENDING  0xD8   // show timer armed
#define UT_TIP_EXPIRED  0xD9   // auto hidden once, blocks re-show until leave

// The tool info is exactly 0x48 wide and butts up against the tooltip
// handle at 0x80, so a wider struct would overwrite it
static_assert(sizeof(TOOLINFOW) == 0x48, "TOOLINFOW must be 0x48 for the 7850 layout");

// Source is cached at a generous size then rescaled per tile rect
#define UT_SRC_SIZE 192

// How far the tooltip floats off the taskbar edge
#define UT_TOOLTIP_GAP 6

//---Shared helpers-----------------------------------------

static inline BYTE* UT_Field(void* obj, int off) { return (BYTE*)obj + off; }

// Explorer\Advanced, CompactUserTile, 0 or absent is the full size flyout
bool IsCompactUserTile();

// The tile object, so the flyout side can reach the tooltip members
extern void* g_pUserTileObj;

//---Entry points-------------------------------------------

// Installs the _Paint and _CalcTileSizeAndPosition patches, 7850 only
void HookUserTile(void);
void UnhookUserTile(void);

// Owned by UserTileFlyout.cpp, driven from the tile subclass
void UserTile_ShowFlyout(HWND tileHwnd);
void UserTile_ShowContextMenu(HWND tileHwnd);
void UserTile_DestroyFlyout(void);
bool UserTile_FlyoutIsShown(void);
void UserTile_SetFlyoutWasVisible(bool wasVisible);
bool UserTile_TakeFlyoutWasVisible(void);
void UserTile_InvalidatePictureCache(void);

// Owned by UserTile.cpp, called from the flyout when it takes focus
void UserTile_DismissTooltip(void);
