#pragma once
// The in-game console's data, free of ImGui and Vulkan: scrollback with line colours, input history, Tab completion.
#include <windows.h>

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace emf::core {

enum class LineKind : uint8_t { Command, Normal, Error, Success };

// "> x" = the echoed command; usage/unknown/not run/could not/refused/failed/crashed = error; gave/spawned/raised,
// "a -> b", "is now" = success; everything else normal.
LineKind classify_reply(std::string_view line);

class ConsoleModel {
public:
    explicit ConsoleModel(size_t max_lines = 2000) : max_lines_(max_lines ? max_lines : 1) {}

    // Scrollback. add_line may be called from any thread.
    void add_line(std::string text);
    size_t size() const;
    std::vector<std::pair<LineKind, std::string>> snapshot(size_t first, size_t count) const;
    uint64_t version() const;  // +1 per added line

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
    std::deque<std::pair<LineKind, std::string>> lines_;
    uint64_t version_ = 0;
    std::vector<std::string> history_;
    size_t history_pos_ = 0;  // == history_.size(): not browsing
};

struct CompletionSources {
    std::vector<std::string> commands, items, components;
    std::vector<std::pair<std::string, std::vector<std::string>>> mod_settings;  // mod id -> its setting keys
};

// Completed lines for `input` (at most `max`, sorted, unique): the first word from commands (prefix, any case); the
// second word of give/stack from items, the third word of tpl/tplset from components (prefix matches first, then
// substring matches), the mod of settings/set from mod_settings and the key of set from that mod's keys (+ enabled).
std::vector<std::string> complete(std::string_view input, const CompletionSources& src, size_t max = 8);

}  // namespace emf::core
