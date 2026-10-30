#include "ui_editor.h"
#include "ui_main.h"

#include <commctrl.h>
#include <string>
#include <thread>
#include <vector>

#include "input_hook.h"
#include "models.h"
#include "native.h"

namespace {
enum {
    E_NAME = 201,
    E_TRIGGER,
    E_EVENT,
    E_CAPTURE,
    E_CAPLBL,
    E_ADDROW,
    E_ROWS,
    E_SUPPRESS,
    E_TEST,
    E_SAVE,
    E_CANCEL,
    E_TESTLBL,
};
const UINT WM_CAP_DONE = WM_APP + 10;

struct Row {
    HWND input = nullptr;
    HWND action = nullptr;
    HWND rm = nullptr;
};

struct Editor {
    HWND hwnd = nullptr;
    HWND owner = nullptr;
    Macro* m = nullptr;
    MacroEngine* engine = nullptr;
    HWND eName = nullptr;
    HWND eTrigger = nullptr;
    HWND eEvent = nullptr;
    HWND lblCap = nullptr;
    HWND rowsBox = nullptr;
    HWND chkSuppress = nullptr;
    HWND lblTest = nullptr;
    std::vector<Row> rows;
    bool saved = false;
    bool capturing = false;
    int scrollY = 0;
};

HFONT g_edFont = nullptr;

HWND ECtl(HWND parent, const wchar_t* cls, const std::wstring& text, DWORD style, int x, int y,
          int w, int h, HMENU id) {
    HWND c = CreateWindowExW(0, cls, text.c_str(), style | WS_CHILD | WS_VISIBLE, x, y, w, h,
                             parent, id, GetModuleHandleW(nullptr), nullptr);
    if (c && g_edFont) SendMessageW(c, WM_SETFONT, (WPARAM)g_edFont, TRUE);
    return c;
}

std::string GetText(HWND c) {
    int n = GetWindowTextLengthW(c);
    std::wstring w(n + 1, 0);
    GetWindowTextW(c, w.data(), n + 1);
    if (!w.empty() && w.back() == 0) w.pop_back();
    return Native::w2s(w);
}

void FillCombo(HWND cb, const std::vector<std::string>& items) {
    for (auto& s : items) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)Native::s2w(s).c_str());
}

// Select an existing item by text. Necessary for CBS_DROPDOWNLIST: SetWindowText
// does not move the selection, so a reopened macro appeared to "forget" its
// Fires-on / behavior values.
void SelectComboItem(HWND cb, const std::wstring& text) {
    int n = (int)SendMessageW(cb, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < n; i++) {
        wchar_t buf[128] = {0};
        SendMessageW(cb, CB_GETLBTEXT, i, (LPARAM)buf);
        if (text == buf) {
            SendMessageW(cb, CB_SETCURSEL, i, 0);
            return;
        }
    }
    int idx = (int)SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)text.c_str());
    if (idx >= 0) SendMessageW(cb, CB_SETCURSEL, idx, 0);
}

void LayoutRows(Editor* e) {
    int y = 4 - e->scrollY;
    for (size_t i = 0; i < e->rows.size(); i++) {
        SetWindowPos(e->rows[i].input, nullptr, 4, y + 3, 230, 200, SWP_NOZORDER);
        SetWindowPos(e->rows[i].action, nullptr, 242, y + 3, 180, 200, SWP_NOZORDER);
        SetWindowPos(e->rows[i].rm, nullptr, 430, y + 2, 34, 24, SWP_NOZORDER);
        // combo dropdown height: keep list part tall
        y += 30;
    }
    int contentH = (int)e->rows.size() * 30;
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = contentH;
    si.nPage = 190;
    si.nPos = e->scrollY;
    SetScrollInfo(e->rowsBox, SB_VERT, &si, TRUE);
    InvalidateRect(e->rowsBox, nullptr, TRUE);
}

