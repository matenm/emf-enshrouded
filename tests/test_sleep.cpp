// Sleep Through the Day (mods/sleep): the clock maths and the decisions (sleep_logic.h), and its game side over a fake
// API (sleep_glue.h): the sleepers counted as the game's night skip counts them, the Daytime read and written.
#include <doctest.h>

#include <cstring>
#include <map>
#include <vector>

#include "../mods/sleep/sleep_glue.h"
#include "../mods/sleep/sleep_logic.h"

using namespace sleep;

namespace {
constexpr int64_t kSec = 1000000000ll;
constexpr int64_t kMin = 60 * kSec;
constexpr int64_t kTick = 16666667;  // 60 Hz

// The game's defaults: day 06:00-18:00 over 30 min of game time, night over 12 min, normal speed.
Clock clock_at(double hours, float scale = 1.0f) {
    Clock c;
    c.tod_ns = hours_ns(hours);
    c.day_start_ns = 6 * kHourNs;
    c.night_start_ns = 18 * kHourNs;
    c.day_dur_ns = 30 * kMin;
    c.night_dur_ns = 12 * kMin;
    c.scale = scale;
    return c;
}
}  // namespace

TEST_CASE("sleep: game time between two times of day follows the day and night phases (30 min day, 12 min night)") {
    const Clock c = clock_at(12);
    CHECK(game_ns_between(c, 6 * kHourNs, 18 * kHourNs) == 30 * kMin);       // the whole day
    CHECK(game_ns_between(c, 12 * kHourNs, 18 * kHourNs) == 15 * kMin);      // half of it
    CHECK(game_ns_between(c, 18 * kHourNs, 6 * kHourNs) == 12 * kMin);       // the night, past midnight
    CHECK(game_ns_between(c, 17 * kHourNs, 19 * kHourNs) == 150 * kSec + 60 * kSec);  // 1 h of day + 1 h of night
    CHECK(game_ns_between(c, 0, 0) == 0);
    CHECK(game_ns_between(c, 6 * kHourNs, 6 * kHourNs + kDayNs) == 0);       // the same time of day
    CHECK(game_ns_between(c, 23 * kHourNs, 1 * kHourNs) == 2 * kMin);        // 2 h of night = 2 min
}

TEST_CASE("sleep: the clock is trusted only with plausible numbers; day is dayStart <= tod < nightStart") {
    CHECK(sane(clock_at(12)));
    Clock bad = clock_at(12);
    bad.night_start_ns = bad.day_start_ns;  // a moved field reads nonsense
    CHECK_FALSE(sane(bad));
    bad = clock_at(12);
    bad.tod_ns = kDayNs;
    CHECK_FALSE(sane(bad));
    bad = clock_at(12);
    bad.day_dur_ns = 0;
    CHECK_FALSE(sane(bad));
    CHECK(is_day(clock_at(6)));
    CHECK(is_day(clock_at(17.99)));
    CHECK_FALSE(is_day(clock_at(18)));
    CHECK_FALSE(is_day(clock_at(3)));
}

TEST_CASE("sleep: the speed the sleepers ask for: everyone, a share, or a percentage") {
    Settings s;
    CHECK(wanted_speed(s, 1, 1) == 60.0);
    CHECK(wanted_speed(s, 1, 2) == 0.0);  // all must sleep (the default)
    CHECK(wanted_speed(s, 0, 2) == 0.0);
    s.who = Who::Share;
    CHECK(wanted_speed(s, 1, 4) == 15.0);  // as the game's night skip: 60 * sleeping / players
    CHECK(wanted_speed(s, 3, 2) == 60.0);  // never more than everyone
    s.who = Who::Percent;
    s.percent = 50;
    CHECK(wanted_speed(s, 2, 4) == 60.0);
    CHECK(wanted_speed(s, 1, 4) == 0.0);
    CHECK(who_of("share") == Who::Share);
    CHECK(who_of("percent") == Who::Percent);
    CHECK(who_of("all") == Who::All);
    CHECK(who_of("nonsense") == Who::All);
}

