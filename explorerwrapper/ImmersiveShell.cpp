#pragma warning(disable:4302) // type conversions used by Windows 8.1 immersive code
#pragma warning(disable:4311) // type conversions used by Windows 8.1 immersive code
#pragma warning(disable:4312) // type conversions used by Windows 8.1 immersive code

#include "ImmersiveShell.h"
#include "dbgprint.h"
#include "OSVersion.h"
#include "WindowArrangementShell.h"

typedef HWND(WINAPI* GetTaskmanWindow)();
typedef BOOL(WINAPI* SetTaskmanWindow)(HWND handle);
typedef HRESULT(CALLBACK* SetShellWindow)(HWND hwnd);

GetTaskmanWindow GetTaskmanWindowFunc = NULL;
SetTaskmanWindow SetTaskmanWindowFunc = NULL;

UINT shellhook = 0;
IImmersiveShellHookService* ShellHookService;
static HWND g_taskmanWnd;

static bool successfullySetShellWindow = false;

// Defined in dllmain.cpp, hooks the 24H2 immersive component dispatcher
extern void InstallImmersiveComponentVeto();

DWORD WINAPI TwinThread( LPVOID lpParameter )
{
	CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
	IImmersiveBehavior* behavior;
	HRESULT ret = CoUnmarshalInterface((IStream*)lpParameter, IID_ImmersiveBehavior, (PVOID*)&behavior);
	dbgprintf(L"IImmersiveBehavior %p %p",ret,behavior);
	UINT count;	
	behavior->GetMaximumComponentCount(&count);
	UINT i;
	for (i=0;i<count;i++)
	{
		dbgprintf(L"creating TwinUI component %d",i);
		IUnknown* component;
		HRESULT ret = behavior->CreateComponent(i,&component);
		dbgprintf(L"created TwinUI component %p %p",ret,component);
	}
	return 0;
}


// Every shell hook writes a log line, and each line opens and closes the file
// That is thousands of file writes a second under churn so it is opt in
static bool ShellHookLoggingEnabled()
{
	static int cached = -1;
	if (cached < 0)
	{
		DWORD v = 0, cb = sizeof(v);
		RegGetValueW(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
			L"LogShellHooks", RRF_RT_REG_DWORD, nullptr, &v, &cb);
		cached = v ? 1 : 0;
	}
	return cached != 0;
}

LRESULT TaskmanWndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
	if (msg == WM_CREATE)
	{
		g_taskmanWnd = hwnd;
		shellhook = RegisterWindowMessageW(L"SHELLHOOK");
		if (!shellhook)
		{
			dbgprintf(L"failed to register shellhook\n");
		}
		if (!SetTaskmanWindowFunc(hwnd))
		{
			dbgprintf(L"failed to register taskman window\n");
		}
		if (!RegisterShellHookWindow(hwnd))
		{
			dbgprintf(L"register shellhook window failed\n");
		}

	}
	else if (msg == WM_DESTROY)
	{
		if (GetTaskmanWindowFunc() == hwnd)
		{
			SetTaskmanWindowFunc(NULL);
		}
		DeregisterShellHookWindow(hwnd);
	}
	else
	{
		if (msg == shellhook && msg != WM_HOTKEY)
		{
			// Every shell hook that reaches the taskman window, the Windows key arrives as code 7
			if (ShellHookLoggingEnabled())
				dbgprintf(L"taskman shellhook code %u lparam %p service %p", (UINT)w, (void*)l, ShellHookService);

			// Code 7 answers the task list request the wrapper sends itself, the kernel has just granted foreground
			if ((UINT)w == 7 && g_osVersion.BuildNumber() >= 26100)
			{
				HWND tray = FindWindowW(L"Shell_TrayWnd", NULL);
				if (tray)
					PostMessageW(tray, 0x504, 0, 0);
				return 0;
			}

			// Which windows the kernel holds as shell and taskman window once the shell is up
			static bool loggedShellState = false;
			if (!loggedShellState)
			{
				loggedShellState = true;
				dbgprintf(L"shell window %p taskman window %p ours %p", GetShellWindow(), GetTaskmanWindowFunc(), hwnd);
			}
			if (ShellHookService)
			{
				BOOL handle = TRUE;
				if ((UINT)w == 12)
				{
					ShellHookService->SetTargetWindowForSerialization((HWND)l);
				}
				else if ((UINT)w == 0x32)
				{
					handle = FALSE;
				}
				if (handle)
				{
					ShellHookService->PostShellHookMessage(w, l);
				}
				return 0;
			}

			// The shell is not registered yet early on, so this has to keep trying
			// It is an out of process activation, so failures back off instead of per hook
			static ULONGLONG lastServiceAttempt = 0;
			ULONGLONG nowAttempt = GetTickCount64();
			if (lastServiceAttempt == 0 || nowAttempt - lastServiceAttempt >= 2000)
			{
				lastServiceAttempt = nowAttempt;

				GUID guidImmersiveShell;
				CLSIDFromString(L"{c2f03a33-21f5-47fa-b4bb-156362a2f239}", &guidImmersiveShell);

				GUID SID_ImmersiveShellHookService;
				CLSIDFromString(L"{4624bd39-5fc3-44a8-a809-163a836e9031}", &SID_ImmersiveShellHookService);

				GUID SID_Unknown;
				CLSIDFromString(L"{914d9b3a-5e53-4e14-bbba-46062acb35a4}", &SID_Unknown);

				IServiceProvider* ImmersiveShell;
				HRESULT hrShell = CoCreateInstance(guidImmersiveShell, 0, 0x404u, IID_IServiceProvider, (LPVOID*)&ImmersiveShell);
				HRESULT hrService = E_FAIL;
				if (hrShell >= 0)
				{
					hrService = ImmersiveShell->QueryService(SID_ImmersiveShellHookService, SID_Unknown, (void**)&ShellHookService);
				}

				if (ShellHookService || ShellHookLoggingEnabled())
					dbgprintf(L"shell hook service lookup, create %08X query %08X service %p",
						hrShell, hrService, ShellHookService);
			}
		}
	}
	return DefWindowProc(hwnd, msg, w, l);
}

