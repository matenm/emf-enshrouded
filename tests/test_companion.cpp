#include <windows.h>

#include <doctest.h>

#include <cstdarg>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "../mods/companion/companion_logic.h"
#include "emf/emf_api.h"
#include "test_scratch.h"

using companion::Companion;
using companion::KeyEdge;
using Phase = Companion::Phase;

TEST_CASE("companion: a spawned companion asks for exactly one StartGuardingEvent") {
    Companion c;
    uint32_t guard = 0, guarded = 0;
    CHECK_FALSE(c.take_start(&guard, &guarded));
    c.on_spawned(6817, 1025, 1000, 1);
    CHECK(c.phase() == Phase::Active);
    CHECK(c.entity() == 6817);
    CHECK(c.player() == 1025);
    REQUIRE(c.take_start(&guard, &guarded));
    CHECK(guard == 6817);
    CHECK(guarded == 1025);
    CHECK_FALSE(c.take_start(&guard, &guarded));
}

TEST_CASE("companion: stop is sent once, then the server tick destroys the companion") {
    Companion c;
    uint32_t guard = 0, guarded = 0;
    CHECK(c.take_destroy() == 0);
    c.request_stop();  // nothing to stop yet
    CHECK(c.phase() == Phase::None);
    c.on_spawned(6817, 1025, 1000, 1);
    c.request_stop();  // before the start was even sent: no start any more
    CHECK_FALSE(c.take_start(&guard, &guarded));
    CHECK(c.take_destroy() == 0);  // the stop has not been sent yet
    REQUIRE(c.take_stop(&guard, &guarded));
    CHECK(guard == 6817);
    CHECK(guarded == 1025);
    CHECK_FALSE(c.take_stop(&guard, &guarded));
    CHECK(c.take_destroy() == 6817);
    CHECK(c.phase() == Phase::None);
    CHECK(c.entity() == 0);
    CHECK(c.take_destroy() == 0);
}

TEST_CASE("companion: death or disappearance forgets the companion") {
    Companion c;
    uint32_t guard = 0, guarded = 0;
    c.on_spawned(6817, 1025, 1000, 1);
    c.on_died(1234);  // someone else died
    CHECK(c.entity() == 6817);
    c.on_died(6817);
    CHECK(c.phase() == Phase::None);
    CHECK_FALSE(c.take_start(&guard, &guarded));
    c.on_spawned(6900, 1025, 2000, 1);
    c.on_gone();
    CHECK(c.entity() == 0);
    CHECK_FALSE(c.take_start(&guard, &guarded));
}

TEST_CASE("companion: the start is re-sent only when the guard target is wrong and 3 s have passed") {
    Companion c;
    uint32_t guard = 0, guarded = 0;
    c.on_spawned(6817, 1025, 1000, 1);
    CHECK_FALSE(c.wants_regard(0, 2000));  // a start is still pending
    REQUIRE(c.take_start(&guard, &guarded));
    CHECK_FALSE(c.wants_regard(1025, 9000));  // already guarding the player
    CHECK_FALSE(c.wants_regard(0, 3500));     // only 2.5 s since the last start
    CHECK(c.wants_regard(0, 4000));
    CHECK_FALSE(c.wants_regard(0, 4100));     // pending again
    REQUIRE(c.take_start(&guard, &guarded));
    CHECK_FALSE(c.wants_regard(0, 6000));
    CHECK(c.wants_regard(0, 7000));
    c.request_stop();
    CHECK_FALSE(c.wants_regard(0, 20000));  // stopping: never re-arm
}

TEST_CASE("companion: a new server session forgets the companion in every phase (the world address may be reused)") {
    Companion c;
    uint32_t guard = 0, guarded = 0;
    c.on_session(1);
    c.on_spawned(6817, 1, 1000, 0xA000);
    c.on_session(1);  // the same session every tick: nothing happens
    CHECK(c.entity() == 6817);
    c.on_session(2);  // a reload: same world address, new session; ids are reused there
    CHECK(c.entity() == 0);
    CHECK(c.phase() == Phase::None);
    CHECK_FALSE(c.take_start(&guard, &guarded));
    c.on_spawned(6817, 1, 2000, 0xA000);
    c.request_stop();
    c.on_session(3);  // switched while a dismissal was pending: never stop or destroy in the new session
    CHECK_FALSE(c.take_stop(&guard, &guarded));
    CHECK(c.take_destroy() == 0);
}

TEST_CASE("companion: a held key counts as one press") {
    KeyEdge k;
    CHECK_FALSE(k.pressed(false));
    CHECK(k.pressed(true));
    CHECK_FALSE(k.pressed(true));
    CHECK_FALSE(k.pressed(true));
    CHECK_FALSE(k.pressed(false));
    CHECK(k.pressed(true));
}

namespace {
std::vector<std::string> g_companion_log;
void companion_fake_log(const char* mod, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_companion_log.push_back(std::string("[") + mod + "] " + buf);
}
std::wstring companion_dll() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p);
    return s.substr(0, s.find_last_of(L"\\/")) + L"\\dist\\emf\\mods\\companion\\companion.dll";
}
}  // namespace