TEST_CASE("sleep: asleep at noon the day races (60x: a step adds 59 steps of game time) and stops exactly at the wake hour") {
    Sleeper z;
    Settings s;  // wake 17:30, all, 60x
    Step st = z.tick(clock_at(12), 1, 1, kTick, s);
    CHECK(st.note == Step::Started);
    CHECK(st.speed == 60.0);
    CHECK(st.add_game_ns == 59 * kTick);  // the game's own speed (1) already moves it one step
    CHECK(z.phase() == Phase::DaySleep);
    st = z.tick(clock_at(13), 1, 1, kTick, s);
    CHECK(st.note == Step::None);
    // a step from 10 s of the day before 17:30 (0.42 s of game time; a 60x step adds 0.98 s) would pass it: only the
    // game time left is added, and the evening is noted
    const Clock late = clock_at(17.5 - 10.0 / 3600);
    st = z.tick(late, 1, 1, kTick, s);
    CHECK(st.add_game_ns == game_ns_between(late, late.tod_ns, hours_ns(17.5)));
    CHECK(st.note == Step::Evening);
    CHECK(z.phase() == Phase::Done);
    // still in bed in the evening, through the night and into the next day: nothing more (no day after day)
    for (double h : {17.6, 19.0, 2.0, 8.0, 12.0}) {
        st = z.tick(clock_at(h), 1, 1, kTick, s);
        CHECK(st.add_game_ns == 0);
        CHECK(st.note == Step::None);
    }
    // up, then back to bed the next day: it races again
    z.tick(clock_at(12), 0, 1, kTick, s);
    CHECK(z.phase() == Phase::Awake);
    CHECK(z.tick(clock_at(12.5), 1, 1, kTick, s).add_game_ns == 59 * kTick);
}

TEST_CASE("sleep: a sleep that began at night (the game's skip) or after the wake hour never speeds up the next day") {
    Settings s;
    Sleeper z;
    CHECK(z.tick(clock_at(22), 1, 1, kTick, s).add_game_ns == 0);
    CHECK(z.phase() == Phase::Done);
    CHECK(z.tick(clock_at(6.5), 1, 1, kTick, s).add_game_ns == 0);  // dawn, still in bed
    Sleeper late;
    CHECK(late.tick(clock_at(17.8), 1, 1, kTick, s).add_game_ns == 0);
    CHECK(late.phase() == Phase::Done);
}

TEST_CASE("sleep: with 'everyone' one sleeper of two waits; when the second lies down it races; getting up is noted") {
    Settings s;
    Sleeper z;
    Step st = z.tick(clock_at(10), 1, 2, kTick, s);
    CHECK(st.add_game_ns == 0);
    CHECK(st.note == Step::None);
    CHECK(z.phase() == Phase::DaySleep);
    st = z.tick(clock_at(10), 2, 2, kTick, s);
    CHECK(st.note == Step::Started);
    CHECK(st.add_game_ns > 0);
    st = z.tick(clock_at(11), 0, 2, kTick, s);  // both up before the evening
    CHECK(st.note == Step::Woke);
    // a sleep that never raced says nothing when it ends
    z.tick(clock_at(10), 1, 2, kTick, s);
    CHECK(z.tick(clock_at(10), 0, 2, kTick, s).note == Step::None);
}

TEST_CASE("sleep: share mode adds in proportion; a long gap between steps (a hitch, a load) adds nothing") {
    Settings s;
    s.who = Who::Share;
    Sleeper z;
    CHECK(z.tick(clock_at(9), 1, 4, kTick, s).add_game_ns == 14 * kTick);  // 15x: 14 steps more
    CHECK(z.tick(clock_at(9), 1, 4, 5 * kSec, s).add_game_ns == 0);
    CHECK(z.tick(clock_at(9), 1, 4, -kTick, s).add_game_ns == 0);
    s.speed = 1.0;  // a speed at or below the game's own: nothing to add
    CHECK(z.tick(clock_at(9), 4, 4, kTick, s).add_game_ns == 0);
}

TEST_CASE("sleep: percent mode also brings the night to full speed when that share sleeps (until dawn only)") {
    Settings s;
    s.who = Who::Percent;
    s.percent = 50;
    Sleeper z;
    // 2 of 4 asleep at night: the game's own skip runs at 30x; the mod adds the other 30x
    Step st = z.tick(clock_at(23, 30.0f), 2, 4, kTick, s);
    CHECK(st.add_game_ns == 30 * kTick);
    CHECK(st.speed == 60.0);
    // close to dawn: never past 06:00
    const Clock dawn = clock_at(6 - 1.0 / 3600, 30.0f);
    st = z.tick(dawn, 2, 4, kTick * 600, s);  // elapsed past the step limit: nothing
    CHECK(st.add_game_ns == 0);
    st = z.tick(dawn, 2, 4, kTick, s);
    CHECK(st.add_game_ns <= game_ns_between(dawn, dawn.tod_ns, 6 * kHourNs));
    // with 'everyone' the night is the game's business only
    s.who = Who::All;
    Sleeper all;
    CHECK(all.tick(clock_at(23, 60.0f), 4, 4, kTick, s).add_game_ns == 0);
}