void AddRow(Editor* e, const std::string& presetInput, const std::string& presetAction) {
    Row r;
    r.input = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWN,
                              0, 0, 230, 200, e->rowsBox, nullptr, GetModuleHandleW(nullptr),
                              nullptr);
    for (auto& s : Models::kOutputChoices)
        SendMessageW(r.input, CB_ADDSTRING, 0, (LPARAM)Native::s2w(s).c_str());
    SetWindowTextW(r.input, Native::s2w(presetInput.empty() ? Models::kOutputChoices[0] : presetInput).c_str());
    r.action = CreateWindowExW(0, L"COMBOBOX", L"",
                               WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 0, 0, 180, 200,
                               e->rowsBox, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(r.action, CB_ADDSTRING, 0, (LPARAM)L"Press & release");
    SendMessageW(r.action, CB_ADDSTRING, 0, (LPARAM)L"Toggle on/off");
    SendMessageW(r.action, CB_ADDSTRING, 0, (LPARAM)L"Hold with trigger");
    SelectComboItem(r.action, Native::s2w(Models::behaviorLabel(presetAction)));
    int idx = (int)e->rows.size();
    r.rm = CreateWindowExW(0, L"BUTTON", L"X", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 34, 24,
                           e->rowsBox, (HMENU)(INT_PTR)(3000 + idx), GetModuleHandleW(nullptr),
                           nullptr);
    if (g_edFont) {
        SendMessageW(r.input, WM_SETFONT, (WPARAM)g_edFont, TRUE);
        SendMessageW(r.action, WM_SETFONT, (WPARAM)g_edFont, TRUE);
        SendMessageW(r.rm, WM_SETFONT, (WPARAM)g_edFont, TRUE);
    }
    e->rows.push_back(r);
    LayoutRows(e);
}

void CollectOutputs(Editor* e, std::vector<OutputRow>& out) {
    for (auto& r : e->rows) {
        if (!IsWindow(r.input)) continue;
        std::string inp = Native::trimAscii(GetText(r.input));
        if (inp.empty()) continue;
        wchar_t ab[64] = {0};
        GetWindowTextW(r.action, ab, 64);
        out.push_back({inp, Models::labelToBehavior(Native::w2s(std::wstring(ab)))});
    }
}

