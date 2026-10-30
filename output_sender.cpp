#include "output_sender.h"
#include "native.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace OutputSender {
namespace {
const std::map<std::string, std::string>& mouseAliases() {
    static const std::map<std::string, std::string> m = {
        {"mouse_left", "left"}, {"mouse1", "left"}, {"m1", "left"},
        {"left_click", "left"}, {"leftclick", "left"}, {"left", "left"},
        {"mouse_right", "right"}, {"mouse2", "right"}, {"m2", "right"},
        {"right_click", "right"}, {"rightclick", "right"}, {"right", "right"},
        {"mouse_middle", "middle"}, {"mouse3", "middle"}, {"m3", "middle"},
        {"middle", "middle"}, {"middle_click", "middle"},
        {"mouse_x1", "x1"}, {"mouse4", "x1"}, {"x1", "x1"}, {"back", "x1"},
        {"mouse_x2", "x2"}, {"mouse5", "x2"}, {"x2", "x2"}, {"forward", "x2"},
    };
    return m;
}

const std::map<std::string, unsigned short>& specialVk() {
    static const std::map<std::string, unsigned short> m = {
        {"ctrl", 0x11}, {"alt", 0x12}, {"shift", 0x10}, {"cmd", 0x5B},
        {"win", 0x5B}, {"windows", 0x5B}, {"super", 0x5B},
        {"enter", 0x0D}, {"return", 0x0D}, {"tab", 0x09},
        {"esc", 0x1B}, {"escape", 0x1B}, {"space", 0x20},
        {"backspace", 0x08}, {"delete", 0x2E}, {"del", 0x2E},
        {"insert", 0x2D}, {"home", 0x24}, {"end", 0x23},
        {"page_up", 0x21}, {"page_down", 0x22},
        {"up", 0x26}, {"down", 0x28}, {"left", 0x25}, {"right", 0x27},
        {"caps_lock", 0x14}, {"num_lock", 0x90}, {"scroll_lock", 0x91},
        {"print_screen", 0x2C}, {"pause", 0x13}, {"menu", 0x5D},
    };
    return m;
}

bool isExtendedVk(unsigned short vk) {
    return vk == 0x25 || vk == 0x26 || vk == 0x27 || vk == 0x28 ||
           vk == 0x24 || vk == 0x23 || vk == 0x21 || vk == 0x22 ||
           vk == 0x2D || vk == 0x2E || vk == 0x2C || vk == 0x5D || vk == 0x13;
}

std::mutex g_lock;
std::map<std::string, std::vector<Pressable>> g_held;

bool resolveToken(const std::string& token, Pressable& out) {
    std::string low = Native::lowerAscii(token);
    auto ma = mouseAliases();
    auto it = ma.find(low);
    if (it != ma.end()) {
        out.isMouse = true;
        out.mouseCanon = it->second;
        return true;
    }
    auto sv = specialVk();
    auto it2 = sv.find(low);
    if (it2 != sv.end()) {
        out.vk = it2->second;
        out.extended = isExtendedVk(out.vk);
        return true;
    }
    if (!low.empty() && low[0] == 'f' && low.size() <= 3) {
        try {
            int fn = std::stoi(low.substr(1));
            if (fn >= 1 && fn <= 24) {
                out.vk = (unsigned short)(0x70 + fn - 1);
                return true;
            }
        } catch (...) {}
    }
    if (token.size() == 1) {
        char ch = token[0];
        if (ch >= 'a' && ch <= 'z') { out.vk = (unsigned short)(0x41 + ch - 'a'); return true; }
        if (ch >= 'A' && ch <= 'Z') { out.vk = (unsigned short)(0x41 + ch - 'A'); return true; }
        if (ch >= '0' && ch <= '9') { out.vk = (unsigned short)(0x30 + ch - '0'); return true; }
        static const std::map<char, unsigned short> punct = {
            {';', 0xBA}, {'=', 0xBB}, {',', 0xBC}, {'-', 0xBD}, {'.', 0xBE},
            {'/', 0xBF}, {'`', 0xC0}, {'[', 0xDB}, {'\\', 0xDC}, {']', 0xDD}, {'\'', 0xDE},
        };
        auto itp = punct.find(ch);
        if (itp != punct.end()) { out.vk = itp->second; return true; }
    }
    return false;
}

std::vector<Pressable> parseCombo(const std::string& combo) {
    std::vector<Pressable> units;
    std::string cur;
    auto flush = [&]() {
        size_t a = cur.find_first_not_of(" \t");
        size_t b = cur.find_last_not_of(" \t");
        if (a == std::string::npos) { cur.clear(); return; }
        std::string tok = cur.substr(a, b - a + 1);
        cur.clear();
        Pressable u;
        if (!resolveToken(tok, u))
            throw std::invalid_argument("Unknown key/mouse token: '" + tok + "'");
        units.push_back(u);
    };
    for (char c : combo) {
        if (c == '+') flush();
        else cur.push_back(c);
    }
    flush();
    return units;
}

void checkSent(UINT sent, UINT want, const std::string& what) {
    if (sent != want) {
        DWORD err = GetLastError();
        throw std::runtime_error(what + ": SendInput delivered " + std::to_string(sent) +
            "/" + std::to_string(want) + " (Win32 error " + std::to_string(err) +
            "). If the target runs elevated, run macro_cpp as administrator.");
    }
}

void sendKey(unsigned short vk, bool extended, bool up) {
    UINT scan = MapVirtualKeyW(vk, 0);
    DWORD flags = (extended ? KEYEVENTF_EXTENDEDKEY : 0) | (up ? KEYEVENTF_KEYUP : 0);
    INPUT inp{};
    inp.type = INPUT_KEYBOARD;
    inp.ki.wVk = vk;
    inp.ki.wScan = (WORD)scan;
    inp.ki.dwFlags = flags;
    UINT sent = SendInput(1, &inp, sizeof(INPUT));
    if (!up) checkSent(sent, 1, "key vk=0x" + std::to_string(vk));
}

void sendMouse(const std::string& canon, bool down) {
    DWORD flag = 0;
    DWORD data = 0;
    if (canon == "left") flag = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    else if (canon == "right") flag = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    else if (canon == "middle") flag = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    else if (canon == "x1") { flag = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP; data = XBUTTON1; }
    else if (canon == "x2") { flag = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP; data = XBUTTON2; }
    else throw std::invalid_argument("Unknown mouse button: '" + canon + "'");
    INPUT inp{};
    inp.type = INPUT_MOUSE;
    inp.mi.dwFlags = flag;
    inp.mi.mouseData = data;
    UINT sent = SendInput(1, &inp, sizeof(INPUT));
    checkSent(sent, 1, std::string("mouse ") + canon + (down ? " down" : " up"));
}

void pressUnit(const Pressable& u) {
    if (u.isMouse) sendMouse(u.mouseCanon, true);
    else sendKey(u.vk, u.extended, false);
    try {
        if (TracePress) TracePress(u.isMouse ? ("mouse_" + u.mouseCanon) : ("vk_" + std::to_string(u.vk)));
    } catch (...) {}
}

void releaseUnit(const Pressable& u) {
    if (u.isMouse) sendMouse(u.mouseCanon, false);
    else sendKey(u.vk, u.extended, true);
}
}  // namespace

