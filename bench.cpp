#include "bench.h"
#include "engine.h"
#include "native.h"
#include "output_sender.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

namespace {
long long nowUs() {
    using namespace std::chrono;
    static auto t0 = steady_clock::now();
    return duration_cast<microseconds>(steady_clock::now() - t0).count();
}
void outLine(const std::string& s) { printf("%s\n", s.c_str()); fflush(stdout);
    static FILE* f = nullptr;
    if (!f) f = fopen("bench_results.txt", "w");
    if (f) { fprintf(f, "%s\n", s.c_str()); fflush(f); } }

struct Trace {
    std::mutex lk;
    std::vector<std::pair<long long, std::string>> stamps;
    static void cb(const std::string& tag, std::vector<std::pair<long long, std::string>>* v,
                   std::mutex* m) {
        std::lock_guard<std::mutex> l(*m);
        v->push_back({nowUs(), tag});
    }
};

void compatCheck() {
    auto macros = Models::loadMacros("D:\\Code\\macro\\macros.json");
    char b[64];
    snprintf(b, sizeof(b), "compat: loaded %d macro(s) from Python macros.json", (int)macros.size());
    outLine(b);
    for (auto& m : macros) outLine(std::string("  - ") + m.summary());
}

void toggleBench() {
    Trace t;
    OutputSender::TracePress = [&](const std::string& tag) { Trace::cb(tag, &t.stamps, &t.lk); };
    Macro m;
    m.name = "bench-toggle";
    m.trigger = "X1 (Back)";
    m.trigger_event = "press";
    m.outputs = {{"F24", "toggle"}, {"F23", "toggle"}};
    std::vector<std::string> logs;
    std::mutex logLk;
    MacroEngine eng([&] { return std::vector<Macro>{m}; },
                    [&](const std::string& s) {
                        std::lock_guard<std::mutex> l(logLk);
                        logs.push_back(s);
                    });
    for (int i = 0; i < 5; i++) {
        {
            std::lock_guard<std::mutex> l(t.lk);
            t.stamps.clear();
        }
        OutputSender::ReleaseAll();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        long long t0 = nowUs();
        eng.HandleMouse("X1 (Back)", "press", 0, 0);
        long long deadline = t0 + 2000000;
        for (;;) {
            {
                std::lock_guard<std::mutex> l(t.lk);
                if (t.stamps.size() >= 2) break;
            }
            if (nowUs() > deadline) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        {
            std::lock_guard<std::mutex> l(t.lk);
            if (t.stamps.size() >= 2) {
                char b[160];
                snprintf(b, sizeof(b),
                         "toggle click%d: trigger->first DOWN %.1f ms, row gap %.1f ms", i + 1,
                         (t.stamps[0].first - t0) / 1000.0,
                         (t.stamps[1].first - t.stamps[0].first) / 1000.0);
                outLine(b);
            } else
                outLine("toggle click" + std::to_string(i + 1) + ": TIMEOUT");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        eng.HandleMouse("X1 (Back)", "press", 0, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    OutputSender::TracePress = nullptr;
}

void tapBench() {
    Trace t;
    OutputSender::TracePress = [&](const std::string& tag) { Trace::cb(tag, &t.stamps, &t.lk); };
    Macro m;
    m.name = "bench-tap";
    m.trigger = "X1 (Back)";
    m.trigger_event = "press";
    m.outputs = {{"F24", "tap"}, {"F23", "tap"}};
    MacroEngine eng([&] { return std::vector<Macro>{m}; }, [&](const std::string&) {});
    for (int i = 0; i < 5; i++) {
        {
            std::lock_guard<std::mutex> l(t.lk);
            t.stamps.clear();
        }
        OutputSender::ReleaseAll();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        long long t0 = nowUs();
        eng.HandleMouse("X1 (Back)", "press", 0, 0);
        long long deadline = t0 + 2000000;
        for (;;) {
            {
                std::lock_guard<std::mutex> l(t.lk);
                if (t.stamps.size() >= 2) break;
            }
            if (nowUs() > deadline) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        {
            std::lock_guard<std::mutex> l(t.lk);
            if (t.stamps.size() >= 2) {
                char b[160];
                snprintf(b, sizeof(b),
                         "tap click%d: trigger->first press %.1f ms, row gap %.1f ms", i + 1,
                         (t.stamps[0].first - t0) / 1000.0,
                         (t.stamps[1].first - t.stamps[0].first) / 1000.0);
                outLine(b);
            } else
                outLine("tap click" + std::to_string(i + 1) + ": TIMEOUT");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    OutputSender::TracePress = nullptr;
    OutputSender::ReleaseAll();
}

void atomicBench() {
    Trace t;
    OutputSender::TracePress = [&](const std::string& tag) { Trace::cb(tag, &t.stamps, &t.lk); };
    std::vector<std::string> logs;
    std::mutex logLk;
    Macro m;
    m.name = "race";
    m.trigger = "X1 (Back)";
    m.trigger_event = "press";
    m.outputs = {{"F24", "toggle"}};
    MacroEngine eng([&] { return std::vector<Macro>{m}; },
                    [&](const std::string& s) {
                        std::lock_guard<std::mutex> l(logLk);
                        logs.push_back(s);
                    });
    eng.VerboseLog = true;
    eng.HandleMouse("X1 (Back)", "press", 0, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    outLine(std::string("single press held=") + (OutputSender::IsHeld("F24") ? "True" : "False") +
            " (expect True)");
    // Two presses spaced past the debounce window must flip twice (atomic state).
    eng.HandleMouse("X1 (Back)", "press", 0, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    eng.HandleMouse("X1 (Back)", "press", 0, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    outLine(std::string("after press(+200ms),press held=") +
            (OutputSender::IsHeld("F24") ? "True" : "False") +
            " (expect True: DOWN->UP->DOWN), badge=" + (eng.IsMacroHeld(m.id) ? "True" : "False"));

    // Debounce: two presses 20ms apart must count as ONE (no flip-flop).
    eng.HandleMouse("X1 (Back)", "press", 0, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    bool beforeDup = OutputSender::IsHeld("F24");
    eng.HandleMouse("X1 (Back)", "press", 0, 0);   // flips
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    eng.HandleMouse("X1 (Back)", "press", 0, 0);   // bounce -> must be ignored
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    bool afterDup = OutputSender::IsHeld("F24");
    outLine(std::string("debounce test before=") + (beforeDup ? "DOWN" : "UP") + " after=" +
            (afterDup ? "DOWN" : "UP") +
            " (expect exactly one flip: DOWN->UP, i.e. before!=after)");

    Macro mk;
    mk.name = "keyrepeat";
    mk.trigger = "g";
    mk.trigger_event = "press";
    mk.outputs = {{"F23", "toggle"}};
    MacroEngine eng2([&] { return std::vector<Macro>{mk}; },
                     [&](const std::string& s) {
                         std::lock_guard<std::mutex> l(logLk);
                         logs.push_back(s);
                     });
    eng2.VerboseLog = true;
    eng2.HandleKey("g", "press");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    eng2.HandleKey("g", "press");  // auto-repeat: must be ignored
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    bool held2 = OutputSender::IsHeld("F23");
    eng2.HandleKey("g", "release");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    outLine(std::string("auto-repeat test held after repeats=") + (held2 ? "True" : "False") +
            " (expect True: single flip), badge=" + (eng2.IsMacroHeld(mk.id) ? "True" : "False"));
    try {
        OutputSender::SendCombo("no_such_key_xyz");
        outLine("bogus token: NO ERROR (bad)");
    } catch (const std::exception& e) {
        outLine(std::string("bogus token error OK: ") + e.what());
    }
    OutputSender::ReleaseAll();

    // Kill switch: Ctrl+F12 (chord).
    {
        MacroEngine ke([] { return std::vector<Macro>{}; },
                       [&](const std::string& s) {
                           std::lock_guard<std::mutex> l(logLk);
                           logs.push_back(s);
                       });
        ke.HandleKey("ctrl", "press");
        bool swallowed = ke.HandleKey("F12", "press");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        outLine(std::string("kill switch Ctrl+F12 fired=") + std::to_string(ke.KillCount()) +
                " (expect 1), swallowed=" + (swallowed ? "True" : "False"));

        MacroEngine ke2([] { return std::vector<Macro>{}; }, [](const std::string&) {});
        ke2.HandleKey("F12", "press");  // no ctrl held -> must not fire
        outLine(std::string("kill switch F12 alone fired=") + std::to_string(ke2.KillCount()) +
                " (expect 0)");
    }

    {
        std::lock_guard<std::mutex> l(logLk);
        for (auto& s : logs) outLine("LOG | " + s);
    }
    outLine("atomic bench done");
}
}  // namespace

namespace Bench {
void run(const std::string& which) {
    if (which == "bench4") {
        atomicBench();
        return;
    }
    outLine("macro_cpp bench - real SendInput path, harmless F24/F23 keys.");
    compatCheck();
    toggleBench();
    tapBench();
    outLine("done.");
}
}  // namespace Bench