TEST_CASE("companion.dll loads against the SDK and stays off when its ini says enabled=0") {
    // The core skips a mod whose ini says enabled=0; the mod checks too (a loose copy, an older core).
    const test_scratch::BuildFolderLock lock;  // the ini is <exe folder>\emf\config\companion.ini for every run of this build
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    dir = dir.substr(0, dir.find_last_of(L"\\/")) + L"\\emf";
    CreateDirectoryW(dir.c_str(), nullptr);
    CreateDirectoryW((dir + L"\\config").c_str(), nullptr);
    const std::wstring ini = dir + L"\\config\\companion.ini";
    WritePrivateProfileStringW(L"companion", L"enabled", L"0", ini.c_str());
    static EmfApi api{};
    api.version = EMF_API_VERSION;
    api.size = sizeof(EmfApi);
    api.log = companion_fake_log;
    HMODULE m = LoadLibraryW(companion_dll().c_str());
    REQUIRE(m != nullptr);
    auto load = reinterpret_cast<EmfModLoadFn>(GetProcAddress(m, "emf_mod_load"));
    REQUIRE(load != nullptr);
    g_companion_log.clear();
    CHECK(load(&api));
    REQUIRE_FALSE(g_companion_log.empty());
    CHECK(g_companion_log[0].find("[companion] disabled") == 0);
    DeleteFileW(ini.c_str());
}

TEST_CASE("companion: follow_distance sets the guard target distance and starts following 2 m further out") {
    float start = 4.0f, target = 2.0f;
    CHECK(companion::apply_follow_distance(6.0f, &start, &target));
    CHECK(target == 6.0f);
    CHECK(start == 8.0f);
    start = 4.0f;
    target = 2.0f;
    CHECK_FALSE(companion::apply_follow_distance(0.0f, &start, &target));  // 0 = keep the template's values
    CHECK(start == 4.0f);
    CHECK(target == 2.0f);
    CHECK_FALSE(companion::apply_follow_distance(-3.0f, &start, &target));
    CHECK(companion::apply_follow_distance(500.0f, &start, &target));  // clamped to 50 m
    CHECK(target == 50.0f);
    CHECK(start == 52.0f);
}

// ---- an ally that fights, and comes back ----
TEST_CASE("companion: faction_to_write gives the configured faction only when it is valid and differs") {
    CHECK_FALSE(companion::faction_to_write(-1, 3).has_value());  // -1 = keep the template's faction
    CHECK(companion::faction_to_write(11, 3) == std::optional<uint8_t>(11));  // Vukah -> PlayerAlly
    CHECK_FALSE(companion::faction_to_write(11, 11).has_value());  // already set
    CHECK_FALSE(companion::faction_to_write(16, 3).has_value());   // not a FactionType
    CHECK(companion::faction_to_write(0, 3) == std::optional<uint8_t>(0));
}

TEST_CASE("companion: auto summon is due 5 s after the session's save has loaded, once") {
    companion::SummonPolicy p;
    p.auto_summon = true;
    CHECK_FALSE(p.due(0));  // no session yet
    p.on_session(1, 10000);
    CHECK_FALSE(p.due(20000));  // the save has not loaded yet
    p.on_loaded(30000);
    CHECK_FALSE(p.due(30000 + companion::kSettleMs - 1));
    CHECK(p.due(30000 + companion::kSettleMs));
    p.on_summoned();
    CHECK_FALSE(p.due(99999));
    p.on_session(1, 40000);  // the same session every tick: nothing new
    p.on_loaded(40000);      // (and a second load notice in the same session changes nothing)
    CHECK_FALSE(p.due(999999));
}

TEST_CASE("companion: a reload (new session, same world address) brings back a companion that was out") {
    companion::SummonPolicy p;  // auto summon off
    p.on_session(1, 0);
    p.on_loaded(0);
    CHECK_FALSE(p.due(60000));
    p.on_summoned();  // F6 summoned one
    p.on_session(2, 100000);  // reload while it was active
    CHECK_FALSE(p.due(100000 + companion::kSettleMs));  // waits for the save
    p.on_loaded(110000);
    CHECK(p.due(110000 + companion::kSettleMs));
}

TEST_CASE("companion: without a load notice the summon still comes 60 s into the session") {
    companion::SummonPolicy p;
    p.auto_summon = true;
    p.on_session(1, 0);
    CHECK_FALSE(p.due(companion::SummonPolicy::kLoadFallbackMs - 1));
    CHECK(p.due(companion::SummonPolicy::kLoadFallbackMs));
}

TEST_CASE("companion: an F6 dismissal is respected until the next session") {
    companion::SummonPolicy p;
    p.auto_summon = true;
    p.on_session(1, 0);
    p.on_loaded(0);
    p.on_summoned();
    p.on_dismissed();
    CHECK_FALSE(p.due(999999));
    p.on_session(2, 1000000);  // a new session: auto summon applies again
    p.on_loaded(1000000);
    CHECK(p.due(1000000 + companion::kSettleMs));
}

