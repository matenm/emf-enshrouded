// The debug bridge's log ring: the last N log lines with sequence numbers, read with `since`.
#include <windows.h>

#include <doctest.h>

#include <string>

#include "common/log.h"
#include "common/log_ring.h"

using emf::LogRing;

TEST_CASE("log ring: lines get numbers 1, 2, 3 and are read back after a number") {
    LogRing ring(10);
    CHECK(ring.last_seq() == 0);
    CHECK(ring.first_seq() == 1);
    CHECK(ring.after(0, 100, SIZE_MAX).empty());
    CHECK(ring.push("a") == 1);
    CHECK(ring.push("b") == 2);
    CHECK(ring.push("c") == 3);
    const auto all = ring.after(0, 100, SIZE_MAX);
    REQUIRE(all.size() == 3);
    CHECK(all[0].seq == 1);
    CHECK(all[0].text == "a");
    CHECK(all[2].text == "c");
    const auto since2 = ring.after(2, 100, SIZE_MAX);
    REQUIRE(since2.size() == 1);
    CHECK(since2[0].seq == 3);
    CHECK(ring.after(3, 100, SIZE_MAX).empty());
    CHECK(ring.after(99, 100, SIZE_MAX).empty());  // a number from the future: nothing, no crash
}

TEST_CASE("log ring: only the last N lines are kept; older numbers start at the oldest kept line") {
    LogRing ring(3);
    for (int i = 1; i <= 5; ++i) ring.push("line " + std::to_string(i));
    CHECK(ring.last_seq() == 5);
    CHECK(ring.first_seq() == 3);
    const auto all = ring.after(0, 100, SIZE_MAX);
    REQUIRE(all.size() == 3);
    CHECK(all[0].seq == 3);
    CHECK(all[0].text == "line 3");
    CHECK(all[2].text == "line 5");
    const auto tail = ring.tail(2);
    REQUIRE(tail.size() == 2);
    CHECK(tail[0].seq == 4);
    CHECK(ring.tail(50).size() == 3);
}

TEST_CASE("log ring: a read stops at the line and byte limits and says more lines are left") {
    LogRing ring(100);
    for (int i = 0; i < 10; ++i) ring.push(std::string(10, char('a' + i)));
    bool more = false;
    auto part = ring.after(0, 4, SIZE_MAX, &more);
    CHECK(part.size() == 4);
    CHECK(more);
    part = ring.after(0, 100, 25, &more);  // 10 bytes each: two fit
    CHECK(part.size() == 2);
    CHECK(more);
    part = ring.after(0, 100, 5, &more);  // even one line is too big: it still comes
    CHECK(part.size() == 1);
    part = ring.after(6, 100, SIZE_MAX, &more);
    CHECK(part.size() == 4);
    CHECK_FALSE(more);
}

TEST_CASE("log ring: a push signals the notify event") {
    LogRing ring(4);
    HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ring.set_notify(e);
    ring.push("x");
    CHECK(WaitForSingleObject(e, 0) == WAIT_OBJECT_0);
    ring.set_notify(nullptr);
    ring.push("y");
    CHECK(WaitForSingleObject(e, 0) == WAIT_TIMEOUT);
    CloseHandle(e);
}

TEST_CASE("log ring: logf lines reach the ring once it is switched on, with the log file's time stamp") {
    emf::log_ring_enable(5000);
    emf::LogRing* ring = emf::log_ring();
    REQUIRE(ring != nullptr);
    emf::logf("bridge ring test %d", 42);
    const auto last = ring->tail(1);
    REQUIRE(last.size() == 1);
    CHECK(last[0].text.find("bridge ring test 42") != std::string::npos);
    CHECK(last[0].text.front() == '[');  // "[hh:mm:ss.mmm tNNNNN] ..." as in emf_core.log
    emf::log_ring_enable(10);            // a second call keeps the first ring
    CHECK(emf::log_ring() == ring);
}
