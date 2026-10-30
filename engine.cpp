#include "engine.h"
#include "native.h"
#include "output_sender.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <thread>

namespace {
long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
std::string joinNames(const std::vector<Macro>& ms) {
    std::string s;
    for (size_t i = 0; i < ms.size(); i++) {
        if (i) s += ",";
        s += "'" + ms[i].name + "'";
    }
    return s;
}
}  // namespace

MacroEngine::MacroEngine(std::function<std::vector<Macro>()> getMacros,
                         std::function<void(const std::string&)> onLog)
    : getMacros_(getMacros), onLog_(onLog) {}

MacroEngine::~MacroEngine() { Stop(); }

void MacroEngine::Log(const std::string& msg) {
    // No timestamp here: the UI log adds exactly one prefix. Adding it in
    // both places produced the doubled "[hh:mm:ss] [hh:mm:ss]" lines.
    try {
        if (onLog_) onLog_(msg);
    } catch (...) {}
}

long long MacroEngine::DebounceWindow(const std::string& button) {
    // Left click gets a tiny window so genuine fast double-clicks still work.
    return Native::TriggersEqual(button, "Left") ? kLeftDebounceMs : kTriggerDebounceMs;
}

std::string MacroEngine::ShortTitle(const ActiveWindow& c) {
    return c.title.size() > 40 ? c.title.substr(0, 40) : c.title;
}

std::string MacroEngine::OutKey(const std::string& id, const std::string& input) {
    std::string low = Native::lowerAscii(input);
    return id + "::" + low;
}

void MacroEngine::Start() {
    if (running_) return;
    // Warm the worker path once: first detached thread creation can be slow
    // in constrained sessions; do it before installing hooks.
    std::thread([] {}).detach();
    hook_ = std::make_unique<InputHook>(
        [this](const std::string& n, const std::string& e) { return HandleKey(n, e); },
        [this](const std::string& b, const std::string& e, int x, int y) { return HandleMouse(b, e, x, y); });
    hook_->Start();
    running_ = true;
    Log("Macro engine STARTED (blocking hooks - original keys/mouse can be unbound).");
}

void MacroEngine::Stop() {
    if (stopping_.exchange(true)) return;  // already stopping (e.g. kill switch)
    if (hook_) {
        try { hook_->Stop(); } catch (...) {}
        hook_.reset();
    }
    {
        std::lock_guard<std::mutex> lk(lock_);
        held_.clear();
    }
    try {
        int n = OutputSender::ReleaseAll();
        if (n > 0) Log("Released " + std::to_string(n) + " held output(s).");
    } catch (...) {}
    {
        std::lock_guard<std::mutex> lk(lock_);
        toggleState_.clear();
        repeatGen_.clear();
    }
    running_ = false;
    Log("Macro engine STOPPED.");
    stopping_ = false;
}

bool MacroEngine::IsOutputHeld(const std::string& macroId, const std::string& outputInput) {
    std::string key = OutKey(macroId, outputInput);
    {
        std::lock_guard<std::mutex> lk(lock_);
        auto it = toggleState_.find(key);
        if (it != toggleState_.end() && it->second) return true;
    }
    try {
        return OutputSender::IsHeld(outputInput);
    } catch (...) {
        return false;
    }
}

bool MacroEngine::IsMacroHeld(const std::string& macroId) {
    std::lock_guard<std::mutex> lk(lock_);
    auto it = toggleState_.find(macroId);
    if (it != toggleState_.end() && it->second) return true;
    std::string prefix = macroId + "::";
    for (auto& kv : toggleState_)
        if (kv.first.compare(0, prefix.size(), prefix) == 0 && kv.second) return true;
    return false;
}