TEST_CASE("companion: a death is followed by a new companion only with respawn_ms") {
    companion::SummonPolicy p;
    p.on_session(1, 0);
    p.on_loaded(0);
    p.on_summoned();
    p.on_died(50000);
    CHECK_FALSE(p.due(999999));  // respawn off: F6 summons a new one
    p.on_session(2, 1000000);    // and a reload does not bring back a dead companion
    p.on_loaded(1000000);
    CHECK_FALSE(p.due(2000000));

    companion::SummonPolicy r;
    r.respawn_ms = 30000;
    r.on_session(1, 0);
    r.on_loaded(0);
    r.on_summoned();
    r.on_died(50000);
    CHECK_FALSE(r.due(79999));
    CHECK(r.due(80000));
    r.on_summoned();
    CHECK_FALSE(r.due(999999));
}

TEST_CASE("companion: a summon that could not spawn stays due and is retried") {
    companion::SummonPolicy p;
    p.auto_summon = true;
    p.on_session(1, 0);
    p.on_loaded(0);
    CHECK(p.due(companion::kSettleMs));
    // (the caller found no player position and did not spawn: no on_summoned)
    CHECK(p.due(companion::kSettleMs + 1000));
}

TEST_CASE("companion: parse_faction takes -1..15 and falls back to PlayerAlly on anything else") {
    CHECK(companion::parse_faction(L"11").value == 11);
    CHECK(companion::parse_faction(L"11").ok);
    CHECK(companion::parse_faction(L"-1").value == -1);
    CHECK(companion::parse_faction(L" 7 ").value == 7);
    CHECK(companion::parse_faction(L"0").value == 0);
    for (const wchar_t* bad : {L"", L"PlayerAlly", L"111", L"-2", L"11x"}) {
        CAPTURE(std::wstring(bad).size());
        CHECK(companion::parse_faction(bad).value == 11);
        CHECK_FALSE(companion::parse_faction(bad).ok);
    }
    CHECK_FALSE(companion::parse_faction(nullptr).ok);
}

TEST_CASE("companion: a death read after a dismissal (or with it) never brings a new companion") {
    companion::SummonPolicy p;
    p.respawn_ms = 10000;
    p.on_session(1, 0);
    p.on_loaded(0);
    p.on_summoned();
    p.on_dismissed();     // F6
    CHECK_FALSE(p.on_died(1000));  // the death of the dismissed companion arrives in the same program run
    CHECK_FALSE(p.due(999999));

    companion::SummonPolicy q;
    q.respawn_ms = 10000;
    q.on_session(1, 0);
    q.on_loaded(0);
    q.on_summoned();
    CHECK(q.on_died(1000));  // died while out: a respawn is armed...
    q.on_dismissed();        // ...and an F6 right after cancels it
    CHECK_FALSE(q.due(999999));
}

TEST_CASE("companion: a failing spawn backs off 5 s, doubling to at most 60 s; a success resets it") {
    companion::SummonPolicy p;
    p.auto_summon = true;
    p.on_session(1, 0);
    p.on_loaded(0);
    uint64_t t = companion::kSettleMs;
    REQUIRE(p.due(t));
    p.on_spawn_failed(t);
    CHECK_FALSE(p.due(t + 4999));
    CHECK(p.due(t + 5000));
    t += 5000;
    p.on_spawn_failed(t);
    CHECK_FALSE(p.due(t + 9999));
    CHECK(p.due(t + 10000));
    t += 10000;
    p.on_spawn_failed(t);
    CHECK(p.due(t + 20000));
    for (int i = 0; i < 5; ++i) {
        t += 100000;
        p.on_spawn_failed(t);
    }
    CHECK_FALSE(p.due(t + 59999));  // capped at 60 s
    CHECK(p.due(t + 60000));
    p.on_summoned();
    p.on_died(t + 70000);  // (respawn off: nothing due)
    p.respawn_ms = 1000;
    p.on_summoned();
    p.on_died(t + 80000);
    p.on_spawn_failed(t + 81000);
    CHECK(p.due(t + 86000));  // back to 5 s after the success
}

TEST_CASE("companion: the death message says what happens next") {
    CHECK(companion::death_message(false, 0).find("F6 summons a new one") != std::string::npos);
    CHECK(companion::death_message(true, 10).find("a new one comes in 10 s") != std::string::npos);
    CHECK(companion::death_message(true, 10).find("F6") == std::string::npos);
}

// ---- orders ----
#include "../mods/companion/orders.h"

namespace {
using companion::Orders;
using companion::OrderAction;
using Act = companion::OrderAction::Kind;
using emf::aim::Vec3;

struct OrderWorld {
    std::map<uint32_t, companion::EntityState> ents;
    companion::OrderLookup fn() {
        return [this](uint32_t id) -> std::optional<companion::EntityState> {
            auto it = ents.find(id);
            if (it == ents.end()) return std::nullopt;
            return it->second;
        };
    }
};
size_t count_kind(const std::vector<OrderAction>& a, Act k) {
    size_t n = 0;
    for (const auto& x : a) n += x.kind == k;
    return n;
}
const OrderAction* first(const std::vector<OrderAction>& a, Act k) {
    for (const auto& x : a)
        if (x.kind == k) return &x;
    return nullptr;
}
const Vec3 kPlayer{0, 0, 0};
}  // namespace

