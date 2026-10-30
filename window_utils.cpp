#include "window_utils.h"
#include "native.h"
#include <algorithm>
#include <psapi.h>

namespace WindowUtils {
static std::string exeOf(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return "";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return "";
    wchar_t buf[MAX_PATH] = {0};
    DWORD n = MAX_PATH;
    std::string exe;
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
        std::wstring full(buf, n);
        size_t i = full.find_last_of(L"\\/");
        exe = Native::w2s(i == std::wstring::npos ? full : full.substr(i + 1));
    } else {
        wchar_t base[MAX_PATH] = {0};
        if (GetModuleBaseNameW(h, nullptr, base, MAX_PATH)) exe = Native::w2s(base);
    }
    CloseHandle(h);
    return exe;
}

ActiveWindow GetActiveWindow() {
    ActiveWindow w;
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return w;
    w.hwnd = hwnd;
    wchar_t buf[512];
    if (GetWindowTextW(hwnd, buf, 512) > 0) w.title = Native::w2s(buf);
    if (GetClassNameW(hwnd, buf, 512) > 0) w.cls = Native::w2s(buf);
    GetWindowRect(hwnd, &w.rect);
    w.exe = exeOf(hwnd);
    return w;
}

static BOOL CALLBACK enumCb(HWND hwnd, LPARAM lp) {
    auto* out = reinterpret_cast<std::vector<ActiveWindow>*>(lp);
    if (!IsWindowVisible(hwnd)) return TRUE;
    wchar_t buf[512];
    if (GetWindowTextW(hwnd, buf, 512) <= 0) return TRUE;
    std::string title = Native::w2s(buf);
    if (title.empty()) return TRUE;
    // trim whitespace-only
    bool allWs = true;
    for (char c : title)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') { allWs = false; break; }
    if (allWs) return TRUE;
    ActiveWindow w;
    w.hwnd = hwnd;
    w.title = title;
    if (GetClassNameW(hwnd, buf, 512) > 0) w.cls = Native::w2s(buf);
    w.exe = exeOf(hwnd);
    out->push_back(w);
    return TRUE;
}

std::vector<ActiveWindow> ListWindows() {
    std::vector<ActiveWindow> out;
    EnumWindows(enumCb, reinterpret_cast<LPARAM>(&out));
    std::sort(out.begin(), out.end(),
              [](const ActiveWindow& a, const ActiveWindow& b) {
                  std::string x = a.title, y = b.title;
                  for (auto& c : x) c = (char)tolower((unsigned char)c);
                  for (auto& c : y) c = (char)tolower((unsigned char)c);
                  return x < y;
              });
    return out;
}

bool MatchesFilter(const std::string& filterType, const std::string& filterValue,
                   const ActiveWindow& current) {
    if (filterType.empty() || filterType == "none") return true;
    std::string fv = Native::lowerAscii(Native::trimAscii(filterValue));
    if (fv.empty()) return true;
    std::string title = Native::lowerAscii(current.title);
    std::string exe = Native::lowerAscii(current.exe);
    std::string cls = Native::lowerAscii(current.cls);
    if (filterType == "title_contains") return title.find(fv) != std::string::npos;
    if (filterType == "title_exact") return title == fv;
    if (filterType == "exe") {
        auto noExt = [](std::string s) {
            if (s.size() > 4 && s.substr(s.size() - 4) == ".exe") s = s.substr(0, s.size() - 4);
            return s;
        };
        std::string e = noExt(exe), f = noExt(fv);
        return exe == fv || e == f || exe.find(fv) != std::string::npos || fv.find(exe) != std::string::npos;
    }
    if (filterType == "class") return cls == fv;
    return true;
}
}  // namespace WindowUtils