TEST_CASE("sleep: /sleepers says who sleeps and what happens") {
    Settings s;
    CHECK(sleepers_text(0, 1, s, true, 12) ==
          "Nobody is asleep (1 player). Sleep by day to skip to 17:30 (everyone must be in bed).");
    CHECK(sleepers_text(1, 1, s, true, 12) == "1 of 1 asleep: the day passes 60x faster until 17:30.");
    CHECK(sleepers_text(1, 2, s, true, 12) == "1 of 2 asleep: not enough to skip the day (everyone must be in bed).");
    CHECK(sleepers_text(1, 1, s, false, 22) == "1 of 1 asleep (night: the game's own night skip decides).");
    CHECK(sleepers_text(1, 1, s, true, 17.8) == "1 of 1 asleep (evening: the game's own night skip decides).");
    CHECK(sleepers_text(0, 0, s, true, 12) == "Sleep Through the Day: no players");
    CHECK(hhmm(17.5) == "17:30");
    CHECK(hhmm(24.0) == "00:00");
}

// ---- the game side over a fake API ----
namespace {
EmfWorld* const kW = reinterpret_cast<EmfWorld*>(0x5000);
std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> g_comps;
std::vector<uint8_t> g_infos(16 * 0xA0, 0);
alignas(8) uint8_t g_daytime[0x70];
void* f_get(EmfWorld*, uint32_t hash, uint32_t entity, uint64_t* size) {
    auto it = g_comps.find({hash, entity});
    if (it == g_comps.end()) return nullptr;
    if (size) *size = it->second.size();
    return it->second.data();
}
void* f_singleton(EmfWorld*, const char* name) {
    if (std::strcmp(name, "BasicPlayerInfos") == 0) return g_infos.data();
    if (std::strcmp(name, "g38_daytime::Daytime") == 0) return g_daytime;
    return nullptr;
}
uint8_t f_kind(EmfWorld*) { return EMF_WORLD_SERVER; }
struct Rig {
    EmfApi api{};
    Rig() {
        g_comps.clear();
        std::fill(g_infos.begin(), g_infos.end(), uint8_t{0});
        std::memset(g_daytime, 0, sizeof g_daytime);
        api.version = EMF_API_VERSION;
        api.size = sizeof api;
        api.get_component = f_get;
        api.singleton = f_singleton;
        api.world_kind = f_kind;
        emf::api_ref() = &api;
    }
    ~Rig() { emf::api_ref() = nullptr; }
};
// A player in BasicPlayerInfos slot `index` with an Actor whose state has the sleep flag or not.
emf::gen::ecs::Actor& player(uint32_t index, uint32_t entity, bool asleep) {
    std::memcpy(g_infos.data() + index * 0xA0, &entity, 4);
    auto& v = g_comps[{emf::gen::ecs::Actor::kHash, entity}];
    v.assign(sizeof(emf::gen::ecs::Actor), 0);
    auto& a = *reinterpret_cast<emf::gen::ecs::Actor*>(v.data());
    if (asleep) a.currentState |= 1ull << 11;
    return a;
}
template <class T>
void put(size_t off, T v) { std::memcpy(g_daytime + off, &v, sizeof v); }
template <class T>
T get(size_t off) {
    T v;
    std::memcpy(&v, g_daytime + off, sizeof v);
    return v;
}
}  // namespace

TEST_CASE("sleep glue: players are counted as the game's night skip counts them (BasicPlayerInfos, Actor state flag 11)") {
    Rig r;
    emf::World w(kW);
    CHECK(count_sleepers(w).players == 0);
    player(0, 1, true);
    player(1, 2, false);
    std::memcpy(g_infos.data() + 2 * 0xA0, "\x07\x00\x00\x00", 4);  // a slot whose entity has no Actor: not counted
    auto n = count_sleepers(w);
    CHECK(n.players == 2);
    CHECK(n.sleeping == 1);
    // an action sequence that sets the flag (playback state bit 0: (state | setStateMask) & ~unsetStateMask)
    auto& a = player(1, 2, false);
    a.currentAction.playbackContext.state = 1;
    a.currentAction.playbackContext.setStateMask = 1ull << 11;
    CHECK(count_sleepers(w).sleeping == 2);
    // ... and one that clears it
    auto& b = player(0, 1, true);
    b.currentAction.playbackContext.state = 1;
    b.currentAction.playbackContext.unsetStateMask = 1ull << 11;
    CHECK(count_sleepers(w).sleeping == 1);
    // without the playback flag the masks do not count
    b.currentAction.playbackContext.state = 0;
    CHECK(count_sleepers(w).sleeping == 2);
}

