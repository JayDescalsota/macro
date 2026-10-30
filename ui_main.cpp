#include "ui_main.h"
#include "ui_editor.h"

#include <commctrl.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "engine.h"
#include "input_hook.h"
#include "models.h"
#include "native.h"
#include "window_utils.h"

#pragma comment(lib, "comctl32.lib")

namespace {
// ---- control ids ----
enum {
    ID_ENGINE = 101,
    ID_STATUS,
    ID_TAB,
    ID_LOG,
    ID_DETECT_START,
    ID_DETECT_CLEAR,
    ID_DETECT_HID,
    ID_DETECT_LAST,
    ID_DETECT_USE,
    ID_LAMP0,
    ID_TESTLOG = 130,
    ID_WIN_REFRESH,
    ID_WIN_FILTER,
    ID_WIN_LIST,
    ID_WIN_USE,
    ID_MAC_ADD,
    ID_MAC_EDIT,
    ID_MAC_TEST,
    ID_MAC_DEL,
    ID_MAC_ENABLE,
    ID_MAC_SAVE,
    ID_MAC_RELEASE,
    ID_MAC_VERBOSE,
    ID_MAC_LIST,
};
const char* kLampNames[7] = {"Left",        "Right",       "Middle",   "X1 (Back)",
                             "X2 (Forward)", "Wheel Up", "Wheel Down"};

const int IDT_WIN = 1;
const int IDT_HELD = 2;

HFONT g_font = nullptr;
HWND g_main = nullptr;
HWND g_tab = nullptr;
HWND g_pages[3] = {nullptr, nullptr, nullptr};
HWND g_log = nullptr;
HWND g_btnEngine = nullptr;
HWND g_lblStatus = nullptr;
// tester
HWND g_btnDetect = nullptr;
HWND g_chkHid = nullptr;
HWND g_lblLast = nullptr;
HWND g_testLog = nullptr;
HWND g_lamps[7] = {nullptr};
bool g_lampOn[7] = {false};
HBRUSH g_brushOn = nullptr;
HBRUSH g_brushOff = nullptr;
// windows
HWND g_lblWin = nullptr;
HWND g_lblMatch = nullptr;
HWND g_winFilter = nullptr;
HWND g_winList = nullptr;
std::vector<ActiveWindow> g_winCache;
// macros
HWND g_macList = nullptr;
std::string g_macSig;
// app state
std::vector<Macro> g_macros;
std::unique_ptr<MacroEngine> g_engine;
std::unique_ptr<InputHook> g_detector;
bool g_detecting = false;
bool g_detectKb = true;
std::string g_lastDetected;
std::string g_pendingTrigger;

std::wstring W(const std::string& s) { return Native::s2w(s); }

HWND Ctl(HWND parent, const wchar_t* cls, const std::wstring& text, DWORD style, int x, int y,
         int w, int h, HMENU id) {
    HWND c = CreateWindowExW(0, cls, text.c_str(), style | WS_CHILD | WS_VISIBLE, x, y, w, h,
                             parent, id, GetModuleHandleW(nullptr), nullptr);
    if (c && g_font) SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

void AppendLog(HWND edit, const std::string& line) {
    int len = GetWindowTextLengthW(edit);
    if (len > 60000) {
        SendMessageW(edit, EM_SETSEL, 0, 10000);
        SendMessageW(edit, EM_REPLACESEL, FALSE, (LPARAM)L"");
        len = GetWindowTextLengthW(edit);
    }
    SendMessageW(edit, EM_SETSEL, len, len);
    std::wstring w = Native::s2w(line + "\r\n");
    SendMessageW(edit, EM_REPLACESEL, FALSE, (LPARAM)w.c_str());
}

void Log(const std::string& msg) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char b[32];
    snprintf(b, sizeof(b), "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond,
             st.wMilliseconds);
    if (g_log && g_main) {
        std::string* s = new std::string(std::string(b) + msg);
        PostMessageW(g_main, WM_APP_LOG, 0, (LPARAM)s);
    }
}

void UpdateStatus() {
    std::string eng = (g_engine && g_engine->Running()) ? "ON" : "OFF";
    std::string det = g_detecting ? "ON" : "OFF";
    SetWindowTextW(g_lblStatus, W("engine: " + eng + "   -   detector: " + det).c_str());
}

void RefreshMacroList() {
    std::string sig;
    for (auto& m : g_macros) sig += m.id + (g_engine && g_engine->IsMacroHeld(m.id) ? "[H]" : "") + m.summary() + "\n";
    if (sig == g_macSig) return;  // avoid flicker
    g_macSig = sig;
    int sel = (int)SendMessageW(g_macList, LB_GETCURSEL, 0, 0);
    std::string selId;
    if (sel >= 0 && sel < (int)g_macros.size()) selId = g_macros[sel].id;
    SendMessageW(g_macList, LB_RESETCONTENT, 0, 0);
    int maxW = 0;
    for (auto& m : g_macros) {
        std::string pre = (g_engine && g_engine->IsMacroHeld(m.id)) ? "[HELD] " : "";
        std::string s = pre + m.summary();
        int idx = (int)SendMessageW(g_macList, LB_ADDSTRING, 0, (LPARAM)W(s).c_str());
        SendMessageW(g_macList, LB_SETITEMDATA, idx, (LPARAM)0);
        if ((int)s.size() > maxW) maxW = (int)s.size();
    }
    SendMessageW(g_macList, LB_SETHORIZONTALEXTENT, maxW * 7, 0);
    if (!selId.empty()) {
        for (size_t i = 0; i < g_macros.size(); i++)
            if (g_macros[i].id == selId) {
                SendMessageW(g_macList, LB_SETCURSEL, i, 0);
                break;
            }
    }
}

int SelectedMacro() {
    int sel = (int)SendMessageW(g_macList, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)g_macros.size()) return -1;
    return sel;
}

