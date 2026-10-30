#include "input_hook.h"
#include "native.h"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace {
std::mutex g_regLock;
std::vector<InputHook*> g_hooks;

DWORD WINAPI pumpThread(LPVOID param) {
    reinterpret_cast<InputHook*>(param)->pump();
    return 0;
}
}  // namespace

InputHook::InputHook(std::function<bool(const std::string&, const std::string&)> onKey,
                     std::function<bool(const std::string&, const std::string&, int, int)> onMouse)
    : onKey_(onKey), onMouse_(onMouse) {
    ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

InputHook::~InputHook() {
    Stop();
    if (ready_) CloseHandle(ready_);
}

void InputHook::Start() {
    if (running_) return;
    running_ = true;
    ResetEvent(ready_);
    {
        std::lock_guard<std::mutex> lk(g_regLock);
        g_hooks.push_back(this);
    }
    thread_ = CreateThread(nullptr, 0, pumpThread, this, 0, &tid_);
    if (!thread_) {
        running_ = false;
        std::lock_guard<std::mutex> lk(g_regLock);
        g_hooks.erase(std::remove(g_hooks.begin(), g_hooks.end(), this), g_hooks.end());
        throw std::runtime_error("CreateThread failed");
    }
    if (WaitForSingleObject(ready_, 5000) != WAIT_OBJECT_0)
        throw std::runtime_error("Hook thread did not start");
    if (!kbHook_ || !msHook_)
        throw std::runtime_error("SetWindowsHookEx failed");
}

void InputHook::Stop() {
    running_ = false;
    if (kbHook_) {
        UnhookWindowsHookEx(kbHook_);
        kbHook_ = nullptr;
    }
    if (msHook_) {
        UnhookWindowsHookEx(msHook_);
        msHook_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(g_regLock);
        g_hooks.erase(std::remove(g_hooks.begin(), g_hooks.end(), this), g_hooks.end());
    }
    if (tid_) {
        PostThreadMessageW(tid_, WM_QUIT, 0, 0);
        tid_ = 0;
    }
    if (thread_) {
        WaitForSingleObject(thread_, 2000);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
}

void InputHook::pump() {
    HMODULE hmod = GetModuleHandleW(nullptr);
    kbHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, kbProc, hmod, 0);
    if (!kbHook_) kbErr_ = GetLastError();
    msHook_ = SetWindowsHookExW(WH_MOUSE_LL, msProc, hmod, 0);
    if (!msHook_) msErr_ = GetLastError();
    SetEvent(ready_);
    MSG msg;
    while (running_) {
        int ret = GetMessageW(&msg, nullptr, 0, 0);
        if (ret == 0 || ret == -1) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

LRESULT CALLBACK InputHook::kbProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0) {
        auto* kbd = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        if (!(kbd->flags & LLKHF_INJECTED)) {
            bool press = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
            std::string name = Native::VkToName((int)kbd->vkCode);
            std::lock_guard<std::mutex> lk(g_regLock);
            for (auto* h : g_hooks) {
                if (!h->running_ || !h->onKey_) continue;
                bool swallow = false;
                try {
                    swallow = h->onKey_(name, press ? "press" : "release");
                } catch (...) {
                    swallow = false;
                }
                if (swallow) return 1;
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

LRESULT CALLBACK InputHook::msProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0) {
        auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        if (!(ms->flags & LLMHF_INJECTED)) {
            std::string btn, ev;
            bool canBlock = true;
            if (wParam == WM_LBUTTONDOWN) { btn = "Left"; ev = "press"; }
            else if (wParam == WM_LBUTTONUP) { btn = "Left"; ev = "release"; }
            else if (wParam == WM_RBUTTONDOWN) { btn = "Right"; ev = "press"; }
            else if (wParam == WM_RBUTTONUP) { btn = "Right"; ev = "release"; }
            else if (wParam == WM_MBUTTONDOWN) { btn = "Middle"; ev = "press"; }
            else if (wParam == WM_MBUTTONUP) { btn = "Middle"; ev = "release"; }
            else if (wParam == WM_XBUTTONDOWN || wParam == WM_XBUTTONUP) {
                unsigned xb = (ms->mouseData >> 16) & 0xFFFF;
                if (xb == 1) btn = "X1 (Back)";
                else if (xb == 2) btn = "X2 (Forward)";
                else btn = "X" + std::to_string(xb);
                ev = (wParam == WM_XBUTTONDOWN) ? "press" : "release";
            } else if (wParam == WM_MOUSEWHEEL) {
                short delta = (short)((ms->mouseData >> 16) & 0xFFFF);
                btn = delta > 0 ? "Wheel Up" : "Wheel Down";
                ev = "press";
                canBlock = false;  // observe-only
            }
            if (!btn.empty()) {
                int x = ms->pt.x, y = ms->pt.y;
                std::lock_guard<std::mutex> lk(g_regLock);
                for (auto* h : g_hooks) {
                    if (!h->running_ || !h->onMouse_) continue;
                    bool swallow = false;
                    try {
                        swallow = h->onMouse_(btn, ev, x, y);
                    } catch (...) {
                        swallow = false;
                    }
                    if (swallow && canBlock) return 1;
                }
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}