//---Windows key on 26100-----------------------------------
// The kernel drops the bare key before it reports it, see notes/24h2-support.md
// A low level hook sees every key and hands a bare tap to the tray as message 0x504

static HHOOK g_winKeyHook;
static bool g_winKeyPending;
static HWND g_winKeyForeground;

// One key event from either route, a bare tap is a press and release with nothing in between
static void WinKeyEvent(UINT vk, bool up)
{
	if (vk == VK_LWIN || vk == VK_RWIN)
	{
		if (!up)
		{
			g_winKeyPending = true;
			g_winKeyForeground = GetForegroundWindow();
		}
		else if (g_winKeyPending)
		{
			g_winKeyPending = false;

			// A foreground change during the hold cancels the tap, the rule the kernel applies too
			HWND foreground = GetForegroundWindow();

			// SC_TASKLIST to our taskman window makes the kernel grant the shell foreground and answer with code 7
			if (foreground == g_winKeyForeground && g_taskmanWnd)
				PostMessageW(g_taskmanWnd, WM_SYSCOMMAND, SC_TASKLIST, 0);
		}
	}
	else if (!up && vk != 0xFF)
	{
		// Any other key while the Windows key is held makes it a chord
		g_winKeyPending = false;
	}
}

static LRESULT CALLBACK WinKeyHook(int code, WPARAM w, LPARAM l)
{
	if (code == HC_ACTION)
	{
		KBDLLHOOKSTRUCT* key = (KBDLLHOOKSTRUCT*)l;
		WinKeyEvent(key->vkCode, (key->flags & LLKHF_UP) != 0);
	}
	return CallNextHookEx(g_winKeyHook, code, w, l);
}

// A raw input sink is fed by the kernel whichever window is in front, the hook is not
static LRESULT CALLBACK WinKeySinkProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
	if (msg == WM_INPUT)
	{
		RAWINPUT raw;
		UINT size = sizeof(raw);
		if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1
			&& raw.header.dwType == RIM_TYPEKEYBOARD)
		{
			WinKeyEvent(raw.data.keyboard.VKey, (raw.data.keyboard.Flags & RI_KEY_BREAK) != 0);
		}
	}
	return DefWindowProcW(hwnd, msg, w, l);
}

