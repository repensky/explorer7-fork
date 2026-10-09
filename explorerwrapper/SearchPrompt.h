#pragma once
#include "common.h"
#include "dbgprint.h"
#include "OSVersion.h"
#include "MinHook.h"

//---Start menu search prompt-------------------------------
// The 24H2 rich edit asks its host for the selection bar width once more after the box takes focus
// ExplorerFrame treats that call as typing and hides "Search programs and files" as the menu opens

// CSearchEditTextHost::TxGetSelectionBarWidth past its setup, read from the 21332 and 19041 frames
static const char c_szSelectionBarWidthBody[] = "83 22 00 48 8B 71 20 83 BE 48 02 00 00 00 74";

// How far those bytes sit from the start, a hook over the start leaves them untouched
#define SELECTION_BAR_WIDTH_BODY_OFFSET 0xF

// Every way through the frame's own version writes a zero width and succeeds, only the hide is left out
static HRESULT SelectionBarWidth_Hook(void* host, LONG* width)
{
	*width = 0;
	return S_OK;
}

// Typing still hides the prompt, that goes through the text change notice and not through here
static void HookSearchPrompt()
{
	// The 19041 rich edit never makes that late call, so its prompt was never at risk
	if (g_osVersion.BuildNumber() < 26100)
		return;

	HMODULE frame = LoadLibrary(L"ExplorerFrame.dll");
	char* body = frame ? (char*)FindPattern((uintptr_t)frame, c_szSelectionBarWidthBody) : nullptr;
	void* target = body ? body - SELECTION_BAR_WIDTH_BODY_OFFSET : nullptr;
	MH_STATUS status = target ? MH_CreateHook(target, (LPVOID)SelectionBarWidth_Hook, NULL) : MH_ERROR_FUNCTION_NOT_FOUND;
	dbgprintf(L"explorer7: start menu search prompt kept, selection bar width %p status %d", target, status);
}