TEST_CASE("orders: follow guards the player once; stay spawns one marker and guards it once it exists") {
    Orders o;
    OrderWorld w;
    CHECK(o.mode() == companion::Mode::Follow);
    o.follow();
    auto a = o.tick(0, kPlayer, w.fn());
    CHECK(count_kind(a, Act::GuardPlayer) == 1);
    CHECK(o.tick(100, kPlayer, w.fn()).empty());  // nothing more until something changes

    o.stay({5, 0, 3});
    a = o.tick(200, kPlayer, w.fn());
    REQUIRE(count_kind(a, Act::SpawnMarker) == 1);
    CHECK(first(a, Act::SpawnMarker)->pos.x == 5);
    CHECK(o.tick(300, kPlayer, w.fn()).empty());  // waits for the marker
    o.on_marker_spawned(900);
    w.ents[900] = {{5, 0, 3}, true};
    a = o.tick(400, kPlayer, w.fn());
    REQUIRE(count_kind(a, Act::GuardEntity) == 1);
    CHECK(first(a, Act::GuardEntity)->id == 900);
    CHECK(o.guard_target() == 900);
    CHECK(o.tick(500, kPlayer, w.fn()).empty());

    o.follow();  // back: the marker goes, the player is guarded again
    a = o.tick(600, kPlayer, w.fn());
    REQUIRE(count_kind(a, Act::DestroyMarker) == 1);
    CHECK(first(a, Act::DestroyMarker)->id == 900);
    CHECK(count_kind(a, Act::GuardPlayer) == 1);
    CHECK(o.guard_target() == 0);
}

TEST_CASE("orders: a marker that disappears while staying is spawned again at the same spot") {
    Orders o;
    OrderWorld w;
    o.stay({1, 2, 3});
    o.tick(0, kPlayer, w.fn());
    o.on_marker_spawned(900);
    w.ents[900] = {{1, 2, 3}, true};
    o.tick(100, kPlayer, w.fn());
    w.ents.erase(900);  // gone
    auto a = o.tick(200, kPlayer, w.fn());
    REQUIRE(count_kind(a, Act::SpawnMarker) == 1);
    CHECK(first(a, Act::SpawnMarker)->pos.z == 3);
}

TEST_CASE("orders: attack sends threat every 2 s and ends when the target is gone, far, or after 30 s") {
    Orders o;
    OrderWorld w;
    w.ents[50] = {{10, 0, 0}, true};
    o.attack(50, 1000);
    auto a = o.tick(1000, kPlayer, w.fn());
    REQUIRE(count_kind(a, Act::Threat) == 1);
    CHECK(first(a, Act::Threat)->id == 50);
    CHECK(count_kind(o.tick(2000, kPlayer, w.fn()), Act::Threat) == 0);
    CHECK(count_kind(o.tick(3000, kPlayer, w.fn()), Act::Threat) == 1);
    w.ents[50].alive = false;  // died
    a = o.tick(3500, kPlayer, w.fn());
    CHECK(count_kind(a, Act::Threat) == 0);
    CHECK(count_kind(a, Act::Notice) == 1);
    CHECK(count_kind(a, Act::GuardPlayer) == 1);
    CHECK(o.mode() == companion::Mode::Follow);

    w.ents[51] = {{10, 0, 0}, true};
    o.attack(51, 10000);
    o.tick(10000, kPlayer, w.fn());
    w.ents[51].pos = {60, 0, 0};  // got away (over 40 m)
    a = o.tick(11000, kPlayer, w.fn());
    CHECK(count_kind(a, Act::Notice) == 1);
    CHECK(o.mode() == companion::Mode::Follow);

    w.ents[52] = {{10, 0, 0}, true};
    o.attack(52, 20000);
    o.tick(20000, kPlayer, w.fn());
    CHECK(o.mode() == companion::Mode::Attack);
    a = o.tick(50000, kPlayer, w.fn());  // 30 s
    CHECK(count_kind(a, Act::Threat) == 0);
    CHECK(count_kind(a, Act::Notice) == 1);
    CHECK(o.mode() == companion::Mode::Follow);

    o.attack(999, 60000);  // a target that does not exist: ends at once, no threat for it
    a = o.tick(60000, kPlayer, w.fn());
    CHECK(count_kind(a, Act::Threat) == 0);
    CHECK(o.mode() == companion::Mode::Follow);
}

TEST_CASE("orders: attacking from stay removes the marker; a new session re-creates the stay marker, never reusing ids") {
    Orders o;
    OrderWorld w;
    o.stay({4, 0, 4});
    o.tick(0, kPlayer, w.fn());
    o.on_marker_spawned(900);
    w.ents[900] = {{4, 0, 4}, true};
    o.tick(100, kPlayer, w.fn());

    o.on_session(2);  // reload: entity ids are reused, the old marker is gone with the old world
    w.ents.clear();
    w.ents[900] = {{99, 0, 99}, true};  // the id now names something else
    auto a = o.tick(200, kPlayer, w.fn());
    CHECK(count_kind(a, Act::DestroyMarker) == 0);  // never destroys the reused id
    REQUIRE(count_kind(a, Act::SpawnMarker) == 1);
    CHECK(first(a, Act::SpawnMarker)->pos.x == 4);
    o.on_marker_spawned(901);
    w.ents[901] = {{4, 0, 4}, true};
    o.tick(300, kPlayer, w.fn());

    w.ents[50] = {{3, 0, 0}, true};
    o.attack(50, 400);
    a = o.tick(400, kPlayer, w.fn());
    CHECK(count_kind(a, Act::DestroyMarker) == 1);
    CHECK(first(a, Act::DestroyMarker)->id == 901);
    CHECK(count_kind(a, Act::GuardPlayer) == 1);  // attacks while guarding the player
    CHECK(count_kind(a, Act::Threat) == 1);
}

