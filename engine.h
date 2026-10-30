#pragma once
// Macro engine: trigger -> outputs, with window filters + blocking.
// Port of macro_engine.py / MacroEngine.cs.
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "input_hook.h"
#include "models.h"
#include "window_utils.h"

class MacroEngine {
  public:
    MacroEngine(std::function<std::vector<Macro>()> getMacros,
                std::function<void(const std::string&)> onLog = nullptr);
    ~MacroEngine();

    void Start();  // throws on hook failure
    void Stop();
    bool Running() const { return running_; }

    bool VerboseLog = false;

    // Hook callbacks (also used directly by bench).
    bool HandleKey(const std::string& name, const std::string& ev);
    bool HandleMouse(const std::string& button, const std::string& ev, int x, int y);

    std::vector<Macro> MatchMacros(const std::string& button, const std::string& ev,
                                   const ActiveWindow& current);
    int KillCount() const { return killCount_.load(); }
    bool IsOutputHeld(const std::string& macroId, const std::string& outputInput);
    bool IsMacroHeld(const std::string& macroId);
    std::vector<std::string> HeldMacroIds();
    int ReleaseAllKeys();
    void TestFire(Macro macro);

  private:
    std::function<std::vector<Macro>()> getMacros_;
    std::function<void(const std::string&)> onLog_;
    std::unique_ptr<InputHook> hook_;
    bool running_ = false;
    std::map<std::string, bool> held_;
    std::map<std::string, bool> toggleState_;
    std::map<std::string, unsigned long long> repeatGen_;
    std::map<std::string, long long> lastPressMs_;  // debounce (bounce / double-report)
    static const long long kTriggerDebounceMs = 120;  // keys + non-left mouse
    static const long long kLeftDebounceMs = 35;      // left mouse: keep fast double-clicks
    static long long DebounceWindow(const std::string& button);
    std::mutex lock_;
    std::atomic<long long> prevLeftPressMs_{0};
    // ---- global kill switch: Ctrl+F12 (chord) ----
    std::atomic<int> killCount_{0};
    std::atomic<bool> stopping_{false};
    bool killCtrlHeld_ = false;
    bool KillCheck(const std::string& name, const std::string& ev);
    static const long long kDoubleClickMs = 350;

    void Log(const std::string& msg);
    static std::string ShortTitle(const ActiveWindow& c);
    void Dispatch(const Macro& m, const std::string& button, const std::string& ev,
                  const ActiveWindow& current);
    void DispatchOutputs(const Macro& m, const std::string& button, const std::string& ev,
                         const ActiveWindow& current, const std::vector<OutputRow>& outs);
    void StartRepeater(Macro macro, const std::string& button);
    void ExecuteTap(Macro macro);
    void ExecuteOutputsTap(Macro m, std::vector<OutputRow> outs);
    void HoldDown(Macro m, ActiveWindow current);
    void HoldUp(Macro m);
    void ToggleFlip(Macro m, const std::string& button);
    void HoldDownOutput(Macro m, std::string input, std::string where);
    void HoldUpOutput(Macro m, std::string input);
    void ToggleFlipOutput(Macro m, std::string input, std::string button);
    bool IsHeldCombo(const std::string& combo);
    static std::string OutKey(const std::string& id, const std::string& input);
};
