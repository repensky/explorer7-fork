#pragma once
#include "common.h"

bool IsHighContrastEnabled();
extern bool g_highContrastThemeActive;
bool HasLoadedInactiveTheme();
bool IsInactiveThemePending();
HTHEME OpenLoadedInactiveTheme(HWND hwnd, LPCWSTR pszClassList, DWORD dwFlags);
void CloseLoadedInactiveThemeHandles();
void CloseRetiredInactiveThemeResources();
void ThemeManagerInitialize();
void ThemeManagerInitializeDeferred();
void ThemeManagerUninitialize();