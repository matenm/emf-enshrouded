#pragma once
// The one wheel mods may have: requested from any thread, opened and finished on the render thread, cancelled when the
// overlay turns off. Every accepted request is handed back exactly once (take -> finish, or turned_off), and input
// capture is switched on and off under the same lock, so it never outlives the wheel.
#include <functional>
#include <mutex>
#include <optional>

#include "core/wheel_request.h"

namespace emf::core {

class WheelGate {
public:
    enum class Refusal { None, NotReady, NoWindow, ConsoleOpen, Empty, Busy };
    // capture(true) when a request is accepted, capture(false) when it is handed back.
    explicit WheelGate(std::function<void(bool)> capture) : capture_(std::move(capture)) {}

    // Any thread. None = accepted (input is captured from now on). overlay_ready is asked under the lock, so a request
    // racing the overlay turning off is either refused or handed back by turned_off.
    Refusal request(const WheelRequest& r, const std::function<bool()>& overlay_ready, bool window_hooked,
                    bool console_open);
    Refusal request(const WheelRequest& r, bool overlay_ready, bool window_hooked, bool console_open) {
        return request(r, [overlay_ready] { return overlay_ready; }, window_hooked, console_open);
    }
    // Any thread: the game window lost the keyboard. Marks a pending or open wheel for cancelling (nothing otherwise,
    // so an old focus loss never reaches a later wheel).
    void focus_lost();
    // Render thread: true once after focus_lost hit the current wheel.
    bool take_cancel();
    // Render thread: the accepted request to open now, once.
    std::optional<WheelRequest> take();
    // before_release(request) runs under the lock just before the capture ends (finish and turned_off).
    using BeforeRelease = std::function<void(const WheelRequest&)>;
    // Render thread: the open wheel closed; its request to hand back with the choice (nothing if none was open).
    std::optional<WheelRequest> finish(const BeforeRelease& before_release = {});
    // The overlay is turning off: the pending or open request to hand back cancelled (nothing if none).
    std::optional<WheelRequest> turned_off(const BeforeRelease& before_release = {});
    bool busy() const;

private:
    enum class State { Idle, Pending, Open };
    std::optional<WheelRequest> close_locked(const BeforeRelease& before_release);
    std::function<void(bool)> capture_;
    mutable std::mutex m_;
    State state_ = State::Idle;
    bool cancel_ = false;
    WheelRequest req_;
};

}  // namespace emf::core
