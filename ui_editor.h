#pragma once
// Modal-ish macro editor popup. Returns true + modified macro on Save.
#include <string>
#include <windows.h>

#include "engine.h"
#include "models.h"

// Edits m in place; returns true if the user pressed Save.
bool EditMacroDialog(HWND owner, Macro* m, MacroEngine* engine, const std::string& pendingTrigger);
