// The one wheel mods may have open: requests from any thread, opened and finished on the render thread, cancelled when
// the overlay turns off. Each accepted request is handed back exactly once and input capture never outlives it.
#include <doctest.h>

#include <vector>

#include "core/overlay/wheel_gate.h"

using emf::core::WheelGate;
using emf::core::WheelRequest;

namespace {
WheelRequest req(const char* title) {
    WheelRequest r;
    r.title = title;
    r.items = {"a", "b"};
    return r;
}
struct Capture {
    bool on = false;
    int changes = 0;
};
}  // namespace

TEST_CASE("wheel gate: one wheel at a time; refused while the overlay cannot take it") {
    Capture cap;
    WheelGate g([&cap](bool on) { cap.on = on; ++cap.changes; });
    CHECK(g.request(req("x"), false, true, false) == WheelGate::Refusal::NotReady);
    CHECK(g.request(req("x"), true, false, false) == WheelGate::Refusal::NoWindow);
    CHECK(g.request(req("x"), true, true, true) == WheelGate::Refusal::ConsoleOpen);
    CHECK(g.request(WheelRequest{}, true, true, false) == WheelGate::Refusal::Empty);
    CHECK_FALSE(cap.on);
    CHECK(g.request(req("x"), true, true, false) == WheelGate::Refusal::None);
    CHECK(cap.on);  // captured from the request on
    CHECK(g.busy());
    CHECK(g.request(req("y"), true, true, false) == WheelGate::Refusal::Busy);
    auto t = g.take();
    REQUIRE(t);
    CHECK(t->title == "x");
    CHECK_FALSE(g.take());  // taken once
    auto f = g.finish();
    REQUIRE(f);
    CHECK(f->title == "x");
    CHECK_FALSE(cap.on);
    CHECK_FALSE(g.finish());  // finished once
    CHECK_FALSE(g.busy());
}

TEST_CASE("wheel gate: the overlay turning off finishes a pending or open wheel exactly once") {
    Capture cap;
    WheelGate g([&cap](bool on) { cap.on = on; ++cap.changes; });
    REQUIRE(g.request(req("pending"), true, true, false) == WheelGate::Refusal::None);
    auto off = g.turned_off();  // before the render thread took it
    REQUIRE(off);
    CHECK(off->title == "pending");
    CHECK_FALSE(cap.on);
    CHECK_FALSE(g.take());
    CHECK_FALSE(g.turned_off());
    CHECK_FALSE(g.finish());

    REQUIRE(g.request(req("open"), true, true, false) == WheelGate::Refusal::None);
    REQUIRE(g.take());
    off = g.turned_off();
    REQUIRE(off);
    CHECK(off->title == "open");
    CHECK_FALSE(g.finish());  // the render thread's own close after that hands nothing back again
    CHECK_FALSE(cap.on);
}

TEST_CASE("wheel gate: a focus loss cancels only a wheel that is pending or open, not the next one") {
    WheelGate g([](bool) {});
    g.focus_lost();  // alt-tab during normal play: no wheel
    REQUIRE(g.request(req("stop"), true, true, false) == WheelGate::Refusal::None);
    REQUIRE(g.take());
    CHECK_FALSE(g.take_cancel());  // the earlier focus loss does not reach this wheel
    g.focus_lost();                // alt-tab while it is open
    CHECK(g.take_cancel());
    CHECK_FALSE(g.take_cancel());  // once
    REQUIRE(g.finish());
    REQUIRE(g.request(req("next"), true, true, false) == WheelGate::Refusal::None);
    g.focus_lost();  // while pending
    REQUIRE(g.take());
    CHECK(g.take_cancel());
}

TEST_CASE("wheel gate: readiness is judged under the lock, so a request after the overlay turned off is refused") {
    bool ready = true;
    Capture cap;
    WheelGate g([&cap](bool on) { cap.on = on; });
    ready = false;  // drawing failed: the overlay is turning off
    CHECK(g.request(req("late"), [&ready] { return ready; }, true, false) == WheelGate::Refusal::NotReady);
    CHECK_FALSE(cap.on);
    ready = true;
    CHECK(g.request(req("ok"), [&ready] { return ready; }, true, false) == WheelGate::Refusal::None);
    CHECK(cap.on);
}
