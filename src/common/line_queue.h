#pragma once
// A bounded queue of text lines between threads (the dedicated server's console writer): past the limit the oldest
// lines go, so a writer that is stuck (a paused console window) never makes the threads that log wait.
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace emf {

class LineQueue {
public:
    explicit LineQueue(size_t max) : max_(max) {}
    void push(std::string line) {
        std::lock_guard<std::mutex> l(m_);
        lines_.push_back(std::move(line));
        while (lines_.size() > max_) {
            lines_.pop_front();
            ++dropped_;
        }
    }
    struct Batch {
        std::vector<std::string> lines;
        size_t dropped = 0;  // lines that went since the last take
    };
    Batch take() {
        std::lock_guard<std::mutex> l(m_);
        Batch b;
        b.lines.assign(std::make_move_iterator(lines_.begin()), std::make_move_iterator(lines_.end()));
        lines_.clear();
        b.dropped = dropped_;
        dropped_ = 0;
        return b;
    }

private:
    std::mutex m_;
    std::deque<std::string> lines_;
    size_t max_;
    size_t dropped_ = 0;
};

}  // namespace emf
