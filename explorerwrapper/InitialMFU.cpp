#include "InitialMFU.h"
#include "dbgprint.h"
#include "OptionConfig.h"
#include <shlobj.h>
#include <shlwapi.h>

//---Table layout, read from explorer 7850 at 0x100014570---

// Each slot names a known folder and a path below it, unused slots are zero
struct MFUSLOT
{
	const KNOWNFOLDERID* folder;
	LPCWSTR path;
};

struct MFURECORD
{
	DWORD product;
	DWORD touch;
	MFUSLOT slots[16];
};
static_assert(sizeof(MFURECORD) == 264, "CreateInitialMFU steps through its table 264 bytes at a time");
static_assert(offsetof(MFURECORD, slots) == 8, "the slots follow the product and touch fields");

//---7601's Ultimate row------------------------------------

enum MFUFOLDER { MF_COMMON, MF_USER };

struct MFUCHOICE
{
	MFUFOLDER folder;
	LPCWSTR path;
};

struct MFUITEM
{
	const MFUCHOICE* choices;
	UINT count;
};

// Each item lists the Windows 7 path first, then where 24H2 or a restoration puts the same shortcut
static const MFUCHOICE c_gettingStarted[] = {
	{ MF_COMMON, L"Accessories\\Welcome Center.lnk" }, { MF_COMMON, L"Accessories\\Getting Started.lnk" },
	{ MF_USER, L"Getting Started.lnk" }, { MF_COMMON, L"Getting Started.lnk" } };
static const MFUCHOICE c_mediaCenter[] = {
	{ MF_COMMON, L"Media Center.lnk" }, { MF_COMMON, L"Accessories\\Windows Media Center.lnk" },
	{ MF_COMMON, L"Windows Media Center.lnk" } };
static const MFUCHOICE c_calculator[] = { { MF_COMMON, L"Accessories\\Calculator.lnk" }, { MF_COMMON, L"Calculator.lnk" } };
static const MFUCHOICE c_stickyNotes[] = { { MF_COMMON, L"Accessories\\Sticky Notes.lnk" }, { MF_COMMON, L"Sticky Notes.lnk" } };
static const MFUCHOICE c_snippingTool[] = { { MF_COMMON, L"Accessories\\Snipping Tool.lnk" }, { MF_COMMON, L"Snipping Tool.lnk" } };
static const MFUCHOICE c_paint[] = { { MF_COMMON, L"Accessories\\Paint.lnk" }, { MF_COMMON, L"Paint.lnk" } };
static const MFUCHOICE c_remoteDesktop[] = { { MF_COMMON, L"Accessories\\Remote Desktop Connection.lnk" } };
static const MFUCHOICE c_magnifier[] = {
	{ MF_USER, L"Accessories\\Accessibility\\Magnify.lnk" }, { MF_USER, L"Accessibility\\Magnify.lnk" },
	{ MF_COMMON, L"Accessibility\\Magnify.lnk" } };

// Solitaire, the ninth item, is copied from 7850's own row since that slot did not change
static const MFUITEM c_ultimate[] = {
	{ c_gettingStarted, ARRAYSIZE(c_gettingStarted) },
	{ c_mediaCenter, ARRAYSIZE(c_mediaCenter) },
	{ c_calculator, ARRAYSIZE(c_calculator) },
	{ c_stickyNotes, ARRAYSIZE(c_stickyNotes) },
	{ c_snippingTool, ARRAYSIZE(c_snippingTool) },
	{ c_paint, ARRAYSIZE(c_paint) },
	{ c_remoteDesktop, ARRAYSIZE(c_remoteDesktop) },
	{ c_magnifier, ARRAYSIZE(c_magnifier) },
};

//---Ultimate taskbar pins, 7850's table at 0x100018DA0-----

// PinInitialItems moves the pins that resolve to the front in this order, so a missing one hands its place on
static const MFUCHOICE c_pinInternetExplorer[] = { { MF_USER, L"Internet Explorer.lnk" }, { MF_COMMON, L"Internet Explorer.lnk" } };
static const MFUCHOICE c_pinExplorer[] = {
	{ MF_USER, L"Accessories\\Windows Explorer.lnk" }, { MF_COMMON, L"Accessories\\Windows Explorer.lnk" },
	{ MF_USER, L"File Explorer.lnk" } };
static const MFUCHOICE c_pinMediaPlayer[] = {
	{ MF_COMMON, L"Windows Media Player.lnk" }, { MF_COMMON, L"Accessories\\Windows Media Player.lnk" },
	{ MF_COMMON, L"Accessories\\Windows Media Player Legacy.lnk" } };

