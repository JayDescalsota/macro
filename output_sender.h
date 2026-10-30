#pragma once
// Keyboard + mouse output via SendInput. Port of key_sender.py / OutputSender.cs.
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace OutputSender {
struct Pressable {
    bool isMouse = false;
    std::string mouseCanon;  // left|right|middle|x1|x2
    unsigned short vk = 0;
    bool extended = false;
};

// Bench/debug hook: invoked after every successful physical press.
extern std::function<void(const std::string&)> TracePress;

std::vector<std::string> ComboMouseButtons(const std::string& combo);
std::string NormCombo(const std::string& combo);
bool IsHeld(const std::string& combo);
std::vector<std::string> HeldCombos();
bool PressDown(const std::string& combo);   // false if already held; throws on unknown token
bool ReleaseUp(const std::string& combo);   // false if not held
int ReleaseAll();
void SendCombo(const std::string& combo);   // tap: press, 20ms hold, release
void MouseClick(const std::string& buttonName, int clicks = 1);
void MouseDown(const std::string& buttonName);
void MouseUp(const std::string& buttonName);
void SendText(const std::string& text);
struct SeqStepLite { std::string kind; std::string value; bool valueIsNum = false; double num = 0; };
void SendSequence(const std::vector<SeqStepLite>& steps, std::function<void(const std::string&)> log = nullptr);
}  // namespace OutputSender