static bool StartWinKeySink()
{
	WNDCLASSEXW cls = { sizeof(cls) };
	cls.lpfnWndProc = WinKeySinkProc;
	cls.hInstance = GetModuleHandleW(NULL);
	cls.lpszClassName = L"Explorer7WinKeySink";
	if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
		return false;
	HWND sink = CreateWindowExW(0, cls.lpszClassName, NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, cls.hInstance, NULL);
	if (!sink)
		return false;
	RAWINPUTDEVICE device = { 1, 6, RIDEV_INPUTSINK, sink };
	BOOL ok = RegisterRawInputDevices(&device, 1, sizeof(device));
	dbgprintf(L"Windows key raw input sink %p ok %d error %u", sink, ok, GetLastError());
	return ok != FALSE;
}

//---Start menu left open after a UAC prompt----------------
// 26100 will not hand the foreground back to the menu when the secure desktop goes away
// Handing it back restores the step Win7 relies on, the menu then dismisses itself as usual

// Later than this after the switch is the user's own doing, not the kernel's restore
#define SWITCH_GRACE_MS 1500
// Tray message that runs CStartButton::CloseStartMenu, the Run dialog uses it too
#define TRAY_CLOSE_STARTMENU 0x40C

static HWINEVENTHOOK g_foregroundHook;
static HWINEVENTHOOK g_menuLifeHook;
static HWND g_startMenu;
static bool g_menuOpen;
static ULONGLONG g_menuOpenedAt;
static ULONGLONG g_lastSwitchTick;
static bool g_handedBack;
static bool g_awaitLaunch;
static DWORD g_ourPid;

static ULONGLONG NowAsFileTime()
{
	FILETIME now = {};
	GetSystemTimeAsFileTime(&now);
	return ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
}

// Only the process that gave the last input may take the foreground, a lone ALT release earns that
// Without it the menu goes active inside this thread only and never hears the deactivation
static void ClaimForegroundRight()
{
	INPUT input = {};
	input.type = INPUT_KEYBOARD;
	input.ki.wVk = VK_MENU;
	input.ki.dwFlags = KEYEVENTF_KEYUP;
	SendInput(1, &input, sizeof(input));
}

// The head of SYSTEM_PROCESS_INFORMATION, CreateTime checked against GetProcessTimes on 142 processes
struct PROCESS_LIST_ENTRY
{
	ULONG NextEntryOffset;
	ULONG NumberOfThreads;
	LARGE_INTEGER WorkingSetPrivateSize;
	ULONG HardFaultCount;
	ULONG NumberOfThreadsHighWatermark;
	ULONGLONG CycleTime;
	LARGE_INTEGER CreateTime;
	LARGE_INTEGER UserTime;
	LARGE_INTEGER KernelTime;
	struct { USHORT Length; USHORT MaximumLength; PWSTR Buffer; } ImageName;
	LONG BasePriority;
	HANDLE UniqueProcessId;
};
static_assert(offsetof(PROCESS_LIST_ENTRY, CreateTime) == 0x20, "CreateTime follows CycleTime");
static_assert(offsetof(PROCESS_LIST_ENTRY, UniqueProcessId) == 0x50, "UniqueProcessId sits where winternl.h puts it");

typedef LONG(NTAPI* NtQuerySystemInformation_t)(ULONG, PVOID, ULONG, PULONG);
static NtQuerySystemInformation_t g_querySystemInformation;

// An elevated program refuses OpenProcess from here, the system process list still carries its start time
static bool StartTimeFromProcessList(DWORD pid, ULONGLONG* created)
{
	if (!g_querySystemInformation)
		return false;
	ULONG size = 0x80000;
	for (int attempt = 0; attempt < 4; attempt++)
	{
		BYTE* buffer = (BYTE*)HeapAlloc(GetProcessHeap(), 0, size);
		if (!buffer)
			return false;
		ULONG needed = 0;
		// Class 5 is SystemProcessInformation, 0xC0000004 asks for a bigger buffer
		LONG status = g_querySystemInformation(5, buffer, size, &needed);
		if (status == (LONG)0xC0000004)
		{
			HeapFree(GetProcessHeap(), 0, buffer);
			size = needed + 0x10000;
			continue;
		}
		bool found = false;
		for (BYTE* entry = buffer; status >= 0;)
		{
			PROCESS_LIST_ENTRY* process = (PROCESS_LIST_ENTRY*)entry;
			if ((DWORD)(ULONG_PTR)process->UniqueProcessId == pid)
			{
				*created = (ULONGLONG)process->CreateTime.QuadPart;
				found = true;
				break;
			}
			if (!process->NextEntryOffset)
				break;
			entry += process->NextEntryOffset;
		}
		HeapFree(GetProcessHeap(), 0, buffer);
		return found;
	}
	return false;
}

