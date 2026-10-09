#pragma once
#include "common.h"
#include "dbgprint.h"
#include "OptionConfig.h"
#include "OSVersion.h"

// Ittr: Pattern imports are defined and rewritten here rather than dllmain. Feel free to further improve this.

// Allow 7 msstyles to load by removing animation map data from uxtheme.dll imports
void RemoveLoadAnimationDataMap();

// Stop uxtheme appending the immersive class for shell targets
void RemoveGetClassIdForShellTarget();

// Remove unwanted immersive shell interfaces, often by preventing them from running
void DisableImmersiveStart();
void DisableImmersiveSearch();
void DisableTaskView();
void RestoreWin32Menus();
void DisableWinXMenu();


// Disable DComp immersive flyouts as applicable
void RevertFlyouts();

// Forcefully enable a conditional check to allow SetWindowRgn to be applied at the right time
void RepairRegionBehaviour();

// Let pinned immersive items show a destination list when right clicked
void FixDestinationListForImmersive();

// Main procedure we call from elsewhere
void ChangePatternImports();