static const MFUITEM c_ultimatePins[] = {
	{ c_pinInternetExplorer, ARRAYSIZE(c_pinInternetExplorer) },
	{ c_pinExplorer, ARRAYSIZE(c_pinExplorer) },
	{ c_pinMediaPlayer, ARRAYSIZE(c_pinMediaPlayer) },
};

//---Finding the rows in the explorer image-----------------

// Three paths that only one table's Ultimate rows carry
struct ROWMARK
{
	UINT slot;
	LPCWSTR path;
};

// 7850's MFU rows start with Media Center, a 7601 row starts with Welcome Center and never matches
static const ROWMARK c_mfuMarks[] = {
	{ 0, L"Media Center.lnk" }, { 1, L"Accessories\\Calculator.lnk" }, { 6, L"Accessories\\Accessibility\\Magnify.lnk" } };
static const ROWMARK c_pinMarks[] = {
	{ 0, L"Internet Explorer.lnk" }, { 1, L"Accessories\\Windows Explorer.lnk" }, { 2, L"Windows Media Player.lnk" } };

// Measures the room left rather than adding to p, a pointer near the top of memory would wrap past the end check
static bool InSection(PIMAGE_NT_HEADERS nt, BYTE* base, const void* p, SIZE_T len)
{
	const BYTE* q = (const BYTE*)p;
	PIMAGE_SECTION_HEADER sec = IMAGE_FIRST_SECTION(nt);
	for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
	{
		BYTE* start = base + sec->VirtualAddress;
		BYTE* end = start + sec->Misc.VirtualSize;
		if (q >= start && q < end && (SIZE_T)(end - q) >= len)
			return true;
	}
	return false;
}

static bool IsText(PIMAGE_NT_HEADERS nt, BYTE* base, LPCWSTR p, LPCWSTR text)
{
	SIZE_T bytes = (lstrlenW(text) + 1) * sizeof(WCHAR);
	if (!InSection(nt, base, p, bytes))
		return false;
	for (SIZE_T i = 0; text[i]; i++)
		if (p[i] != text[i])
			return false;
	return p[lstrlenW(text)] == 0;
}

static UINT FindUltimateRows(const ROWMARK* marks, UINT markCount, MFURECORD** rows, UINT max)
{
	BYTE* base = (BYTE*)GetModuleHandle(NULL);
	PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)(base + ((PIMAGE_DOS_HEADER)base)->e_lfanew);
	UINT found = 0;

	// 7850 has no .rdata, its tables and strings sit inside .text
	PIMAGE_SECTION_HEADER sec = IMAGE_FIRST_SECTION(nt);
	for (WORD s = 0; s < nt->FileHeader.NumberOfSections && found < max; s++, sec++)
	{
		if (!(sec->Characteristics & IMAGE_SCN_MEM_READ) || (sec->Characteristics & IMAGE_SCN_MEM_DISCARDABLE))
			continue;
		BYTE* start = base + sec->VirtualAddress;
		BYTE* end = start + sec->Misc.VirtualSize;
		for (BYTE* p = start; p + sizeof(MFURECORD) <= end && found < max; p += sizeof(UINT_PTR))
		{
			MFURECORD* row = (MFURECORD*)p;
			if (row->product != PRODUCT_ULTIMATE || row->touch > 1)
				continue;
			bool match = true;
			for (UINT m = 0; m < markCount && match; m++)
			{
				const MFUSLOT* slot = &row->slots[marks[m].slot];
				match = IsText(nt, base, slot->path, marks[m].path) && InSection(nt, base, slot->folder, sizeof(GUID));
			}
			if (match)
				rows[found++] = row;
		}
	}
	return found;
}

//---Rewriting the rows-------------------------------------

static bool ShortcutExists(const KNOWNFOLDERID* folder, LPCWSTR relative)
{
	PWSTR root = NULL;
	if (FAILED(SHGetKnownFolderPath(*folder, 0, NULL, &root)))
		return false;
	WCHAR full[MAX_PATH];
	bool exists = PathCombineW(full, root, relative) && GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES;
	CoTaskMemFree(root);
	return exists;
}

