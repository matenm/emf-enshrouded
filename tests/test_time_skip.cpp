#include <doctest.h>

#include <cstring>

#include "../mods/sample_mod/time_skip.h"

namespace {
constexpr int64_t kHour = 3600ll * 1000000000ll;
void* g_set_daytime = nullptr;
int64_t g_set_tod = -1;
int64_t g_set_now = -1;
void fake_set_tod(void* daytime, int64_t tod, int64_t now) {
    g_set_daytime = daytime;
    g_set_tod = tod;
    g_set_now = now;
}
template <class T>
void put(uint8_t* p, size_t off, T v) { std::memcpy(p + off, &v, sizeof(T)); }
template <class T>
T get(const uint8_t* p, size_t off) { T v; std::memcpy(&v, p + off, sizeof(T)); return v; }
}  // namespace

TEST_CASE("time_skip wraps around midnight and calls setTimeOfDay with the ECS time") {
    alignas(8) uint8_t daytime[0x70] = {};
    put<int64_t>(daytime, 0x48, 22 * kHour);
    auto r = sample::time_skip(daytime, nullptr, fake_set_tod, 123456, 3 * kHour);
    CHECK(r.old_tod == 22 * kHour);
    CHECK(r.new_tod == 1 * kHour);
    CHECK(g_set_daytime == daytime);
    CHECK(g_set_tod == 1 * kHour);
    CHECK(g_set_now == 123456);
    CHECK(r.layers_reset == 0);
}

TEST_CASE("time_skip resets nextPhenomenonTime of every layer on the server weather system") {
    alignas(8) uint8_t daytime[0x70] = {};
    alignas(8) uint8_t layers[3 * 0x18] = {};
    put<int64_t>(layers, 0x00, 111);
    put<int64_t>(layers, 0x18, 222);
    put<int64_t>(layers, 0x30, 333);
    put<uint32_t>(layers, 0x10, 7);  // current instance handle stays as is
    alignas(8) uint8_t weather[0xE0] = {};
    put<uint32_t>(weather, 0x08, 0);  // mode 0 = simulating (server)
    put<const void*>(weather, 0xC8, layers);
    put<uint64_t>(weather, 0xD0, 3);
    auto r = sample::time_skip(daytime, weather, fake_set_tod, 0, kHour);
    CHECK(r.layers_reset == 3);
    CHECK(get<int64_t>(layers, 0x00) == 0);
    CHECK(get<int64_t>(layers, 0x18) == 0);
    CHECK(get<int64_t>(layers, 0x30) == 0);
    CHECK(get<uint32_t>(layers, 0x10) == 7);
}

TEST_CASE("time_skip leaves a client-side (mode 1) or implausible weather system alone") {
    alignas(8) uint8_t daytime[0x70] = {};
    alignas(8) uint8_t layers[0x18] = {};
    put<int64_t>(layers, 0, 99);
    alignas(8) uint8_t weather[0xE0] = {};
    put<uint32_t>(weather, 0x08, 1);
    put<const void*>(weather, 0xC8, layers);
    put<uint64_t>(weather, 0xD0, 1);
    CHECK(sample::time_skip(daytime, weather, fake_set_tod, 0, kHour).layers_reset == 0);
    CHECK(get<int64_t>(layers, 0) == 99);
    put<uint32_t>(weather, 0x08, 0);
    put<uint64_t>(weather, 0xD0, 100000);  // nonsense count
    CHECK(sample::time_skip(daytime, weather, fake_set_tod, 0, kHour).layers_reset == 0);
}
