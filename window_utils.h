#pragma once
// Active-window helpers + filter matching. Port of window_utils.py.
#include <string>
#include <vector>
#include <windows.h>

struct ActiveWindow {
    HWND hwnd = nullptr;
    std::string title;  // UTF-8
    std::string exe;    // e.g. "notepad.exe"
    std::string cls;
    RECT rect{};
};

namespace WindowUtils {
ActiveWindow GetActiveWindow();
std::vector<ActiveWindow> ListWindows();
bool MatchesFilter(const std::string& filterType, const std::string& filterValue,
                   const ActiveWindow& current);
}  // namespace WindowUtils
