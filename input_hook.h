#pragma once
// Global low-level keyboard + mouse hooks that can block the original input
// (return non-zero = swallow). Injected (SendInput) events are filtered out.
// Port of input_hook.py / InputHook.cs.
#include <functional>
#include <string>
#include <windows.h>

class InputHook {
  public:
    // onKey(name, "press"|"release") -> true = swallow/block
    // onMouse(button, ev, x, y) -> true = swallow/block (wheel never blocked)
    InputHook(std::function<bool(const std::string&, const std::string&)> onKey = nullptr,
              std::function<bool(const std::string&, const std::string&, int, int)> onMouse = nullptr);
    ~InputHook();
    void Start();  // throws std::runtime_error on failure
    void Stop();
    int kbError() const { return kbErr_; }
    int msError() const { return msErr_; }
    void pump();  // runs on pump thread

  private:
    std::function<bool(const std::string&, const std::string&)> onKey_;
    std::function<bool(const std::string&, const std::string&, int, int)> onMouse_;
    HHOOK kbHook_ = nullptr;
    HHOOK msHook_ = nullptr;
    HANDLE thread_ = nullptr;
    DWORD tid_ = 0;
    volatile bool running_ = false;
    HANDLE ready_ = nullptr;
    int kbErr_ = 0;
    int msErr_ = 0;

    static LRESULT CALLBACK kbProc(int nCode, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK msProc(int nCode, WPARAM wParam, LPARAM lParam);
};