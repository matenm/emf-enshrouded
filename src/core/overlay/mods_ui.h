#pragma once
// The console's Mods tab: every mod the loader found (name, version, status), and the selected mod's switch and
// settings as widgets: switches, sliders, number boxes, lists, colours, key binds and text, under their group headings,
// greyed out while their "enabledWhen" does not hold. Drawing is checked in game; the text, the edit path and the key
// capture are unit-tested.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/mod_registry.h"
#include "core/overlay/input_router.h"
#include "core/settings.h"
#include "imgui.h"

namespace emf::core {

struct PendingEdit {
    std::string value;  // a slider's, number box's or colour's value while it is dragged or typed
    int frame = 0;      // the last ImGui frame its widget was drawn in
};

struct ModsUiState {
    std::string selected;               // the selected mod's source (folder or file: unique, ids may repeat)
    std::string message;                // the last change's result, shown under the settings
    bool message_error = false;
    std::map<std::string, PendingEdit> pending;  // "mod.key" -> an unsaved edit
    // The selected mod's switch as last read from its ini (see mod_switch).
    std::string switch_mod;
    bool switch_on = true;
    uint64_t switch_read_ms = 0;
    uint64_t switch_version = 0;
    // A key bind waiting for a key: "mod.key" of the widget (empty: none), the key's name once pressed, and the last
    // frame the widget was drawn in (a capture whose widget is gone is dropped).
    std::string capturing;
    std::string captured;
    int capture_frame = 0;
    std::string capture_modifier;  // Shift, Ctrl or Alt held while waiting: bound if released alone
};

// The list row's text and ImGui id: "Clock  1.1.0##clock\" (keyed by folder, so a copied folder with the same id
// never shares an id with the original).
std::string mod_row_label(const ModRecord& r);
// "one of: a, b", "a whole number, 2 to 50", "on or off"...
std::string describe_setting(const SettingDef& d);
// The widget a number setting gets: a slider when it has min and max (a whole-number one only when they are at most
// 1,000,000 apart, unless it asked to be a slider), else a box; whole numbers as 64-bit values (no overflow).
enum class NumberWidget { SliderInt, SliderDouble, InputInt, InputDouble };
NumberWidget number_widget(const SettingDef& d);
ImVec4 status_colour(ModStatus s);
// A widget changed a setting: saves it through the store and sets ui.message. False when refused.
bool apply_setting_edit(SettingsStore& store, const std::string& mod, const std::string& key, const std::string& value,
                        ModsUiState& ui);
// An edit in progress of widget `full` ("mod.key"), or nullptr; marks the widget as drawn in `frame`.
const std::string* pending_edit(ModsUiState& ui, const std::string& full, int frame);
void set_pending_edit(ModsUiState& ui, const std::string& full, std::string value, int frame);
// Drops edits whose widget was not drawn in the frame before `frame` (the tab was left, the console closed or another
// mod selected while dragging or typing): the saved value shows again instead of a stale one. Call before drawing.
// A key capture whose widget was not drawn is dropped the same way.
void forget_stale_edits(ModsUiState& ui, int frame);
// The mod's on/off switch as the tab shows it: read from the mod's ini when the selection or the store changed, else at
// most once a second (the tab draws every frame; the file can also change outside the game).
bool mod_switch(SettingsStore& store, const ModRecord& mod, ModsUiState& ui, uint64_t now_ms);
// A key from the console's input while a key bind waits for one: true when the tab took the press (it then does not
// reach ImGui; releases always do). A press is named as EMF's key settings read it ("G", "F7", "Shift", "0xba"); Esc
// cancels. Shift, Ctrl and Alt bind only when released alone (Shift+G binds G); the IME's key never binds. `frame` is the last ImGui frame (ImGui::GetFrameCount()): a capture whose widget was not drawn in it or the
// one before (another tab, the console closed) is dropped and takes nothing.
bool capture_key_event(ModsUiState& ui, const KeyEvent& e, int frame);
// The name a key setting stores for a key press ("" for a key that cannot be bound).
std::string key_event_name(const KeyEvent& e);
// "#rrggbb" as 0..1 floats and back (false for anything else).
bool color_to_floats(const std::string& hex, float rgb[3]);
std::string floats_to_color(const float rgb[3]);
// Draws the tab's content into the current window (the console's).
void draw_mods_tab(const std::vector<ModRecord>& mods, SettingsStore* settings, ModsUiState& ui);

}  // namespace emf::core
