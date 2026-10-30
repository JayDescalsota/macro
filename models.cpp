#include "models.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <random>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#endif

namespace Models {
const std::vector<std::string> kTriggerNames = {
    "Left", "Right", "Middle", "X1 (Back)", "X2 (Forward)",
    "Wheel Up", "Wheel Down", "Double-Click Left",
    "F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20",
    "F21", "F22", "F23", "F24", "Extra 6", "Extra 7", "Extra 8",
};
const std::vector<std::string> kTriggerEvents = {"press", "release", "double-click"};
const std::vector<std::string> kOutputChoices = {
    "mouse_left", "mouse_right", "mouse_middle", "mouse_x1", "mouse_x2",
    "w", "a", "s", "d", "q", "e", "r", "f",
    "shift", "ctrl", "alt", "space", "enter", "tab", "esc",
    "F5",
    "F13", "F14", "F15", "F16", "F17", "F18",
    "F19", "F20", "F21", "F22", "F23", "F24",
};

std::string behaviorLabel(const std::string& action) {
    if (action == "toggle") return "Toggle on/off";
    if (action == "hold") return "Hold with trigger";
    return "Press & release";
}

std::string labelToBehavior(const std::string& label) {
    if (label == "Toggle on/off") return "toggle";
    if (label == "Hold with trigger") return "hold";
    return "tap";
}

std::string lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::vector<OutputRow> normalizeOutputs(const std::vector<OutputRow>& raw) {
    std::vector<OutputRow> out;
    for (auto o : raw) {
        // trim input
        size_t a = o.input.find_first_not_of(" \t");
        size_t b = o.input.find_last_not_of(" \t");
        if (a == std::string::npos) continue;
        o.input = o.input.substr(a, b - a + 1);
        o.action = lower(o.action);
        if (o.action != "toggle" && o.action != "hold") o.action = "tap";
        out.push_back(o);
    }
    return out;
}

std::string newId() {
    static std::mt19937_64 rng{std::random_device{}()};
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 8; i++) s.push_back(hex[rng() & 15]);
    return s;
}

std::string configPath() {
#ifdef _WIN32
    char buf[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, buf, MAX_PATH);
    std::string p = buf;
    size_t i = p.find_last_of("\\/");
    return (i == std::string::npos ? "" : p.substr(0, i + 1)) + "macros.json";
#else
    return "macros.json";
#endif
}
}  // namespace Models

static std::vector<std::string> splitPlus(const std::string& s) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : s) {
        if (c == '+') {
            size_t a = cur.find_first_not_of(" \t");
            size_t b = cur.find_last_not_of(" \t");
            if (a != std::string::npos) parts.push_back(cur.substr(a, b - a + 1));
            cur.clear();
        } else cur.push_back(c);
    }
    size_t a = cur.find_first_not_of(" \t");
    size_t b = cur.find_last_not_of(" \t");
    if (a != std::string::npos) parts.push_back(cur.substr(a, b - a + 1));
    return parts;
}

std::vector<OutputRow> Macro::effectiveOutputs() const {
    auto outs = Models::normalizeOutputs(outputs);
    if (!outs.empty()) return outs;
    if ((action_type.empty() ? "combo" : action_type) == "combo" && !combo.empty()) {
        std::string act = Models::lower(press_mode.empty() ? "tap" : press_mode);
        if (act != "toggle" && act != "hold") act = "tap";
        auto parts = splitPlus(combo);
        if (parts.empty()) parts.push_back(combo);
        std::vector<OutputRow> r;
        for (auto& p : parts) r.push_back({p, act});
        return r;
    }
    return {};
}

std::string Macro::summary() const {
    auto outs = effectiveOutputs();
    std::string act;
    if (!outs.empty()) {
        for (size_t i = 0; i < outs.size(); i++) {
            if (i) act += " + ";
            std::string tag = outs[i].action == "toggle" ? "TOGGLE" : outs[i].action == "hold" ? "HOLD" : "tap";
            act += outs[i].input + " [" + tag + "]";
        }
    } else if (action_type == "text") {
        act = "Text: " + text.substr(0, 30);
    } else if (action_type == "sequence") {
        act = "Sequence: " + std::to_string(sequence.size()) + " steps";
    } else {
        act = "Keys: " + combo;
    }
    std::string filt = (filter_type == "none" || filter_type.empty()) ? "Always" : "If " + filter_type + "='" + filter_value + "'";
    std::string block = suppress_original ? "BLOCKED" : "passthrough";
    return trigger + " [" + trigger_event + "] -> " + act + " | " + block + " | " + filt;
}