// A program born after the menu opened is a real launch and has every right to the foreground
static bool StartedAfterTheMenu(DWORD pid)
{
	ULONGLONG when = 0;
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (process)
	{
		FILETIME created = {}, exited = {}, kernel = {}, user = {};
		if (GetProcessTimes(process, &created, &exited, &kernel, &user))
			when = ((ULONGLONG)created.dwHighDateTime << 32) | created.dwLowDateTime;
		CloseHandle(process);
	}
	// An elevated window left open from before has to count as old, or the hand back never happens
	if (!when && !StartTimeFromProcessList(pid, &when))
		return true;
	return when >= g_menuOpenedAt;
}

// The menu window is built once and reused, so its own hide is the only honest end of a session
static void CALLBACK MenuLifeProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
	LONG idObject, LONG idChild, DWORD, DWORD)
{
	if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF)
		return;
	if (!hwnd || hwnd != g_startMenu)
		return;
	if (event != EVENT_OBJECT_HIDE && event != EVENT_OBJECT_DESTROY)
		return;
	dbgprintf(L"start menu closed");
	g_startMenu = NULL;
	g_menuOpen = false;
	g_awaitLaunch = false;
}

static void CALLBACK ForegroundProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
	LONG idObject, LONG idChild, DWORD, DWORD)
{
	if (event == EVENT_SYSTEM_DESKTOPSWITCH)
	{
		g_lastSwitchTick = GetTickCount64();
		g_handedBack = false;
		g_awaitLaunch = false;
		if (g_startMenu)
			dbgprintf(L"start menu open, desktop switch");
		return;
	}
	if (event != EVENT_SYSTEM_FOREGROUND)
		return;
	if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd)
		return;

	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);

	// The menu takes the foreground as it opens, that is the window worth remembering
	if (pid == g_ourPid)
	{
		WCHAR cls[64] = L"";
		GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
		if (wcscmp(cls, L"DV2ControlHost") == 0)
		{
			// Only a menu that was not already open starts the clock, a hand back must not restart it
			if (!g_menuOpen || g_startMenu != hwnd)
			{
				g_startMenu = hwnd;
				g_menuOpen = true;
				g_menuOpenedAt = NowAsFileTime();
			}
			return;
		}
	}

	if (!g_startMenu)
		return;

	// The desktop and the taskbar are this process too, and never the launch
	bool launch = pid != g_ourPid && StartedAfterTheMenu(pid);
	ULONGLONG sinceSwitch = GetTickCount64() - g_lastSwitchTick;
	dbgprintf(L"start menu open, foreground went to pid %u %s, %u ms after a desktop switch, handed back %d, waiting %d",
		pid, launch ? L"started after the menu" : L"older than the menu", (DWORD)sinceSwitch, g_handedBack, g_awaitLaunch);

	// A program born after the menu opened is the launch, anything older was already running
	if (launch)
	{
		// A menu that really holds the foreground closes itself, this is only for when it does not
		if (g_awaitLaunch)
		{
			g_awaitLaunch = false;
			HWND tray = FindWindowW(L"Shell_TrayWnd", NULL);
			if (tray)
				PostMessageW(tray, TRAY_CLOSE_STARTMENU, 0, 0);
			dbgprintf(L"pid %u launched and took over, closing the Start menu", pid);
		}
		return;
	}

	// Outside the grace window the user picked this program, leave the foreground alone
	if (sinceSwitch > SWITCH_GRACE_MS)
		return;
	// One hand back per switch, so a program that starts a moment later cannot be robbed twice
	if (g_handedBack)
		return;

	// Left over from before the menu opened, so this is the restore picking the wrong window
	g_handedBack = true;
	ClaimForegroundRight();
	BOOL ok = SetForegroundWindow(g_startMenu);
	// Refused leaves the menu active in name only, so wait for a launch rather than closing now
	g_awaitLaunch = !ok;
	dbgprintf(L"desktop switch gave the foreground to pid %u, returned it to the Start menu, ok %d", pid, ok);
}

