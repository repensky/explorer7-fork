#pragma once
#include "common.h"

////
// 
// TODO: Cleanup of NscTree.h for Milestone 3
// This will include hopefully better names for variables as well as checking the build of %windir%\System32\ExplorerFrame.dll rather than the OS version
// 
////

DEFINE_GUID(CLSID_PersonalStartMenu, 0x3F6953F0, 0x5359, 0x47FC, 0x0BD, 0x99, 0x9F, 0x2C, 0x0B9, 0x5A, 0x62, 0x0FD);

MIDL_INTERFACE("00000000-0000-0000-0000-000000000000")
INameSpaceTreeControlValuesPrivate : IUnknown
{
public:
	virtual void stub() = 0;
	virtual void SetIndentValue(int indent) = 0;
};

UINT (__fastcall*fGetDpiForWindow)(HWND hwnd);
DPI_AWARENESS_CONTEXT (__fastcall*fGetWindowDpiAwarenessContext)(HWND hwnd);
BOOL (__fastcall*fAreDpiAwarenessContextsEqual)(DPI_AWARENESS_CONTEXT A, DPI_AWARENESS_CONTEXT B);

static void __fastcall SHComputeDPI(HWND a1, int* a2, int* a3)
{
	DPI_AWARENESS_CONTEXT v7; // rax
	HDC DC; // rax
	HDC v9; // rdi
	int DeviceCaps; // ebx
	int v11; // esi

	if (a1 && (v7 = fGetWindowDpiAwarenessContext(a1), fAreDpiAwarenessContextsEqual(v7, (DPI_AWARENESS_CONTEXT)-4LL)))
	{
		DeviceCaps = fGetDpiForWindow(a1);
		v11 = DeviceCaps;
	}
	else
	{
		DC = GetDC(0LL);
		v9 = DC;
		if (DC)
		{
			DeviceCaps = GetDeviceCaps(DC, 88);
			v11 = GetDeviceCaps(v9, 90);
			ReleaseDC(0LL, v9);
		}
		else
		{
			DeviceCaps = 96;
			v11 = 96;
		}
	}
	if (a2)
		*a2 = DeviceCaps;
	if (a3)
		*a3 = v11;
}

// CNscTree field offsets, read out of ExplorerFrame with symbols, see notes/24h2-support.md
// 24H2 put one more pointer ahead of the tree window, so everything past it moved by eight
struct NscTreeLayout
{
	int indentWrite;   // SetIndentValue stores here, from the values private interface
	int heightWrite;   // SetItemHeight stores here, from the visual properties interface
	int treeWnd;       // the treeview window, from the object base
	int dpiWnd;        // the window the DPI is measured on, from the object base
	int indentRead;    // what ScaleAndSetIndent scales, from the object base
	int heightRead;    // what ScaleAndSetRowHeight scales, from the object base
};

// 19041 through 23H2 use the first set, 24H2 the second
static const NscTreeLayout c_nscLayout19041 = { 0xA0, 0xC8, 0x178, 0x188, 0x1D0, 0x1C8 };
static const NscTreeLayout c_nscLayout26100 = { 0xA8, 0xD0, 0x180, 0x190, 0x1D8, 0x1D0 };
static const NscTreeLayout* g_nscLayout = &c_nscLayout19041;

// Pulls the store offset out of ExplorerFrame's own method, mov [rcx+disp32],edx
// Zero when the code does not open that way, a hook from another mod lands here too
static int ReadNscStoreOffset(void* iface, int slot, int skip)
{
	BYTE* fn = (BYTE*)(*(void***)iface)[slot];
	if (!fn || IsBadReadPtr(fn, skip + 6))
		return 0;
	fn += skip;
	if (fn[0] == 0x89 && fn[1] == 0x91)
		return *(int*)(fn + 2);
	return 0;
}

// Picks the layout from what ExplorerFrame actually does, the build only settles a tie
static const NscTreeLayout* PickNscLayout(void* visualProps, void* privatec)
{
	// Slot 4 is SetIndentValue, slot 6 is SetItemHeight, which starts with a 4 byte sub rsp
	// Both slots were read off the ExplorerFrame vtables on 19044 and 26100
	int indentStore = ReadNscStoreOffset(privatec, 4, 0);
	int heightStore = ReadNscStoreOffset(visualProps, 6, 4);
	ULONG build = g_osVersion.BuildNumber();

	const NscTreeLayout* pick = NULL;
	if (indentStore == c_nscLayout19041.indentWrite && heightStore == c_nscLayout19041.heightWrite)
		pick = &c_nscLayout19041;
	else if (indentStore == c_nscLayout26100.indentWrite && heightStore == c_nscLayout26100.heightWrite)
		pick = &c_nscLayout26100;
	else if (!indentStore && !heightStore)
		pick = build >= 26100 ? &c_nscLayout26100 : &c_nscLayout19041;

	dbgprintf(L"explorer7: CNscTree stores at %X and %X on build %u, layout %s",
		indentStore, heightStore, build,
		pick == &c_nscLayout26100 ? L"26100" : pick == &c_nscLayout19041 ? L"19041" : L"unknown");
	return pick;
}

