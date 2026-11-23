#pragma once
// The in-game console's data, free of ImGui and Vulkan: scrollback with line colours, input history, Tab completion.
#include <windows.h>

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace emf::core {

enum class LineKind : uint8_t { Command, Normal, Error, Success };

// "> x" = the echoed command; usage/unknown/not run/could not/refused/failed/crashed = error; gave/spawned/raised,
// "a -> b", "is now" = success; everything else normal.
LineKind classify_reply(std::string_view line);

// A scrollback line with the local time of day it came (seconds since midnight).
struct ConsoleLine {
    LineKind kind = LineKind::Normal;
    std::string text;
    uint32_t seconds = 0;
};

// The console's own command: "clear" or "cls" (any case, spaces around), which empties the scrollback.
bool is_console_clear(std::string_view line);

class ConsoleModel {
public:
    explicit ConsoleModel(size_t max_lines = 2000) : max_lines_(max_lines ? max_lines : 1) {}

    // Scrollback. add_line, clear and the reads may be called from any thread.
    void add_line(std::string text);                     // at the clock's time of day
    void add_line(std::string text, uint32_t seconds);  // at `seconds` since midnight
    size_t size() const;
    std::vector<std::pair<LineKind, std::string>> snapshot(size_t first, size_t count) const;
    std::vector<ConsoleLine> lines(size_t first, size_t count) const;  // with their times
    uint64_t version() const;  // +1 per added line, and +1 per clear
    void clear();              // every line goes
    uint64_t cleared_version() const;  // the version right after the last clear (0: never cleared)
    // Lines [first, first + count) as text, each on its own line (CRLF between them), with "hh:mm:ss " in front when
    // `times` (what the console shows: its kinds are colours, not text).
    std::string text(size_t first, size_t count, bool times) const;
    // The indices of the lines that hold `needle` (any case), oldest first; none for an empty needle.
    std::vector<size_t> find_all(std::string_view needle) const;
    static std::string stamp_text(uint32_t seconds);  // "21:04:17"

    // History (render thread). Up = history_prev, Down = history_next (past the newest: "").
    void push_history(const std::string& line);
    std::string history_prev();
    std::string history_next();
    bool load_history(const std::wstring& path);
    bool save_history(const std::wstring& path) const;

private:
    static constexpr size_t kMaxHistory = 100;
    size_t max_lines_;
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    std::deque<ConsoleLine> lines_;
    uint64_t version_ = 0;
    uint64_t cleared_version_ = 0;
    std::vector<std::string> history_;
    size_t history_pos_ = 0;  // == history_.size(): not browsing
};

// A mod's setting as `set` takes it.
struct SettingSource {
    std::string key;
    std::string label;  // what the Mods tab calls it
    std::vector<std::pair<std::string, std::string>> values;  // the values it takes (on/off, choices, options), each
                                                              // with what it means ("" = the value says it)
    SettingSource() = default;
    SettingSource(const char* k) : key(k) {}  // NOLINT: a list of keys reads as settings
    SettingSource(std::string k) : key(std::move(k)) {}  // NOLINT
    bool operator==(const SettingSource&) const = default;
};

struct ModSource {
    std::string id;
    std::vector<SettingSource> settings;
    std::string name;  // what the Mods tab calls the mod
    bool operator==(const ModSource&) const = default;
};

struct CompletionSources {
    std::vector<std::string> commands, items, components;
    std::vector<std::string> command_help;  // commands[i]'s help, "<args>: what it does" (when known)
    std::vector<ModSource> mod_settings;    // the mods and their settings
    uint64_t version = 0;                   // changes whenever anything above changed (suggestions recompute then)
};

// Completed lines for `input` (at most `max`, sorted, unique): the first word from commands (prefix, any case); the
// second word of give/stack from items, the third word of tpl/tplset from components (prefix matches first, then
// substring matches), the mod of settings/set from mod_settings and the key of set from that mod's keys (+ enabled).
std::vector<std::string> complete(std::string_view input, const CompletionSources& src, size_t max = 8);

}  // namespace emf::core
