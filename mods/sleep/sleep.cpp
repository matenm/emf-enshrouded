// sleep (Sleep Through the Day): an EMF mod. While players sleep in a bed by day, the day passes 60x faster until the
// evening (the wake hour, 17:30 by default); then the clock runs normally again. "Who has to sleep": everyone (the
// default), each sleeper a share (as the game's own night skip), or a percentage of the players. Runs where the world
// runs (the host or a dedicated server); players who join need nothing. /sleep.status for every player.
// The decisions and the clock maths (tested without the game) are sleep_logic.h; the game side is sleep_glue.h.
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "emf/emf.hpp"
#include "sleep_glue.h"
#include "sleep_logic.h"

namespace {
constexpr const char* kMod = "sleep";

struct Config {
    sleep::Settings s;
    bool notes = true;
};
std::mutex g_config_lock;
Config g_config;
std::atomic<bool> g_switched_off{false};

// The mod's own switch, as the Mods tab saved it (emf\config\sleep.ini, [sleep] enabled; the API has no getter).
bool switch_on() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring ini(exe);
    ini = ini.substr(0, ini.find_last_of(L"\\/")) + L"\\emf\\config\\sleep.ini";
    return GetPrivateProfileIntW(L"sleep", L"enabled", 0, ini.c_str()) != 0;
}

Config config() {
    std::lock_guard<std::mutex> l(g_config_lock);
    return g_config;
}
void read_settings() {
    Config c;
    c.s.wake_hour = emf::setting_float(kMod, "wake_hour", 17.5);
    c.s.who = sleep::who_of(emf::setting_text(kMod, "who", "all"));
    c.s.percent = static_cast<int>(emf::setting_int(kMod, "percent", 50));
    c.s.speed = emf::setting_float(kMod, "speed", 60.0);
    c.notes = emf::setting_bool(kMod, "notes", true);
    std::lock_guard<std::mutex> l(g_config_lock);
    g_config = c;
}

// ---- server tick state ----
sleep::Sleeper g_sleeper;
uint32_t g_session = 0;
int64_t g_last_ecs = 0;
bool g_told_insane = false;
bool g_known = true;  // a game build EMF knows (the Daytime's fields are checked there)
std::atomic<int> g_players{0}, g_sleeping{0};
bool g_pretend = false;
sleep::CounterPace g_pace;  // sleep.pretend: every player counts as asleep (for tests; off with each new session)

void note(const std::string& text, const Config& c) {
    EMF_LOG(kMod, "%s", text.c_str());
    if (c.notes) EMF_NOTIFY("%s", text.c_str());
}

void on_tick(emf::World w) {
    if (!w.is_server() || !g_known) return;
    const uint32_t session = emf::server_session();
    if (session != g_session) {  // another world, or a reload: a sleep of the old one means nothing here
        g_session = session;
        g_sleeper.reset();
        g_last_ecs = 0;
        g_pretend = false;
    }
    const int64_t now = w.ecs_time_ns();
    const int64_t elapsed = g_last_ecs && now > g_last_ecs ? now - g_last_ecs : 0;
    g_last_ecs = now;
    if (g_switched_off) return;
    const auto n = sleep::pretended(sleep::count_sleepers(w), g_pretend);
    g_players = n.players;
    g_sleeping = n.sleeping;
    if (!n.sleeping && g_sleeper.phase() == sleep::Phase::Awake) return;  // nobody sleeps: nothing to read
    const auto clock = sleep::read_clock(w);
    if (!clock || !sleep::sane(*clock)) {
        if (!g_told_insane) {
            g_told_insane = true;
            EMF_LOG(kMod, "the game's day clock reads implausible numbers (a game update?): Sleep Through the Day does nothing");
        }
        return;
    }
    const Config c = config();
    const auto step = g_sleeper.tick(*clock, n.sleeping, n.players, elapsed, c.s);
    if (step.add_game_ns > 0 &&
        !sleep::add_game_time(w, step.add_game_ns, g_pace.bump(GetTickCount64(), step.note == sleep::Step::Evening)))
        EMF_LOG(kMod, "the day clock could not be moved (see the core log)");
    const double wake = static_cast<double>(sleep::effective_wake(*clock, c.s.wake_hour)) / static_cast<double>(sleep::kHourNs);
    switch (step.note) {
        case sleep::Step::Started:
            note("Sleeping through the day: the clock runs " + std::to_string(static_cast<int>(step.speed + 0.5)) +
                     "x faster until " + sleep::hhmm(wake), c);
            break;
        case sleep::Step::Evening:
            note("Evening (" + sleep::hhmm(wake) + "): the clock runs normally again", c);
            break;
        case sleep::Step::Woke: note("Everyone is up: the clock runs normally again", c); break;
        default: break;
    }
}

void cmd_status(emf::World w, const std::vector<std::string>&) {
    const Config c = config();
    if (g_switched_off) {
        EMF_REPLY("Sleep Through the Day is switched off");
        return;
    }
    const auto n = sleep::pretended(sleep::count_sleepers(w), g_pretend);
    const auto clock = sleep::read_clock(w);
    const bool day = clock && sleep::sane(*clock) && sleep::is_day(*clock);
    const double hours = clock ? static_cast<double>(clock->tod_ns) / static_cast<double>(sleep::kHourNs) : 12.0;
    EMF_REPLY("%s", sleep::sleepers_text(n.sleeping, n.players, c.s, day, hours).c_str());
}
void cmd_pretend(emf::World, const std::vector<std::string>& a) {
    if (a.size() > 1 && a[1] != "on" && a[1] != "off") {
        EMF_REPLY("usage: sleep.pretend [on|off] (every player counts as asleep: for tests)");
        return;
    }
    g_pretend = a.size() > 1 ? a[1] == "on" : !g_pretend;
    EMF_REPLY("sleep.pretend %s: %s", g_pretend ? "on" : "off",
              g_pretend ? "every player counts as asleep until sleep.pretend off or the next world load" : "the real sleepers count");
}
}  // namespace

extern "C" __declspec(dllexport) bool emf_mod_load(const EmfApi* api) {
    if (!emf::init(api)) return false;
    if (!emf::has_v17()) {
        EMF_LOG(kMod, "needs EMF API v17 or newer");
        return false;
    }
    if (const auto b = emf::game_build(); b && !b->known) {
        g_known = false;
        EMF_LOG(kMod, "a game build EMF does not know: the day clock's layout is not checked there, so Sleep Through the Day stays idle");
    }
    read_settings();
    emf::on_setting_changed(kMod, [](emf::World, const std::string& key) {
        if (key == "enabled") g_switched_off = !switch_on();
        read_settings();
    });
    if (!emf::add_command("sleep.status", ": who is asleep, and whether the day is being skipped", cmd_status,
                          EMF_COMMAND_PLAYERS))
        EMF_LOG(kMod, "the command sleep.status could not be added (see the core log)");
    if (!emf::add_command("sleep.pretend", "[on|off]: every player counts as asleep (host, for tests)", cmd_pretend, 0))
        EMF_LOG(kMod, "the command sleep.pretend could not be added (see the core log)");
    emf::on_tick(on_tick);
    const Config c = config();
    EMF_LOG(kMod, "ready: sleeping by day skips to %s at %.0fx (%s)", sleep::hhmm(c.s.wake_hour).c_str(), c.s.speed,
            sleep::who_text(c.s.who));
    return true;
}