LRESULT CALLBACK EditorProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Editor* e = reinterpret_cast<Editor*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (!e) break;
            if (id == E_CAPTURE) {
                SetWindowTextW(e->lblCap, L"Click any mouse button or press a key... (10s)");
                e->capturing = true;
                std::thread([e] {
                    std::string got;
                    InputHook cap(
                        [&](const std::string& n, const std::string& ev) {
                            if (ev == "press" && got.empty()) got = n;
                            return false;
                        },
                        [&](const std::string& b, const std::string& ev, int, int) {
                            if (ev == "press" && got.empty()) got = "mouse:" + b;
                            return false;
                        });
                    try {
                        cap.Start();
                    } catch (...) {
                        std::string* s = new std::string("ERR");
                        PostMessageW(e->hwnd, WM_CAP_DONE, 0, (LPARAM)s);
                        return;
                    }
                    for (int i = 0; i < 100 && got.empty(); i++) Sleep(100);
                    cap.Stop();
                    std::string* s = new std::string(got);
                    PostMessageW(e->hwnd, WM_CAP_DONE, 0, (LPARAM)s);
                }).detach();
            } else if (id == E_ADDROW) {
                if (e->rows.size() >= 12) {
                    MessageBoxW(hwnd, L"Maximum 12 outputs per macro.", L"Limit",
                                MB_ICONINFORMATION);
                } else
                    AddRow(e, "", "tap");
            } else if (id >= 3000 && id < 4000) {
                size_t idx = (size_t)(id - 3000);
                if (idx < e->rows.size()) {
                    DestroyWindow(e->rows[idx].input);
                    DestroyWindow(e->rows[idx].action);
                    DestroyWindow(e->rows[idx].rm);
                    e->rows.erase(e->rows.begin() + idx);
                    // reassign remove-button ids
                    for (size_t i = 0; i < e->rows.size(); i++)
                        SetWindowLongPtrW(e->rows[i].rm, GWLP_ID, (LONG_PTR)(3000 + i));
                    if (e->scrollY > (int)e->rows.size() * 30 - 190)
                        e->scrollY = std::max(0, (int)e->rows.size() * 30 - 190);
                    LayoutRows(e);
                }
            } else if (id == E_TEST) {
                std::vector<OutputRow> outs;
                CollectOutputs(e, outs);
                if (outs.empty()) {
                    SetWindowTextW(e->lblTest, L"Add at least one output first.");
                } else {
                    Macro tmp = *e->m;
                    tmp.outputs = outs;
                    SetWindowTextW(
                        e->lblTest,
                        L"Fired! Watch your tester - each output clicked once. See main log.");
                    try {
                        if (e->engine) e->engine->TestFire(tmp);
                    } catch (...) {}
                }
            } else if (id == E_SAVE) {
                std::string name = GetText(e->eName);
                e->m->name = name.empty() ? "Unnamed" : name;
                std::string trig = GetText(e->eTrigger);
                e->m->trigger = trig.empty() ? "X1 (Back)" : trig;
                wchar_t evb[32] = {0};
                GetWindowTextW(e->eEvent, evb, 32);
                std::string ev = Native::w2s(std::wstring(evb));
                e->m->trigger_event = ev.empty() ? "press" : ev;
                std::vector<OutputRow> outs;
                CollectOutputs(e, outs);
                if (outs.empty()) {
                    MessageBoxW(hwnd, L"Add at least one output row first.", L"No outputs",
                                MB_ICONINFORMATION);
                    break;
                }
                e->m->outputs = outs;
                e->m->action_type = "combo";
                std::string joined;
                for (size_t i = 0; i < outs.size(); i++) {
                    if (i) joined += "+";
                    joined += outs[i].input;
                }
                e->m->combo = joined;
                std::string first = outs[0].action;
                bool allSame = true;
                for (auto& o : outs)
                    if (o.action != first) allSame = false;
                e->m->press_mode = allSame ? first : "tap";
                if (e->m->filter_type.empty()) {
                    e->m->filter_type = "none";
                    e->m->filter_value = "";
                }
                e->m->suppress_original =
                    SendMessageW(e->chkSuppress, BM_GETCHECK, 0, 0) == BST_CHECKED;
                if (e->m->suppress_original && Native::lowerAscii(e->m->trigger) == "left") {
                    if (MessageBoxW(hwnd,
                                    L"Trigger is Left mouse + 'Block original' is ON.\nThis will "
                                    L"swallow ALL left clicks while the engine runs.\n\nContinue?",
                                    L"Block Left click?", MB_YESNO | MB_ICONWARNING) != IDYES)
                        break;
                }
                e->saved = true;
                DestroyWindow(hwnd);
            } else if (id == E_CANCEL) {
                DestroyWindow(hwnd);
            }
            return 0;
        }
        case WM_CAP_DONE: {
            std::string* s = reinterpret_cast<std::string*>(lParam);
            if (s && e) {
                if (*s == "ERR")
                    SetWindowTextW(e->lblCap, L"Capture failed.");
                else if (s->empty())
                    SetWindowTextW(e->lblCap, L"Nothing captured (timeout).");
                else {
                    std::string got = *s;
                    // mouse capture is prefixed to distinguish from key names
                    if (got.rfind("mouse:", 0) == 0) got = got.substr(6);
                    SetWindowTextW(e->eTrigger, Native::s2w(got).c_str());
                    SetWindowTextW(e->lblCap, Native::s2w("Captured: " + got).c_str());
                }
                delete s;
            }
            e->capturing = false;
            return 0;
        }
        case WM_VSCROLL: {
            if (!e || (HWND)lParam != e->rowsBox) break;
            int y = e->scrollY;
            int contentH = (int)e->rows.size() * 30;
            int maxPos = contentH > 190 ? contentH - 190 : 0;
            switch (LOWORD(wParam)) {
                case SB_LINEUP: y -= 30; break;
                case SB_LINEDOWN: y += 30; break;
                case SB_PAGEUP: y -= 190; break;
                case SB_PAGEDOWN: y += 190; break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: y = HIWORD(wParam); break;
                default: break;
            }
            if (y < 0) y = 0;
            if (y > maxPos) y = maxPos;
            e->scrollY = y;
            LayoutRows(e);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (!e) break;
            int d = GET_WHEEL_DELTA_WPARAM(wParam);
            e->scrollY -= (d > 0 ? 30 : -30);
            int contentH = (int)e->rows.size() * 30;
            int maxPos = contentH > 190 ? contentH - 190 : 0;
            if (e->scrollY < 0) e->scrollY = 0;
            if (e->scrollY > maxPos) e->scrollY = maxPos;
            LayoutRows(e);
            return 0;
        }
        case WM_CLOSE: {
            int r = MessageBoxW(hwnd, L"Save your changes before closing?", L"Edit macro",
                                MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDYES) PostMessageW(hwnd, WM_COMMAND, (WPARAM)E_SAVE, 0);
            else if (r == IDNO) DestroyWindow(hwnd);
            return 0;
        }
        case WM_DESTROY: {
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

bool EditMacroDialog(HWND owner, Macro* m, MacroEngine* engine, const std::string& pendingTrigger) {
    if (g_edFont == nullptr)
        g_edFont = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (!pendingTrigger.empty()) m->trigger = pendingTrigger;

    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = EditorProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"MacroCppEditor";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        RegisterClassW(&wc);
        registered = true;
    }

    Editor* e = new Editor();
    e->owner = owner;
    e->m = m;
    e->engine = engine;
    HWND hwnd = CreateWindowExW(0, L"MacroCppEditor", L"Edit macro",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
                                660, 700, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    e->hwnd = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)e);

    int y = 10;
    ECtl(hwnd, L"STATIC", L"Name", WS_VISIBLE, 12, y, 200, 18, nullptr);
    y += 20;
    e->eName = ECtl(hwnd, L"EDIT", Native::s2w(m->name), WS_VISIBLE | WS_TABSTOP | WS_BORDER, 12,
                    y, 610, 24, (HMENU)E_NAME);
    y += 34;
    ECtl(hwnd, L"STATIC", L"When this is pressed... (trigger + fires-on, same row)", WS_VISIBLE, 12,
         y, 500, 18, nullptr);
    y += 20;
    e->eTrigger = ECtl(hwnd, L"COMBOBOX", L"", WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWN, 12, y, 220,
                       200, (HMENU)E_TRIGGER);
    FillCombo(e->eTrigger, Models::kTriggerNames);
    SetWindowTextW(e->eTrigger, Native::s2w(m->trigger.empty() ? "X1 (Back)" : m->trigger).c_str());
    e->eEvent =
        ECtl(hwnd, L"COMBOBOX", L"", WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 240, y, 140, 200,
             (HMENU)E_EVENT);
    FillCombo(e->eEvent, Models::kTriggerEvents);
    // validate event value
    {
        std::string ev = m->trigger_event;
        if (ev != "press" && ev != "release" && ev != "double-click") ev = "press";
        SelectComboItem(e->eEvent, Native::s2w(ev));
    }
    HWND btnCap = ECtl(hwnd, L"BUTTON", L"Capture...", WS_VISIBLE | WS_TABSTOP, 388, y - 1, 110, 26,
                       (HMENU)E_CAPTURE);
    (void)btnCap;
    y += 30;
    e->lblCap = ECtl(hwnd, L"STATIC", L"", WS_VISIBLE, 12, y, 610, 18, (HMENU)E_CAPLBL);
    y += 22;
    ECtl(hwnd, L"STATIC", L"Do these (outputs)", WS_VISIBLE, 12, y, 300, 18, nullptr);
    y += 20;
    ECtl(hwnd, L"STATIC",
         L"Each row: pick a key/mouse button (or type one, e.g. ctrl+c) + how it should behave.",
         WS_VISIBLE, 12, y, 610, 18, nullptr);
    y += 22;
    e->rowsBox = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL,
                                 12, y, 610, 190, hwnd, (HMENU)E_ROWS, GetModuleHandleW(nullptr),
                                 nullptr);
    if (g_edFont) SendMessageW(e->rowsBox, WM_SETFONT, (WPARAM)g_edFont, TRUE);
    y += 198;
    ECtl(hwnd, L"BUTTON", L"+ Add output", WS_VISIBLE | WS_TABSTOP, 12, y, 130, 26,
         (HMENU)E_ADDROW);
    y += 36;
    // initial rows from macro
    {
        auto outs = m->effectiveOutputs();
        for (auto& o : outs) AddRow(e, o.input, o.action);
        if (e->rows.empty()) AddRow(e, "mouse_left", "tap");
    }
    e->chkSuppress = ECtl(hwnd, L"BUTTON", L"Block original trigger (unbind it, macro only)",
                          WS_VISIBLE | BS_AUTOCHECKBOX, 12, y, 610, 20, (HMENU)E_SUPPRESS);
    SendMessageW(e->chkSuppress, BM_SETCHECK, m->suppress_original ? BST_CHECKED : BST_UNCHECKED, 0);
    y += 24;
    ECtl(hwnd, L"STATIC",
         L"ON = trigger does nothing by itself. OFF = both original AND outputs fire. Needs "
         L"engine ON.",
         WS_VISIBLE, 12, y, 610, 18, nullptr);
    y += 28;
    e->lblTest = ECtl(hwnd, L"STATIC", L"", WS_VISIBLE, 12, y + 6, 300, 36, (HMENU)E_TESTLBL);
    ECtl(hwnd, L"BUTTON", L"Test output", WS_VISIBLE | WS_TABSTOP, 424, y, 100, 28, (HMENU)E_TEST);
    ECtl(hwnd, L"BUTTON", L"Save", WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 530, y, 92, 28,
         (HMENU)E_SAVE);
    ECtl(hwnd, L"BUTTON", L"Cancel", WS_VISIBLE | WS_TABSTOP, 338, y, 80, 28, (HMENU)E_CANCEL);

    EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    MSG msg;
    while (e->hwnd && IsWindow(e->hwnd) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_QUIT) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (!IsWindow(e->hwnd)) break;
    }
    // drain quit posted by editor destroy
    MSG dummy;
    while (PeekMessageW(&dummy, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {
    }
    EnableWindow(owner, TRUE);
    SetFocus(owner);
    bool saved = e->saved;
    delete e;
    return saved;
}