namespace {
Macro macroFromJson(const JVal& d) {
    Macro m;
    m.id = d.sval("id", Models::newId());
    m.name = d.sval("name", "New macro");
    m.enabled = d.bval("enabled", true);
    m.trigger = d.sval("trigger", "X1 (Back)");
    m.trigger_event = d.sval("trigger_event", "press");
    if (const JVal* o = d.find("outputs")) {
        if (o->type == JVal::Arr)
            for (auto& e : o->arr) {
                if (e.type != JVal::Obj) continue;
                OutputRow r;
                r.input = e.sval("input");
                r.action = e.sval("action", "tap");
                m.outputs.push_back(r);
            }
    }
    m.action_type = d.sval("action_type", "combo");
    m.combo = d.sval("combo", "ctrl+c");
    m.text = d.sval("text", "");
    if (const JVal* s = d.find("sequence")) {
        if (s->type == JVal::Arr)
            for (auto& e : s->arr) {
                if (e.type != JVal::Obj) continue;
                SeqStep st;
                st.kind = e.sval("kind");
                if (const JVal* v = e.find("value")) st.value = *v;
                m.sequence.push_back(st);
            }
    }
    m.filter_type = d.sval("filter_type", "none");
    m.filter_value = d.sval("filter_value", "");
    m.hold_repeat = d.bval("hold_repeat", false);
    m.repeat_interval_ms = (int)d.nval("repeat_interval_ms", 100);
    m.press_mode = d.sval("press_mode", "tap");
    if (const JVal* so = d.find("suppress_original"))
        m.suppress_original = so->type == JVal::Bool ? so->b : true;
    m.description = d.sval("description", "");
    // migrate legacy combo -> per-output rows
    if (Models::normalizeOutputs(m.outputs).empty() && !m.combo.empty() &&
        (m.action_type.empty() || m.action_type == "combo")) {
        std::string act = Models::lower(m.press_mode.empty() ? "tap" : m.press_mode);
        if (act != "toggle" && act != "hold") act = "tap";
        auto parts = splitPlus(m.combo);
        if (parts.empty()) parts.push_back(m.combo);
        for (auto& p : parts) m.outputs.push_back({p, act});
    }
    return m;
}

void writeOutputs(std::ostringstream& o, const Macro& m, int indent);
void writeSeq(std::ostringstream& o, const Macro& m, int indent);

std::string indentStr(int n) { return std::string((size_t)n, ' '); }

void macroToJson(std::ostringstream& o, const Macro& m) {
    auto f = [&](const char* k, const std::string& v, bool comma = true) {
        o << indentStr(2) << "\"" << k << "\": \"" << JEscape(v) << "\"" << (comma ? ",\n" : "\n");
    };
    o << "{\n";
    f("id", m.id);
    f("name", m.name);
    o << indentStr(2) << "\"enabled\": " << (m.enabled ? "true" : "false") << ",\n";
    f("trigger", m.trigger);
    f("trigger_event", m.trigger_event);
    o << indentStr(2) << "\"outputs\": [";
    if (m.outputs.empty()) {
        o << "],\n";
    } else {
        o << "\n";
        for (size_t i = 0; i < m.outputs.size(); i++) {
            o << indentStr(4) << "{\"input\": \"" << JEscape(m.outputs[i].input)
              << "\", \"action\": \"" << JEscape(m.outputs[i].action) << "\"}"
              << (i + 1 < m.outputs.size() ? ",\n" : "\n");
        }
        o << indentStr(2) << "],\n";
    }
    f("action_type", m.action_type);
    f("combo", m.combo);
    f("text", m.text);
    o << indentStr(2) << "\"sequence\": [";
    if (m.sequence.empty()) {
        o << "],\n";
    } else {
        o << "\n";
        for (size_t i = 0; i < m.sequence.size(); i++) {
            const auto& s = m.sequence[i];
            o << indentStr(4) << "{\"kind\": \"" << JEscape(s.kind) << "\", \"value\": ";
            if (s.value.type == JVal::Str) o << "\"" << JEscape(s.value.str) << "\"";
            else if (s.value.type == JVal::Num) {
                long long iv = (long long)s.value.num;
                if (s.value.num == (double)iv) o << iv; else o << s.value.num;
            } else if (s.value.type == JVal::Bool) o << (s.value.b ? "true" : "false");
            else o << "null";
            o << "}" << (i + 1 < m.sequence.size() ? ",\n" : "\n");
        }
        o << indentStr(2) << "],\n";
    }
    f("filter_type", m.filter_type);
    f("filter_value", m.filter_value);
    o << indentStr(2) << "\"hold_repeat\": " << (m.hold_repeat ? "true" : "false") << ",\n";
    o << indentStr(2) << "\"repeat_interval_ms\": " << m.repeat_interval_ms << ",\n";
    f("press_mode", m.press_mode);
    o << indentStr(2) << "\"suppress_original\": " << (m.suppress_original ? "true" : "false") << ",\n";
    f("description", m.description, false);
    o << "}";
}
}  // namespace

namespace Models {
std::vector<Macro> loadMacros(const std::string& path) {
    std::string p = path.empty() ? configPath() : path;
    std::vector<Macro> out;
    std::ifstream f(p, std::ios::binary);
    if (!f) return out;
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    // Tolerate a UTF-8 BOM (Notepad/PowerShell write one); otherwise the
    // whole file silently fails to parse and the list comes up empty.
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
        (unsigned char)s[2] == 0xBF)
        s.erase(0, 3);
    JVal root;
    std::string err;
    if (!JParser::parse(s, root, err)) return out;
    if (root.type != JVal::Arr) return out;
    for (auto& e : root.arr) {
        if (e.type != JVal::Obj) continue;
        out.push_back(macroFromJson(e));
    }
    return out;
}

bool saveMacros(const std::vector<Macro>& macros, const std::string& path) {
    std::string p = path.empty() ? configPath() : path;
    std::ostringstream o;
    o << "[\n";
    for (size_t i = 0; i < macros.size(); i++) {
        macroToJson(o, macros[i]);
        o << (i + 1 < macros.size() ? ",\n" : "\n");
    }
    o << "]\n";
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << o.str();
    return true;
}
}  // namespace Models
