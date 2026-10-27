// The console writer's queue: lines in order, bounded (a stalled console window must not stall the game).
#include <doctest.h>

#include <string>

#include "common/line_queue.h"

TEST_CASE("line queue: lines come out in order; past the limit the oldest go and are counted") {
    emf::LineQueue q(3);
    q.push("a");
    q.push("b");
    auto out = q.take();
    REQUIRE(out.lines.size() == 2);
    CHECK(out.lines[0] == "a");
    CHECK(out.dropped == 0);
    for (const char* s : {"1", "2", "3", "4", "5"}) q.push(s);
    out = q.take();
    REQUIRE(out.lines.size() == 3);
    CHECK(out.lines[0] == "3");
    CHECK(out.lines[2] == "5");
    CHECK(out.dropped == 2);
    CHECK(q.take().lines.empty());
}