TEST_CASE("orders: a new companion gets the current order again; the order line is saved and read back") {
    Orders o;
    OrderWorld w;
    o.stay({7.5, 905, -3.25});
    o.tick(0, kPlayer, w.fn());
    o.on_marker_spawned(900);
    w.ents[900] = {{7.5, 905, -3.25}, true};
    o.tick(100, kPlayer, w.fn());
    o.on_companion_spawned();  // a respawned companion must be told to guard the marker
    auto a = o.tick(200, kPlayer, w.fn());
    REQUIRE(count_kind(a, Act::GuardEntity) == 1);
    CHECK(o.serialize() == "stay 7.50 905.00 -3.25");

    Orders back;
    REQUIRE(back.parse("stay 7.50 905.00 -3.25"));
    CHECK(back.mode() == companion::Mode::Stay);
    a = back.tick(0, kPlayer, w.fn());
    REQUIRE(count_kind(a, Act::SpawnMarker) == 1);
    CHECK(first(a, Act::SpawnMarker)->pos.y == doctest::Approx(905));
    Orders f;
    REQUIRE(f.parse("follow"));
    CHECK(f.mode() == companion::Mode::Follow);
    CHECK_FALSE(f.parse("dance"));
    w.ents[50] = {{3, 0, 0}, true};
    f.attack(50, 0);
    CHECK(f.serialize() == "follow");  // an attack is not saved
}

TEST_CASE("companion: an order can make it guard another entity; the regard check then keeps that target") {
    Companion c;
    c.on_spawned(500, 1, 0, 0x1000);
    uint32_t g = 0, t = 0;
    REQUIRE(c.take_start(&g, &t));
    CHECK(t == 1);  // the player
    c.request_guard(900, 100);
    REQUIRE(c.take_start(&g, &t));
    CHECK(g == 500);
    CHECK(t == 900);  // the stay marker
    CHECK_FALSE(c.wants_regard(900, 10000));  // guarding the marker is right
    CHECK(c.wants_regard(1, 10000));          // drifted back to the player: guard the marker again
    REQUIRE(c.take_start(&g, &t));
    CHECK(t == 900);
    c.request_guard(0, 20000);  // follow: the player again
    REQUIRE(c.take_start(&g, &t));
    CHECK(t == 1);
    c.on_gone();
    c.request_guard(900, 30000);  // no companion: nothing to send
    CHECK_FALSE(c.take_start(&g, &t));
}

// ---- gear ----
#include "../mods/companion/gear.h"

namespace {
using companion::GearBook;
using companion::Piece;
using companion::PieceState;
using companion::Slot;
Piece sword(uint32_t level) { return Piece{0x5000, level, 2, Slot::Weapon}; }
Piece helmet() { return Piece{0x6000, 5, 1, Slot::Head}; }
Piece boots() { return Piece{0x7000, 5, 1, Slot::Feet}; }
}  // namespace

TEST_CASE("gear: the bonus counts confirmed pieces: weapon by its level against yours, +1 per armour piece, capped") {
    GearBook g;
    CHECK(g.bonus(8, 10) == 0);
    REQUIRE(g.give(sword(12), 0).ok);
    CHECK(g.bonus(8, 10) == 0);  // not confirmed yet
    g.on_take_result(Slot::Weapon, true, 100);
    CHECK(g.bonus(8, 10) == 3);  // (12 - 8) / 2 + 1
    REQUIRE(g.give(helmet(), 200).ok);
    g.on_take_result(Slot::Head, true, 300);
    REQUIRE(g.give(boots(), 400).ok);
    g.on_take_result(Slot::Feet, true, 500);
    CHECK(g.bonus(8, 10) == 5);
    CHECK(g.bonus(8, 4) == 4);   // capped
    CHECK(g.bonus(20, 10) == 3);  // a weapon below your level still gives +1
    CHECK(companion::companion_level(8, 5) == 13);
    CHECK(companion::companion_level(0, 0) == 1);
    CHECK(companion::companion_level(99, 10) == 100);  // at most 100
}

