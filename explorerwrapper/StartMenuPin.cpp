#pragma warning(disable:4244) // type conversion used for getting help text

#include "StartMenuPin.h"
#include "dbgprint.h"
#include "OSVersion.h"
#include "Explorer7Base.h"
#include "MinHook.h"

CreateInstance_API CreateStartMenuPinInstance;
PSTARTPINVTBL origStartPinVtbl;
bool bFinished = false;
//PSTARTPINVTBL newStartPinVtbl;

HMODULE h_shell32;

const LPWSTR sz_StartPage2 = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartPage2";
const LPWSTR sz_StartPin = L"startpin";
const LPWSTR sz_StartUnpin = L"startunpin";

int WINAPI Shell32_LoadString(HINSTANCE hInstance, UINT uID, LPWSTR lpBuffer, int nBufferMax)
{
	int result = 0;
	if (hInstance == h_shell32 && (uID == 0x1505 || uID == 0x1506 || uID == 0x1508 || uID == 0x1509))
	{
		//try loading shell32.dll.mui
		WCHAR locales[100] = {};
		ULONG clangs = 0;
		ULONG cblocales = 100;
		GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &clangs, locales, &cblocales);
		// Our MUI sits next to the install, not next to whatever explorer is hosting us
		WCHAR muipath[MAX_PATH];
		GetExplorer7BaseDir(muipath, MAX_PATH);
		PathAddBackslash(muipath);
		PathAppend(muipath, locales);
		PathAddBackslash(muipath);
		PathAppend(muipath, L"shell32.dll.mui");
		HINSTANCE hmui = LoadLibraryEx(muipath, 0, LOAD_LIBRARY_AS_DATAFILE);
		result = LoadStringW(hmui, uID, lpBuffer, nBufferMax);
		if (hmui)
			FreeLibrary(hmui);
		if (result == 0) //fallback - load from us
			result = LoadStringW(g_hInstance, uID, lpBuffer, nBufferMax);
		if (result == 0)
			dbgprintf(L"StartMenuPin: string %i missing from \"%s\" and from us", uID, muipath);
	}
	else
		result = LoadStringW(hInstance, uID, lpBuffer, nBufferMax);
	return result;
}

static LRESULT RegGetDWORD(HKEY key, LPWSTR subkey, LPWSTR value, DWORD* dwVal)
{
	DWORD sz = 4;
	return SHRegGetValueW(key, subkey, value, SRRF_RT_REG_DWORD, NULL, dwVal, &sz);
}

static LRESULT RegSetDWORD(HKEY key, LPWSTR subkey, LPWSTR value, DWORD* dwVal)
{
	return SHSetValueW(key, subkey, value, REG_DWORD, dwVal, 4);
}

void CStartMenuPin::QueryInterface(){};
void CStartMenuPin::AddRef(){};
void CStartMenuPin::Release(){};
void CStartMenuPin::Initialize(){};

// Tells the Start Menu its pin list moved, wParam 8 means reread the store
#define WM_PINLISTCHANGED 0x40B

typedef struct { HWND favorites; HWND mfu; } STARTMENUWNDS;

static bool WindowClassIs(HWND hwnd, LPCWSTR name)
{
	WCHAR cls[64] = {};
	return GetClassName(hwnd, cls, 64) > 0 && lstrcmpi(cls, name) == 0;
}

static BOOL CALLBACK FindStartMenuWnd(HWND hwnd, LPARAM lParam)
{
	STARTMENUWNDS* w = (STARTMENUWNDS*)lParam;
	if (!w->favorites && WindowClassIs(hwnd, L"StartMenuFavorites"))
		w->favorites = hwnd;
	if (!w->mfu && WindowClassIs(hwnd, L"DesktopProgramsMFU"))
		w->mfu = hwnd;
	return TRUE;
}