// Each item takes its first shortcut that exists, or its Windows 7 path so a missing one is skipped as before
static void PickSlots(LPCWSTR table, const MFUITEM* items, UINT count, const KNOWNFOLDERID* common, const KNOWNFOLDERID* user, MFUSLOT* fresh)
{
	for (UINT i = 0; i < 16; i++)
	{
		fresh[i].folder = NULL;
		fresh[i].path = NULL;
	}
	for (UINT i = 0; i < count; i++)
	{
		const MFUCHOICE* pick = &items[i].choices[0];
		for (UINT c = 0; c < items[i].count; c++)
		{
			const MFUCHOICE* choice = &items[i].choices[c];
			if (ShortcutExists(choice->folder == MF_COMMON ? common : user, choice->path))
			{
				pick = choice;
				break;
			}
		}
		fresh[i].folder = pick->folder == MF_COMMON ? common : user;
		fresh[i].path = pick->path;
		dbgprintf(L"InitialMFU: %s slot %u %s %s", table, i, pick->folder == MF_COMMON ? L"common" : L"user", pick->path);
	}
}

// The row lives in .text, so its page has to stay runnable while it is written
static bool WriteRow(MFURECORD* row, const MFUSLOT* fresh)
{
	DWORD old;
	if (!VirtualProtect(row, sizeof(*row), PAGE_EXECUTE_READWRITE, &old))
	{
		dbgprintf(L"InitialMFU: row %p could not be made writable, error %u", row, GetLastError());
		return false;
	}
	for (UINT i = 0; i < 16; i++)
		row->slots[i] = fresh[i];
	VirtualProtect(row, sizeof(*row), old, &old);
	return true;
}

static void RewriteUltimateRows()
{
	MFURECORD* rows[4];
	UINT count = FindUltimateRows(c_mfuMarks, ARRAYSIZE(c_mfuMarks), rows, ARRAYSIZE(rows));
	dbgprintf(L"InitialMFU: %u Ultimate MFU rows found in explorer", count);
	if (!count)
		return;

	// Both rows share explorer's own known folder ids, Media Center sits in the common folder and Magnify in the user's
	const KNOWNFOLDERID* common = rows[0]->slots[0].folder;
	const KNOWNFOLDERID* user = rows[0]->slots[6].folder;

	MFUSLOT fresh[16];
	UINT n = ARRAYSIZE(c_ultimate);
	PickSlots(L"mfu", c_ultimate, n, common, user, fresh);

	UINT written = 0;
	for (UINT r = 0; r < count; r++)
	{
		fresh[n] = rows[r]->slots[7];
		if (WriteRow(rows[r], fresh))
			written++;
	}
	dbgprintf(L"InitialMFU: %u rows now hold 7601's %u items", written, n + 1);
}

static void RewriteUltimatePinRows()
{
	MFURECORD* rows[4];
	UINT count = FindUltimateRows(c_pinMarks, ARRAYSIZE(c_pinMarks), rows, ARRAYSIZE(rows));
	dbgprintf(L"InitialMFU: %u Ultimate pin rows found in explorer", count);
	if (!count)
		return;

	// Internet Explorer names the user's Programs folder and Media Player the common one
	const KNOWNFOLDERID* user = rows[0]->slots[0].folder;
	const KNOWNFOLDERID* common = rows[0]->slots[2].folder;

	MFUSLOT fresh[16];
	PickSlots(L"pin", c_ultimatePins, ARRAYSIZE(c_ultimatePins), common, user, fresh);

	UINT written = 0;
	for (UINT r = 0; r < count; r++)
		if (WriteRow(rows[r], fresh))
			written++;
	dbgprintf(L"InitialMFU: %u pin rows rewritten", written);
}

//---Getting Started jump list, ported from 7601------------

// The folder by its Control Panel path, the same identity the OobeFldr mod publishes under
static const WCHAR c_gettingStartedPath[] =
	L"::{26EE0668-A00A-44D7-9371-BEB064C98683}\\0\\::{CB1B7F8C-C50A-4176-B604-9E24DEE8D4D1}";

// The app id 7601's CreateInitialMFU hands SetAppID, the Start menu shortcut carries the same one
static const WCHAR c_gettingStartedAppId[] = L"Microsoft.Windows.GettingStarted";

// Columns read from the stack writes in 7601's GettingStarted_AddLinksToList at 100088DB4
static const SHCOLUMNID c_colTaskTitle = { { 0xD376374E, 0xA9D9, 0x4BA0, { 0x9E, 0x61, 0xEF, 0xA9, 0x93, 0x7B, 0x1D, 0xED } }, 2 };
static const SHCOLUMNID c_colTaskTip = { { 0xE1229318, 0xC934, 0x45E8, { 0xA0, 0x9A, 0x7B, 0xF1, 0xFB, 0xB5, 0x2A, 0x57 } }, 2 };