TEST_CASE("sleep glue: the Daytime is read at its offsets; added game time moves the anchor and bumps the change counter") {
    Rig r;
    emf::World w(kW);
    put<int64_t>(0x10, 5 * kSec);          // anchor ECS time
    put<int64_t>(0x18, 700 * kSec);        // anchor game time
    put<float>(0x20, 1.0f);                // scale
    put<uint32_t>(0x24, 7);                // change counter
    put<int64_t>(0x28, 6 * kHourNs);
    put<int64_t>(0x30, 18 * kHourNs);
    put<int64_t>(0x38, 30 * kMin);
    put<int64_t>(0x40, 12 * kMin);
    put<int64_t>(0x48, 12 * kHourNs);
    const auto c = read_clock(w);
    REQUIRE(c);
    CHECK(c->tod_ns == 12 * kHourNs);
    CHECK(c->day_dur_ns == 30 * kMin);
    CHECK(c->scale == 1.0f);
    CHECK(sane(*c));
    CHECK(add_game_time(w, 59 * kTick));
    CHECK(get<int64_t>(0x18) == 700 * kSec + 59 * kTick);
    CHECK(get<uint32_t>(0x24) == 8u);
    CHECK(get<int64_t>(0x10) == 5 * kSec);  // the anchor's ECS time and the speed stay the game's
    CHECK(get<float>(0x20) == 1.0f);
    CHECK_FALSE(add_game_time(w, 0));
}

TEST_CASE("sleep glue: sleep.pretend (host, for tests) counts every player as asleep; off, the real count stands") {
    Count n;
    n.players = 3;
    n.sleeping = 1;
    CHECK(pretended(n, false).sleeping == 1);
    CHECK(pretended(n, true).sleeping == 3);
    CHECK(pretended(n, true).players == 3);
    Count none;
    CHECK(pretended(none, true).sleeping == 0);  // nobody to pretend with
}

TEST_CASE("sleep: someone still in bed from the night does not stop the others: a new sleeper by day re-arms the skip") {
    Settings s;  // everyone must sleep
    Sleeper z;
    CHECK(z.tick(clock_at(23), 1, 2, kTick, s).add_game_ns == 0);  // A lies down at night
    CHECK(z.phase() == Phase::Done);
    CHECK(z.tick(clock_at(9), 1, 2, kTick, s).add_game_ns == 0);   // morning: A still in bed, B up
    Step st = z.tick(clock_at(10), 2, 2, kTick, s);                 // B lies down by day: both asleep
    CHECK(z.phase() == Phase::DaySleep);
    CHECK(st.note == Step::Started);
    CHECK(st.add_game_ns == 59 * kTick);
    // a sleeper count that rises in the evening or at night does not re-arm
    Sleeper late;
    late.tick(clock_at(17.6), 1, 2, kTick, s);
    late.tick(clock_at(17.7), 2, 2, kTick, s);
    CHECK(late.phase() == Phase::Done);
}

TEST_CASE("sleep: a wake hour past nightfall stops at nightfall (the game's own night skip takes over there)") {
    Settings s;
    s.wake_hour = 21;
    Sleeper z;
    const Clock late = clock_at(18 - 10.0 / 3600);
    const Step st = z.tick(late, 1, 1, kTick, s);
    CHECK(st.add_game_ns == game_ns_between(late, late.tod_ns, 18 * kHourNs));
    CHECK(st.note == Step::Evening);
    CHECK(effective_wake(late, 21) == 18 * kHourNs);
    CHECK(effective_wake(late, 17.5) == hours_ns(17.5));
}

TEST_CASE("sleep glue: the change counter (players' clocks) is bumped at most every 250 ms, and when the evening comes") {
    CounterPace pace;
    CHECK(pace.bump(1000, false));
    CHECK_FALSE(pace.bump(1100, false));
    CHECK_FALSE(pace.bump(1249, false));
    CHECK(pace.bump(1250, false));
    CHECK(pace.bump(1260, true));  // the last step of a skip always
}

