#pragma once
// The in-game drop-down console: ties the console model and UI, the input hooks and the Vulkan drawing to the
// command center. Started by the core when [overlay] enabled is not 0 in emf\emf_core.ini.
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "hud_model.h"
#include "core/commands.h"
#include "core/mod_registry.h"
#include "core/settings.h"
#include "core/wheel_request.h"
#include "core/window_request.h"
#include "core/world_tracker.h"
#include "console_model.h"
#include "imgui.h"
#include "input_router.h"
#include "look_settings.h"

namespace emf::core {

namespace hud {
class HudEditor;
}
class MarkerStore;
class CanvasStore;
class FogStore;

constexpr size_t kMinScrollback = 500, kMaxScrollback = 20000;  // [overlay] scrollback's range

struct OverlayConfig {
    bool enabled = true;
    uint16_t toggle_scan = 0x29;  // the key under Esc
    bool chat_commands = true;    // "/give ..." typed into the game's chat runs as a command
    float mouse_speed = 1.0f;     // the console's cursor: pixels per mouse count
    bool suggest = true;          // suggestions as you type in the console (off: Tab completes as before)
    size_t suggest_rows = 8;      // at most this many suggestions (3-12)
    bool timestamps = false;      // the console shows each line's time ("21:04:17")
    size_t scrollback = 2000;     // lines the console keeps (500-20000; from the next start)
    LookSettings look;            // [hud] and the look keys of [overlay] (look_settings.h)
    std::vector<std::string> look_problems;  // values in them that could not be used, one line each (for the log)
    bool debug_keys = false;  // [overlay] debug_keys=1: the first console keys go to the log (KeyDebugLog), for reports
};

// [overlay] enabled=0|1 (default 1), key=<scan code, decimal or 0x hex, 1-0x7F> (default 0x29), chat=0|1 (default 1),
// mouse_speed=<0.1-10> (default 1), suggest=0|1 (default 1), suggest_rows=<3-12> (default 8; more or fewer are kept
// within 3-12), timestamps=0|1 (default 0), scrollback=<500-20000> (default 2000; kept within), debug_keys=0|1 (default
// 0), and the look (read_look_settings). A "; comment" after a value on the same line is not part of it.
OverlayConfig read_overlay_config(const std::wstring& ini_path);

// The config with what the store holds for EMF's own settings (the Settings tab's values, one parser): the look, the
// key, chat, mouse speed and suggestions; `enabled` stays the ini's. Unchanged when the store has no EMF settings.
OverlayConfig overlay_config_with_store(OverlayConfig c, const SettingsStore& s);

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
    // A mod window's event (API v24): EMF_UI_CLICK / _RIGHT_CLICK with the widget's key, or EMF_UI_CLOSED with the
    // reason. Render thread (or the caller's, for ui.click and window_close).
    std::function<void(const WindowRequest&, uint32_t kind, const std::string& key, uint32_t reason)> window_event;
    const HudModel* hud = nullptr;  // the mods' on-screen lines (API v12); drawn while the console is closed
    const ModRegistry* mods = nullptr;  // the Mods tab (null: no tab)
    SettingsStore* settings = nullptr;  // mods' settings, changed in the Mods tab
    hud::HudEditor* hud_editor = nullptr;  // the HUD page of the Settings tab (null: the page says it is not available)
    const MarkerStore* markers = nullptr;  // markers (API v33): drawn over the 3D view and the map (null: none)
    const CanvasStore* canvases = nullptr;  // canvases (API v35): mods' drawing panels (null: none)
    const FogStore* fog = nullptr;          // the fog of war for the canvases' FOG command (API v35)
};

bool start_overlay(const OverlayConfig& config, OverlayDeps deps);
// Opens (or closes) the console on a Settings page, as the HUD page key does: closed gives open on that page; open on it
// gives closed; open elsewhere gives that page. Any thread; false when the overlay cannot draw or a mod's wheel or
// window holds the input.
bool overlay_toggle_settings_page(const char* page);
ConsoleModel* overlay_console();  // null until started
// Shows the replies that follow (a command typed into the game's chat) for `ms` (0: [hud] notes_seconds) where notes
// show.
void overlay_show_replies(uint64_t ms = 0);
// Opens a mod's radial wheel (API v10). False when the overlay is not running or cannot draw, the console is open, or
// another wheel is open. Any thread; the input is captured at once, the wheel appears on the next frame.
bool overlay_open_wheel(const WheelRequest& request);
// A mod's window (API v24), any thread. Open: false with *why when refused (the overlay cannot draw, the window is not
// hooked, a wheel or another window is open); `replaced`: the window with the same id it replaced. Update: false when
// that id is not open. Close: false when not open; `closed`: the window closed.
bool overlay_open_window(WindowRequest r, std::optional<WindowRequest>* replaced, std::string* why);
bool overlay_update_window(const WindowRequest& contents);
bool overlay_close_window(const std::string& id, std::optional<WindowRequest>* closed);
// The open window (ui.windows), nothing when none.
std::optional<WindowRequest> overlay_window();
// ui.click: the same event as a click on the widget `key` of the open window. False with *why when there is none.
bool overlay_click_window(const std::string& key, bool right, std::string* why);
// ui.close: the window closed as by the player (CLOSED, BY_PLAYER). False when none is open.
bool overlay_close_window_by_player();
// A note for [hud] notes_seconds where [hud] notes_place says (top centre by default), also kept in the console. Any
// thread.
void overlay_notify(const std::string& text);
// The pictures mods loaded (API v26 image_load): decoded now, uploaded by the render thread before its next frame.
// Any thread.
uint32_t overlay_load_image(const std::wstring& path, const std::string& key, std::string* why);
class ImageStore;
ImageStore& overlay_images();

// What the debug bridge's state reports about the overlay. Any thread.
struct OverlayStatus {
    bool running = false;  // started, and the Vulkan layer can draw
    bool console_open = false;
    bool wheel_open = false;
    std::string window;  // the id of a mod's open window (API v24), "" when none
};
OverlayStatus overlay_status();
// A short status shown at the right end of the console's tab bar ("debug bridge on"). Any thread.
void overlay_set_status(const std::string& text);

}  // namespace emf::core
