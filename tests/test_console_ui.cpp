// The console UI drawn headless (no renderer): an ImGui context, a built font atlas, frames run by hand.
#include <doctest.h>

#include <string>
#include <vector>

#include "core/overlay/console_model.h"
#include "core/overlay/console_ui.h"
#include "imgui.h"
#include "imgui_internal.h"

using namespace emf::core;

namespace {
struct Headless {
    ImGuiContext* ctx;
    Headless() {
        ctx = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1920, 1080);
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    }
    ~Headless() { ImGui::DestroyContext(ctx); }
};

struct Rig {
    ConsoleModel model;
    ConsoleUiState ui;
    CompletionSources src;
    std::vector<std::string> submitted;
    ImVec2 display{1920, 1080};
    Rig() {
        src.commands = {"give", "help", "items", "time"};
        src.items = {"Material_T1_Wood_Logs"};
        ui.open_fraction = 1.0f;
    }
    void frame() {
        ImGui::NewFrame();
        draw_console(model, ui, src, display, [this](const std::string& s) { submitted.push_back(s); });
        ImGui::Render();
    }
    void type(const char* text) {
        for (const char* p = text; *p; ++p) ImGui::GetIO().AddInputCharacter(static_cast<unsigned>(*p));
        frame();
    }
    void press(ImGuiKey k) {
        ImGui::GetIO().AddKeyEvent(k, true);
        frame();
        ImGui::GetIO().AddKeyEvent(k, false);
        frame();
    }
};
// The scrollback child window (ImGui names children "<parent>/<name>_<id>").
ImGuiWindow* scrollback() {
    for (ImGuiWindow* w : GImGui->Windows)
        if (std::string(w->Name).find("##scrollback") != std::string::npos) return w;
    return nullptr;
}
}  // namespace

TEST_CASE("console UI: after the window grows and shrinks again, the scrollback still shows the newest line") {
    Headless h;
    Rig r;
    for (int i = 0; i < 60; ++i) r.model.add_line("line " + std::to_string(i));
    for (int i = 0; i < 4; ++i) r.frame();
    ImGuiWindow* w = scrollback();
    REQUIRE(w != nullptr);
    REQUIRE(w->ScrollMax.y > 0.0f);
    CHECK(w->Scroll.y == doctest::Approx(w->ScrollMax.y));  // follows the newest line
    r.display = ImVec2(1920, 6000);  // maximized on a tall screen: every line fits
    for (int i = 0; i < 4; ++i) r.frame();
    CHECK(w->ScrollMax.y == 0.0f);
    r.display = ImVec2(1920, 1080);  // restored
    for (int i = 0; i < 4; ++i) r.frame();
    REQUIRE(w->ScrollMax.y > 0.0f);
    CHECK(w->Scroll.y == doctest::Approx(w->ScrollMax.y));
}

TEST_CASE("console UI: Enter submits the line, clears the input and remembers it for Up") {
    Headless h;
    Rig r;
    r.frame();  // first frame focuses the input
    r.frame();
    r.type("help");
    CHECK(r.ui.input == "help");
    r.press(ImGuiKey_Enter);
    REQUIRE(r.submitted.size() == 1);
    CHECK(r.submitted[0] == "help");
    CHECK(r.ui.input.empty());
    r.press(ImGuiKey_UpArrow);
    CHECK(r.ui.input == "help");
}

TEST_CASE("console UI: Tab completes a command and then an item name") {
    Headless h;
    Rig r;
    r.frame();
    r.frame();
    r.type("gi");
    r.press(ImGuiKey_Tab);
    CHECK(r.ui.input == "give ");
    r.type("mat");
    r.press(ImGuiKey_Tab);
    CHECK(r.ui.input == "give Material_T1_Wood_Logs ");
}

TEST_CASE("console UI: while closed it takes no typing, and many lines draw without trouble") {
    Headless h;
    Rig r;
    r.ui.open_fraction = 0.0f;
    r.frame();
    r.type("w");
    CHECK(r.ui.input.empty());
    for (int i = 0; i < 2500; ++i) r.model.add_line(i % 7 ? "player 1 at (1.0, 2.0, 3.0) m" : "usage: give <item name> [count]");
    r.ui.open_fraction = 1.0f;
    for (int i = 0; i < 5; ++i) r.frame();
    CHECK(r.model.size() == 2000);
}

// ---- overlay glue: settings and key mapping ----
#include <windows.h>

#include "core/overlay/overlay.h"