static BOOL CALLBACK FindStartMenuTopWnd(HWND hwnd, LPARAM lParam)
{
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid != GetCurrentProcessId())
		return TRUE;
	FindStartMenuWnd(hwnd, lParam);
	EnumChildWindows(hwnd, FindStartMenuWnd, lParam);
	return TRUE; //both panes are wanted, so keep walking
}

// shell32's own copy only raises a change event, nothing redraws our strip
void CStartMenuPin::NotifyPinListChange()
{
	STARTMENUWNDS wnds = {};
	EnumWindows(FindStartMenuTopWnd, (LPARAM)&wnds);
	if (wnds.favorites)
		PostMessage(wnds.favorites, WM_PINLISTCHANGED, 8, 0);
	if (wnds.mfu)
		PostMessage(wnds.mfu, WM_PINLISTCHANGED, 8, 0);
	dbgprintf(L"StartMenuPin: pin list changed, favorites %p mfu %p", wnds.favorites, wnds.mfu);
};
void CStartMenuPin::Unimpl1(){};
void CStartMenuPin::UpgradeItem(){};
void CStartMenuPin::IsAcceptableTarget(){};

// Zero means keep the menu item, shell32's own copy returns one and hides it
DWORD CStartMenuPin::ShouldHideMenu(){ return 0; };
void CStartMenuPin::SendPinRearrangeSQM(){};
void CStartMenuPin::GetPinnedAppSQMEventID(){};
void CStartMenuPin::AppliesTo(){};
void CStartMenuPin::v_GetPinListMutexName() {};

LRESULT CStartMenuPin::SetChangeCount(ULONG value)
{
	//dbgprintf(L"SetChangeCount %i",value);
	return RegSetDWORD(HKEY_CURRENT_USER, sz_StartPage2, L"FavoritesChanges", &value);
}

IStream* CStartMenuPin::OpenPinRegStream(ULONG grfMode)
{
	//dbgprintf(L"OpenPinRegStream %i", grfMode);
	return SHOpenRegStream2W(HKEY_CURRENT_USER, sz_StartPage2, L"Favorites", grfMode);
}

IStream* CStartMenuPin::OpenLinksRegStream(ULONG grfMode)
{
	//dbgprintf(L"OpenLinksRegStream %i", grfMode);
	return SHOpenRegStream2W(HKEY_CURRENT_USER, sz_StartPage2, L"FavoritesResolve", grfMode);
}

LRESULT CStartMenuPin::GetPinStreamVersion()
{
	DWORD value = 0;
	RegGetDWORD(HKEY_CURRENT_USER, sz_StartPage2, L"FavoritesVersion", &value);
	//dbgprintf(L"GetPinStreamVersion %i", value);
	return value;
}

LRESULT CStartMenuPin::SetPinStreamVersion(ULONG value)
{
	//dbgprintf(L"SetPinStreamVersion %i", value);
	return RegSetDWORD(HKEY_CURRENT_USER, sz_StartPage2, L"FavoritesVersion", &value);
}

LRESULT CStartMenuPin::GetBackupSubDirName(LPWSTR szOut, UINT cbLen)
{
	//dbgprintf(L"GetBackupSubDirName");
	lstrcpyn(szOut, L"StartMenu", cbLen);
	return S_OK; //...right?
}

DWORD CStartMenuPin::IsRestricted()
{
	//dbgprintf(L"IsRestricted");
	return SHRestricted(REST_NOSMPINNEDLIST);
}

LRESULT(__fastcall* fGetMenuStringID)(void*, UINT*);
LRESULT CStartMenuPin::GetMenuStringID(UINT* w)
{
	// shell32 answers with a pair, the first is pin and the second is unpin
	// Map whichever pair this build uses onto our own two strings
	fGetMenuStringID(this, w);
	if (*w == IDS_SHELL32_PIN_START || *w == IDS_SHELL32_PIN_START + 1)
		*w = IDS_PIN_TO_START_MENU + (*w - IDS_SHELL32_PIN_START);
	else if (*w == IDS_SHELL32_PIN_TASKBAR || *w == IDS_SHELL32_PIN_TASKBAR + 1)
		*w = IDS_PIN_TO_START_MENU + (*w - IDS_SHELL32_PIN_TASKBAR);
	else
		dbgprintf(L"StartMenuPin: unknown menu string id %i, leaving it alone", *w);
	return S_OK;
}

