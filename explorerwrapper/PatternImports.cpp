#include "PatternImports.h"

// Remove AMAP class from loaded msstyle so that Vista and 7 msstyles are compatible
void RemoveLoadAnimationDataMap()
{
	// thank you amrsatrio for the pattern + offsetting method
	char* LoadAnimationDataMap = "48 8B 53 20 48 8B ?? E8 ?? ?? ?? ?? 8B ?? 48 8B";

	HMODULE uxTheme = GetModuleHandle(L"uxtheme.dll");
	if (uxTheme)
	{
		char* LADMPattern = (char*)FindPattern((uintptr_t)uxTheme, LoadAnimationDataMap);

		if (LADMPattern)
		{
			LADMPattern += 7;
			LADMPattern += 5 + *(int*)(LADMPattern + 1);

			unsigned char bytes[] = { 0x31, 0xC0, 0xC3 };
			ChangeImportedPattern(LADMPattern, bytes, sizeof(bytes));
		}
	}
}


// Stops uxtheme asking for the immersive class, Vista and 7 msstyles have no such class
void RemoveGetClassIdForShellTarget()
{
	char* GetClassIdForShellTarget = "4C 8B DC 4D 89 43 18 49 89 4B 08 53 48 83 EC 30";

	HMODULE uxTheme = GetModuleHandle(L"uxtheme.dll");
	if (uxTheme)
	{
		char* GCIFSTPattern = (char*)FindPattern((uintptr_t)uxTheme, GetClassIdForShellTarget);

		if (GCIFSTPattern)
		{
			unsigned char bytes[] = { 0x31, 0xC0, 0xC3 };
			ChangeImportedPattern(GCIFSTPattern, bytes, sizeof(bytes));
			dbgprintf(L"explorer7: neutered GetClassIdForShellTarget at %p", GCIFSTPattern);
		}
		else
		{
			dbgprintf(L"explorer7: GetClassIdForShellTarget pattern not found, shell targets stay immersive");
		}
	}
}

// Ittr: Hide the immersive start menu on TH1+ when immersive shell is on
void DisableImmersiveStart()
{
	if (s_EnableImmersiveShellStack == 1) // don't run this if the user isn't using immersive shell
	{
		char* ShowStartView; // XamlLauncher::ShowStartView
		char* SSVPattern;
		// S_OK with nothing shown, a bare ret handed the caller whatever eax held
		unsigned char bytes[] = { 0x33, 0xC0, 0xC3 };

		// Load twinui.pcshell.dll where it is used
		// Otherwise we load twinui.dll further below
		HMODULE twinui_pcshell = LoadLibrary(L"twinui.pcshell.dll");

		if (twinui_pcshell) // only run if DLL is present
		{
			// 24H2, the frame lea grew a disp32 and the cookie slot is wildcarded, see notes/24h2-support.md
			ShowStartView = "40 55 53 56 57 41 56 48 8D AC 24 ?? FF FF FF 48 81 EC ?? 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 ?? 00 00 00 45 8B F0 8B DA 48 8B F1 48 83 B9 ?? ?? 00 00 00";
			SSVPattern = (char*)FindPattern((uintptr_t)twinui_pcshell, ShowStartView);
			if (!SSVPattern)
			{
				ShowStartView = "40 55 53 56 57 41 56 48 8D 6C 24 80 48 81 EC 80 01 00 00 48 8B 05 06 39 8E 00 48 33 C4";
				SSVPattern = (char*)FindPattern((uintptr_t)twinui_pcshell, ShowStartView);
			}

			if (SSVPattern) // first run, 24H2 then late NI
			{
				ChangeImportedPattern(SSVPattern, bytes, sizeof(bytes));
			}
			else
			{
				ShowStartView = "48 89 5C 24 20 55 56 57 48 81 EC ?? 01 00 00 48 8B 05 ?? ?? ?? 00 48 33 C4 48 89 84 24 ?? 01 00 00 48 83 B9 ?? ?? 00 00 00";
				SSVPattern = (char*)FindPattern((uintptr_t)twinui_pcshell, ShowStartView);

				if (SSVPattern) // second run, RS3 to early NI
				{
					ChangeImportedPattern(SSVPattern, bytes, sizeof(bytes));
				}
				else
				{
					ShowStartView = "48 89 5C 24 20 55 56 57 48 83 EC 30 48 83 B9 F8 00 00 00 00 41 8B E8";
					SSVPattern = (char*)FindPattern((uintptr_t)twinui_pcshell, ShowStartView);

					if (SSVPattern) // third run, RS2
					{
						ChangeImportedPattern(SSVPattern, bytes, sizeof(bytes));
					}
					else
					{
						// RS1 where twinui.pcshell.dll exists, but isn't used for this so we have to go to twinui version
						// this is an attempt to avoid additional build checks where they aren't needed
						goto DisableImmersiveStart_TWINUI;
					}
				}
			}
		}
		else // fourth run, TH1 to RS1, before twinui.pcshell was used here
		{
DisableImmersiveStart_TWINUI:
			HMODULE twinui = LoadLibrary(L"twinui.dll");

			if (twinui)
			{
				ShowStartView = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 83 B9 ?? 00 00 00 00 41 8B E8";
				SSVPattern = (char*)FindPattern((uintptr_t)twinui, ShowStartView);

				if (SSVPattern)
				{
					ChangeImportedPattern(SSVPattern, bytes, sizeof(bytes));
				}
			}
		}
	}

}