TEST_CASE("read_overlay_config: on by default with the key under Esc; enabled=0 and key= are read") {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    const std::wstring ini = std::wstring(dir) + L"emf_test_overlay_" + std::to_wstring(GetCurrentProcessId()) + L".ini";
    DeleteFileW(ini.c_str());
    OverlayConfig c = read_overlay_config(ini);
    CHECK(c.enabled);
    CHECK(c.toggle_scan == 0x29);
    CHECK(c.chat_commands);
    WritePrivateProfileStringW(L"overlay", L"enabled", L"0", ini.c_str());
    WritePrivateProfileStringW(L"overlay", L"key", L"0x44", ini.c_str());
    WritePrivateProfileStringW(L"overlay", L"chat", L"0", ini.c_str());
    c = read_overlay_config(ini);
    CHECK_FALSE(c.enabled);
    CHECK(c.toggle_scan == 0x44);
    CHECK_FALSE(c.chat_commands);
    WritePrivateProfileStringW(L"overlay", L"key", L"999", ini.c_str());  // out of range: default
    CHECK(read_overlay_config(ini).toggle_scan == 0x29);
    DeleteFileW(ini.c_str());
}

TEST_CASE("toast: shows the lines added since it started, newest last, until it ends") {
    ConsoleModel m;
    m.add_line("old 1");
    m.add_line("old 2");
    const ToastState t{m.version(), 6000};
    CHECK(toast_lines(m, t, 100).empty());  // nothing new yet
    m.add_line("> give Material_T1_Wood_Logs 5");
    m.add_line("gave 5 x Material_T1_Wood_Logs to player 1");
    auto lines = toast_lines(m, t, 100);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].first == LineKind::Command);
    CHECK(lines[1].first == LineKind::Success);
    CHECK(lines[1].second == "gave 5 x Material_T1_Wood_Logs to player 1");
    CHECK(toast_lines(m, t, 6000).empty());  // over
    CHECK(toast_lines(m, ToastState{}, 100).empty());  // never started
}

TEST_CASE("toast: many new lines show only the newest few; a scrollback that dropped lines is fine") {
    ConsoleModel m(5);
    const ToastState t{m.version(), 1000};
    for (int i = 0; i < 20; ++i) m.add_line("line " + std::to_string(i));
    auto lines = toast_lines(m, t, 0, 4);
    REQUIRE(lines.size() == 4);
    CHECK(lines[0].second == "... 17 more lines in the console");
    CHECK(lines[1].second == "line 17");
    CHECK(lines[3].second == "line 19");
    CHECK(toast_lines(m, t, 0, 10).size() == 6);  // only 5 lines are kept: the note and those 5
}

TEST_CASE("toast: a long reply (like /help) says how many lines it does not show") {
    ConsoleModel m;
    const ToastState t{m.version(), 1000};
    m.add_line("> help");
    for (int i = 0; i < 12; ++i) m.add_line("command " + std::to_string(i));
    auto lines = toast_lines(m, t, 0, 6);
    REQUIRE(lines.size() == 6);
    CHECK(lines[0].second == "... 8 more lines in the console");
    CHECK(lines[1].second == "command 7");
    CHECK(lines[5].second == "command 11");
    CHECK(toast_lines(m, t, 0, 13).size() == 13);  // all fit: no note
}

TEST_CASE("toast: draws headless without trouble") {
    Headless h;
    ConsoleModel m;
    const ToastState t{m.version(), 1000};
    m.add_line("> time 12");
    m.add_line("time of day is now 12.00 h (was 3.00 h)");
    ImGui::NewFrame();
    draw_toast(toast_lines(m, t, 0), ImVec2(1920, 1080));
    ImGui::Render();
    bool found = false;
    for (ImGuiWindow* w : GImGui->Windows) found |= std::string(w->Name).find("##emf_toast") != std::string::npos;
    CHECK(found);
}

TEST_CASE("imgui_key: the keys the console uses") {
    CHECK(imgui_key(KeyEvent{0x0F, VK_TAB, true, false}) == ImGuiKey_Tab);
    CHECK(imgui_key(KeyEvent{0x1C, VK_RETURN, true, false}) == ImGuiKey_Enter);
    CHECK(imgui_key(KeyEvent{0x1C, VK_RETURN, true, true}) == ImGuiKey_KeypadEnter);
    CHECK(imgui_key(KeyEvent{0x48, VK_UP, true, true}) == ImGuiKey_UpArrow);
    CHECK(imgui_key(KeyEvent{0x1D, VK_CONTROL, true, false}) == ImGuiKey_LeftCtrl);
    CHECK(imgui_key(KeyEvent{0x2F, 'V', true, false}) == ImGuiKey_V);
    CHECK(imgui_key(KeyEvent{0x1E, 0, true, false}) == ImGuiKey_A);  // scan code only
    CHECK(imgui_key(KeyEvent{0x3B, VK_F1, true, false}) == ImGuiKey_None);
}

TEST_CASE("console UI: after a click on the scrollback, typing still goes to the input line") {
    Headless h;
    Rig r;
    for (int i = 0; i < 5; ++i) r.model.add_line("line " + std::to_string(i));
    r.frame();
    r.frame();
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(400, 100);  // over the scrollback
    r.frame();
    io.AddMouseButtonEvent(0, true);
    r.frame();
    io.AddMouseButtonEvent(0, false);
    r.frame();
    r.frame();
    r.type("help");
    CHECK(r.ui.input == "help");
}