// Both routes need a thread that pumps messages and never does anything else
// An out of context WinEvent hook is delivered on that queue, so these live here too
static DWORD WINAPI WinKeyHookThread(LPVOID)
{
	if (!StartWinKeySink())
	{
		g_winKeyHook = SetWindowsHookExW(WH_KEYBOARD_LL, WinKeyHook, GetModuleHandleW(NULL), 0);
		dbgprintf(L"Windows key hook %p error %u", g_winKeyHook, GetLastError());
	}

	g_ourPid = GetCurrentProcessId();
	g_querySystemInformation = (NtQuerySystemInformation_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
	g_foregroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_DESKTOPSWITCH,
		NULL, ForegroundProc, 0, 0, WINEVENT_OUTOFCONTEXT);
	g_menuLifeHook = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE,
		NULL, MenuLifeProc, 0, 0, WINEVENT_OUTOFCONTEXT);
	dbgprintf(L"start menu foreground watch %p life watch %p", g_foregroundHook, g_menuLifeHook);

	MSG msg;
	while (GetMessageW(&msg, NULL, 0, 0) > 0)
		DispatchMessageW(&msg);

	if (g_foregroundHook)
		UnhookWinEvent(g_foregroundHook);
	if (g_menuLifeHook)
		UnhookWinEvent(g_menuLifeHook);
	return 0;
}

void StartWinKeyHook()
{
	static bool started = false;
	if (started || g_osVersion.BuildNumber() < 26100)
		return;
	started = true;
	HANDLE thread = CreateThread(NULL, 0, WinKeyHookThread, NULL, 0, NULL);
	if (thread)
		CloseHandle(thread);
}

void CreateTaskManWindow()
{
	// create taskman class (handles taskbar buttons)
	WNDCLASSEX taskmanclass = {};

	taskmanclass.cbClsExtra = 0;
	taskmanclass.hIcon = 0;
	taskmanclass.lpszMenuName = 0;
	taskmanclass.hIconSm = 0;
	taskmanclass.cbSize = sizeof(WNDCLASSEXW);
	taskmanclass.style = 8;
	taskmanclass.lpfnWndProc = (WNDPROC)TaskmanWndProc;
	taskmanclass.cbWndExtra = 8;
	taskmanclass.hInstance = GetModuleHandle(NULL);
	taskmanclass.hCursor = LoadCursor(NULL, IDC_ARROW);
	taskmanclass.hbrBackground = (HBRUSH)2;
	taskmanclass.lpszClassName = TEXT("TaskmanWndClass");

	if (!RegisterClassExW(&taskmanclass))
	{
		return;
	}
	auto Taskman = CreateWindowExW(0, L"TaskmanWndClass", NULL, 0x82000000, 0, 0, 0, 0, 0, 0, 0, 0);
}

void CreateTwinUI()
{
	PVOID pv;
	if (SUCCEEDED(CoCreateInstance(CLSID_ImmersiveShellBuilder, NULL, 1, IID_ImmersiveShellBuilder, &pv)))
	{
		dbgprintf(L"TwinUI factory created!");
		IImmersiveShellCreator* ImmersiveShellCreator = (IImmersiveShellCreator*)pv;
		IImmersiveShellController* controller;
		HRESULT ret = ImmersiveShellCreator->CreateShell(&controller);
		dbgprintf(L"TwinUI instance created %p %p", ret, controller);
		if (SUCCEEDED(ret))
		{
			//HRESULT ret = controller->Start();
			IStream* someinterface = (IStream*)*(DWORD*)((DWORD)controller + 0x34);
			IImmersiveBehavior* behavior;
			CoUnmarshalInterface((IStream*)someinterface, IID_ImmersiveBehavior, (PVOID*)&behavior);
			controller->SetCreationBehavior(new CImmersiveBehaviorWrapper(behavior));
			controller->Start();
			/*CreateThread(NULL,0,TwinThread,(PVOID)someinterface,0,NULL);*/
		}
		/*ret = CoCreateInstance(CLSID_ImmersiveShell,NULL,0x404,IID_ImmersiveShell,&pv);
		dbgprintf(L"Immersive Shell created: %p",ret);*/
	}
}