void SaveAll(bool silent) {
    Models::saveMacros(g_macros);
    if (!silent) Log("Macros saved to macros.json");
}

void ToggleEngine() {
    if (g_engine->Running()) {
        g_engine->Stop();
        SetWindowTextW(g_btnEngine, L"> Start macros");
    } else {
        if (g_macros.empty()) {
            MessageBoxW(g_main, L"Add a macro first (Macros tab).", L"No macros", MB_ICONINFORMATION);
            return;
        }
        try {
            g_engine->Start();
        } catch (const std::exception& e) {
            MessageBoxW(g_main, (L"Could not start engine:\r\n" + W(e.what())).c_str(),
                        L"Engine error", MB_ICONERROR);
            return;
        }
        SetWindowTextW(g_btnEngine, L"[] Stop macros");
    }
    UpdateStatus();
}

void OpenEditor(Macro* macro, bool isNew) {
    std::string pend = g_pendingTrigger;
    g_pendingTrigger.clear();
    bool saved = EditMacroDialog(g_main, macro, g_engine.get(), pend);
    if (saved) {
        if (isNew) {
            // macro was edited in place via pointer; find/add
            bool found = false;
            for (auto& m : g_macros)
                if (m.id == macro->id) {
                    m = *macro;
                    found = true;
                    break;
                }
            if (!found) g_macros.push_back(*macro);
        } else {
            for (auto& m : g_macros)
                if (m.id == macro->id) {
                    m = *macro;
                    break;
                }
        }
        SaveAll(true);
        g_macSig.clear();
        RefreshMacroList();
    }
}

// ---- tester ----
void TLog(const std::string& msg) {
    if (g_testLog) AppendLog(g_testLog, msg);
}

void FlashLamp(const char* name) {
    for (int i = 0; i < 7; i++)
        if (strcmp(kLampNames[i], name) == 0) {
            g_lampOn[i] = true;
            InvalidateRect(g_lamps[i], nullptr, TRUE);
            HWND lamp = g_lamps[i];
            // revert after 350ms via timer callback thread
            std::thread([lamp, i] {
                Sleep(350);
                g_lampOn[i] = false;
                InvalidateRect(lamp, nullptr, TRUE);
            }).detach();
            break;
        }
}

bool DetectKey(const std::string& name, const std::string& ev) {
    if (!g_detectKb || !g_main) return false;
    std::string* s = new std::string("K:" + name + ":" + ev);
    PostMessageW(g_main, WM_APP_DETECT, 0, (LPARAM)s);
    return false;
}

bool DetectMouse(const std::string& button, const std::string& ev, int x, int y) {
    if (!g_main) return false;
    char b[128];
    snprintf(b, sizeof(b), "M:%s:%s:%d:%d", button.c_str(), ev.c_str(), x, y);
    std::string* s = new std::string(b);
    PostMessageW(g_main, WM_APP_DETECT, 0, (LPARAM)s);
    return false;
}

