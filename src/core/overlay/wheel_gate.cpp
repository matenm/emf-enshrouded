#include "wheel_gate.h"

namespace emf::core {

WheelGate::Refusal WheelGate::request(const WheelRequest& r, const std::function<bool()>& overlay_ready,
                                      bool window_hooked, bool console_open) {
    std::lock_guard<std::mutex> l(m_);
    if (state_ != State::Idle) return Refusal::Busy;
    if (!overlay_ready || !overlay_ready()) return Refusal::NotReady;
    if (!window_hooked) return Refusal::NoWindow;  // no keyboard or mouse for it: it could not be closed
    if (console_open) return Refusal::ConsoleOpen;
    if (r.items.empty()) return Refusal::Empty;
    req_ = r;
    state_ = State::Pending;
    cancel_ = false;
    if (capture_) capture_(true);  // now: the hold key's release may come before the next frame
    return Refusal::None;
}

std::optional<WheelRequest> WheelGate::take() {
    std::lock_guard<std::mutex> l(m_);
    if (state_ != State::Pending) return std::nullopt;
    state_ = State::Open;
    return req_;
}

std::optional<WheelRequest> WheelGate::close_locked(const BeforeRelease& before_release) {
    state_ = State::Idle;
    if (before_release) before_release(req_);
    if (capture_) capture_(false);
    std::optional<WheelRequest> out = std::move(req_);
    req_ = WheelRequest{};
    return out;
}

std::optional<WheelRequest> WheelGate::finish(const BeforeRelease& before_release) {
    std::lock_guard<std::mutex> l(m_);
    if (state_ != State::Open) return std::nullopt;
    return close_locked(before_release);
}

std::optional<WheelRequest> WheelGate::turned_off(const BeforeRelease& before_release) {
    std::lock_guard<std::mutex> l(m_);
    if (state_ == State::Idle) return std::nullopt;
    return close_locked(before_release);
}

void WheelGate::focus_lost() {
    std::lock_guard<std::mutex> l(m_);
    if (state_ != State::Idle) cancel_ = true;
}

bool WheelGate::take_cancel() {
    std::lock_guard<std::mutex> l(m_);
    const bool c = cancel_;
    cancel_ = false;
    return c;
}

bool WheelGate::busy() const {
    std::lock_guard<std::mutex> l(m_);
    return state_ != State::Idle;
}

}  // namespace emf::core
