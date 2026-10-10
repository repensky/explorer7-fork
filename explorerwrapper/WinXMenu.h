#pragma once
#include "common.h"

// Hotkey id for Win+X on the tray window, clear of the tray's own 500 to 566
#define WINX_HOTKEY_ID 0x7858

// Posted to the tray so the hotkey is registered from the tray thread
extern UINT g_winXRegisterMsg;

// Hooks the Start button menu in explorer's imports, off unless EnableWinXMenu is 1
void InstallWinXMenu();

// Called from the tray subclass on the tray thread
void RegisterWinXHotkey(HWND tray);
void OpenWinXMenuFromKeyboard(HWND tray);

// Defined with the RegisterHotKey hook so explorer7's own call skips it
BOOL RegisterHotKeyUnhooked(HWND hwnd, int id, UINT mod, UINT vk);
