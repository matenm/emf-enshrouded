#pragma once
// A one-way gate between threads. EMF opens it when its start-up has loaded the mods. The dedicated server sets up its
// world while that start-up is still running (a game client shows its menu first), so the world's setup waits here:
// otherwise mods' programs and event subscriptions are refused because the world already exists.
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace emf::core {

class ReadyGate {
public:
    // GaveUp: an earlier wait timed out (the opener seems stuck), so this one returns at once instead of waiting the
    // whole time again.
    enum class Wait { AlreadyOpen, Opened, TimedOut, OpenerThread, GaveUp };

    // The thread that will open the gate: a wait from it would wait for itself, so it returns at once.
    void set_opener(std::thread::id id) {
        std::lock_guard<std::mutex> l(m_);
        opener_ = id;
    }
    void open() {
        {
            std::lock_guard<std::mutex> l(m_);
            open_ = true;
        }
        cv_.notify_all();
    }
    bool is_open() const {
        std::lock_guard<std::mutex> l(m_);
        return open_;
    }
    Wait wait(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> l(m_);
        if (open_) return Wait::AlreadyOpen;
        if (opener_ == std::this_thread::get_id()) return Wait::OpenerThread;
        if (gave_up_) return Wait::GaveUp;
        if (cv_.wait_for(l, timeout, [&] { return open_; })) return Wait::Opened;
        gave_up_ = true;
        return Wait::TimedOut;
    }

private:
    mutable std::mutex m_;
    std::condition_variable cv_;
    bool open_ = false;
    bool gave_up_ = false;  // a wait timed out: later waits do not wait
    std::thread::id opener_;
};

}  // namespace emf::core