static HRESULT TaskColumn(IShellFolder2* folder, PCUITEMID_CHILD child, const SHCOLUMNID* key, LPWSTR out, UINT cch)
{
	VARIANT v;
	VariantInit(&v);
	HRESULT hr = folder->GetDetailsEx(child, key, &v);
	if (SUCCEEDED(hr))
		hr = (v.vt == VT_BSTR && v.bstrVal && v.bstrVal[0]) ? StringCchCopyW(out, cch, v.bstrVal) : E_FAIL;
	VariantClear(&v);
	return hr;
}

// 7601's title, tooltip and icon, opened by id list like the OobeFldr mod's links so GettingStarted.exe is not needed
static HRESULT MakeTaskLink(IShellFolder2* folder, PCIDLIST_ABSOLUTE root, PCUITEMID_CHILD child, IShellLinkW** out)
{
	*out = NULL;
	WCHAR title[512];
	WCHAR tip[1024];
	HRESULT hr = TaskColumn(folder, child, &c_colTaskTitle, title, ARRAYSIZE(title));
	if (FAILED(hr))
		return hr;
	if (FAILED(TaskColumn(folder, child, &c_colTaskTip, tip, ARRAYSIZE(tip))))
		StringCchCopyW(tip, ARRAYSIZE(tip), title);

	PIDLIST_ABSOLUTE full = ILCombine(root, child);
	if (!full)
		return E_OUTOFMEMORY;

	IShellLinkW* link = NULL;
	hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
	if (SUCCEEDED(hr))
		hr = link->SetIDList(full);
	if (SUCCEEDED(hr))
		hr = link->SetDescription(tip);
	if (SUCCEEDED(hr))
	{
		// The jump list draws the Title property, the description is only the tooltip
		IPropertyStore* store = NULL;
		hr = link->QueryInterface(IID_PPV_ARGS(&store));
		if (SUCCEEDED(hr))
		{
			PROPVARIANT pv = {};
			pv.vt = VT_LPWSTR;
			pv.pwszVal = title;
			hr = store->SetValue(PKEY_Title, pv);
			if (SUCCEEDED(hr))
				hr = store->Commit();
			store->Release();
		}
	}
	if (SUCCEEDED(hr))
	{
		// The icon is cosmetic, a task without one still opens
		IExtractIconW* extract = NULL;
		if (SUCCEEDED(folder->GetUIObjectOf(NULL, 1, &child, IID_IExtractIconW, NULL, (void**)&extract)))
		{
			WCHAR icon[MAX_PATH] = L"";
			int index = 0;
			UINT flags = 0;
			if (SUCCEEDED(extract->GetIconLocation(0, icon, ARRAYSIZE(icon), &index, &flags)) && icon[0])
				link->SetIconLocation(icon, index);
			extract->Release();
		}
	}
	ILFree(full);

	if (FAILED(hr))
	{
		if (link)
			link->Release();
		return hr;
	}
	*out = link;
	return S_OK;
}

// 7601 builds this at the top of CreateInitialMFU, 7850 has none of the code
static void BuildGettingStartedJumpList()
{
	PIDLIST_ABSOLUTE root = NULL;
	IShellFolder2* folder = NULL;
	HRESULT hr = SHParseDisplayName(c_gettingStartedPath, NULL, &root, 0, NULL);
	if (SUCCEEDED(hr))
		hr = SHBindToObject(NULL, root, NULL, IID_PPV_ARGS(&folder));
	if (FAILED(hr))
	{
		dbgprintf(L"InitialMFU: no Getting Started folder, hr %08X", hr);
		ILFree(root);
		return;
	}

	ICustomDestinationList* list = NULL;
	IObjectCollection* tasks = NULL;
	IObjectArray* removed = NULL;
	IEnumIDList* items = NULL;
	UINT slots = 0;
	UINT made = 0;
	bool begun = false;

	hr = CoCreateInstance(CLSID_DestinationList, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&list));
	if (SUCCEEDED(hr))
		hr = list->SetAppID(c_gettingStartedAppId);
	if (SUCCEEDED(hr))
	{
		hr = list->BeginList(&slots, IID_PPV_ARGS(&removed));
		begun = SUCCEEDED(hr);
	}
	if (SUCCEEDED(hr))
		hr = CoCreateInstance(CLSID_EnumerableObjectCollection, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&tasks));

	// SHCONTF_NONFOLDERS, the flag 7601 enumerates with, and no more tasks than the list has slots
	if (SUCCEEDED(hr))
		hr = folder->EnumObjects(NULL, SHCONTF_NONFOLDERS, &items);
	if (hr == S_OK && items)
	{
		PITEMID_CHILD child;
		ULONG got;
		while (made < slots && items->Next(1, &child, &got) == S_OK && got == 1)
		{
			IShellLinkW* link;
			if (SUCCEEDED(MakeTaskLink(folder, root, child, &link)))
			{
				if (SUCCEEDED(tasks->AddObject(link)))
					made++;
				link->Release();
			}
			ILFree(child);
		}
	}

	if (made)
	{
		IObjectArray* array = NULL;
		hr = tasks->QueryInterface(IID_PPV_ARGS(&array));
		if (SUCCEEDED(hr))
		{
			hr = list->AddUserTasks(array);
			array->Release();
		}
		if (SUCCEEDED(hr))
			hr = list->CommitList();
	}
	else if (SUCCEEDED(hr))
		hr = E_FAIL;
	if (FAILED(hr) && begun)
		list->AbortList();
	dbgprintf(L"InitialMFU: Getting Started jump list, %u tasks of %u slots, hr %08X", made, slots, hr);

	if (items)
		items->Release();
	if (tasks)
		tasks->Release();
	if (removed)
		removed->Release();
	if (list)
		list->Release();
	folder->Release();
	ILFree(root);
}