void ToggleDetect() {
    if (g_detecting) {
        try {
            if (g_detector) g_detector->Stop();
        } catch (...) {}
        g_detector.reset();
        g_detecting = false;
        SetWindowTextW(g_btnDetect, L"Start listening");
    } else {
        g_detector = std::make_unique<InputHook>(DetectKey, DetectMouse);
        try {
            g_detector->Start();
        } catch (const std::exception& ex) {
            MessageBoxW(g_main, (L"Could not start listener:\r\n" + W(ex.what())).c_str(),
                        L"Listener error", MB_ICONERROR);
            g_detector.reset();
            return;
        }
        g_detecting = true;
        SetWindowTextW(g_btnDetect, L"Stop listening");
        TLog("Listening... click mouse buttons / scroll / press gaming buttons.");
    }
    UpdateStatus();
}

void OnDetectEvent(const std::string& payload) {
    // "K:name:ev" or "M:btn:ev:x:y"
    char kind = payload.empty() ? '?' : payload[0];
    size_t p1 = payload.find(':');
    size_t p2 = payload.find(':', p1 + 1);
    if (p1 == std::string::npos || p2 == std::string::npos) return;
    std::string a = payload.substr(p1 + 1, p2 - p1 - 1);
    size_t p3 = payload.find(':', p2 + 1);
    std::string ev = payload.substr(p2 + 1, p3 == std::string::npos ? std::string::npos : p3 - p2 - 1);
    char b[256];
    if (kind == 'K') {
        snprintf(b, sizeof(b), "%s  [keyboard] %s %s", "", a.c_str(), ev.c_str());
        TLog(b + 1);
        if (ev == "press") {
            g_lastDetected = a;
            SetWindowTextW(g_lblLast, W("Last: " + a).c_str());
        }
    } else {
        std::string rest = p3 == std::string::npos ? "" : payload.substr(p3 + 1);
        snprintf(b, sizeof(b), "  [mouse] %s %s @(%s)", a.c_str(), ev.c_str(), rest.c_str());
        TLog(b + 1);
        if (ev == "press") {
            g_lastDetected = a;
            SetWindowTextW(g_lblLast, W("Last: " + a).c_str());
            FlashLamp(a.c_str());
        }
    }
}

// ---- windows tab ----
void TickActiveWindow(HWND lblWin, HWND lblMatch) {
    ActiveWindow cur = WindowUtils::GetActiveWindow();
    char b[1024];
    snprintf(b, sizeof(b), "Title: %s\r\nEXE: %s    Class: %s    HWND: %p", cur.title.c_str(),
             cur.exe.c_str(), cur.cls.c_str(), cur.hwnd);
    SetWindowTextW(lblWin, W(b).c_str());
    std::string hits;
    for (auto& m : g_macros)
        if (m.enabled && WindowUtils::MatchesFilter(m.filter_type, m.filter_value, cur)) {
            if (!hits.empty()) hits += ", ";
            hits += m.name;
        }
    if (hits.empty()) hits = "-";
    SetWindowTextW(lblMatch, W("Macros that would fire here: " + hits).c_str());
}

void RefreshWindows() {
    wchar_t f[256] = {0};
    GetWindowTextW(g_winFilter, f, 256);
    std::string q = Native::lowerAscii(Native::w2s(f));
    auto wins = WindowUtils::ListWindows();
    g_winCache.clear();
    for (auto& w : wins) {
        if (q.empty() || Native::lowerAscii(w.title).find(q) != std::string::npos ||
            Native::lowerAscii(w.exe).find(q) != std::string::npos)
            g_winCache.push_back(w);
    }
    SendMessageW(g_winList, LB_RESETCONTENT, 0, 0);
    for (auto& w : g_winCache) {
        std::string s = w.title + "   | " + w.exe + " | " + w.cls;
        SendMessageW(g_winList, LB_ADDSTRING, 0, (LPARAM)W(s).c_str());
    }
    if (g_winCache.empty()) SendMessageW(g_winList, LB_ADDSTRING, 0, (LPARAM)L"(no windows match)");
}

