// Repeated log lines: the first one, then at most one per window with a count of the quiet repeats.
#include <doctest.h>

#include "common/log_limiter.h"

using emf::LogLimiter;

TEST_CASE("log limiter: a line is logged the first time, then at most once per window with the skipped count") {
    LogLimiter lim(10000);
    CHECK(lim.pass("put refused: x", 1000) == "put refused: x");
    CHECK(lim.pass("put refused: x", 2000).empty());
    CHECK(lim.pass("put refused: x", 5000).empty());
    CHECK(lim.pass("put refused: y", 5000) == "put refused: y");  // another line has its own window
    CHECK(lim.pass("put refused: x", 11000) == "put refused: x (2 more since the last one)");
    CHECK(lim.pass("put refused: x", 12000).empty());
    CHECK(lim.pass("put refused: x", 30000) == "put refused: x (1 more since the last one)");
    CHECK(lim.pass("put refused: x", 50000) == "put refused: x");  // nothing skipped in between
}