// The wrapper links no C runtime and the SDK's ntdll.lib lacks this export, so __try reaches ntdll's handler here
typedef EXCEPTION_DISPOSITION(__cdecl* CSpecificHandler_t)(PEXCEPTION_RECORD, void*, PCONTEXT, void*);
static CSpecificHandler_t s_ntdllSpecificHandler = NULL;

extern "C" EXCEPTION_DISPOSITION __cdecl InitialMFU_CSpecificHandler(PEXCEPTION_RECORD record, void* frame, PCONTEXT context, void* dispatch)
{
	if (!s_ntdllSpecificHandler)
		return ExceptionContinueSearch;
	return s_ntdllSpecificHandler(record, frame, context, dispatch);
}
#pragma comment(linker, "/alternatename:__C_specific_handler=InitialMFU_CSpecificHandler")

// A crash in the first logon block repeats on every logon, so a fault inside OobeFldr costs only the jump list
static void BuildGettingStartedJumpListGuarded()
{
	__try
	{
		BuildGettingStartedJumpList();
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		dbgprintf(L"InitialMFU: Getting Started jump list faulted, code %08X", GetExceptionCode());
	}
}

//---GetProductInfo override--------------------------------

typedef BOOL(WINAPI* GetProductInfo_t)(DWORD, DWORD, DWORD, DWORD, PDWORD);
static GetProductInfo_t s_GetProductInfo = NULL;
static LONG s_firstLogonDone = 0;

// Only explorer's first logon block calls this, its tables know Windows 7 editions up to 0x47
static BOOL WINAPI GetProductInfoNEW(DWORD major, DWORD minor, DWORD spMajor, DWORD spMinor, PDWORD type)
{
	DWORD real = 0;
	BOOL ok = s_GetProductInfo(major, minor, spMajor, spMinor, &real);

	// 7850 dropped Getting Started from the row and its jump list, 7601 still has both and needs nothing
	if (s_BuildRuntime == BUILDRUNTIME_7850 && InterlockedExchange(&s_firstLogonDone, 1) == 0)
	{
		BuildGettingStartedJumpListGuarded();
		RewriteUltimateRows();

		// CreateInitialMFU makes the first call, so the pin rows are rewritten before PinInitialItems reads them
		RewriteUltimatePinRows();
	}

	if (!type)
		return ok;
	*type = PRODUCT_ULTIMATE;
	dbgprintf(L"InitialMFU: GetProductInfo answered Ultimate, the real edition is 0x%X, ok %d", real, ok);
	return TRUE;
}

void InstallInitialMFU()
{
	// DisplaySwitch loads this DLL too, only the shell seeds a first logon
	WCHAR host[MAX_PATH] = L"";
	GetModuleFileNameW(NULL, host, MAX_PATH);
	if (lstrcmpiW(PathFindFileNameW(host), L"explorer.exe") != 0)
		return;
	s_ntdllSpecificHandler = (CSpecificHandler_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "__C_specific_handler");

	s_GetProductInfo = (GetProductInfo_t)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetProductInfo");
	if (!s_GetProductInfo)
		return;
	BOOL hooked = ChangeImportedAddress(GetModuleHandle(NULL), "kernel32.dll", s_GetProductInfo, GetProductInfoNEW);
	dbgprintf(L"InitialMFU: GetProductInfo import %s", hooked ? L"hooked" : L"not found");
}
