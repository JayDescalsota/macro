#pragma once
// Macro data model + JSON persistence. Same macros.json schema as the
// Python and C# versions (field names identical).
#include <string>
#include <vector>
#include "json_min.h"

struct OutputRow {
    std::string input;
    std::string action = "tap";  // tap | toggle | hold
};

struct SeqStep {
    std::string kind;
    JVal value;  // string or number
};

struct Macro {
    std::string id;
    std::string name = "New macro";
    bool enabled = true;
    std::string trigger = "X1 (Back)";
    std::string trigger_event = "press";
    std::vector<OutputRow> outputs;
    // legacy fields (kept for compat; migrated to outputs on load)
    std::string action_type = "combo";
    std::string combo = "ctrl+c";
    std::string text = "hello";
    std::vector<SeqStep> sequence;
    std::string filter_type = "none";
    std::string filter_value;
    bool hold_repeat = false;
    int repeat_interval_ms = 100;
    std::string press_mode = "tap";
    bool suppress_original = true;
    std::string description;

    std::vector<OutputRow> effectiveOutputs() const;
    std::string summary() const;
};

namespace Models {
extern const std::vector<std::string> kTriggerNames;
extern const std::vector<std::string> kTriggerEvents;
extern const std::vector<std::string> kOutputChoices;
std::string behaviorLabel(const std::string& action);
std::string labelToBehavior(const std::string& label);
std::vector<OutputRow> normalizeOutputs(const std::vector<OutputRow>& raw);
std::string configPath();
std::vector<Macro> loadMacros(const std::string& path = "");
bool saveMacros(const std::vector<Macro>& macros, const std::string& path = "");
std::string newId();
}  // namespace Models
