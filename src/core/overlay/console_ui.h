#pragma once
// Draws the drop-down console with Dear ImGui (mockup B): top half of the screen, see-through dark, coloured lines,
// an input line with Tab completion and Up/Down history. Renderer-agnostic (no Vulkan here).
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "console_model.h"
#include "hud_model.h"
#include "imgui.h"

namespace emf::core {

struct ConsoleUiState {
    std::string input;
    float open_fraction = 0.0f;  // 0 = closed, 1 = fully down (the slide animation sets it)
    bool focus_input = true;     // focus the input line on the next frame
    bool follow = true;          // stick to the newest line
    uint64_t seen_version = 0;
    float seen_height = 0.0f;    // the scrollback's height last frame
    std::vector<std::string> completions;  // shown under the input while Tab cycles
    size_t completion_index = 0;
    int tab = 0;               // 0 = Console, 1 = Mods
    bool switch_tab = false;   // Ctrl+Tab asked for the other tab (applied on the next frame)
    std::string status;        // shown at the right end of the tab bar ("debug bridge on"); "" = none
};

// `mods_tab` (optional) draws the Mods tab's content; with it the console shows a tab bar (Console | Mods; Ctrl+Tab
// switches).
void draw_console(ConsoleModel& model, ConsoleUiState& ui, const CompletionSources& sources, ImVec2 display,
                  const std::function<void(const std::string&)>& submit,
                  const std::function<void()>& mods_tab = std::function<void()>());

// A short-lived box at the top of the screen with the replies to a command typed into the game's chat (the console
// stays closed). It shows the lines added since `from_version` until `until_ms`.
struct ToastState {
    uint64_t from_version = 0;
    uint64_t until_ms = 0;
};
using ConsoleLines = std::vector<std::pair<LineKind, std::string>>;
// The lines the toast shows at now_ms: those added since it started, at most `max` (when more came, a note saying how
// many are left out, then the newest); none once it is over.
ConsoleLines toast_lines(const ConsoleModel& model, const ToastState& toast, uint64_t now_ms, size_t max = 8);
void draw_toast(const ConsoleLines& lines, ImVec2 display);
// The mods' on-screen lines (API v12), one small window per place.
void draw_hud(const std::vector<HudLine>& lines, ImVec2 display);

}  // namespace emf::core
