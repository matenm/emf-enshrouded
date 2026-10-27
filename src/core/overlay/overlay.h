#pragma once
// The in-game drop-down console: ties the console model and UI, the input hooks and the Vulkan drawing to the
// command center. Started by the core when [overlay] enabled is not 0 in emf\emf_core.ini.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "hud_model.h"
#include "core/commands.h"
#include "core/mod_registry.h"
#include "core/settings.h"
#include "core/wheel_request.h"
#include "core/world_tracker.h"
#include "console_model.h"
#include "imgui.h"
#include "input_router.h"

namespace emf::core {

struct OverlayConfig {
    bool enabled = true;
    uint16_t toggle_scan = 0x29;  // the key under Esc
    bool chat_commands = true;    // "/give ..." typed into the game's chat runs as a command
    float mouse_speed = 1.0f;     // the console's cursor: pixels per mouse count
};

// [overlay] enabled=0|1 (default 1), key=<scan code, decimal or 0x hex, 1-0x7F> (default 0x29), chat=0|1 (default 1),
// mouse_speed=<0.1-10> (default 1).
OverlayConfig read_overlay_config(const std::wstring& ini_path);

// The ImGui key for a console key event (ImGuiKey_None if the console does not use it).
ImGuiKey imgui_key(const KeyEvent& e);

struct ModsUiState;
// A key from the console's input: to the Mods tab when one of its key binds waits for a key (that press never reaches
// ImGui), else to ImGui (with Ctrl, Shift and Alt as modifiers). `frame` is ImGui::GetFrameCount().
void feed_key_event(ModsUiState& mods_ui, const KeyEvent& e, ImGuiIO& io, int frame);

struct OverlayDeps {
    CommandCenter* center = nullptr;
    WorldTracker* tracker = nullptr;
    std::wstring emf_dir;                                    // <game>\emf (history file)
    ReplyFn reply;                                           // where answers given at once go (the core's sink)
    std::function<std::vector<std::string>()> item_names;    // the hosted world's item names (Tab completion)
    std::function<void(const char*)> log;
    // A wheel closed: its request and the choice (-1 = cancelled). Called on the render thread.
    std::function<void(const WheelRequest&, int)> wheel_done;
    const HudModel* hud = nullptr;  // the mods' on-screen lines (API v12); drawn while the console is closed
    const ModRegistry* mods = nullptr;  // the Mods tab (null: no tab)
    SettingsStore* settings = nullptr;  // mods' settings, changed in the Mods tab
};

bool start_overlay(const OverlayConfig& config, OverlayDeps deps);
ConsoleModel* overlay_console();  // null until started
// Shows the replies that follow (a command typed into the game's chat) for `ms` at the top of the screen.
void overlay_show_replies(uint64_t ms = 6000);
// Opens a mod's radial wheel (API v10). False when the overlay is not running or cannot draw, the console is open, or
// another wheel is open. Any thread; the input is captured at once, the wheel appears on the next frame.
bool overlay_open_wheel(const WheelRequest& request);
// A line at the top of the screen for a few seconds, also kept in the console. Any thread.
void overlay_notify(const std::string& text);

// What the debug bridge's state reports about the overlay. Any thread.
struct OverlayStatus {
    bool running = false;  // started, and the Vulkan layer can draw
    bool console_open = false;
    bool wheel_open = false;
};
OverlayStatus overlay_status();
// A short status shown at the right end of the console's tab bar ("debug bridge on"). Any thread.
void overlay_set_status(const std::string& text);

}  // namespace emf::core