std::function<void(const std::string&)> TracePress;

std::vector<std::string> ComboMouseButtons(const std::string& combo) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&]() {
        std::string t = Native::lowerAscii(Native::trimAscii(cur));
        cur.clear();
        if (t.empty()) return;
        // split further on spaces/commas
        size_t i = 0;
        while (i < t.size()) {
            while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == ',')) i++;
            size_t j = i;
            while (j < t.size() && t[j] != ' ' && t[j] != '\t' && t[j] != ',') j++;
            if (j > i) {
                auto it = mouseAliases().find(t.substr(i, j - i));
                if (it != mouseAliases().end()) out.push_back(it->second);
            }
            i = j;
        }
    };
    for (char c : combo) {
        if (c == '+') flush();
        else cur.push_back(c);
    }
    flush();
    return out;
}

std::string NormCombo(const std::string& combo) {
    std::string out;
    std::string cur;
    bool first = true;
    auto flush = [&]() {
        std::string t = Native::lowerAscii(Native::trimAscii(cur));
        cur.clear();
        if (t.empty()) return;
        if (!first) out.push_back('+');
        out += t;
        first = false;
    };
    for (char c : combo) {
        if (c == '+') flush();
        else if (c != ' ') cur.push_back(c);
    }
    flush();
    return out;
}

bool IsHeld(const std::string& combo) {
    std::lock_guard<std::mutex> lk(g_lock);
    return g_held.find(NormCombo(combo)) != g_held.end();
}

std::vector<std::string> HeldCombos() {
    std::lock_guard<std::mutex> lk(g_lock);
    std::vector<std::string> r;
    for (auto& kv : g_held) r.push_back(kv.first);
    return r;
}

bool PressDown(const std::string& combo) {
    std::string norm = NormCombo(combo);
    auto units = parseCombo(combo);  // throws on unknown token before state change
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_held.find(norm) != g_held.end()) return false;
    for (auto& u : units) pressUnit(u);
    g_held[norm] = units;
    return true;
}

