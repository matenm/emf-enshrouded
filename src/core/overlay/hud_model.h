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
