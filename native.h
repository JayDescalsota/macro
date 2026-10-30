#pragma once
// Win32 helpers shared by all modules. UTF-8 std::string at the logic layer,
// wide conversion only at API boundaries.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cctype>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace Native {

inline std::string lowerAscii(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
inline std::string trimAscii(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// UTF-8 <-> UTF-16
inline std::wstring s2w(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w((size_t)n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    if (!w.empty() && w.back() == 0) w.pop_back();
    return w;
}
inline std::string w2s(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    if (!s.empty() && s.back() == 0) s.pop_back();
    return s;
}
inline std::string w2s(const wchar_t* w, int len) {
    if (!w || len <= 0) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w, len, s.data(), n, nullptr, nullptr);
    return s;
}

inline std::string VkToName(int vk) {
    static const std::map<int, const char*> names = {
        {0x08, "backspace"}, {0x09, "tab"}, {0x0D, "enter"}, {0x1B, "esc"},
        {0x20, "space"},
        {0x10, "shift"}, {0xA0, "shift"}, {0xA1, "shift"},
        {0x11, "ctrl"}, {0xA2, "ctrl"}, {0xA3, "ctrl"},
        {0x12, "alt"}, {0xA4, "alt"}, {0xA5, "alt"},
        {0x5B, "cmd"}, {0x5C, "cmd"},
        {0x2E, "delete"}, {0x2D, "insert"}, {0x24, "home"}, {0x23, "end"},
        {0x21, "page_up"}, {0x22, "page_down"},
        {0x25, "left"}, {0x26, "up"}, {0x27, "right"}, {0x28, "down"},
        {0x14, "caps_lock"}, {0x90, "num_lock"}, {0x91, "scroll_lock"},
        {0x2C, "print_screen"}, {0x13, "pause"},
        {0xBA, ";"}, {0xBB, "="}, {0xBC, ","}, {0xBD, "-"}, {0xBE, "."},
        {0xBF, "/"}, {0xC0, "`"}, {0xDB, "["}, {0xDC, "\\"}, {0xDD, "]"}, {0xDE, "'"},
    };
    if (vk >= 0x41 && vk <= 0x5A) return std::string(1, (char)tolower(vk));
    if (vk >= 0x30 && vk <= 0x39) return std::string(1, (char)vk);
    if (vk >= 0x60 && vk <= 0x69) return std::to_string(vk - 0x60);
    if (vk >= 0x70 && vk <= 0x87) return "F" + std::to_string(vk - 0x70 + 1);
    auto it = names.find(vk);
    if (it != names.end()) return it->second;
    char b[16];
    snprintf(b, sizeof(b), "vk_0x%x", vk);
    return b;
}

inline std::string CanonicalTrigger(const std::string& name) {
    std::string n = lowerAscii(trimAscii(name));
    if (n == "escape") return "esc";
    if (n == "win" || n == "windows" || n == "super" || n == "meta" || n == "cmd") return "cmd";
    if (n == "return") return "enter";
    if (n == "del") return "delete";
    if (n == "pgup" || n == "pageup") return "page_up";
    if (n == "pgdn" || n == "pagedn" || n == "pagedown") return "page_down";
    return n;
}

inline bool TriggersEqual(const std::string& a, const std::string& b) {
    return CanonicalTrigger(a) == CanonicalTrigger(b);
}

inline const std::map<std::string, std::string>& MouseCanonToTrigger() {
    static const std::map<std::string, std::string> m = {
        {"left", "Left"}, {"right", "Right"}, {"middle", "Middle"},
        {"x1", "X1 (Back)"}, {"x2", "X2 (Forward)"},
    };
    return m;
}

}  // namespace Native