// Tab pages are real windows that host child controls. WM_COMMAND /
// WM_NOTIFY from those children is delivered to the page, NOT to the main
// window, so every page forwards them up. (Without this, no button on any
// tab works: the click is simply swallowed by the page's DefWindowProc.)
LRESULT CALLBACK PageProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wParam;
            HWND ctl = (HWND)lParam;
            for (int i = 0; i < 7; i++)
                if (ctl == g_lamps[i]) {
                    SetBkColor(hdc, g_lampOn[i] ? RGB(0, 160, 0) : RGB(43, 43, 43));
                    SetTextColor(hdc, RGB(255, 255, 255));
                    return (LRESULT)(g_lampOn[i] ? g_brushOn : g_brushOff);
                }
            break;
        }
        case WM_COMMAND:
        case WM_NOTIFY:
        case WM_VSCROLL:
        case WM_HSCROLL:
            if (g_main && IsWindow(g_main)) return SendMessageW(g_main, msg, wParam, lParam);
            break;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---- main window ----
LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_APP_LOG: {
            std::string* s = reinterpret_cast<std::string*>(lParam);
            if (s) {
                if (g_log) AppendLog(g_log, *s);
                delete s;
            }
            return 0;
        }
        case WM_APP_DETECT: {
            std::string* s = reinterpret_cast<std::string*>(lParam);
            if (s) {
                OnDetectEvent(*s);
                delete s;
            }
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            int code = HIWORD(wParam);
            if (id == ID_ENGINE) {
                ToggleEngine();
            } else if (id == ID_DETECT_START) {
                ToggleDetect();
            } else if (id == ID_DETECT_CLEAR) {
                SetWindowTextW(g_testLog, L"");
            } else if (id == ID_DETECT_HID) {
                g_detectKb = SendMessageW(g_chkHid, BM_GETCHECK, 0, 0) == BST_CHECKED;
                if (g_detecting) {
                    try {
                        g_detector->Stop();
                    } catch (...) {}
                    g_detector = std::make_unique<InputHook>(DetectKey, DetectMouse);
                    g_detector->Start();
                }
            } else if (id == ID_DETECT_USE) {
                if (g_lastDetected.empty()) {
                    MessageBoxW(hwnd, L"Start listening and click a mouse button first.",
                                L"No button yet", MB_ICONINFORMATION);
                } else {
                    g_pendingTrigger = g_lastDetected;
                    TabCtrl_SetCurSel(g_tab, 2);
                    ShowWindow(g_pages[2], SW_SHOW);
                    ShowWindow(g_pages[0], SW_HIDE);
                    ShowWindow(g_pages[1], SW_HIDE);
                    Macro* m = new Macro();
                    m->name = g_lastDetected + " macro";
                    m->trigger = g_lastDetected;
                    OpenEditor(m, true);
                    delete m;
                }
            } else if (id == ID_WIN_REFRESH) {
                RefreshWindows();
            } else if (id == ID_WIN_USE) {
                int sel = (int)SendMessageW(g_winList, LB_GETCURSEL, 0, 0);
                ActiveWindow* w = nullptr;
                if (sel >= 0 && sel < (int)g_winCache.size()) w = &g_winCache[sel];
                if (!w) w = g_winCache.empty() ? nullptr : &g_winCache[0];
                if (!w) {
                    MessageBoxW(hwnd, L"Refresh the list and select a window first.", L"No window",
                                MB_ICONINFORMATION);
                } else {
                    std::string exe = w->exe;
                    Macro* m = new Macro();
                    m->name = "Macro for " + (exe.empty() ? w->title.substr(0, 20) : exe);
                    m->trigger = g_lastDetected.empty() ? "X1 (Back)" : g_lastDetected;
                    if (!exe.empty()) {
                        m->filter_type = "exe";
                        m->filter_value = exe;
                    } else {
                        m->filter_type = "title_contains";
                        m->filter_value = w->title;
                    }
                    TabCtrl_SetCurSel(g_tab, 2);
                    ShowWindow(g_pages[2], SW_SHOW);
                    ShowWindow(g_pages[0], SW_HIDE);
                    ShowWindow(g_pages[1], SW_HIDE);
                    OpenEditor(m, true);
                    delete m;
                }
            } else if (id == ID_MAC_LIST && code == LBN_DBLCLK) {
                int sel = SelectedMacro();
                if (sel >= 0) {
                    Macro tmp = g_macros[sel];
                    OpenEditor(&tmp, false);
                }
            } else if (id == ID_MAC_ADD) {
                Macro* m = new Macro();
                OpenEditor(m, true);
                delete m;
            } else if (id == ID_MAC_EDIT || id == ID_MAC_TEST || id == ID_MAC_DEL ||
                       id == ID_MAC_ENABLE) {
                int sel = SelectedMacro();
                if (sel < 0) {
                    MessageBoxW(hwnd, L"Select a macro first.", L"No selection",
                                MB_ICONINFORMATION);
                    break;
                }
                if (id == ID_MAC_EDIT) {
                    Macro tmp = g_macros[sel];
                    OpenEditor(&tmp, false);
                } else if (id == ID_MAC_TEST) {
                    Macro mm = g_macros[sel];
                    Log("Manual TEST '" + mm.name +
                        "': firing outputs now (engine can be OFF). Watch your tester - each "
                        "output clicks once.");
                    g_engine->TestFire(mm);
                } else if (id == ID_MAC_DEL) {
                    std::string q = "Delete '" + g_macros[sel].name + "'?";
                    if (MessageBoxW(hwnd, W(q).c_str(), L"Delete", MB_YESNO) == IDYES) {
                        g_macros.erase(g_macros.begin() + sel);
                        SaveAll(true);
                        g_macSig.clear();
                        RefreshMacroList();
                    }
                } else {  // ENABLE toggle
                    g_macros[sel].enabled = !g_macros[sel].enabled;
                    SaveAll(true);
                    g_macSig.clear();
                    RefreshMacroList();
                }
            } else if (id == ID_MAC_SAVE) {
                SaveAll(false);
            } else if (id == ID_MAC_RELEASE) {
                g_engine->ReleaseAllKeys();
            } else if (id == ID_MAC_VERBOSE && code == BN_CLICKED) {
                g_engine->VerboseLog =
                    SendMessageW((HWND)lParam, BM_GETCHECK, 0, 0) == BST_CHECKED;
                Log(std::string("Verbose input log ") + (g_engine->VerboseLog ? "ON." : "OFF."));
            }
            if (code == EN_CHANGE && (HWND)lParam == g_winFilter) RefreshWindows();
            return 0;
        }
        case WM_NOTIFY: {
            LPNMHDR nm = (LPNMHDR)lParam;
            if (nm->idFrom == ID_TAB && nm->code == TCN_SELCHANGE) {
                int sel = TabCtrl_GetCurSel(g_tab);
                for (int i = 0; i < 3; i++) ShowWindow(g_pages[i], i == sel ? SW_SHOW : SW_HIDE);
                if (sel == 2) RefreshMacroList();
            }
            return 0;
        }
        case WM_TIMER:
            if (wParam == IDT_WIN) {
                TickActiveWindow(g_lblWin, g_lblMatch);
            } else if (wParam == IDT_HELD) {
                RefreshMacroList();
                // The engine can also stop itself (Ctrl+F12 kill switch), so
                // keep the top bar truthful instead of only on button clicks.
                UpdateStatus();
                SetWindowTextW(g_btnEngine, (g_engine && g_engine->Running()) ? L"[] Stop macros"
                                                                             : L"> Start macros");
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            try {
                if (g_detector) g_detector->Stop();
            } catch (...) {}
            g_detector.reset();
            try {
                if (g_engine) g_engine->Stop();
            } catch (...) {}
            try {
                Models::saveMacros(g_macros);
            } catch (...) {}
            if (g_brushOn) DeleteObject(g_brushOn);
            if (g_brushOff) DeleteObject(g_brushOff);
            if (g_font) DeleteObject(g_font);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static HWND MakePage(HWND tab) {
    RECT rc;
    GetClientRect(tab, &rc);
    TabCtrl_AdjustRect(tab, FALSE, &rc);
    return CreateWindowExW(0, L"STATIC", L"", WS_CHILD, rc.left, rc.top,
                           rc.right - rc.left, rc.bottom - rc.top, tab, nullptr,
                           GetModuleHandleW(nullptr), nullptr);
}

}  // namespace

