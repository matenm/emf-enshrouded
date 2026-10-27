#pragma once
// The last N log lines in memory, each with a sequence number (1, 2, 3, ... for the process), for the debug bridge's
// `log` and `wait`. A push takes one short lock; readers copy the lines they want.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "srw.h"

namespace emf {

class LogRing {
public:
    struct Line {
        uint64_t seq = 0;
        std::string text;
    };

    explicit LogRing(size_t capacity = 5000) : lines_(capacity ? capacity : 1) {}
    LogRing(const LogRing&) = delete;
    LogRing& operator=(const LogRing&) = delete;

    // Keeps `text` as the newest line; returns its sequence number. Signals the notify event, if one is set.
    uint64_t push(std::string text) {
        uint64_t seq = 0;
        {
            const SrwExclusive lock(lock_);
            seq = ++last_;
            Line& slot = lines_[(seq - 1) % lines_.size()];
            slot.seq = seq;
            slot.text = std::move(text);
        }
        if (HANDLE e = notify_.load(std::memory_order_acquire)) SetEvent(e);
        return seq;
    }

    // The newest line's number (0 before the first line).
    uint64_t last_seq() const {
        const SrwShared lock(lock_);
        return last_;
    }
    // The oldest line still kept (last_seq() + 1 while empty).
    uint64_t first_seq() const {
        const SrwShared lock(lock_);
        return first_locked();
    }
    size_t capacity() const { return lines_.size(); }

    // The lines numbered after `since`, oldest first: at most `max_lines`, and no more text than `max_bytes` (but always
    // at least one line). *more (optional) says whether newer lines were left out.
    std::vector<Line> after(uint64_t since, size_t max_lines, size_t max_bytes, bool* more = nullptr) const {
        std::vector<Line> out;
        const SrwShared lock(lock_);
        uint64_t seq = (std::max)(since + 1, first_locked());
        size_t bytes = 0;
        for (; seq <= last_ && out.size() < max_lines; ++seq) {
            const Line& l = lines_[(seq - 1) % lines_.size()];
            if (!out.empty() && bytes + l.text.size() > max_bytes) break;
            bytes += l.text.size();
            out.push_back(l);
        }
        if (more) *more = seq <= last_;
        return out;
    }

    // The newest `n` lines, oldest first.
    std::vector<Line> tail(size_t n) const {
        uint64_t since = 0;
        {
            const SrwShared lock(lock_);
            since = last_ > n ? last_ - n : 0;
        }
        return after(since, n, SIZE_MAX);
    }

    // An event to signal after each push (the bridge's wake-up), or nullptr.
    void set_notify(HANDLE event) { notify_.store(event, std::memory_order_release); }

private:
    uint64_t first_locked() const { return last_ >= lines_.size() ? last_ - lines_.size() + 1 : 1; }

    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    std::vector<Line> lines_;
    uint64_t last_ = 0;
    std::atomic<HANDLE> notify_{nullptr};
};

}  // namespace emf
