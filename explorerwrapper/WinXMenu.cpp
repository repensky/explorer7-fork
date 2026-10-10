#include "AuthUI.h"
#include "WinXMenu.h"
#include "OptionConfig.h"
#include "resource.h"
#include <shellapi.h>

// How each piece was read out of the binaries is in notes/winx-menu.md

UINT g_winXRegisterMsg = 0;

//---Tray commands and shutdown choices---------------------

// CTray::_Command ids, the same in 7601 and 7850
#define TRAYCMD_RUN      401
#define TRAYCMD_LOGOFF   402
#define TRAYCMD_DESKTOP  407
#define TRAYCMD_EXIT     506

// shutdownux choice values, the Start menu posts them to the tray as they are
#define CHOICE_LOGOFF     0x1
#define CHOICE_SHUTDOWN   0x2
#define CHOICE_RESTART    0x4
#define CHOICE_SLEEP      0x10
#define CHOICE_HIBERNATE  0x40
#define CHOICE_UNUSABLE   0xC0000
#define CHOICE_SEPARATOR  0x400000

// Properties in CStartButton::OnContextMenu, 7601 and 7850 alike
#define STARTBUTTON_CMD_PROPERTIES 0x7FF3

// Only Next is called, the Windows 7 Start menu uses the same slot
struct DECLSPEC_NOVTABLE IEnumShutdownChoices: public IUnknown
{
	STDMETHOD(Next)(ULONG celt, ULONG* rgelt, ULONG* pceltFetched) PURE;
};

//---Text---------------------------------------------------

// Text lives in wrapper.rc so muirct can split it into wrp64.dll.mui
// The literals below are only used when the resource is missing

enum
{
	WXS_PROGRAMS = 0,
	WXS_POWER,
	WXS_EVENTVWR,
	WXS_SYSTEM,
	WXS_DEVMGR,
	WXS_NETWORK,
	WXS_DISKMGMT,
	WXS_COMPMGMT,
	WXS_CMD,
	WXS_CMD_ADMIN,
	WXS_POWERSHELL,
	WXS_POWERSHELL_ADMIN,
	WXS_TASKMGR,
	WXS_CONTROL,
	WXS_EXPLORER,
	WXS_SEARCH,
	WXS_RUN,
	WXS_SHUTDOWN_MENU,
	WXS_DESKTOP,
	WXS_LOGOFF,
	WXS_SLEEP,
	WXS_HIBERNATE,
	WXS_SHUTDOWN,
	WXS_RESTART,
	WXS_COUNT
};

static const struct { UINT id; const wchar_t* fallback; } kWinXText[WXS_COUNT] = {
	{ IDS_WX_PROGRAMS,         L"Programs and &Features"       },
	{ IDS_WX_POWER,            L"Power &Options"               },
	{ IDS_WX_EVENTVWR,         L"Event &Viewer"                },
	{ IDS_WX_SYSTEM,           L"S&ystem"                      },
	{ IDS_WX_DEVMGR,           L"Device &Manager"              },
	{ IDS_WX_NETWORK,          L"Net&work Connections"         },
	{ IDS_WX_DISKMGMT,         L"Dis&k Management"             },
	{ IDS_WX_COMPMGMT,         L"Computer Mana&gement"         },
	{ IDS_WX_CMD,              L"&Command Prompt"              },
	{ IDS_WX_CMD_ADMIN,        L"Command Prompt (&Admin)"      },
	{ IDS_WX_POWERSHELL,       L"W&indows PowerShell"          },
	{ IDS_WX_POWERSHELL_ADMIN, L"Windows PowerShell (&Admin)"  },
	{ IDS_WX_TASKMGR,          L"&Task Manager"                },
	{ IDS_WX_CONTROL,          L"Co&ntrol Panel"               },
	{ IDS_WX_EXPLORER,         L"File &Explorer"               },
	{ IDS_WX_SEARCH,           L"&Search"                      },
	{ IDS_WX_RUN,              L"&Run"                         },
	{ IDS_WX_SHUTDOWN_MENU,    L"Sh&ut down or log off"        },
	{ IDS_WX_DESKTOP,          L"&Desktop"                     },
	{ IDS_WX_LOGOFF,           L"L&og off"                     },
	{ IDS_WX_SLEEP,            L"&Sleep"                       },
	{ IDS_WX_HIBERNATE,        L"&Hibernate"                   },
	{ IDS_WX_SHUTDOWN,         L"Sh&ut down"                   },
	{ IDS_WX_RESTART,          L"&Restart"                     },
};

