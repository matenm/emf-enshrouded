#pragma once
// The text lines mods keep on screen (API v12 hud_set): an id per line, where it sits, its colour. No drawing here
// (unit-tested); the overlay draws lines() every frame. Thread-safe: mods set lines from any thread.
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace emf::core {

enum class HudPlace : uint8_t { TopRight = 0, BottomRight = 1, BottomLeft = 2 };

struct HudLine {
    std::string id, text;
    HudPlace place = HudPlace::TopRight;
    uint32_t rgba = 0;  // 0xRRGGBBAA; 0 = the overlay's default text colour
};

// Whether the lines are drawn: some exist, the console is closed, and a world runs (on the main menu and loading
// screens the last values would be stale; the lines are kept and come back with the next world).
inline bool show_hud(bool console_open, bool any_lines, bool world_live) {
    return !console_open && any_lines && world_live;
}

// Whether the overlay draws this frame (else the game's frame goes without EMF's pass, and nothing of EMF's shows).
struct OverlayDue {
    bool console = false;     // open, or still sliding
    bool toast = false;       // a note is up
    bool busy = false;        // a wheel or a mod window
    bool hud_lines = false;   // the mods' HUD has lines
    bool world_live = false;  // a world runs
    bool map_open = false;    // the game's map screen: map markers, the hold ring, map clicks
    bool world_ui = false;    // the game's play HUD: canvases and 3D markers show there
    bool canvases = false;    // a mod canvas exists
    bool markers = false;     // a marker exists
};
// Canvases and markers wake it too: with no HUD line (the clock off) a minimap was drawn only on the frame the
// console finished closing.
inline bool overlay_due(const OverlayDue& d) {
    if (d.console || d.toast || d.busy) return true;
    if (!d.world_live) return false;
    return d.hud_lines || d.map_open || (d.world_ui && (d.canvases || d.markers));
}

class HudModel {
public:
    static constexpr size_t kMaxLines = 32, kMaxId = 48, kMaxText = 160;
    enum class Result { Ok, Removed, BadId, BadPlace, Full };

    // Sets (or replaces) line `id`; empty text removes it. id: 1-48 of A-Z a-z 0-9 _ . - ; place 0-2. Text longer than
    // kMaxText bytes is cut on a UTF-8 character boundary.
    Result set(const std::string& id, const std::string& text, uint32_t place, uint32_t rgba);
    // Every line, by place, then id.
    std::vector<HudLine> lines() const;
    bool empty() const;
    uint64_t version() const;  // changes whenever a line changes

private:
    mutable std::mutex m_;
    std::map<std::string, HudLine> lines_;
    uint64_t version_ = 0;
};

}  // namespace emf::core