TEST_CASE("gear: a give that fails leaves the old piece; one that succeeds returns the old piece exactly once") {
    GearBook g;
    REQUIRE(g.give(sword(5), 0).ok);
    g.on_take_result(Slot::Weapon, true, 10);
    auto r = g.give(sword(9), 20);  // a better sword for the same slot
    REQUIRE(r.ok);
    CHECK_FALSE(g.give(sword(10), 25).ok);  // one give per slot at a time
    auto back = g.on_take_result(Slot::Weapon, false, 30);  // the take failed (item gone from the inventory)
    CHECK(back.empty());
    REQUIRE(g.held().size() == 1);
    CHECK(g.held()[0].level == 5);  // the old sword is still worn

    REQUIRE(g.give(sword(9), 40).ok);
    back = g.on_take_result(Slot::Weapon, true, 50);
    REQUIRE(back.size() == 1);
    CHECK(back[0].level == 5);  // the old one goes back to the player
    CHECK(back[0].state == PieceState::Returning);
    CHECK(g.on_take_result(Slot::Weapon, true, 60).empty());  // a second answer changes nothing
    REQUIRE(g.held().size() == 1);
    CHECK(g.held()[0].level == 9);
}

TEST_CASE("gear: taking all back returns every piece; a confirmed return is forgotten after 120 s, a failed one is worn again") {
    GearBook g;
    g.give(sword(9), 0);
    g.on_take_result(Slot::Weapon, true, 0);
    g.give(helmet(), 0);
    g.on_take_result(Slot::Head, true, 0);
    auto back = g.take_back_all(1000);
    CHECK(back.size() == 2);
    CHECK(g.held().empty());
    CHECK(g.bonus(1, 10) == 0);
    g.on_put_result(back[0].token, true, 1100);   // the sword arrived
    g.on_put_result(back[1].token, false, 1100);  // the helmet did not fit
    CHECK(g.held().size() == 1);           // the helmet is worn again (nothing lost)
    CHECK(g.serialize().find("returning") != std::string::npos);  // the sword is kept in the file for a while
    g.tick(1100 + 119000);
    CHECK(g.serialize().find("returning") != std::string::npos);
    g.tick(1100 + 120000);
    CHECK(g.serialize().find("returning") == std::string::npos);
}

TEST_CASE("gear: the file keeps worn and returning pieces; on load a returning piece is worn again and an unconfirmed take is dropped") {
    GearBook g;
    g.give(sword(9), 0);
    g.on_take_result(Slot::Weapon, true, 0);
    g.give(helmet(), 0);
    g.on_take_result(Slot::Head, true, 0);
    g.give(boots(), 0);  // never confirmed (crash)
    const auto sent = g.take_back_all(100);
    REQUIRE(sent.size() == 2);
    g.on_put_result(sent[1].token, true, 200);  // the helmet is on its way back (returning)
    const std::string text = g.serialize();
    GearBook back;
    back.load(text);
    // after a crash: the returning helmet counts as worn again (it may be duplicated, never lost); the boots whose take
    // was never confirmed are dropped (still in the player's inventory)
    bool helmet_worn = false, boots = false;
    for (const auto& p : back.held()) {
        helmet_worn |= p.item == 0x6000;
        boots |= p.item == 0x7000;
    }
    CHECK(helmet_worn);
    CHECK_FALSE(boots);
}

TEST_CASE("gear: on load a returning piece whose slot is worn by a newer one stays returning, to be put back") {
    GearBook g;
    g.load("piece 0 00005000 9 2 held\npiece 0 00005001 5 1 returning\njunk line\npiece 9 00000001 1 1 held\n");
    REQUIRE(g.held().size() == 1);
    CHECK(g.held()[0].item == 0x5000);
    const auto back = g.unreturned();
    REQUIRE(back.size() == 1);
    CHECK(back[0].item == 0x5001);  // the old sword is put back again: never lost
}

// ---- level and the per-world file ----
TEST_CASE("companion: the level is written only when it differs and the companion is confirmed on the player's side") {
    CHECK(companion::level_to_write(1, 13, true) == std::optional<uint32_t>(13));
    CHECK_FALSE(companion::level_to_write(13, 13, true).has_value());  // already right
    CHECK_FALSE(companion::level_to_write(1, 13, false).has_value());  // faction not PlayerAlly yet: never a strong hostile
    CHECK_FALSE(companion::level_to_write(0, 13, true).has_value());   // no Level component read
}

TEST_CASE("companion: the per-world file keeps whether it was out, its order and its gear") {
    companion::SavedCompanion s;
    s.out = true;
    s.order = "stay 1.00 2.00 3.00";
    s.gear = "piece 0 00005000 9 2 held\n";
    const std::string text = companion::serialize(s);
    CHECK(text == "out 1\norder stay 1.00 2.00 3.00\npiece 0 00005000 9 2 held\n");
    const auto back = companion::parse_saved(text);
    CHECK(back.out);
    CHECK(back.order == "stay 1.00 2.00 3.00");
    CHECK(back.gear == "piece 0 00005000 9 2 held\n");
    const auto none = companion::parse_saved("");
    CHECK_FALSE(none.out);
    CHECK(none.order == "follow");
    CHECK(none.gear.empty());
}

TEST_CASE("companion: a saved 'it was out' brings one after the world loads, also after a game restart") {
    companion::SummonPolicy p;  // fresh process: nothing was active
    p.on_session(1, 0);
    p.want_on_load();           // the world's file says a companion was out
    CHECK_FALSE(p.due(10000));  // waits for the load notice
    p.on_loaded(20000);
    CHECK(p.due(20000 + companion::kSettleMs));
    p.on_summoned();
    p.on_session(2, 50000);  // another world without the flag: nothing
    p.on_loaded(50000);
    CHECK(p.due(50000 + companion::kSettleMs));  // (it was active when the session ended: back after a reload, as before)
}