// Ittr: Hide the immersive search interface on TH1+ with ImmersiveShell enabled
// When invoked it can take up half of the screen
void DisableImmersiveSearch()
{
	if (s_EnableImmersiveShellStack == 1)
	{
		char* CortanaDesktopExperienceView_ShowInternal;
		char* CDEVSIPattern;
		char* XamlLauncherState_ShowSearchFromOpenStart;
		char* XLSSSFOSPattern = NULL;
		// Zero in all of eax, every one of these returns a 32 bit HRESULT
		unsigned char bytes[] = { 0x33, 0xC0, 0xC3 };
		ULONG build = g_osVersion.BuildNumber();

		HMODULE twinui_pcshell = LoadLibrary(L"twinui.pcshell.dll");

		// Disable CortanaDesktopExperienceView and ShowSearchFromOpenStart
		if (twinui_pcshell)
		{
			// Disable ShowCortanaFromOpenStart from 19H1 onwards (later renamed ShowSearchFromOpenStart)
			// Each shape is tied to its builds, the loose 22H2 one lands on a snap assist method on 19044
			if (build >= 26100)
			{
				// 24H2, XamlLauncherState::ShowSearchFromOpenStart with its first three moves
				XamlLauncherState_ShowSearchFromOpenStart = "48 89 54 24 10 55 53 56 57 41 54 41 56 41 57 48 8B EC 48 83 EC 40 45 8B E0 48 8B F2 48 8B F9 4C 8D 79 40";
				XLSSSFOSPattern = (char*)FindPattern((uintptr_t)twinui_pcshell, XamlLauncherState_ShowSearchFromOpenStart);
			}
			else if (build >= 22000)
			{
				// 22H2 to 23H2, not checked against those binaries here
				XamlLauncherState_ShowSearchFromOpenStart = "48 89 54 24 10 55 53 56 57 41 54 41 56 41 57 48 8B EC 48 83 EC";
				XLSSSFOSPattern = (char*)FindPattern((uintptr_t)twinui_pcshell, XamlLauncherState_ShowSearchFromOpenStart);
			}

			if (XLSSSFOSPattern) // 22H2 and later
			{
				ChangeImportedPattern(XLSSSFOSPattern, bytes, sizeof(bytes));
			}
			else if (build < 22000)
			{
				XamlLauncherState_ShowSearchFromOpenStart = "48 89 54 24 10 55 53 56 57 41 56 41 57 48 8B EC 48 83 EC";
				XLSSSFOSPattern = (char*)FindPattern((uintptr_t)twinui_pcshell, XamlLauncherState_ShowSearchFromOpenStart);

				if (XLSSSFOSPattern) // VB to 21H2
				{
					ChangeImportedPattern(XLSSSFOSPattern, bytes, sizeof(bytes));
				}
				else
				{
					XamlLauncherState_ShowSearchFromOpenStart = "48 89 54 24 10 55 53 56 57 41 56 48 8B EC 48 83 EC 40 48 C7 45 E0 FE FF FF FF";
					XLSSSFOSPattern = (char*)FindPattern((uintptr_t)twinui_pcshell, XamlLauncherState_ShowSearchFromOpenStart);

					if (XLSSSFOSPattern) // 19H1 to 19H2
					{
						ChangeImportedPattern(XLSSSFOSPattern, bytes, sizeof(bytes));
					}
				}
			}

			// Now proceed to disabling CortanaDesktopExperienceView (used in TH1 to RS5, still present but unused)
			if (build >= 26100)
			{
				// 24H2 carries two ShowInternal bodies of this shape, Cortana and the search app
				// Both are hidden, the loose VB shape below would only ever reach the first
				CortanaDesktopExperienceView_ShowInternal = "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 41 8B F1 41 8B E8 48 8B FA 48 8B D9 E8 ?? ?? ?? ?? 48 8D 4C 24 30 89 44 24 30";
				uintptr_t hit = FindPattern((uintptr_t)twinui_pcshell, CortanaDesktopExperienceView_ShowInternal);
				int hidden = 0;
				while (hit && hidden < 4)
				{
					ChangeImportedPattern((void*)hit, bytes, sizeof(bytes));
					hidden++;
					hit = FindPatternAfter((uintptr_t)twinui_pcshell, CortanaDesktopExperienceView_ShowInternal, hit);
				}
				dbgprintf(L"explorer7: %d desktop experience views hidden", hidden);
				return;
			}

			CortanaDesktopExperienceView_ShowInternal = "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 20 57 48 83 EC 20 41 8B ?? 41 8B ?? 48 8B FA";
			CDEVSIPattern = (char*)FindPattern((uintptr_t)twinui_pcshell, CortanaDesktopExperienceView_ShowInternal);

			if (CDEVSIPattern) // VB and later
			{
				ChangeImportedPattern(CDEVSIPattern, bytes, sizeof(bytes));
			}
			else
			{
				CortanaDesktopExperienceView_ShowInternal = "40 55 53 56 57 41 54 41 56 41 57 48 8B EC 48 81 EC 80 00 00 00";
				CDEVSIPattern = (char*)FindPattern((uintptr_t)twinui_pcshell, CortanaDesktopExperienceView_ShowInternal);

				if (CDEVSIPattern) // 19H1 to 19H2
				{
					ChangeImportedPattern(CDEVSIPattern, bytes, sizeof(bytes));
				}
				else
				{
					CortanaDesktopExperienceView_ShowInternal = "40 55 53 56 57 41 56 48 8D 6C 24 C9 48 81 EC 90 00 00 00";
					CDEVSIPattern = (char*)FindPattern((uintptr_t)twinui_pcshell, CortanaDesktopExperienceView_ShowInternal);

					if (CDEVSIPattern) // RS4 to RS5
					{
						ChangeImportedPattern(CDEVSIPattern, bytes, sizeof(bytes));
					}
					else
					{
						CortanaDesktopExperienceView_ShowInternal = "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 41 8B D9 41 8B E8 48 8B F2";
						CDEVSIPattern = (char*)FindPattern((uintptr_t)twinui_pcshell, CortanaDesktopExperienceView_ShowInternal);

						if (CDEVSIPattern) // RS3
						{
							ChangeImportedPattern(CDEVSIPattern, bytes, sizeof(bytes));
						}
						else
						{
							// RS1 where twinui.pcshell.dll exists, but isn't used for this so we have to go to twinui version
							// this is an attempt to avoid additional build checks where they aren't needed
							goto DisableImmersiveSearch_TWINUI;
						}
					}
				}
			}
		}
		else // TH1 to RS1
		{
DisableImmersiveSearch_TWINUI:
			HMODULE twinui = LoadLibrary(L"twinui.dll");

			if (twinui)
			{
				CortanaDesktopExperienceView_ShowInternal = "40 55 53 56 57 41 56 48 8B EC 48 83 EC 70";
				CDEVSIPattern = (char*)FindPattern((uintptr_t)twinui, CortanaDesktopExperienceView_ShowInternal);

				if (CDEVSIPattern) // RS1
				{
					ChangeImportedPattern(CDEVSIPattern, bytes, sizeof(bytes));
				}
				else
				{
					CortanaDesktopExperienceView_ShowInternal = "48 8B C4 55 56 57 41 54 41 55 41 56 41 57 ?? ?? ?? ?? 48 81 EC 90 00 00 00 48 C7 45 DF FE FF FF FF 48 89 58 20";
					CDEVSIPattern = (char*)FindPattern((uintptr_t)twinui, CortanaDesktopExperienceView_ShowInternal);

					if (CDEVSIPattern) // TH2
					{
						ChangeImportedPattern(CDEVSIPattern, bytes, sizeof(bytes));
					}
					else // no way of making a cross-compatible pattern despite a one-byte difference, thanks Microsoft
					{
						CortanaDesktopExperienceView_ShowInternal = "48 8B C4 55 56 57 41 54 41 55 41 56 41 57 ?? ?? ?? ?? 48 81 EC 90 00 00 00 48 C7 45 E7 FE FF FF FF";
						CDEVSIPattern = (char*)FindPattern((uintptr_t)twinui, CortanaDesktopExperienceView_ShowInternal);

						if (CDEVSIPattern) // TH1
						{
							unsigned char ThresholdBytes[]  = { 0xB0, 0x00, 0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
							ChangeImportedPattern(CDEVSIPattern, ThresholdBytes, sizeof(ThresholdBytes));
						}
					}
				}
			}
		}
	}
}

// Ittr: Hide the half broken TaskView on TH1+ when UWP is on
// Also avoids a crash on pre Germanium Windows 11, see notes/explorer7-pattern-notes.md
void DisableTaskView()
{
	if (s_EnableImmersiveShellStack == 1) // this on its own should be enough as we enforce this value to 0 prior to TH1
	{
		// Renamed across builds, latest name used here, see notes/explorer7-pattern-notes.md
		char* TaskViewHostShow;
		char* TVHSPattern;
		// Zero in all of eax, Show returns a 32 bit HRESULT and a bare ret was unreliable here
		unsigned char bytes[] = { 0x33, 0xC0, 0xC3 };

		HMODULE twinui_pcshell = LoadLibrary(L"twinui.pcshell.dll");

		if (twinui_pcshell)
		{
			// 24H2, TaskViewHost::Show, the gaming host has a different prologue and is left alone
			TaskViewHostShow = "48 89 5C 24 20 57 41 54 41 55 41 56 41 57 48 81 EC ?? ?? 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 ?? ?? 00 00 45 8B E9 4C 89 44 24 ?? 4C 8B FA 48 8B F9 48 89 4C 24 ??";
			TVHSPattern = (char*)FindPattern((uintptr_t)twinui_pcshell, TaskViewHostShow);
			if (!TVHSPattern)
			{
				TaskViewHostShow = "48 89 5C 24 20 56 57 41 54 41 55 41 57 48 81 EC";
				TVHSPattern = (char*)FindPattern((uintptr_t)twinui_pcshell, TaskViewHostShow);
			}

			if (TVHSPattern) // 24H2, then 22H2 and later
			{
				ChangeImportedPattern(TVHSPattern, bytes, sizeof(bytes));
			}
			else
			{
				TaskViewHostShow = "4C 8B DC 57 41 54 41 55 41 56 41 57 48 81 EC 40 03 00 00";
				TVHSPattern = (char*)FindPattern((uintptr_t)twinui_pcshell, TaskViewHostShow);

				if (TVHSPattern) // RS5 to 19H2
				{
					ChangeImportedPattern(TVHSPattern, bytes, sizeof(bytes));
				}
				else
				{
					TaskViewHostShow = "4C 8B DC ?? 41 54 41 55 41 56 41 57 48 83 EC";
					TVHSPattern = (char*)FindPattern((uintptr_t)twinui_pcshell, TaskViewHostShow);

					if (TVHSPattern) // RS2 to RS4
					{
						ChangeImportedPattern(TVHSPattern, bytes, sizeof(bytes));
					}
					else
					{
						// RS1 where twinui.pcshell.dll exists, but isn't used for this so we have to go to twinui version
						// this is an attempt to avoid additional build checks where they aren't needed
						goto DisableTaskView_TWINUI;
					}
				}
			}
		}
		else // TH1 to RS1, where twinui is used instead
		{
DisableTaskView_TWINUI:
			HMODULE twinui = LoadLibrary(L"twinui.dll");

			if (twinui)
			{
				TaskViewHostShow = "48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57 ?? ?? ?? ?? ?? ?? ?? ?? 48 81 EC 50 02 00 00";
				TVHSPattern = (char*)FindPattern((uintptr_t)twinui, TaskViewHostShow);

				if (TVHSPattern) // TH2 to RS1
				{
					ChangeImportedPattern(TVHSPattern, bytes, sizeof(bytes));
				}
				else
				{
					TaskViewHostShow = "48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57 ?? ?? ?? ?? ?? ?? ?? ?? 48 81 EC E0 01 00 00";
					TVHSPattern = (char*)FindPattern((uintptr_t)twinui, TaskViewHostShow);

					if (TVHSPattern) // TH1
					{
						ChangeImportedPattern(TVHSPattern, bytes, sizeof(bytes));
					}
				}
			}
		}
	}
}

// Ittr: Remove broken leftovers of immersive context menus, starting in Windows 10
void RestoreWin32Menus()
{
	// Refactor: Do this in two separate parts - more readable but more lines of code (compiler should optimise...)
	// Pattern variables initialised locally in both to prevent race-conditions
	char unsigned bytes[] = { 0xB0, 0x00, 0xC3 }; // mov al 0, retn - running retn on its own here has inconsistent results
	
	// SHELL32
	HMODULE shell32 = GetModuleHandle(L"shell32.dll");

	if (shell32)
	{
		char* CanApplyOwnerDrawToMenu = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 70 48 8B F2 48 8B E9 33 FF 33 D2";
		char* CAODTMPattern = (char*)FindPattern((uintptr_t)shell32, CanApplyOwnerDrawToMenu);

		if (!CAODTMPattern)
		{
			// 24H2 zeroes ebx first and moves rcx last, ImmersiveContextMenuHelper::CanApplyOwnerDrawToMenu
			CanApplyOwnerDrawToMenu = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 70 33 DB 48 8B F2 33 FF 48 8B E9 85 DB";
			CAODTMPattern = (char*)FindPattern((uintptr_t)shell32, CanApplyOwnerDrawToMenu);
		}

		if (CAODTMPattern) // TH1 to 23H2, then 24H2
		{
			ChangeImportedPattern(CAODTMPattern, bytes, sizeof(bytes));
		}
	}

	// EXPLORERFRAME
	HMODULE explorerFrame = LoadLibrary(L"ExplorerFrame.dll");

	if (explorerFrame)
	{
		char* CanApplyOwnerDrawToMenu = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 70 48 8B F2 48 8B E9 33 FF 33 D2 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? C7 44 24 20 50 00 00 00";
		char* CAODTMPattern = (char*)FindPattern((uintptr_t)explorerFrame, CanApplyOwnerDrawToMenu);

		if (CAODTMPattern) // 21H2 to 23H2
		{
			ChangeImportedPattern(CAODTMPattern, bytes, sizeof(bytes));
		}
		else
		{
			CanApplyOwnerDrawToMenu = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 70 48 8B F2 48 8B E9 33 FF 33 D2";
			CAODTMPattern = (char*)FindPattern((uintptr_t)explorerFrame, CanApplyOwnerDrawToMenu);

			if (CAODTMPattern) // TH1 to VB
			{
				ChangeImportedPattern(CAODTMPattern, bytes, sizeof(bytes));
			}
		}
	}
}


// Disable the hotkey for Win+X menu which is not meant to be enabled at present
void DisableWinXMenu()
{
	// Ittr: This only applies to Windows 10
	char* ShowLauncherTipContextMenu; // CImmersiveHotkeyNotification::_ShowLauncherTipContextMenu
	char* SLTCMPattern;
	unsigned char bytes[] = { 0xB0, 0x01, 0xC3 };

	// 24H2 moved CLauncherTipContextMenu into twinui.pcshell as a two byte stub, nothing to patch
	if (g_osVersion.BuildNumber() >= 26100)
	{
		dbgprintf(L"explorer7: Win+X menu is a stub on this build, no patch needed");
		return;
	}

	HMODULE twinui = LoadLibrary(L"twinui.dll");

	if (twinui)
	{
		ShowLauncherTipContextMenu = "48 89 5C 24 08 57 48 83 EC 20 48 83 64 24 40 00 48 8B D9 48 8D ?? ?? ?? E8 ?? ?? ?? ?? 45 33 C0"; 
		SLTCMPattern = (char*)FindPattern((uintptr_t)twinui, ShowLauncherTipContextMenu);

		if (SLTCMPattern) // first run, TH1 to TH2
		{
			ChangeImportedPattern(SLTCMPattern, bytes, sizeof(bytes));
		}
		else
		{
			ShowLauncherTipContextMenu = "40 53 48 83 EC 20 48 83 64 24 40 00 48 8B D9 48 8D ?? ?? ?? E8 ?? ?? ?? ?? 45 33 C0";
			SLTCMPattern = (char*)FindPattern((uintptr_t)twinui, ShowLauncherTipContextMenu);

			if (SLTCMPattern) // second run, RS1 to RS5
			{
				ChangeImportedPattern(SLTCMPattern, bytes, sizeof(bytes));
			}
			else
			{
				ShowLauncherTipContextMenu = "40 53 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 8B D9 48 83 64 24 50 00 48 8D ?? ?? ?? E8 ?? ?? ?? ?? 45 33 C0";
				SLTCMPattern = (char*)FindPattern((uintptr_t)twinui, ShowLauncherTipContextMenu);

				if (SLTCMPattern) // third run, 19H1 to VB
				{
					ChangeImportedPattern(SLTCMPattern, bytes, sizeof(bytes));
				}
			}
		}
	}
}


// Revert flyout behaviour to non-immersive behaviours as applicable, when immersive shell is off
void RevertFlyouts()
{
		if (!s_UseDCompFlyouts || !s_EnableImmersiveShellStack)
		{
			////// VOLUME FLYOUT
			char* LaunchSndVol = "0F 1F 44 00 00 83 FB 66 75 11";

			HMODULE SVS = LoadLibrary(L"SndVolSSO.dll");
			if (SVS) // only run if DLL is present
			{
				char* LSVPattern = (char*)FindPattern((uintptr_t)SVS, LaunchSndVol);

				if (LSVPattern) // first run, VB and later
				{
					unsigned char bytes[] = { 0x0F, 0x1F, 0x44, 0x00, 0x00, 0x83, 0xFB, 0x66, 0xEB, 0x11 };
					ChangeImportedPattern(LSVPattern, bytes, sizeof(bytes));
				}
				else
				{
					LaunchSndVol = "0F B7 44 24 50 66 83 F8 66 75";

					LSVPattern = (char*)FindPattern((uintptr_t)SVS, LaunchSndVol);

					if (LSVPattern) // second run, TH1 to 19H1
					{
						unsigned char bytes[] = { 0x0F, 0xB7, 0x44, 0x24, 0x50, 0x66, 0x83, 0xF8, 0x66, 0xEB };
						ChangeImportedPattern(LSVPattern, bytes, sizeof(bytes));
					}
				}
			}

			////// NETWORK FLYOUT - See MinHookImports.h
			////// BATTERY FLYOUT - TODO
		}
}

// Ensure that the start menu region is corrected as applicable
void RepairRegionBehaviour()
{
	// For most versions this involves needing to change an conditional jump to an unconditional jump
	char* _ReapplyRegionConditional = "44 38 AB 78 08 00 00 0F 84 E6 00 00 00";
	char* RRCPattern = (char*)FindPattern((uintptr_t)GetModuleHandle(NULL), _ReapplyRegionConditional);

	if (RRCPattern)
	{
		unsigned char bytes[] = { 0x44, 0x38, 0xAB, 0x78, 0x08, 0x00, 0x00, 0xE9, 0xE7, 0x00, 0x00, 0x00, 0x90 };
		ChangeImportedPattern(RRCPattern, bytes, sizeof(bytes));
	}
	else
	{
		// In this case the jump is flipped, so it has to be skipped over
		_ReapplyRegionConditional = "44 38 B3 78 08 00 00 0F 85 ?? ?? ?? ?? FF";
		RRCPattern = (char*)FindPattern((uintptr_t)GetModuleHandle(NULL), _ReapplyRegionConditional);

		if (RRCPattern)
		{
			unsigned char bytes[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0xFF };
			ChangeImportedPattern(RRCPattern, bytes, sizeof(bytes));
		}
	}
}

// Let pinned immersive items show a destination list when right clicked
void FixDestinationListForImmersive()
{
	// Removes a conditional that blocks the display name being retrieved
	// Same effect as checking for immersive CTaskGroup entries
	char* CTaskGroup_GetLauncherName = "0F BA 64 24 48 10 ?? ?? 48 8B 4C 24 40";
	char* CTGGLNPattern = (char*)FindPattern((uintptr_t)GetModuleHandle(NULL), CTaskGroup_GetLauncherName);

	if (CTGGLNPattern)
	{
		unsigned char bytes[] = { 0x0F, 0xBA, 0x64, 0x24, 0x48, 0x10, 0x90, 0x90, 0x48, 0x8B, 0x4C, 0x24, 0x40 };
		ChangeImportedPattern(CTGGLNPattern, bytes, sizeof(bytes));
		return;
	}

	// 7785 and 7850 test the same bit with TEST and JZ instead of BT and JNC
	CTaskGroup_GetLauncherName = "F7 44 24 48 00 00 01 00 74 ?? 48 8B 4C 24 40";
	CTGGLNPattern = (char*)FindPattern((uintptr_t)GetModuleHandle(NULL), CTaskGroup_GetLauncherName);

	if (CTGGLNPattern)
	{
		unsigned char bytes[] = { 0xF7, 0x44, 0x24, 0x48, 0x00, 0x00, 0x01, 0x00, 0x90, 0x90, 0x48, 0x8B, 0x4C, 0x24, 0x40 };
		ChangeImportedPattern(CTGGLNPattern, bytes, sizeof(bytes));
	}
}

void ChangePatternImports()
{
	// Remove Windows 10 animation msstyle classes so that legacy msstyles from Vista onwards are compatible with our theming system
	RemoveLoadAnimationDataMap();
	RemoveGetClassIdForShellTarget();

	// Disable various unwanted immersive interfaces
	DisableImmersiveStart(); // Remove Windows 10+ immersive start menu for UWP mode
	DisableImmersiveSearch(); // Remove Windows 10+ immersive search menu for UWP mode
	DisableTaskView(); // Remove Windows 10+ virtual desktops functionality for UWP mode
	RestoreWin32Menus(); // Remove the immersive menu leftovers so that the taskbar behaves properly in accordance with Windows 7
	DisableWinXMenu(); // Remove Windows 10 Win+X menu functionality for UWP mode


	// Revert flyouts to non-DComp versions on non-UWP or when the user has selected to do this
	RevertFlyouts();

	// Amend some code behaviour so the start menu expand animation behaves more predictably
	RepairRegionBehaviour();

	// Show a destination list when right clicking pinned immersive items
	FixDestinationListForImmersive();
}