int CStartMenuPin::GetHelpText(unsigned __int64 id, LPWSTR buf, UINT nCharMax)
{
	return Shell32_LoadString(h_shell32, id + 0x1508ul, buf, nCharMax);
}

WCHAR* CStartMenuPin::GetVerb(UINT op)
{
	//dbgprintf(L"getverb %i", op);
	if (op == 0) return sz_StartPin;
	if (op == 1) return sz_StartUnpin;
	return NULL;
}

LRESULT CStartMenuPin::GetChangeCount(ULONG* pdwVal)
{
	//dbgprintf(L"GetChangeCount ");
	*pdwVal = 0;
	return RegGetDWORD(HKEY_CURRENT_USER, sz_StartPage2, L"FavoritesChanges", pdwVal);
}

__int64 CStartMenuPin::GetRemovedChangeCount()
{
	//dbgprintf(L"GetRemovedChangeCount ");
	DWORD value = 0;
	RegGetDWORD(HKEY_CURRENT_USER, sz_StartPage2, L"FavoritesRemovedChanges", &value);
	return value;
}

LRESULT CStartMenuPin::SetRemovedChangeCount(ULONG value)
{
	//dbgprintf(L"SetRemovedChangeCount %i",value);
	return RegSetDWORD(HKEY_CURRENT_USER, sz_StartPage2, L"FavoritesRemovedChanges", &value);
}

static void* DetourVtable(void* vtable, int offset, void* FunctionPtr)
{
	uintptr_t* og = (uintptr_t*)((uintptr_t)vtable + offset);

	DWORD dwProtection;
	VirtualProtect(og, 8, PAGE_EXECUTE_READWRITE, &dwProtection);

	uintptr_t originalfunction = *og;
	*og = (uintptr_t)FunctionPtr;

	VirtualProtect(og, 8, dwProtection, NULL);

	return (void*)originalfunction;
}

template <class T>
static inline void* GetMemberFuncPtr(T Func) { return reinterpret_cast<void*&>(Func); }
#define MEMBER_FUNC(a) GetMemberFuncPtr(&a)