void UiLog(const std::string& m) { Log(m); }

int UiRun() {
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_TAB_CLASSES;
    InitCommonControlsEx(&icc);
    InitCommonControlsEx(&icc);
    g_font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    g_macros = Models::loadMacros();
    g_engine = std::make_unique<MacroEngine>([] { return g_macros; }, Log);
    g_brushOn = CreateSolidBrush(RGB(0, 160, 0));
    g_brushOff = CreateSolidBrush(RGB(43, 43, 43));

    WNDCLASSW wc{};
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MacroCppMain";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    // shared page class: forwards child notifications to the main window
    WNDCLASSW wct{};
    wct.lpfnWndProc = PageProc;
    wct.hInstance = GetModuleHandleW(nullptr);
    wct.lpszClassName = L"MacroCppPage";
    wct.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wct.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wct);

    HWND hwnd =
        CreateWindowExW(0, L"MacroCppMain", L"Mouse Macro (C++)",
                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT,
                        CW_USEDEFAULT, 920, 740, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    g_main = hwnd;

    // top bar
    Ctl(hwnd, L"STATIC", L"Mouse Macro (C++)", WS_VISIBLE, 12, 10, 220, 26, nullptr);
    {
        HWND t = GetWindow(hwnd, GW_CHILD);
        SendMessageW(t, WM_SETFONT,
                     (WPARAM)CreateFontW(-20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                         DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI"),
                     TRUE);
    }
    g_btnEngine = Ctl(hwnd, L"BUTTON", L"> Start macros", WS_VISIBLE | WS_TABSTOP, 748, 8, 140,
                      30, (HMENU)ID_ENGINE);
    g_lblStatus = Ctl(hwnd, L"STATIC", L"engine: OFF   -   detector: OFF",
                      WS_VISIBLE | SS_RIGHT, 460, 14, 280, 20, (HMENU)ID_STATUS);

    // tabs
    g_tab = Ctl(hwnd, L"SysTabControl32", L"", WS_VISIBLE | WS_CLIPSIBLINGS | TCS_TABS, 12, 48,
                880, 470, (HMENU)ID_TAB);
    TCITEMW ti{};
    ti.mask = TCIF_TEXT;
    ti.pszText = (LPWSTR)L"Mouse Tester";
    TabCtrl_InsertItem(g_tab, 0, &ti);
    ti.pszText = (LPWSTR)L"Windows";
    TabCtrl_InsertItem(g_tab, 1, &ti);
    ti.pszText = (LPWSTR)L"Macros";
    TabCtrl_InsertItem(g_tab, 2, &ti);

    for (int i = 0; i < 3; i++) {
        RECT rc;
        GetClientRect(g_tab, &rc);
        TabCtrl_AdjustRect(g_tab, FALSE, &rc);
        g_pages[i] = CreateWindowExW(0, L"MacroCppPage", L"",
                                     WS_CHILD | WS_CLIPCHILDREN | (i == 0 ? WS_VISIBLE : 0),
                                     rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                                     g_tab, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (g_font) SendMessageW(g_pages[i], WM_SETFONT, (WPARAM)g_font, TRUE);
    }

    // ---- tester page ----
    HWND pg = g_pages[0];
    g_btnDetect = Ctl(pg, L"BUTTON", L"Start listening", WS_VISIBLE | WS_TABSTOP, 12, 8, 120, 28,
                      (HMENU)ID_DETECT_START);
    Ctl(pg, L"BUTTON", L"Clear", WS_VISIBLE | WS_TABSTOP, 140, 8, 70, 28, (HMENU)ID_DETECT_CLEAR);
    g_chkHid = Ctl(pg, L"BUTTON", L"Also detect keyboard HID (gaming mice)",
                   WS_VISIBLE | BS_AUTOCHECKBOX, 218, 12, 300, 20, (HMENU)ID_DETECT_HID);
    SendMessageW(g_chkHid, BM_SETCHECK, BST_CHECKED, 0);
    Ctl(pg, L"BUTTON", L"Use in new macro ->", WS_VISIBLE | WS_TABSTOP, 520, 8, 150, 28,
        (HMENU)ID_DETECT_USE);
    g_lblLast = Ctl(pg, L"STATIC", L"Last: -", WS_VISIBLE, 680, 12, 180, 22, (HMENU)ID_DETECT_LAST);
    for (int i = 0; i < 7; i++) {
        g_lamps[i] = Ctl(pg, L"STATIC", W(kLampNames[i]).c_str(),
                         WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE, 12 + i * 122, 44, 116, 44,
                         (HMENU)(ID_LAMP0 + i));
    }
    g_testLog = Ctl(pg, L"EDIT", L"",
                    WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 12, 96,
                    848, 300, (HMENU)ID_TESTLOG);
    SetWindowTextW(g_testLog,
                   L"Press 'Start listening', then click any mouse button.\r\nExtra gaming buttons "
                   L"usually show up as F13..F24 or a key name - that IS your button.\r\n\r\n");

    // ---- windows page ----
    pg = g_pages[1];
    g_lblWin = Ctl(pg, L"STATIC", L"-", WS_VISIBLE, 12, 8, 848, 44, nullptr);
    g_lblMatch =
        Ctl(pg, L"STATIC", L"", WS_VISIBLE, 12, 54, 848, 20, nullptr);
    Ctl(pg, L"BUTTON", L"Refresh window list", WS_VISIBLE | WS_TABSTOP, 12, 78, 150, 28,
        (HMENU)ID_WIN_REFRESH);
    g_winFilter = Ctl(pg, L"EDIT", L"", WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 170,
                      80, 220, 24, (HMENU)ID_WIN_FILTER);
    Ctl(pg, L"BUTTON", L"Use selected as macro filter ->", WS_VISIBLE | WS_TABSTOP, 650, 78, 210,
        28, (HMENU)ID_WIN_USE);
    g_winList = Ctl(pg, L"LISTBOX", L"", WS_VISIBLE | WS_TABSTOP | WS_BORDER | WS_VSCROLL |
                                                       LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                    12, 112, 848, 300, (HMENU)ID_WIN_LIST);

    // ---- macros page ----
    pg = g_pages[2];
    Ctl(pg, L"BUTTON", L"+ Add macro", WS_VISIBLE | WS_TABSTOP, 12, 8, 100, 28, (HMENU)ID_MAC_ADD);
    Ctl(pg, L"BUTTON", L"Save", WS_VISIBLE | WS_TABSTOP, 120, 8, 70, 28, (HMENU)ID_MAC_SAVE);
    Ctl(pg, L"BUTTON", L"Release all keys", WS_VISIBLE | WS_TABSTOP, 198, 8, 130, 28,
        (HMENU)ID_MAC_RELEASE);
    Ctl(pg, L"BUTTON", L"Verbose input log", WS_VISIBLE | BS_AUTOCHECKBOX, 336, 12, 150, 20,
        (HMENU)ID_MAC_VERBOSE);
    Ctl(pg, L"STATIC", L"Macros fire only while the engine (top-right) is ON.", WS_VISIBLE, 500,
        12, 360, 20, nullptr);
    g_macList = Ctl(pg, L"LISTBOX", L"", WS_VISIBLE | WS_TABSTOP | WS_BORDER | WS_VSCROLL |
                                                     WS_HSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                    12, 44, 848, 368, (HMENU)ID_MAC_LIST);
    // row 2 buttons
    Ctl(pg, L"BUTTON", L"Edit", WS_VISIBLE | WS_TABSTOP, 12, 418 - 6, 70, 26, (HMENU)ID_MAC_EDIT);
    Ctl(pg, L"BUTTON", L"Test", WS_VISIBLE | WS_TABSTOP, 90, 418 - 6, 70, 26, (HMENU)ID_MAC_TEST);
    Ctl(pg, L"BUTTON", L"Delete", WS_VISIBLE | WS_TABSTOP, 168, 418 - 6, 70, 26, (HMENU)ID_MAC_DEL);
    Ctl(pg, L"BUTTON", L"Enable/Disable", WS_VISIBLE | WS_TABSTOP, 246, 418 - 6, 110, 26,
        (HMENU)ID_MAC_ENABLE);

    // ---- bottom log ----
    g_log = Ctl(hwnd, L"EDIT", L"", WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                                              ES_AUTOVSCROLL,
                12, 524, 880, 150, (HMENU)ID_LOG);

    RefreshMacroList();
    RefreshWindows();
    Log("Ready. Go to Mouse Tester -> Start listening, then click your mouse.");
    UpdateStatus();

    SetTimer(hwnd, IDT_WIN, 500, nullptr);
    SetTimer(hwnd, IDT_HELD, 400, nullptr);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return (int)m.wParam;
}

