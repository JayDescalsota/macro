#pragma once
// Main window: 3 tabs (Tester / Windows / Macros) + log. Raw Win32.
#include <string>
#include <windows.h>

// Entry point for the UI. Returns exit code.
int UiRun();

// Log a line into the main window log (used by the editor for diagnostics).
void UiLog(const std::string& msg);

// Custom window messages.
#define WM_APP_LOG (WM_APP + 1)        // lParam = new std::string* (takes ownership)
#define WM_APP_DETECT (WM_APP + 2)     // lParam = new std::string* ("K:name:ev" or "M:btn:ev:x:y")
#define WM_APP_CAP (WM_APP + 3)        // editor capture result; lParam = new std::string*