std::vector<std::string> MacroEngine::HeldMacroIds() {
    std::lock_guard<std::mutex> lk(lock_);
    std::vector<std::string> ids;
    for (auto& kv : toggleState_) {
        if (!kv.second) continue;
        size_t i = kv.first.find("::");
        ids.push_back(i == std::string::npos ? kv.first : kv.first.substr(0, i));
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

int MacroEngine::ReleaseAllKeys() {
    int n = 0;
    try {
        n = OutputSender::ReleaseAll();
    } catch (...) {}
    {
        std::lock_guard<std::mutex> lk(lock_);
        toggleState_.clear();
    }
    Log("Released all keys/mouse (" + std::to_string(n) + " combo(s)).");
    return n;
}

// Global kill switch: Ctrl+F12. Runs BEFORE macro matching, window filters
// and blocking, so a filter or a swallowed trigger can never hide it.
bool MacroEngine::KillCheck(const std::string& name, const std::string& ev) {
    std::string k = Native::lowerAscii(Native::trimAscii(name));
    bool isCtrl = (k == "ctrl");
    bool isF12 = (k == "f12");
    if (!isCtrl && !isF12) return false;

    bool fire = false;
    {
        std::lock_guard<std::mutex> lk(lock_);
        if (isCtrl) killCtrlHeld_ = (ev == "press");
        if (isF12 && ev == "press") {
            // Ctrl may have been held before the engine started, so also read
            // the physical key state as a fallback.
            bool ctrlDown = killCtrlHeld_ || ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0);
            if (ctrlDown) fire = true;
        }
    }
    if (!fire) return false;
    killCount_.fetch_add(1);
    Log("KILL SWITCH (Ctrl+F12): stopping engine, releasing all keys/mouse.");
    // Cannot unhook from inside the hook callback (self-join), so defer.
    std::thread([this] {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        try { Stop(); } catch (...) {}
    }).detach();
    return true;  // swallow the F12 press
}

bool MacroEngine::HandleKey(const std::string& name, const std::string& ev) {
    if (KillCheck(name, ev)) return true;
    bool repeat = false;
    bool dup = false;
    long long now = nowMs();
    {
        std::lock_guard<std::mutex> lk(lock_);
        // No auto-repeat flag in the LL hook struct: a press with no
        // intervening release is a keyboard auto-repeat - never re-fire it.
        auto it = held_.find(name);
        if (ev == "press" && it != held_.end() && it->second) repeat = true;
        if (ev == "press" && !repeat) {
            // Physical switch bounce / driver double-report: two presses
            // closer than the debounce window count as ONE press.
            auto lp = lastPressMs_.find(name);
            if (lp != lastPressMs_.end() && now - lp->second < kTriggerDebounceMs) dup = true;
            lastPressMs_[name] = now;
        }
        held_[name] = (ev == "press");
    }
    ActiveWindow current = WindowUtils::GetActiveWindow();
    auto matches = MatchMacros(name, ev, current);
    if (matches.empty()) {
        if (VerboseLog) Log("[HOOK] key '" + name + "' [" + ev + "] - no macro (ignored).");
        return false;
    }
    if (repeat || dup) {
        bool sw = false;
        for (auto& m : matches)
            if (m.suppress_original) sw = true;
        if (repeat && VerboseLog)
            Log("[HOOK] key '" + name + "' [" + ev + "] auto-repeat - ignored (swallow=" +
                std::string(sw ? "1" : "0") + ").");
        if (dup)
            Log("[DEBOUNCE] key '" + name + "' press within " + std::to_string(kTriggerDebounceMs) +
                "ms of previous - ignored (bounce/duplicate), still blocked.");
        return sw;
    }
    Log("[HOOK] key '" + name + "' [" + ev + "] -> " + std::to_string(matches.size()) +
        " macro(s) [" + joinNames(matches) + "], swallow=" +
        (std::any_of(matches.begin(), matches.end(),
                     [](const Macro& m) { return m.suppress_original; })
             ? "1"
             : "0") +
        " in '" + ShortTitle(current) + "'.");
    for (auto& m : matches) Dispatch(m, name, ev, current);
    bool swallowed = false;
    for (auto& m : matches)
        if (m.suppress_original) swallowed = true;
    if (swallowed) Log("[BLOCKED] original key '" + name + "' [" + ev + "] -> macro only.");
    return swallowed;
}

bool MacroEngine::HandleMouse(const std::string& button, const std::string& ev, int x, int y) {
    (void)x;
    (void)y;
    long long now = nowMs();
    bool dup = false;
    {
        std::lock_guard<std::mutex> lk(lock_);
        if (ev == "press") {
            // Some mice/drivers deliver two down events for one physical
            // click (switch bounce). Without this the toggle flipped down,
            // then immediately back up - net zero.
            auto lp = lastPressMs_.find(button);
            long long win = DebounceWindow(button);
            if (lp != lastPressMs_.end() && now - lp->second < win) dup = true;
            lastPressMs_[button] = now;
        }
        held_[button] = (ev == "press");
    }
    if (ev == "press" && Native::TriggersEqual(button, "Left")) {
        long long prev = prevLeftPressMs_.load();
        if (prev != 0 && now - prev < kDoubleClickMs) {
            ActiveWindow cur0 = WindowUtils::GetActiveWindow();
            for (auto& m : MatchMacros("Double-Click Left", "press", cur0))
                Dispatch(m, "Double-Click Left", "press", cur0);
            for (auto& m : MatchMacros("Double-Click Left", "double-click", cur0))
                Dispatch(m, "Double-Click Left", "double-click", cur0);
        }
        prevLeftPressMs_.store(now);
    }
    ActiveWindow current = WindowUtils::GetActiveWindow();
    auto matches = MatchMacros(button, ev, current);
    if (matches.empty()) {
        if (VerboseLog) Log("[HOOK] mouse '" + button + "' [" + ev + "] - no macro (ignored).");
        return false;
    }
    if (dup) {
        bool sw = false;
        for (auto& m : matches)
            if (m.suppress_original) sw = true;
        Log("[DEBOUNCE] mouse '" + button + "' press within " + std::to_string(DebounceWindow(button)) +
            "ms of previous - ignored (bounce/duplicate), still blocked.");
        return sw;
    }
    Log("[HOOK] mouse '" + button + "' [" + ev + "] -> " + std::to_string(matches.size()) +
        " macro(s) [" + joinNames(matches) + "], swallow=" +
        (std::any_of(matches.begin(), matches.end(),
                     [](const Macro& m) { return m.suppress_original; })
             ? "1"
             : "0") +
        " in '" + ShortTitle(current) + "'.");
    for (auto& m : matches) Dispatch(m, button, ev, current);
    bool swallowed = false;
    for (auto& m : matches)
        if (m.suppress_original) swallowed = true;
    if (swallowed) Log("[BLOCKED] original mouse '" + button + "' [" + ev + "] -> macro only.");
    return swallowed;
}

std::vector<Macro> MacroEngine::MatchMacros(const std::string& button, const std::string& ev,
                                            const ActiveWindow& current) {
    std::vector<Macro> out;
    std::vector<Macro> macros;
    try {
        macros = getMacros_();
    } catch (...) {
        return out;
    }
    for (auto& m : macros) {
        if (!m.enabled) continue;
        if (!Native::TriggersEqual(m.trigger, button)) continue;
        std::string want = m.trigger_event;
        if (want != "press" && want != "release" && want != "double-click" && want != "both")
            want = "press";
        if (want == "press" && ev != "press") {
            bool hasHold = false;
            try {
                for (auto& o : m.effectiveOutputs())
                    if (o.action == "hold") hasHold = true;
            } catch (...) {}
            std::string pm = Native::lowerAscii(m.press_mode.empty() ? "tap" : m.press_mode);
            if (pm != "hold" && !hasHold) continue;
        }
        if (want == "release" && ev != "release") continue;
        if (want == "double-click" &&
            !(Native::TriggersEqual(button, "Double-Click Left") ||
              (Native::TriggersEqual(button, "Left") && ev == "press")))
            continue;
        try {
            if (!WindowUtils::MatchesFilter(m.filter_type, m.filter_value, current)) continue;
        } catch (...) {
            continue;
        }
        out.push_back(m);
    }
    return out;
}

void MacroEngine::Dispatch(const Macro& m, const std::string& button, const std::string& ev,
                           const ActiveWindow& current) {
    std::vector<OutputRow> outs;
    try {
        outs = m.effectiveOutputs();
    } catch (...) {}
    if (!outs.empty()) {
        DispatchOutputs(m, button, ev, current, outs);
        return;
    }
    std::string mode = Native::lowerAscii(m.press_mode.empty() ? "tap" : m.press_mode);
    if (mode != "toggle" && mode != "hold") mode = "tap";
    if (mode == "hold") {
        if (m.action_type != "combo") {
            if (ev == "press") {
                Log("TRIGGER '" + m.name + "': " + button + " [" + ev +
                    "/HOLD-as-tap] -> firing once.");
                std::thread([this, m] { ExecuteTap(m); }).detach();
            }
            return;
        }
        if (ev == "press") {
            std::thread([this, m, current] { HoldDown(m, current); }).detach();
        } else {
            std::thread([this, m] { HoldUp(m); }).detach();
        }
        return;
    }
    if (mode == "toggle") {
        bool flipEdge =
            (m.trigger_event == "release") ? ev == "release" : ev == "press";
        if (!flipEdge) return;
        if (m.action_type != "combo") {
            if (ev == "press") {
                Log("TRIGGER '" + m.name + "': " + button + " [" + ev +
                    "/TOGGLE-as-tap] -> firing once.");
                std::thread([this, m] { ExecuteTap(m); }).detach();
            }
            return;
        }
        std::thread([this, m, button] { ToggleFlip(m, button); }).detach();
        return;
    }
    std::string title = current.title.size() > 50 ? current.title.substr(0, 50) : current.title;
    Log("TRIGGER '" + m.name + "': " + button + " [" + ev + "] in '" + title + "' -> firing.");
    std::thread([this, m] { ExecuteTap(m); }).detach();
    if (m.hold_repeat && ev == "press") StartRepeater(m, button);
}

void MacroEngine::DispatchOutputs(const Macro& m, const std::string& button, const std::string& ev,
                                  const ActiveWindow& current,
                                  const std::vector<OutputRow>& outs) {
    std::vector<OutputRow> taps, toggles, holds;
    for (auto& o : outs) {
        if (o.action == "toggle") toggles.push_back(o);
        else if (o.action == "hold") holds.push_back(o);
        else taps.push_back(o);
    }
    std::string where = ShortTitle(current);
    if (!holds.empty()) {
        if (ev == "press") {
            for (auto& o : holds)
                std::thread([this, m, o, where] { HoldDownOutput(m, o.input, where); }).detach();
        } else {
            for (auto& o : holds)
                std::thread([this, m, o] { HoldUpOutput(m, o.input); }).detach();
        }
        if (ev != "press") return;
    }
    if (!toggles.empty()) {
        bool flipEdge = (m.trigger_event == "release") ? ev == "release" : ev == "press";
        if (flipEdge)
            for (auto& o : toggles)
                std::thread([this, m, o, button] { ToggleFlipOutput(m, o.input, button); }).detach();
    }
    if (!taps.empty()) {
        std::string want = m.trigger_event.empty() ? "press" : m.trigger_event;
        bool fire = (want == "press" && ev == "press") || (want == "release" && ev == "release") ||
                    want == "double-click";
        if (fire) {
            Log("TRIGGER '" + m.name + "': " + button + " [" + ev + "] in '" + where +
                "' -> firing " + std::to_string(taps.size()) + " output(s).");
            std::thread([this, m, taps] { ExecuteOutputsTap(m, taps); }).detach();
        }
    }
}

void MacroEngine::StartRepeater(Macro macro, const std::string& button) {
    unsigned long long gen;
    {
        std::lock_guard<std::mutex> lk(lock_);
        auto it = repeatGen_.find(button);
        if (it != repeatGen_.end()) {
            // a repeater may already run; bump generation so only the newest survives
        }
        gen = ++repeatGen_[button];
    }
    int interval = std::max(macro.repeat_interval_ms, 30);
    std::thread([this, macro, button, gen, interval] {
        std::this_thread::sleep_for(std::chrono::milliseconds(interval));
        while (true) {
            bool held, en;
            unsigned long long cur;
            {
                std::lock_guard<std::mutex> lk(lock_);
                auto ith = held_.find(button);
                held = ith != held_.end() && ith->second;
                en = macro.enabled && running_;
                cur = repeatGen_[button];
            }
            if (!(held && en) || cur != gen) break;
            ExecuteTap(macro);
            std::this_thread::sleep_for(std::chrono::milliseconds(interval));
        }
    }).detach();
}

void MacroEngine::ExecuteTap(Macro macro) {
    try {
        if (macro.action_type == "combo" || macro.action_type.empty()) {
            OutputSender::SendCombo(macro.combo);
            Log("  sent: " + macro.combo);
        } else if (macro.action_type == "text") {
            OutputSender::SendText(macro.text);
            Log("  typed text (" + std::to_string(macro.text.size()) + " chars)");
        } else if (macro.action_type == "sequence") {
            std::vector<OutputSender::SeqStepLite> steps;
            for (auto& s : macro.sequence) {
                OutputSender::SeqStepLite lite;
                lite.kind = s.kind;
                if (s.value.type == JVal::Str) lite.value = s.value.str;
                else if (s.value.type == JVal::Num) {
                    lite.valueIsNum = true;
                    lite.num = s.value.num;
                    lite.value = std::to_string((long long)s.value.num);
                }
                steps.push_back(lite);
            }
            OutputSender::SendSequence(steps, [this](const std::string& s) { Log(s); });
            Log("  sequence done");
        }
    } catch (const std::exception& e) {
        Log(std::string("  ERROR sending: ") + e.what());
    }
}

void MacroEngine::ExecuteOutputsTap(Macro m, std::vector<OutputRow> outs) {
    for (auto& o : outs) {
        try {
            OutputSender::SendCombo(o.input);
            Log("  output tap: " + o.input + " clicked.");
        } catch (const std::exception& e) {
            Log("  output ERROR " + o.input + ": " + e.what());
        }
    }
}

void MacroEngine::HoldDown(Macro m, ActiveWindow current) {
    std::lock_guard<std::mutex> lk(lock_);
    try {
        auto it = toggleState_.find(m.id);
        bool down = (it != toggleState_.end() && it->second) || OutputSender::IsHeld(m.combo);
        if (down) {
            toggleState_[m.id] = true;
            Log("HOLD '" + m.name + "': " + m.combo + " already down (state check) - keeping held.");
            return;
        }
        toggleState_[m.id] = true;
        bool ok = OutputSender::PressDown(m.combo);
        Log(ok ? "HOLD '" + m.name + "': " + m.combo + " DOWN in '" + ShortTitle(current) + "'"
               : "HOLD '" + m.name + "': " + m.combo + " already held (lost race) - keeping held.");
    } catch (const std::exception& e) {
        toggleState_.erase(m.id);
        Log(std::string("  HOLD DOWN ERROR: ") + e.what());
    }
}

void MacroEngine::HoldUp(Macro m) {
    std::lock_guard<std::mutex> lk(lock_);
    try {
        bool was = toggleState_.erase(m.id) > 0;
        if (OutputSender::IsHeld(m.combo)) {
            OutputSender::ReleaseUp(m.combo);
            Log("HOLD '" + m.name + "': " + m.combo + " UP (was DOWN; trigger released).");
        } else if (was) {
            Log("HOLD '" + m.name + "': already up (state check).");
        }
    } catch (const std::exception& e) {
        Log(std::string("  HOLD UP ERROR: ") + e.what());
    }
}

void MacroEngine::ToggleFlip(Macro m, const std::string& button) {
    std::lock_guard<std::mutex> lk(lock_);
    try {
        auto it = toggleState_.find(m.id);
        bool state = (it != toggleState_.end() && it->second) || IsHeldCombo(m.combo);
        bool newState = !state;
        toggleState_[m.id] = newState;
        if (newState) {
            if (!OutputSender::IsHeld(m.combo)) OutputSender::PressDown(m.combo);
            Log("TOGGLE '" + m.name + "': " + button + " -> " + m.combo +
                " DOWN (was UP; click again to release).");
        } else {
            if (OutputSender::IsHeld(m.combo)) OutputSender::ReleaseUp(m.combo);
            Log("TOGGLE '" + m.name + "': " + button + " -> " + m.combo +
                " UP (was DOWN; released).");
        }
    } catch (const std::exception& e) {
        toggleState_.erase(m.id);
        Log(std::string("  TOGGLE ERROR: ") + e.what());
    }
}

bool MacroEngine::IsHeldCombo(const std::string& combo) {
    try {
        return OutputSender::IsHeld(combo);
    } catch (...) {
        return false;
    }
}

void MacroEngine::HoldDownOutput(Macro m, std::string input, std::string where) {
    std::string key = OutKey(m.id, input);
    std::lock_guard<std::mutex> lk(lock_);
    try {
        auto it = toggleState_.find(key);
        bool down = (it != toggleState_.end() && it->second) || OutputSender::IsHeld(input);
        if (down) {
            toggleState_[key] = true;
            Log("HOLD '" + m.name + "': " + input + " already down (state check) - keeping held.");
            return;
        }
        toggleState_[key] = true;
        bool ok = OutputSender::PressDown(input);
        Log(ok ? "HOLD '" + m.name + "': " + input + " DOWN (was UP; trigger held) in '" + where + "'"
               : "HOLD '" + m.name + "': " + input + " already held (lost race) - keeping held.");
    } catch (const std::exception& e) {
        toggleState_.erase(key);
        Log("  HOLD DOWN ERROR " + input + ": " + e.what());
    }
}

void MacroEngine::HoldUpOutput(Macro m, std::string input) {
    std::string key = OutKey(m.id, input);
    std::lock_guard<std::mutex> lk(lock_);
    try {
        bool was = toggleState_.erase(key) > 0;
        if (OutputSender::IsHeld(input)) {
            OutputSender::ReleaseUp(input);
            Log("HOLD '" + m.name + "': " + input + " UP (was DOWN; trigger released).");
        } else if (was) {
            Log("HOLD '" + m.name + "': " + input + " already up (state check).");
        }
    } catch (const std::exception& e) {
        Log("  HOLD UP ERROR " + input + ": " + e.what());
    }
}

void MacroEngine::ToggleFlipOutput(Macro m, std::string input, std::string button) {
    std::string key = OutKey(m.id, input);
    std::lock_guard<std::mutex> lk(lock_);
    try {
        auto it = toggleState_.find(key);
        bool state = (it != toggleState_.end() && it->second) || IsHeldCombo(input);
        bool newState = !state;
        toggleState_[key] = newState;
        if (newState) {
            if (!OutputSender::IsHeld(input)) OutputSender::PressDown(input);
            Log("TOGGLE '" + m.name + "': " + button + " -> " + input +
                " DOWN (was UP; click again to release).");
        } else {
            if (OutputSender::IsHeld(input)) OutputSender::ReleaseUp(input);
            Log("TOGGLE '" + m.name + "': " + button + " -> " + input +
                " UP (was DOWN; released).");
        }
    } catch (const std::exception& e) {
        toggleState_.erase(key);
        Log("  TOGGLE ERROR " + input + ": " + e.what());
    }
}

void MacroEngine::TestFire(Macro macro) {
    std::thread([this, macro] {
        std::vector<OutputRow> outs;
        try {
            outs = macro.effectiveOutputs();
        } catch (...) {}
        if (!outs.empty()) {
            Log("TEST '" + macro.name + "': firing " + std::to_string(outs.size()) +
                " output(s) as clicks...");
            for (auto& o : outs) {
                try {
                    OutputSender::SendCombo(o.input);
                    Log("  TEST output: " + o.input + " clicked. Did your tester register it?");
                } catch (const std::exception& e) {
                    Log("  TEST ERROR " + o.input + ": " + e.what());
                }
            }
            Log("TEST '" + macro.name + "' done.");
            return;
        }
        Log("TEST '" + macro.name + "': legacy macro, firing once...");
        try {
            ExecuteTap(macro);
        } catch (const std::exception& e) {
            Log(std::string("  TEST ERROR: ") + e.what());
        }
    }).detach();
}
