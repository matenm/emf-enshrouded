#pragma once
// The HUD page of the console's Settings tab: the gate's message, the layout bar, the list of HUD parts and the selected
// part's offset, scale, hide and reset, with keys. Drawn headless in the tests.
#include <cstddef>
#include <cstdint>
#include <string>

#include "core/hud_editor/editor.h"
#include "imgui.h"

namespace emf::core {

struct HudPageState {
    size_t selected = 0;
    std::string save_name;
    uint64_t confirm_delete_ms = 0, confirm_hide_ms = 0;  // a first click waits for a second within 3 s
    std::string message;
    bool message_error = false;
    // mouse picking and dragging (hud_pick.h)
    int dragging = -1;
    ImVec2 drag_start_mouse{0, 0};
    float drag_start_x = 0, drag_start_y = 0;
};

// Draws the page into the current window. `display` is the game's screen in pixels.
void draw_hud_page(hud::HudEditor& editor, HudPageState& st, ImVec2 display);

// The colour a picker on a bar's card shows (0xRRGGBBAA): the player's (a colour without alpha shows the game's alpha
// for that part when known, else 0xff), else the game's for that part, else white (the bar was not drawn yet).
uint32_t page_colour(const hud::HudView& v, int bar, hud::BarPart part);
// The panel's picker: the set panel colour, else health's frame colour from the game, else 0x000000B0.
uint32_t page_panel_colour(const hud::HudView& v);
// The panel switch works when the panel colour does, and always to turn a set colour off.
bool panel_switch_enabled(const hud::HudView& v);
// Whether a part's offset and Hidden do anything: a bar's only while the bars move on their own (bars_work).
bool part_moves(const hud::HudView& v, size_t part);
// The keys line under a part's card: only the keys that do something for that part (a bar has no scale of its own).
std::string page_keys_line(const hud::HudView& v, size_t part);
// A picker's floats (0..1, clamped) as a colour with alpha.
hud::Colour picked(const float rgba[4]);
// The set colours the page has no button for, as "end #00ff00, heal #0000ff80"; "" for none.
std::string console_only_colours(const hud::BarColours& c);

enum class PageToggle { Open, Show, Close, Refuse };
// What the HUD page key does: `busy` is a mod's wheel or window holding the input.
PageToggle page_toggle(bool console_open, bool showing_page, bool busy);

}  // namespace emf::core