#pragma function(memcpy)
HRESULT WINAPI NewCreateStartMenuPinInstance(PVOID dummy,REFIID riid,PVOID* ppv)
{
	IUnknown* pinobj;
	HRESULT rslt = CreateStartMenuPinInstance(dummy,IID_IShellExtInit,(PVOID*)&pinobj);
	if ( SUCCEEDED(rslt))
	{
		int SetChangeCountIndex = 4 * sizeof(uintptr_t);
		int OpenPinRegStreamIndex = 5 * sizeof(uintptr_t);
		int OpenLinksRegStreamIndex = 6 * sizeof(uintptr_t);
		int GetPinStreamVersionIndex = 8 * sizeof(uintptr_t);
		int SetPinStreamVersionIndex = 9 * sizeof(uintptr_t);
		int GetBackupSubDirNameIndex = 12 * sizeof(uintptr_t);
		int IsRestrictedIndex = 14 * sizeof(uintptr_t);
		int GetMenuStringIDIndex = 16 * sizeof(uintptr_t);
		int GetHelpTextIndex = 17 * sizeof(uintptr_t);
		int GetChangeCountIndex = 18 * sizeof(uintptr_t);
		int GetVerbIndex = 19 * sizeof(uintptr_t);
		int SetRemovedChangeCountIndex = 21 * sizeof(uintptr_t);
		int GetRemovedChangeCountIndex = 22 * sizeof(uintptr_t);

		PSTARTPINOBJ startobj = (PSTARTPINOBJ)pinobj;
		PSTARTPINVTBL ogTable = startobj->pStartPinVtbl;
		static CStartMenuPin* HackHack = new CStartMenuPin();
		startobj->pStartPinVtbl = *(PSTARTPINVTBL*)(HackHack);

		fGetMenuStringID = decltype(fGetMenuStringID)(ogTable->GetMenuStringID);
		if (!bFinished) //only needs to be done once
		{
			DetourVtable(startobj->pStartPinVtbl, 0 * 8, ogTable->QueryInterface);
			DetourVtable(startobj->pStartPinVtbl, 1 * 8, ogTable->AddRef);
			DetourVtable(startobj->pStartPinVtbl, 2 * 8, ogTable->Release);
			DetourVtable(startobj->pStartPinVtbl, 3 * 8, ogTable->Initialize);
			// Slot 7 stays ours, shell32's copy notifies the Win10 start menu
			DetourVtable(startobj->pStartPinVtbl, 10 * 8, ogTable->Unimpl1);
			DetourVtable(startobj->pStartPinVtbl, 11 * 8, ogTable->UpgradeItem);
			DetourVtable(startobj->pStartPinVtbl, 13 * 8, ogTable->IsAcceptableTarget);
			// Slot 15 stays ours, shell32's copy always hides the menu item
			DetourVtable(startobj->pStartPinVtbl, 20 * 8, ogTable->SendPinRearrangeSQM);
			DetourVtable(startobj->pStartPinVtbl, 23 * 8, ogTable->GetPinnedAppSQMEventID);
			if (g_osVersion.BuildNumber() >= 17763)
			{
				DetourVtable(startobj->pStartPinVtbl, 24 * 8, ogTable->AppliesTo);
				DetourVtable(startobj->pStartPinVtbl, 25 * 8, ogTable->v_GetPinListMutexName);
			}
			bFinished = true;
		}
	}
	rslt = pinobj->QueryInterface(riid, ppv);
	pinobj->Release();
	return rslt;
}

BOOL WINAPI IsProcessAnExplorerHook()
{
	return TRUE;
}

// shell32 hides the pin verbs and refuses taskbar pins unless this says yes
// The real one compares the image path with the system explorer, see notes/24h2-support.md
static void HookIsProcessAnExplorer()
{
	// 24H2 shell32 reaches the windows.storage copy through a thunk, so both exports are hooked
	static const LPCWSTR c_mods[] = { L"shell32.dll", L"windows.storage.dll" };
	for (int i = 0; i < ARRAYSIZE(c_mods); i++)
	{
		HMODULE hMod = LoadLibraryW(c_mods[i]);
		FARPROC fn = hMod ? GetProcAddress(hMod, "IsProcessAnExplorer") : NULL;
		if (!fn)
			continue;
		MH_STATUS st = MH_CreateHook((LPVOID)fn, (LPVOID)IsProcessAnExplorerHook, NULL);
		dbgprintf(L"StartMenuPin: IsProcessAnExplorer in %s at %p hooked, status %d", c_mods[i], fn, (int)st);
	}
}

// True when an address lands in a section of the module that holds code
static bool IsInModuleCode(HMODULE hMod, uintptr_t addr)
{
	uintptr_t base = (uintptr_t)hMod;
	PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)(base + ((PIMAGE_DOS_HEADER)base)->e_lfanew);
	PIMAGE_SECTION_HEADER sec = IMAGE_FIRST_SECTION(nt);
	for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
	{
		if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE))
			continue;
		uintptr_t start = base + sec->VirtualAddress;
		if (addr >= start && addr < start + sec->Misc.VirtualSize)
			return true;
	}
	return false;
}