//custom versions of the functions because ppl use patched dlls and aerexplorer messes this up
static void __fastcall CNscTree_ScaleAndSetIndent(__int64 a1)
{
	int v1; // ebx
	int v3; // eax
	int nNumerator; // [rsp+30h] [rbp+8h] BYREF
	int v6; // [rsp+38h] [rbp+10h] BYREF

	v1 = *(DWORD*)(a1 + g_nscLayout->indentRead);
	SHComputeDPI(*(HWND*)(a1 + g_nscLayout->dpiWnd), &v6, &nNumerator);
	v3 = MulDiv(v1, nNumerator, 96);
	SendMessageW(*(HWND*)(a1 + g_nscLayout->treeWnd), 0x1107u, v3, 0LL);
}

static void __fastcall CNscTree_SetIndentValue(__int64 a1, int a2)
{

	*(DWORD*)(a1 + g_nscLayout->indentWrite) = a2;
	CNscTree_ScaleAndSetIndent(a1 - 304);
}

static void __fastcall CNscTree_ScaleAndSetRowHeight(__int64 a1)
{
	int v1; // ebp
	HWND v2; // rdi
	DPI_AWARENESS_CONTEXT v5; // rax
	HDC DC; // rax
	HDC v7; // rbx
	int DeviceCaps; // edi
	int v9; // eax

	v1 = *(DWORD*)(a1 + g_nscLayout->heightRead);
	v2 = *(HWND*)(a1 + g_nscLayout->dpiWnd);
	if (v2 && (v5 = fGetWindowDpiAwarenessContext(v2), fAreDpiAwarenessContextsEqual(v5, (DPI_AWARENESS_CONTEXT)-4LL)))
	{
		DeviceCaps = fGetDpiForWindow(v2);
	}
	else
	{
		DC = GetDC(0LL);
		v7 = DC;
		if (DC)
		{
			GetDeviceCaps(DC, 88);
			DeviceCaps = GetDeviceCaps(v7, 90);
			ReleaseDC(0LL, v7);
		}
		else
		{
			DeviceCaps = 96;
		}
	}
	v9 = MulDiv(v1, DeviceCaps, 96);
	SendMessageW(*(HWND*)(a1 + g_nscLayout->treeWnd), 0x111Bu, v9, 0LL);
}

static __int64 __fastcall CNscTree_SetItemHeight(__int64 a1, int a2)
{

	*(DWORD*)(a1 + g_nscLayout->heightWrite) = a2;
	CNscTree_ScaleAndSetRowHeight(a1 - 256);
	return 0LL;
}

extern HRESULT(__fastcall* CNSCHost_FillNSCOg)(uintptr_t nscHost);
static HRESULT __fastcall CNSCHost_FillNSC(uintptr_t nscHost) //todo: reimplement the filter from 7 shell32, CLSID_PersonalStartMenu GUID_2659b475_eeb8_48b7_8f07_b378810f48cf
{
	const int indentValue = 0;
	const int itemHeight = 19;

	bool isFilled = *(DWORD*)(nscHost + 0xCC);
	HRESULT result = CNSCHost_FillNSCOg(nscHost);

	if (!isFilled)
	{
		HMODULE modUser32 = GetModuleHandleW(L"User32.dll");

		fGetDpiForWindow = (decltype(fGetDpiForWindow))GetProcAddress(modUser32, "GetDpiForWindow");
		fGetWindowDpiAwarenessContext = (decltype(fGetWindowDpiAwarenessContext))GetProcAddress(modUser32, "GetWindowDpiAwarenessContext");
		fAreDpiAwarenessContextsEqual = (decltype(fAreDpiAwarenessContextsEqual))GetProcAddress(modUser32, "AreDpiAwarenessContextsEqual");

		INameSpaceTreeControl2* control = *(INameSpaceTreeControl2**)(nscHost + 0x70);

		//ideally we would want to queryinterface to get these, but im too lazy to get the guids for these, and i doubt these offsets would change
		IVisualProperties* visualProps = (IVisualProperties*)(__int64(control) + 0x20);
		INameSpaceTreeControlValuesPrivate* privatec = (INameSpaceTreeControlValuesPrivate*)(__int64(control) + 0x50);

		// An unknown layout goes through ExplorerFrame's own methods rather than guessed offsets
		const NscTreeLayout* layout = g_osVersion.BuildNumber() < 14393 ? NULL : PickNscLayout(visualProps, privatec);

		if (!layout) // handle TH1 and TH2 - less explorerframe modding exists, so should be fine
		{
			privatec->SetIndentValue(indentValue);
			visualProps->SetItemHeight(itemHeight);
		}
		else
		{
			g_nscLayout = layout;
			CNscTree_SetIndentValue((uintptr_t)privatec, indentValue);
			CNscTree_SetItemHeight((uintptr_t)visualProps, itemHeight);
		}
	}
	
	return result;
}