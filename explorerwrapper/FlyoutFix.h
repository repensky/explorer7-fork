#pragma once
#include "common.h"

// Ported from the aero-flyout-fix Windhawk mod by aubymori and repensky
// Symbol lookup swapped for byte scans, see notes/flyout-frame-signatures.md

// Installs the hooks that give legacy system flyouts a real glass frame
void InstallFlyoutFix();

// Puts tray icon tooltips back against the notification area the way Windows 7 placed them
void InstallTrayTooltipFix();

// Adds or strips WS_THICKFRAME depending on whether composition is on
BOOL ApplyFlyoutFrame(HWND hwnd);

// Collapses a frame hit to a dead border so the window cannot be dragged
LRESULT LockFrameHit(LRESULT hit);

// True for the messages that would start a move or resize loop
bool IsFrameDragMessage(UINT msg, WPARAM wp);

// The float the shell reserves between a flyout and the work area edge
int NativeFlyoutGap();

// Clamps a window rect inside the work area with that gap on every edge
POINT AdjustWindowPosForTaskbar(HWND hwnd, RECT rc);

// Called from the wrapper's own CreateWindowInBand hooks in util.h
// MinHook allows one hook per target and that one was installed first
void FlyoutFixOnBandWindow(HWND hwnd, LPCWSTR className, LPCWSTR source);

// Frame and float gap for a flyout in another process, driven from explorer
// SndVol is one process per flyout, so there is nothing there to inject into
void FlyoutFixFloatForeignWindow(HWND hwnd);

// Same window again once it has moved itself back out of the gap
void FlyoutFixReclampForeignWindow(HWND hwnd);
