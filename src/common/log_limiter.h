#pragma once
// Repeated log lines: each distinct line is written the first time, then at most once per window, with the number of
// repeats that stayed quiet in between. Not thread-safe (callers hold their own lock).
#include <cstdint>
#include <string>
#include <unordered_map>

namespace emf {

class LogLimiter {
public:
    explicit LogLimiter(uint64_t window_ms = 10000) : window_ms_(window_ms) {}

    // The text to log for `line` now, or "" to stay quiet.
    std::string pass(const std::string& line, uint64_t now_ms) {
        auto it = seen_.find(line);
        if (it == seen_.end()) {
            if (seen_.size() >= kMaxLines) seen_.clear();  // a flood of distinct lines: start over
            seen_[line] = Entry{now_ms, 0};
            return line;
        }
        Entry& e = it->second;
        if (now_ms - e.last_ms < window_ms_) {
            ++e.skipped;
            return {};
        }
        std::string out = line;
        if (e.skipped) out += " (" + std::to_string(e.skipped) + " more since the last one)";
        e = Entry{now_ms, 0};
        return out;
    }

private:
    static constexpr size_t kMaxLines = 256;
    struct Entry {
        uint64_t last_ms;
        uint64_t skipped;
    };
    uint64_t window_ms_;
    std::unordered_map<std::string, Entry> seen_;
};

}  // namespace emf