// The class table entry found through the data, a CLSID pointer with a code pointer after it
// 24H2 turned the entry round, the create function still follows the CLSID, see notes/24h2-support.md
static CreateInstance_API* FindPinCreateSlotByClsid(HMODULE hMod)
{
	uintptr_t base = (uintptr_t)hMod;
	PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)(base + ((PIMAGE_DOS_HEADER)base)->e_lfanew);
	uintptr_t end = base + nt->OptionalHeader.SizeOfImage;

	CreateInstance_API* slot = NULL;
	int slots = 0;
	int copies = 0;

	for (uintptr_t g = base; g + sizeof(GUID) <= end; g += 4)
	{
		if (memcmp((void*)g, &CLSID_StartMenuPin, sizeof(GUID)) != 0)
			continue;
		copies++;

		for (uintptr_t q = base; q + 2 * sizeof(uintptr_t) <= end; q += sizeof(uintptr_t))
		{
			if (*(uintptr_t*)q != g)
				continue;
			uintptr_t fn = *(uintptr_t*)(q + sizeof(uintptr_t));
			if (!IsInModuleCode(hMod, fn))
				continue;
			slot = (CreateInstance_API*)(q + sizeof(uintptr_t));
			slots++;
		}
	}

	dbgprintf(L"StartMenuPin: %d CLSID copies, %d table slots found by data", copies, slots);
	return slots == 1 ? slot : NULL;
}

void StartMenuPin_PatchShell32()
{
	h_shell32 = GetModuleHandle(L"shell32.dll");
	ChangeImportedAddress(h_shell32,"api-ms-win-core-libraryloader-l1-2-0.dll",GetProcAddress(GetModuleHandle(L"kernelbase.dll"),"LoadStringW"),Shell32_LoadString);
	ChangeImportedAddress(GetModuleHandle(0), "shell32.dll", GetProcAddress(GetModuleHandle(L"shell32.dll"), "IsProcessAnExplorer"), IsProcessAnExplorerHook);
	HookIsProcessAnExplorer();

	CreateInstance_API* slot = NULL;

	DWORD_PTR addr = FindPattern((uintptr_t)h_shell32, "48 85 C0 0F 85 ?? ?? ?? ?? 45 8B C5 4C 8D 15 ?? ?? ?? ??");
	if (addr)
		addr += 15;
	else
	{
		addr = FindPattern((uintptr_t)h_shell32, "41 8B FD 48 8D 1D ?? ?? ?? ?? 4C 8D 3D");
		if (addr)
			addr += 12;
	}

	if (addr)
	{
		//DWORD_PTR addr = (DWORD_PTR)GetProcAddress(h_shell32,"DllGetClassObject") + 0x85;
		addr = addr + 4 + *(DWORD*)addr;
		PSHELLGUIDS table = (PSHELLGUIDS)addr;

		dbgprintf(L"Got table at %p",table);
		// Bounded, the old walk ran until it found the entry or fell off the table
		for (int i = 0; i < 4096 && &table->rclsid; i++, table++)
		{
			if ( table->rclsid == CLSID_StartMenuPin )
			{
				slot = &table->CreateFunc;
				break;
			}
		}
	}

	// 24H2 has neither code shape, so the entry is found from the CLSID itself
	if (!slot)
		slot = FindPinCreateSlotByClsid(h_shell32);

	if (!slot)
	{
		dbgprintf(L"StartMenuPin_PatchShell32 SIG DID NOT WORK!!!\n");
		dbgprintf(L"StartMenuPin_PatchShell32 SIG DID NOT WORK!!!\n");
		dbgprintf(L"StartMenuPin_PatchShell32 SIG DID NOT WORK!!!\n");
		return;
	}

	DWORD old;
	VirtualProtect(slot,sizeof(*slot),PAGE_EXECUTE_READWRITE,&old);
	CreateStartMenuPinInstance = *slot;
	dbgprintf(L"CreateStartMenuPinInstance = %p",CreateStartMenuPinInstance);
	*slot = NewCreateStartMenuPinInstance;
	VirtualProtect(slot,sizeof(*slot),old,&old);
}