static wchar_t g_winXText[WXS_COUNT][64];
static bool    g_winXTextReady = false;

// Loaded once, the menu is only ever built on the tray thread
static const wchar_t* WX_Text(int idx)
{
	if (!g_winXTextReady)
	{
		for (int i = 0; i < WXS_COUNT; ++i)
		{
			if (LoadStringW(g_hInstance, kWinXText[i].id, g_winXText[i],
			                ARRAYSIZE(g_winXText[i])) <= 0)
				StrCpyNW(g_winXText[i], kWinXText[i].fallback, ARRAYSIZE(g_winXText[i]));
		}
		g_winXTextReady = true;
	}
	return g_winXText[idx];
}

//---Menu---------------------------------------------------

enum
{
	WXC_PROGRAMS = 1,
	WXC_POWER,
	WXC_EVENTVWR,
	WXC_SYSTEM,
	WXC_DEVMGR,
	WXC_NETWORK,
	WXC_DISKMGMT,
	WXC_COMPMGMT,
	WXC_CONSOLE,
	WXC_CONSOLE_ADMIN,
	WXC_TASKMGR,
	WXC_CONTROL,
	WXC_EXPLORER,
	WXC_SEARCH,
	WXC_RUN,
	WXC_DESKTOP,
	WXC_CHOICE_FIRST = 0x100
};

// Filled each time the menu is built, read back when an item is picked
static ULONG g_choices[16];
static int   g_choiceCount = 0;
static bool  g_useCmd = false;