bool ReleaseUp(const std::string& combo) {
    std::string norm = NormCombo(combo);
    std::vector<Pressable> units;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        auto it = g_held.find(norm);
        if (it == g_held.end()) return false;
        units = it->second;
        g_held.erase(it);
    }
    for (auto it = units.rbegin(); it != units.rend(); ++it) {
        try { releaseUnit(*it); } catch (...) {}
    }
    return true;
}

int ReleaseAll() {
    std::vector<std::vector<Pressable>> all;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        for (auto& kv : g_held) all.push_back(kv.second);
        g_held.clear();
    }
    for (auto& units : all)
        for (auto it = units.rbegin(); it != units.rend(); ++it) {
            try { releaseUnit(*it); } catch (...) {}
        }
    return (int)all.size();
}

void SendCombo(const std::string& combo) {
    auto units = parseCombo(combo);
    std::vector<Pressable> pressed;
    try {
        for (auto& u : units) {
            pressUnit(u);
            pressed.push_back(u);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } catch (...) {
        for (auto it = pressed.rbegin(); it != pressed.rend(); ++it) {
            try { releaseUnit(*it); } catch (...) {}
        }
        throw;
    }
    for (auto it = pressed.rbegin(); it != pressed.rend(); ++it) {
        try { releaseUnit(*it); } catch (...) {}
    }
}

static std::string canonMouse(const std::string& name) {
    std::string low = Native::lowerAscii(Native::trimAscii(name));
    auto it = mouseAliases().find(low);
    if (it != mouseAliases().end()) return it->second;
    return low;
}

void MouseClick(const std::string& buttonName, int clicks) {
    std::string canon = canonMouse(buttonName);
    for (int i = 0; i < std::max(1, clicks); i++) {
        sendMouse(canon, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        sendMouse(canon, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
}

void MouseDown(const std::string& buttonName) { sendMouse(canonMouse(buttonName), true); }
void MouseUp(const std::string& buttonName) { sendMouse(canonMouse(buttonName), false); }

static void sendUnicode(uint32_t cp) {
    if (cp < 0x10000) {
        INPUT down{};
        down.type = INPUT_KEYBOARD;
        down.ki.wScan = (WORD)cp;
        down.ki.dwFlags = KEYEVENTF_UNICODE;
        checkSent(SendInput(1, &down, sizeof(INPUT)), 1, "text key down");
        INPUT up = down;
        up.ki.dwFlags |= KEYEVENTF_KEYUP;
        checkSent(SendInput(1, &up, sizeof(INPUT)), 1, "text key up");
    } else {
        cp -= 0x10000;
        sendUnicode(0xD800 + (cp >> 10));
        sendUnicode(0xDC00 + (cp & 0x3F));
    }
}

void SendText(const std::string& text) {
    for (size_t i = 0; i < text.size();) {
        unsigned char c = (unsigned char)text[i];
        uint32_t cp = 0;
        size_t len = 1;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { i++; continue; }
        if (i + len > text.size()) break;
        for (size_t k = 1; k < len; k++) cp = (cp << 6) | (((unsigned char)text[i + k]) & 0x3F);
        i += len;
        sendUnicode(cp);
    }
}

void SendSequence(const std::vector<SeqStepLite>& steps, std::function<void(const std::string&)> log) {
    for (auto& st : steps) {
        std::string kind = Native::lowerAscii(st.kind);
        std::string val = st.value;
        try {
            if (kind == "key") {
                if (log) log("  key: " + val);
                SendCombo(val);
            } else if (kind == "mouse_down" || kind == "mdown" || kind == "mouse_press") {
                if (log) log("  mouse DOWN: " + val);
                MouseDown(val);
            } else if (kind == "mouse_up" || kind == "mup" || kind == "mouse_release") {
                if (log) log("  mouse UP: " + val);
                MouseUp(val);
            } else if (kind == "mouse_click" || kind == "mclick" || kind == "click" || kind == "mouse") {
                if (log) log("  mouse click: " + val);
                size_t ci = val.find(':');
                if (ci != std::string::npos) {
                    int n = 1;
                    try { n = std::stoi(val.substr(ci + 1)); } catch (...) {}
                    MouseClick(val.substr(0, ci), n);
                } else if (mouseAliases().count(Native::lowerAscii(Native::trimAscii(val)))) {
                    MouseClick(val);
                } else {
                    SendCombo(val);
                }
            } else if (kind == "text") {
                if (log) log("  text: " + val);
                SendText(val);
            } else if (kind == "delay") {
                int ms = st.valueIsNum ? (int)st.num : 0;
                if (!st.valueIsNum) { try { ms = std::stoi(val); } catch (...) {} }
                if (log) log("  delay " + std::to_string(ms) + "ms");
                if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            }
        } catch (const std::exception& e) {
            if (log) log(std::string("  step error: ") + e.what());
        }
    }
}
}  // namespace OutputSender