void CreateTwinUI_UWP()
{
	auto user32 = LoadLibrary(TEXT("user32.dll"));
	GetTaskmanWindowFunc = (GetTaskmanWindow)GetProcAddress(user32, "GetTaskmanWindow");
	SetTaskmanWindowFunc = (SetTaskmanWindow)GetProcAddress(user32, "SetTaskmanWindow");

	CreateTaskManWindow();

	// Before twinui loads, its immersive thread takes the key the side snap window needs
	InstallWindowArrangementShell();

	IImmersiveShellCreator* ImmersiveShellCreator;
	if (SUCCEEDED(CoCreateInstance(CLSID_ImmersiveShellBuilder, NULL, CLSCTX_INPROC_SERVER, IID_ImmersiveShellBuilder, (LPVOID*)&ImmersiveShellCreator)))
	{
		dbgprintf(L"TwinUI factory created!");

		IImmersiveShellController* controller;
		HRESULT ret = ImmersiveShellCreator->CreateShell(&controller);
		dbgprintf(L"TwinUI instance created %p %p", ret, controller);
		if (SUCCEEDED(ret))
		{
			// Arm the component veto before Start spawns the service thread
			InstallImmersiveComponentVeto();
			HRESULT hr = controller->Start();

			dbgprintf(L"Immersive Shell Controller Result: %x", hr);
		}
	}
}

// Ittr: Below has to exist for non-UWP mode to work

CImmersiveBehaviorWrapper::CImmersiveBehaviorWrapper(IImmersiveBehavior* behavior)
{
	m_cRef = 1;
	m_behavior = behavior;
	m_behavior->AddRef();
}

CImmersiveBehaviorWrapper::~CImmersiveBehaviorWrapper()
{
	dbgprintf(L"CImmersiveBehaviorWrapper::~CImmersiveBehaviorWrapper()");
	m_behavior->Release();
}

HRESULT STDMETHODCALLTYPE CImmersiveBehaviorWrapper::QueryInterface(REFIID riid, void** ppvObject)
{
	WCHAR iid[100];
	StringFromGUID2(riid, iid, 100);
	dbgprintf(L"CImmersiveBehaviorWrapper::QueryInterface %s", iid);
	if (riid == IID_ImmersiveBehavior)
	{
		*ppvObject = static_cast<IImmersiveBehavior*>(this);
		return S_OK;
	}
	return m_behavior->QueryInterface(riid, ppvObject);
}

ULONG STDMETHODCALLTYPE CImmersiveBehaviorWrapper::AddRef(void)
{
	return InterlockedIncrement(&m_cRef);
}

ULONG STDMETHODCALLTYPE CImmersiveBehaviorWrapper::Release(void)
{
	dbgprintf(L"CImmersiveBehaviorWrapper::release()");
	if (InterlockedDecrement(&m_cRef) == 0)
	{
		delete this;
		return 0;
	}
	return m_cRef;
}

HRESULT STDMETHODCALLTYPE CImmersiveBehaviorWrapper::OnImmersiveThreadStart(void)
{
	dbgprintf(L"CImmersiveBehaviorWrapper::OnImmersiveThreadStart");
	return m_behavior->OnImmersiveThreadStart();
}

HRESULT STDMETHODCALLTYPE CImmersiveBehaviorWrapper::OnImmersiveThreadStop(void)
{
	dbgprintf(L"CImmersiveBehaviorWrapper::OnImmersiveThreadStop");
	return m_behavior->OnImmersiveThreadStart();
}

HRESULT STDMETHODCALLTYPE CImmersiveBehaviorWrapper::GetMaximumComponentCount(unsigned int* count)
{
	dbgprintf(L"CImmersiveBehaviorWrapper::GetMaximumComponentCount %p", count);
	return m_behavior->GetMaximumComponentCount(count);
}

HRESULT STDMETHODCALLTYPE CImmersiveBehaviorWrapper::CreateComponent(unsigned int number, IUnknown** component)
{
	//if (number == 1) DebugBreak();
	HRESULT ret = m_behavior->CreateComponent(number, component);
	dbgprintf(L"CImmersiveBehaviorWrapper::CreateComponent %d = %p", number, ret);
	/*IUnknown* wtf = *component;
	HRESULT ret2 = wtf->QueryInterface(IID_ImmersiveShell,(PVOID*)&wtf);
	dbgprintf(L"CImmersiveBehaviorWrapper::GetInterfaceList %p",ret2);*/
	return ret;
}

HRESULT STDMETHODCALLTYPE CImmersiveBehaviorWrapper::ShouldCreateComponent(unsigned int number, int* allowed)
{
	dbgprintf(L"CImmersiveBehaviorWrapper::ShouldCreateComponent %d %p", number, allowed);
	if (number == 9)
	{
		*allowed = 0;
		return S_OK;
	}
	return m_behavior->ShouldCreateComponent(number, allowed);
}