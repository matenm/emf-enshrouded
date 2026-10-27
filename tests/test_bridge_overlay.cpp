// The console shows "debug bridge on" at the right end of its tab bar while the bridge is on (drawn headless).
#include <doctest.h>

#include <string>
#include <vector>

#include "core/overlay/console_model.h"
#include "core/overlay/console_ui.h"
#include "imgui.h"
#include "imgui_internal.h"

using namespace emf::core;

namespace {
std::vector<std::string> tab_labels() {
    std::vector<std::string> out;
    for (int i = 0; i < GImGui->TabBars.GetMapSize(); ++i)
        if (ImGuiTabBar* bar = GImGui->TabBars.TryGetMapData(i))
            for (auto& tab : bar->Tabs) out.emplace_back(ImGui::TabBarGetTabName(bar, &tab));
    return out;
}
bool has(const std::vector<std::string>& v, const std::string& s) {
    for (const auto& x : v)
        if (x == s) return true;
    return false;
}
}  // namespace

TEST_CASE("console UI: the tab bar shows the status text on the right, and nothing without one") {
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1920, 1080);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    ConsoleModel model;
    ConsoleUiState ui;
    ui.open_fraction = 1.0f;
    CompletionSources src;
    auto frame = [&] {
        ImGui::NewFrame();
        draw_console(model, ui, src, ImVec2(1920, 1080), [](const std::string&) {}, [] {});
        ImGui::Render();
    };
    frame();
    frame();
    CHECK(has(tab_labels(), "Console"));
    CHECK_FALSE(has(tab_labels(), "debug bridge on"));
    ui.status = "debug bridge on";
    frame();
    frame();
    CHECK(has(tab_labels(), "debug bridge on"));
    CHECK(ui.tab == 0);  // a label, not a tab: the console tab stays selected
    ImGui::DestroyContext(ctx);
}