TEST_CASE("companion: 'coming' is true while a summon is wanted or armed, so the file keeps saying it was out") {
    companion::SummonPolicy p;
    p.respawn_ms = 10000;
    CHECK_FALSE(p.coming());
    p.on_session(1, 0);
    p.want_on_load();
    CHECK(p.coming());  // waiting for the load
    p.on_loaded(0);
    CHECK(p.coming());  // armed
    p.on_summoned();
    CHECK_FALSE(p.coming());
    p.on_died(1000);
    CHECK(p.coming());  // a respawn is coming
    p.on_dismissed();
    CHECK_FALSE(p.coming());
}

TEST_CASE("gear: the slot comes from the item's debug name; other items are not gear") {
    using companion::slot_from_name;
    CHECK(slot_from_name("Weapon_T0_1H_Mace_Club_Craft") == std::optional<Slot>(Slot::Weapon));
    CHECK(slot_from_name("Armor_T0_Fur_Helmet") == std::optional<Slot>(Slot::Head));
    CHECK(slot_from_name("Armor_T0_Fur_Chest") == std::optional<Slot>(Slot::Chest));
    CHECK(slot_from_name("Armor_T0_Fur_Arms") == std::optional<Slot>(Slot::Hands));
    CHECK(slot_from_name("Armor_T0_Fur_Legs") == std::optional<Slot>(Slot::Legs));
    CHECK(slot_from_name("Armor_T0_Fur_Boots") == std::optional<Slot>(Slot::Feet));
    CHECK(slot_from_name("Armor_T3_Scout_Helmet_Loot") == std::optional<Slot>(Slot::Head));
    CHECK_FALSE(slot_from_name("Armor_Helmet_MarianneReed_NPC_ONLY").has_value());
    CHECK_FALSE(slot_from_name("Material_T1_Wood_Logs").has_value());
    CHECK_FALSE(slot_from_name("Ring_T1_Ring_of_Health").has_value());
    CHECK_FALSE(slot_from_name("Craft_Weapon_T1_Sword").has_value());
}

TEST_CASE("gear: slot_name names each slot as the enum orders them") {
    using companion::Slot;
    CHECK(std::string(companion::slot_name(Slot::Weapon)) == "weapon");
    CHECK(std::string(companion::slot_name(Slot::Head)) == "head");
    CHECK(std::string(companion::slot_name(Slot::Chest)) == "chest");
    CHECK(std::string(companion::slot_name(Slot::Legs)) == "legs");
    CHECK(std::string(companion::slot_name(Slot::Hands)) == "hands");
    CHECK(std::string(companion::slot_name(Slot::Feet)) == "feet");
    CHECK(std::string(companion::slot_name(static_cast<Slot>(9))) == "?");
}

TEST_CASE("gear: a replaced piece that did not fit waits and goes back with the next Take all gear back") {
    using companion::PutOutcome;
    GearBook g;
    g.give(sword(5), 0);
    g.on_take_result(Slot::Weapon, true, 10);
    g.give(sword(9), 20);
    const auto old = g.on_take_result(Slot::Weapon, true, 30);  // the level-5 sword goes back
    REQUIRE(old.size() == 1);
    // no room for it: the level-9 sword wears the slot, so it cannot be worn again; it waits
    CHECK((g.on_put_result(old[0].token, false, 40) == PutOutcome::Waiting));
    REQUIRE(g.unreturned().size() == 1);
    CHECK(g.unreturned()[0].level == 5);
    auto back = g.take_back_all(50);  // the player makes room and takes everything back
    REQUIRE(back.size() == 2);        // the worn sword and the waiting one, once each
    CHECK(back[0].level + back[1].level == 14);
    CHECK(g.take_back_all(60).empty());  // both are on their way: nothing to send twice
    const uint32_t five = back[0].level == 5 ? back[0].token : back[1].token;
    const uint32_t nine = back[0].level == 9 ? back[0].token : back[1].token;
    CHECK((g.on_put_result(five, true, 70) == PutOutcome::Returned));
    CHECK((g.on_put_result(nine, false, 70) == PutOutcome::WornAgain));  // the other one did not fit: worn again
    REQUIRE(g.held().size() == 1);
    CHECK(g.held()[0].level == 9);
}

TEST_CASE("gear: results for two identical items on their way back reach the right piece (each piece has a token)") {
    using companion::PutOutcome;
    GearBook g;
    g.give(sword(5), 0);
    g.on_take_result(Slot::Weapon, true, 10);
    g.give(sword(9), 20);  // the same item id, another level
    const auto old = g.on_take_result(Slot::Weapon, true, 30);
    REQUIRE(old.size() == 1);  // the level-5 sword is on its way back (its put not answered yet)
    const auto back = g.take_back_all(40);
    REQUIRE(back.size() == 1);  // the level-9 one now too
    CHECK(back[0].token != old[0].token);
    CHECK((g.on_put_result(back[0].token, false, 50) == PutOutcome::WornAgain));  // the level-9 one did not fit
    REQUIRE(g.held().size() == 1);
    CHECK(g.held()[0].level == 9);  // not the level-5 one
    CHECK((g.on_put_result(old[0].token, true, 60) == PutOutcome::Returned));
    CHECK((g.on_put_result(old[0].token, true, 61) == PutOutcome::Unknown));  // answered already
    const std::string text = g.serialize();
    CHECK(text.find("piece 0 00005000 9 2 held") != std::string::npos);
    CHECK(text.find("piece 0 00005000 5 2 returning") != std::string::npos);
}