// Command Prompt or PowerShell, decided the way 19044 twinui decides it
static bool UseCommandPrompt()
{
	// With no value x86 and x64 get PowerShell, any other machine gets Command Prompt
	bool useCmd = true;
	typedef BOOL(WINAPI* IsWow64Process2_t)(HANDLE, USHORT*, USHORT*);
	IsWow64Process2_t isWow64Process2 = (IsWow64Process2_t)GetProcAddress(
		GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2");
	USHORT process = 0, native = 0;
	if (isWow64Process2 && isWow64Process2(GetCurrentProcess(), &process, &native))
		useCmd = !(native == IMAGE_FILE_MACHINE_I386 || native == IMAGE_FILE_MACHINE_AMD64);

	// Nonzero picks Command Prompt, read on every open like twinui does
	DWORD value = 0, cb = sizeof(value);
	if (RegGetValueW(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
		L"DontUsePowerShellOnWinX", RRF_RT_REG_DWORD, NULL, &value, &cb) == ERROR_SUCCESS)
		useCmd = value != 0;
	return useCmd;
}

// Our own text only stands in when shutdownux cannot name a choice
static const wchar_t* ChoiceFallback(ULONG choice)
{
	switch (choice)
	{
	case CHOICE_LOGOFF:    return WX_Text(WXS_LOGOFF);
	case CHOICE_SLEEP:     return WX_Text(WXS_SLEEP);
	case CHOICE_HIBERNATE: return WX_Text(WXS_HIBERNATE);
	case CHOICE_SHUTDOWN:  return WX_Text(WXS_SHUTDOWN);
	case CHOICE_RESTART:   return WX_Text(WXS_RESTART);
	}
	return nullptr;
}

static void AddChoice(HMENU menu, IShutdownChoices10* choices, ULONG choice)
{
	if (g_choiceCount >= ARRAYSIZE(g_choices))
		return;

	// Index 1 is the menu form with its accelerator, as the Start menu asks for it
	WCHAR name[128];
	LPCWSTR text = nullptr;
	if (choices && SUCCEEDED(choices->GetChoiceName(choice, 1, name, ARRAYSIZE(name))))
		text = name;
	else
		text = ChoiceFallback(choice);
	if (!text)
		return;

	g_choices[g_choiceCount] = choice;
	AppendMenuW(menu, MF_STRING, WXC_CHOICE_FIRST + g_choiceCount, text);
	g_choiceCount++;
}

// The same list the Start menu power button shows, so sleep and hibernate follow Power Options
static HMENU BuildShutdownMenu()
{
	HMENU menu = CreatePopupMenu();
	if (!menu)
		return nullptr;
	g_choiceCount = 0;

	IShutdownChoices10* choices = nullptr;
	HRESULT hr = CoCreateInstance(CLSID_AuthUIShutdownChoices, NULL, CLSCTX_INPROC_SERVER,
		IID_IShutdownChoices10, (void**)&choices);

	// The default choice mask leaves log off out, so it goes in by hand
	AddChoice(menu, choices, CHOICE_LOGOFF);

	if (choices)
	{
		// shutdownux only rereads sleep and hibernate on Refresh
		choices->Refresh();
		IEnumShutdownChoices* list = nullptr;
		if (SUCCEEDED(choices->GetChoiceEnumerator((IUnknown**)&list)) && list)
		{
			ULONG choice = 0;
			while (list->Next(1, &choice, NULL) == S_OK)
			{
				// Separators, greyed out choices and a second log off are left out
				if ((choice & (CHOICE_SEPARATOR | CHOICE_UNUSABLE)) || choice == CHOICE_LOGOFF)
					continue;
				AddChoice(menu, choices, choice);
			}
			list->Release();
		}
		choices->Release();
	}
	else
	{
		AddChoice(menu, nullptr, CHOICE_SHUTDOWN);
		AddChoice(menu, nullptr, CHOICE_RESTART);
	}

	dbgprintf(L"Win+X shutdown choices hr %X, %d listed", hr, g_choiceCount);
	return menu;
}

static HMENU BuildWinXMenu()
{
	HMENU menu = CreatePopupMenu();
	if (!menu)
		return nullptr;

	g_useCmd = UseCommandPrompt();

	AppendMenuW(menu, MF_STRING, WXC_PROGRAMS, WX_Text(WXS_PROGRAMS));
	AppendMenuW(menu, MF_STRING, WXC_POWER, WX_Text(WXS_POWER));
	AppendMenuW(menu, MF_STRING, WXC_EVENTVWR, WX_Text(WXS_EVENTVWR));
	AppendMenuW(menu, MF_STRING, WXC_SYSTEM, WX_Text(WXS_SYSTEM));
	AppendMenuW(menu, MF_STRING, WXC_DEVMGR, WX_Text(WXS_DEVMGR));
	AppendMenuW(menu, MF_STRING, WXC_NETWORK, WX_Text(WXS_NETWORK));
	AppendMenuW(menu, MF_STRING, WXC_DISKMGMT, WX_Text(WXS_DISKMGMT));
	AppendMenuW(menu, MF_STRING, WXC_COMPMGMT, WX_Text(WXS_COMPMGMT));

	AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

	AppendMenuW(menu, MF_STRING, WXC_CONSOLE, WX_Text(g_useCmd ? WXS_CMD : WXS_POWERSHELL));
	AppendMenuW(menu, MF_STRING, WXC_CONSOLE_ADMIN, WX_Text(g_useCmd ? WXS_CMD_ADMIN : WXS_POWERSHELL_ADMIN));

	AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

	AppendMenuW(menu, MF_STRING, WXC_TASKMGR, WX_Text(WXS_TASKMGR));
	AppendMenuW(menu, MF_STRING, WXC_CONTROL, WX_Text(WXS_CONTROL));
	AppendMenuW(menu, MF_STRING, WXC_EXPLORER, WX_Text(WXS_EXPLORER));
	AppendMenuW(menu, MF_STRING, WXC_SEARCH, WX_Text(WXS_SEARCH));
	AppendMenuW(menu, MF_STRING, WXC_RUN, WX_Text(WXS_RUN));

	AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

	HMENU shutdown = BuildShutdownMenu();
	if (shutdown)
		AppendMenuW(menu, MF_POPUP, (UINT_PTR)shutdown, WX_Text(WXS_SHUTDOWN_MENU));
	AppendMenuW(menu, MF_STRING, WXC_DESKTOP, WX_Text(WXS_DESKTOP));
	return menu;
}

//---Running a pick-----------------------------------------

static void Launch(LPCWSTR file, LPCWSTR params, bool admin, LPCWSTR dir)
{
	SHELLEXECUTEINFOW sei = { sizeof(sei) };
	sei.lpVerb = admin ? L"runas" : L"open";
	sei.lpFile = file;
	sei.lpParameters = params;
	sei.lpDirectory = dir;
	sei.nShow = SW_SHOWNORMAL;
	BOOL ok = ShellExecuteExW(&sei);
	dbgprintf(L"Win+X launch %s %s admin %d, ok %d error %u",
		file, params ? params : L"", admin, ok, ok ? 0 : GetLastError());
}

// The consoles start in the user's folder, the elevated ones in System32 as Windows 10 does
static void LaunchConsole(bool admin)
{
	WCHAR path[MAX_PATH];
	UINT len = GetSystemDirectoryW(path, MAX_PATH);
	if (!len || len >= MAX_PATH)
		return;
	if (FAILED(StringCchCatW(path, MAX_PATH,
		g_useCmd ? L"\\cmd.exe" : L"\\WindowsPowerShell\\v1.0\\powershell.exe")))
		return;

	PWSTR profile = nullptr;
	if (!admin)
		SHGetKnownFolderPath(FOLDERID_Profile, 0, NULL, &profile);
	Launch(path, nullptr, admin, profile);
	if (profile)
		CoTaskMemFree(profile);
}

static void OpenComputer()
{
	PIDLIST_ABSOLUTE pidl = nullptr;
	if (FAILED(SHGetKnownFolderIDList(FOLDERID_ComputerFolder, 0, NULL, &pidl)))
		return;
	SHELLEXECUTEINFOW sei = { sizeof(sei) };
	sei.fMask = SEE_MASK_IDLIST;
	sei.lpIDList = pidl;
	sei.nShow = SW_SHOWNORMAL;
	BOOL ok = ShellExecuteExW(&sei);
	dbgprintf(L"Win+X open Computer, ok %d", ok);
	CoTaskMemFree(pidl);
}

static void RunWinXCommand(UINT cmd, HWND tray)
{
	switch (cmd)
	{
	case WXC_PROGRAMS:      Launch(L"control.exe", L"appwiz.cpl", false, nullptr); return;
	case WXC_POWER:         Launch(L"control.exe", L"powercfg.cpl", false, nullptr); return;
	case WXC_EVENTVWR:      Launch(L"mmc.exe", L"eventvwr.msc", false, nullptr); return;
	case WXC_SYSTEM:        Launch(L"control.exe", L"/name Microsoft.System", false, nullptr); return;
	case WXC_DEVMGR:        Launch(L"mmc.exe", L"devmgmt.msc", false, nullptr); return;
	case WXC_NETWORK:       Launch(L"control.exe", L"ncpa.cpl", false, nullptr); return;
	case WXC_DISKMGMT:      Launch(L"mmc.exe", L"diskmgmt.msc", false, nullptr); return;
	case WXC_COMPMGMT:      Launch(L"mmc.exe", L"compmgmt.msc", false, nullptr); return;
	case WXC_CONSOLE:       LaunchConsole(false); return;
	case WXC_CONSOLE_ADMIN: LaunchConsole(true); return;
	case WXC_TASKMGR:       Launch(L"taskmgr.exe", nullptr, false, nullptr); return;
	case WXC_CONTROL:       Launch(L"control.exe", nullptr, false, nullptr); return;
	case WXC_EXPLORER:      OpenComputer(); return;
	case WXC_SEARCH:        Launch(L"search-ms:", nullptr, false, nullptr); return;
	case WXC_RUN:           PostMessageW(tray, WM_COMMAND, TRAYCMD_RUN, 0); return;
	case WXC_DESKTOP:       PostMessageW(tray, WM_COMMAND, TRAYCMD_DESKTOP, 0); return;
	}

	int index = (int)cmd - WXC_CHOICE_FIRST;
	if (index < 0 || index >= g_choiceCount)
		return;

	// Exactly what CLogoffPane::PostTrayCommand sends, so nothing is forced closed
	ULONG choice = g_choices[index];
	UINT trayCmd = choice == CHOICE_LOGOFF ? TRAYCMD_LOGOFF : TRAYCMD_EXIT;
	PostMessageW(tray, WM_COMMAND, trayCmd, (LPARAM)choice);
	dbgprintf(L"Win+X shutdown choice %X sent to the tray as %u", choice, trayCmd);
}

//---Start button menu hooks--------------------------------

typedef BOOL(WINAPI* TrackPopupMenu_t)(HMENU, UINT, int, int, int, HWND, const RECT*);
typedef BOOL(WINAPI* TrackPopupMenuEx_t)(HMENU, UINT, int, int, HWND, LPTPMPARAMS);
static TrackPopupMenu_t   g_realTrackPopupMenu;
static TrackPopupMenuEx_t g_realTrackPopupMenuEx;

// The owner test comes first, it is one property read for every other menu
static bool IsStartButtonMenu(HMENU menu, HWND owner)
{
	return owner && GetPropW(owner, L"StartButtonTag")
		&& GetMenuState(menu, STARTBUTTON_CMD_PROPERTIES, MF_BYCOMMAND) != (UINT)-1;
}

// Docks the menu on the taskbar edge beside the Start button, where Windows 10 puts it
// Explorer keeps it clear of the whole Start button, which left a gap above the taskbar
static UINT PlaceOnTaskbar(HWND owner, HWND tray, int* x, int* y, TPMPARAMS* params)
{
	RECT trayRect, buttonRect;
	if (!tray || !GetWindowRect(tray, &trayRect) || !GetWindowRect(owner, &buttonRect))
		return 0;
	MONITORINFO mi = { sizeof(mi) };
	if (!GetMonitorInfoW(MonitorFromWindow(tray, MONITOR_DEFAULTTONEAREST), &mi))
		return 0;

	params->cbSize = sizeof(*params);
	params->rcExclude = trayRect;

	const RECT& screen = mi.rcMonitor;
	if (trayRect.right - trayRect.left >= trayRect.bottom - trayRect.top)
	{
		bool atTop = trayRect.top + trayRect.bottom < screen.top + screen.bottom;
		*x = buttonRect.left > screen.left ? buttonRect.left : screen.left;
		*y = atTop ? trayRect.bottom : trayRect.top;
		return TPM_VERTICAL | TPM_LEFTALIGN | (atTop ? TPM_TOPALIGN : TPM_BOTTOMALIGN);
	}

	bool atLeft = trayRect.left + trayRect.right < screen.left + screen.right;
	*x = atLeft ? trayRect.right : trayRect.left;
	*y = buttonRect.top > screen.top ? buttonRect.top : screen.top;
	return TPM_TOPALIGN | (atLeft ? TPM_LEFTALIGN : TPM_RIGHTALIGN);
}

// Shown against the taskbar, then explorer is told nothing was picked
static void TrackWinXMenu(UINT flags, int x, int y, HWND owner, LPTPMPARAMS params)
{
	HMENU menu = BuildWinXMenu();
	if (!menu)
		return;

	HWND tray = GetWindow(owner, GW_OWNER);
	if (!tray)
		tray = FindWindowW(L"Shell_TrayWnd", NULL);

	// Explorer's own place is kept only if the taskbar cannot be measured
	TPMPARAMS docked;
	UINT place = PlaceOnTaskbar(owner, tray, &x, &y, &docked);
	if (place)
	{
		flags = (flags & (TPM_LAYOUTRTL | TPM_RIGHTBUTTON)) | place;
		params = &docked;
	}

	// A menu only closes on an outside click while its owner holds the foreground
	BOOL foreground = SetForegroundWindow(owner);
	UINT cmd = (UINT)g_realTrackPopupMenuEx(menu, flags | TPM_RETURNCMD | TPM_NONOTIFY, x, y, owner, params);
	PostMessageW(owner, WM_NULL, 0, 0);
	DestroyMenu(menu);

	dbgprintf(L"Win+X menu at %d,%d flags %X, docked %d, foreground %d, picked %u", x, y, flags, place != 0, foreground, cmd);
	if (cmd && tray)
		RunWinXCommand(cmd, tray);
}

// Right click on the Start button, CStartButton::OnContextMenu
static BOOL WINAPI TrackPopupMenu_WinX(HMENU menu, UINT flags, int x, int y, int reserved, HWND owner, const RECT* rect)
{
	if (!IsStartButtonMenu(menu, owner))
		return g_realTrackPopupMenu(menu, flags, x, y, reserved, owner, rect);
	TrackWinXMenu(flags, x, y, owner, nullptr);
	return FALSE;
}

// Keyboard route, from CStartButton::TrackMenu
static BOOL WINAPI TrackPopupMenuEx_WinX(HMENU menu, UINT flags, int x, int y, HWND owner, LPTPMPARAMS params)
{
	if (!IsStartButtonMenu(menu, owner))
		return g_realTrackPopupMenuEx(menu, flags, x, y, owner, params);
	TrackWinXMenu(flags, x, y, owner, params);
	return FALSE;
}

void InstallWinXMenu()
{
	if (!s_EnableWinXMenu)
		return;

	HMODULE user32 = GetModuleHandleW(L"user32.dll");
	g_realTrackPopupMenu = (TrackPopupMenu_t)GetProcAddress(user32, "TrackPopupMenu");
	g_realTrackPopupMenuEx = (TrackPopupMenuEx_t)GetProcAddress(user32, "TrackPopupMenuEx");
	if (!g_realTrackPopupMenu || !g_realTrackPopupMenuEx)
		return;

	HMODULE explorer = GetModuleHandleW(NULL);
	BOOL mouse = ChangeImportedAddress(explorer, "user32.dll", g_realTrackPopupMenu, TrackPopupMenu_WinX);
	BOOL keyboard = ChangeImportedAddress(explorer, "user32.dll", g_realTrackPopupMenuEx, TrackPopupMenuEx_WinX);

	// Only set while the menu is on, so the tray subclass ignores it otherwise
	g_winXRegisterMsg = RegisterWindowMessageW(L"explorer7.WinXMenu.Register");
	dbgprintf(L"Win+X menu on, Start button hooks right click %d keyboard %d", mouse, keyboard);
}

//---Win+X hotkey-------------------------------------------

void RegisterWinXHotkey(HWND tray)
{
	BOOL ok = RegisterHotKeyUnhooked(tray, WINX_HOTKEY_ID, MOD_WIN | MOD_NOREPEAT, 'X');
	dbgprintf(L"Win+X hotkey on the tray %p, registered %d error %u", tray, ok, ok ? 0 : GetLastError());
}

// The Start button is a popup owned by the tray, tagged when CTray::_CreateWindows makes it
static HWND FindStartButton(HWND tray)
{
	HWND button = NULL;
	while ((button = FindWindowExW(NULL, button, L"Button", NULL)) != NULL)
	{
		if (GetWindow(button, GW_OWNER) == tray && GetPropW(button, L"StartButtonTag"))
			return button;
	}
	return NULL;
}

// A keyboard context menu on the Start button, the hook then swaps and docks it
void OpenWinXMenuFromKeyboard(HWND tray)
{
	HWND start = FindStartButton(tray);
	dbgprintf(L"Win+X pressed, Start button %p", start);
	if (start)
		PostMessageW(start, WM_CONTEXTMENU, (WPARAM)start, (LPARAM)-1);
}
