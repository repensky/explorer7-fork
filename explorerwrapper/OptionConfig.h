#pragma once
#include "common.h"
#include "dbgprint.h"
#include "OSVersion.h"
#include "RegistryManager.h"

// Which explorer build the byte patterns and struct offsets are chosen for
#define BUILDRUNTIME_WIN7 1
#define BUILDRUNTIME_7850 2

// Individual option definitions
// - These are external to ensure we can call them elsewhere
extern int s_BuildRuntime;
extern bool s_ClassicTheme;
extern bool s_DisableComposition;
extern int s_EnableImmersiveShellStack;
extern bool s_UseTaskbarPinning;
extern bool s_ShowStoreAppsOnTaskbar;
extern bool s_ShowStoreAppsInStart;
extern int s_AcrylicAlt;
extern int s_ColorizationOptions;
extern bool s_OverrideAlpha;
extern DWORD s_AlphaValue;
extern bool s_UseDCompFlyouts;
extern bool s_ShellUIAccentOverride;
extern int s_Win7DesktopIconRows;
extern bool s_EnableWinXMenu;

// Responsible for settings these values and calling them from registry
extern void RefreshThemeConfiguration();
extern void InitializeConfiguration();