// ---- edge cases ----
TEST_CASE("orders: a stay marker that keeps vanishing is put again at most every 5 s, then the companion follows") {
    Orders o;
    OrderWorld w;
    o.stay({1, 2, 3});
    uint64_t now = 0;
    auto a = o.tick(now, kPlayer, w.fn());
    REQUIRE(count_kind(a, Act::SpawnMarker) == 1);
    uint32_t id = 900;
    int spawns = 1;
    // every marker vanishes right away (streamed out, smashed): ticks at 60 Hz for 30 s
    for (int i = 0; i < 1800; ++i) {
        now += 16;
        if (o.mode() == companion::Mode::Stay && i % 2 == 0) o.on_marker_spawned(id++);  // spawn answers, entity never seen
        a = o.tick(now, kPlayer, w.fn());
        spawns += static_cast<int>(count_kind(a, Act::SpawnMarker));
        if (count_kind(a, Act::Notice)) break;
    }
    CHECK(spawns <= 5);  // not one per tick
    CHECK(o.mode() == companion::Mode::Follow);
    CHECK(count_kind(a, Act::GuardPlayer) == 1);
}

TEST_CASE("orders: release_marker hands the stay marker out at once (dismiss), so no later tick destroys a reused id") {
    Orders o;
    OrderWorld w;
    o.stay({1, 2, 3});
    o.tick(0, kPlayer, w.fn());
    o.on_marker_spawned(900);
    w.ents[900] = {{1, 2, 3}, true};
    o.tick(100, kPlayer, w.fn());
    CHECK(o.release_marker() == 900);  // the companion is dismissed: the caller destroys it now
    CHECK(o.mode() == companion::Mode::Follow);
    CHECK(o.release_marker() == 0);
    CHECK(count_kind(o.tick(200, kPlayer, w.fn()), Act::DestroyMarker) == 0);
}

TEST_CASE("threat box: targets reach only the companion and session they were meant for") {
    companion::ThreatBox box;
    box.push(6875, 3, 7001);
    box.push(6875, 3, 7002);
    CHECK(box.take(6810, 3).empty());  // another companion: dropped
    box.push(6875, 3, 7003);
    CHECK(box.take(6875, 4).empty());  // a new session: dropped
    box.push(6875, 4, 7004);
    CHECK(box.take(6875, 4) == std::vector<uint32_t>{7004});
    CHECK(box.take(6875, 4).empty());
}

TEST_CASE("death log: a death counts only in the session it was seen in, and the log stays small") {
    companion::DeathLog d;
    d.record(3, 7001);
    CHECK(d.dead(3, 7001));
    CHECK_FALSE(d.dead(4, 7001));  // the id is reused in the next session: alive
    for (uint32_t i = 0; i < 1000; ++i) d.record(5, i);
    CHECK(d.size() <= 256);
    CHECK(d.dead(5, 999));  // the newest is kept
}

TEST_CASE("gear wheel: at most 63 pieces are offered so Take all gear back always fits (64 entries)") {
    std::vector<std::string> many(70, "Weapon_X (level 1, Common)");
    const auto wheel = companion::gear_wheel_labels(many);
    REQUIRE(wheel.size() == 64);
    CHECK(wheel.back() == "Take all gear back");
    CHECK(companion::gear_wheel_offered(many.size()) == 63);
    CHECK(companion::gear_wheel_labels({}).size() == 1);
    CHECK(companion::gear_wheel_offered(2) == 2);
}

TEST_CASE("companion: gear may change hands only once this session's world file is known") {
    CHECK_FALSE(companion::world_file_known(0, 0, 1));  // no world slot (no on_load on this build)
    CHECK_FALSE(companion::world_file_known(1, 1, 2));  // the file belongs to the previous session
    CHECK(companion::world_file_known(1, 2, 2));
}

TEST_CASE("companion: the state file is replaced in one step (a crash never leaves it empty)") {
    const std::wstring path = test_scratch::temp_path(L"emf_companion_atomic_test", L".txt");
    DeleteFileW(path.c_str());
    REQUIRE(companion::write_text_atomic(path, "out 1\norder follow\n"));
    REQUIRE(companion::write_text_atomic(path, "out 0\norder follow\n"));
    FILE* f = nullptr;
    REQUIRE(_wfopen_s(&f, path.c_str(), L"rb") == 0);
    char buf[64] = {};
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    CHECK(std::string(buf, n) == "out 0\norder follow\n");
    CHECK(GetFileAttributesW((path + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES);  // no temp file left behind
    DeleteFileW(path.c_str());
}
