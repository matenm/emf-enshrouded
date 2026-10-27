#pragma once
// sleep (Sleep Through the Day): the decisions and the clock maths, without the game (unit-tested).
//
// The game's day clock (g38_daytime::Daytime, docs/engine_parts/world_building_daytime.md) keeps an anchor: game time
// at an ECS time, and a speed (timeScale). gameTime(now) = anchorGameTime + (now - anchorEcsTime) * timeScale, and the
// time of day follows the game time: the day phase (dayStart..nightStart, 06:00-18:00) lasts dayDuration of game time
// (30 min by default), the night phase nightDuration (12 min). The game's own night skip (program
// server_nighttime_skip) raises timeScale to 60 * sleeping / players at night only and sets it back to 1 by day, every
// tick it differs; so this mod never touches timeScale (it would fight that program each tick). While players sleep by
// day it adds the extra game time to the anchor instead: (speed - timeScale) * the ECS time since the last step. The
// result is the same clock a timeScale of `speed` would give, with no fight and no dependence on the order of steps.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

namespace sleep {

constexpr int64_t kHourNs = 3600ll * 1000000000ll;
constexpr int64_t kDayNs = 24 * kHourNs;
constexpr int64_t kMaxStepNs = 200ll * 1000000;  // a longer gap between steps (a hitch, a load) adds nothing

// The Daytime numbers the mod reads (offsets in sleep_glue.h).
struct Clock {
    int64_t tod_ns = 0;          // +0x48 the time of day, 0..24 h
    int64_t day_start_ns = 0;    // +0x28 (06:00)
    int64_t night_start_ns = 0;  // +0x30 (18:00)
    int64_t day_dur_ns = 0;      // +0x38 game time the day phase lasts (30 min)
    int64_t night_dur_ns = 0;    // +0x40 game time the night phase lasts (12 min)
    float scale = 1.0f;          // +0x20 the game's speed (1 by day; the night skip raises it at night)
};

// Plausible numbers only (a game update that moved the fields reads nonsense: then the mod does nothing).
inline bool sane(const Clock& c) {
    return c.tod_ns >= 0 && c.tod_ns < kDayNs && c.day_start_ns > 0 && c.day_start_ns < c.night_start_ns &&
           c.night_start_ns < kDayNs && c.day_dur_ns >= 1000000000ll && c.day_dur_ns <= kDayNs &&
           c.night_dur_ns >= 1000000000ll && c.night_dur_ns <= kDayNs && c.scale >= 0.0f && c.scale <= 1000.0f;
}

// As the game's Daytime_isDaytime: dayStart <= tod < nightStart.
inline bool is_day(const Clock& c) { return c.tod_ns >= c.day_start_ns && c.tod_ns < c.night_start_ns; }

inline int64_t hours_ns(double h) { return static_cast<int64_t>(h * static_cast<double>(kHourNs)); }

// The hour the day races to: the wake hour, but never past nightfall (from there the game's own night skip decides).
inline int64_t effective_wake(const Clock& c, double wake_hour) { return (std::min)(hours_ns(wake_hour), c.night_start_ns); }

// Game time from the time of day `from` forward to `to` (wrapping past midnight), through the day and night phases.
inline int64_t game_ns_between(const Clock& c, int64_t from, int64_t to) {
    from = ((from % kDayNs) + kDayNs) % kDayNs;
    to = ((to % kDayNs) + kDayNs) % kDayNs;
    if (to < from) to += kDayNs;
    const long double day_tod = static_cast<long double>(c.night_start_ns - c.day_start_ns);
    const long double night_tod = static_cast<long double>(kDayNs) - day_tod;
    long double game = 0;
    int64_t t = from;
    while (t < to) {  // walk the phase boundaries
        const int64_t in_day = ((t % kDayNs) + kDayNs) % kDayNs;
        const int64_t base = t - in_day;
        const bool day = in_day >= c.day_start_ns && in_day < c.night_start_ns;
        int64_t edge;
        if (day) edge = base + c.night_start_ns;
        else if (in_day < c.day_start_ns) edge = base + c.day_start_ns;
        else edge = base + kDayNs + c.day_start_ns;
        const int64_t end = std::min(edge, to);
        const long double span = static_cast<long double>(end - t);
        game += day ? span * c.day_dur_ns / day_tod : span * c.night_dur_ns / night_tod;
        t = end;
    }
    return static_cast<int64_t>(game);
}

// Who has to be in bed (the `who` setting).
enum class Who { All, Share, Percent };
inline Who who_of(const std::string& v) { return v == "share" ? Who::Share : v == "percent" ? Who::Percent : Who::All; }

struct Settings {
    double wake_hour = 17.5;  // the day speeds up until this hour
    Who who = Who::All;
    int percent = 50;         // Who::Percent: this share of the players in bed gives the full speed (also at night)
    double speed = 60.0;      // how much faster the day passes (the game's night skip uses 60)
};

// The speed the sleepers ask for (0 = none): All: every player in bed; Share: speed * sleeping / players (as the game's
// night skip); Percent: at least `percent` % in bed.
inline double wanted_speed(const Settings& s, int sleeping, int players) {
    if (sleeping <= 0 || players <= 0) return 0.0;
    sleeping = std::min(sleeping, players);
    switch (s.who) {
        case Who::All: return sleeping == players ? s.speed : 0.0;
        case Who::Share: return s.speed * sleeping / players;
        case Who::Percent: return sleeping * 100 >= s.percent * players ? s.speed : 0.0;
    }
    return 0.0;
}

// The sleep in progress (one for the world: it starts when the first player lies down and ends when nobody sleeps).
enum class Phase {
    Awake,     // nobody sleeps
    DaySleep,  // a sleep that began by day before the wake hour: the day speeds up
    Done,      // this sleep reached the evening, or began at night or after the wake hour: nothing more until everyone
               // is up (so someone who stays in bed is never carried through day after day), or until another player
               // lies down by day before the wake hour (one still in bed from the night does not block the others)
};

struct Step {
    int64_t add_game_ns = 0;  // game time to add to the clock's anchor now
    double speed = 0;         // the speed now (0: normal)
    enum Note { None, Started, Evening, Woke } note = None;
};

class Sleeper {
public:
    // One server step: `elapsed_ns` = ECS time since the last step (0 on the first).
    Step tick(const Clock& c, int sleeping, int players, int64_t elapsed_ns, const Settings& s) {
        Step out;
        if (elapsed_ns < 0 || elapsed_ns > kMaxStepNs) elapsed_ns = 0;
        if (sleeping <= 0) {
            if (phase_ == Phase::DaySleep && announced_) out.note = Step::Woke;
            phase_ = Phase::Awake;
            announced_ = false;
            last_sleeping_ = 0;
            return out;
        }
        const int64_t wake = effective_wake(c, s.wake_hour);
        const bool before_wake = is_day(c) && c.tod_ns < wake;
        if (phase_ == Phase::Awake) phase_ = before_wake ? Phase::DaySleep : Phase::Done;
        else if (phase_ == Phase::Done && sleeping > last_sleeping_ && before_wake) {  // someone lay down by day
            phase_ = Phase::DaySleep;
            announced_ = false;
        }
        last_sleeping_ = sleeping;
        const double want = wanted_speed(s, sleeping, players);
        if (phase_ == Phase::DaySleep) {
            if (!is_day(c) || c.tod_ns >= wake) {
                phase_ = Phase::Done;
                if (announced_) out.note = Step::Evening;
                return out;
            }
            if (want <= c.scale) return out;  // not enough of them in bed yet
            const int64_t left = game_ns_between(c, c.tod_ns, wake);
            int64_t add = static_cast<int64_t>((want - c.scale) * static_cast<double>(elapsed_ns));
            out.speed = want;
            if (!announced_) {
                announced_ = true;
                out.note = Step::Started;
            }
            if (add >= left) {  // this step reaches the wake hour
                add = left;
                phase_ = Phase::Done;
                out.note = Step::Evening;
            }
            out.add_game_ns = std::max<int64_t>(add, 0);
            return out;
        }
        // Done: by night, Percent tops the game's own night skip up to the full speed (until dawn)
        if (s.who == Who::Percent && !is_day(c) && want > c.scale) {
            const int64_t left = game_ns_between(c, c.tod_ns, c.day_start_ns);
            out.add_game_ns = std::min<int64_t>(static_cast<int64_t>((want - c.scale) * static_cast<double>(elapsed_ns)), left);
            out.speed = want;
        }
        return out;
    }
    Phase phase() const { return phase_; }
    void reset() {
        phase_ = Phase::Awake;
        announced_ = false;
        last_sleeping_ = 0;
    }

private:
    Phase phase_ = Phase::Awake;
    bool announced_ = false;  // Started was noted for this sleep
    int last_sleeping_ = 0;   // sleepers at the last step (a rise by day re-arms a finished sleep)
};

// "17:30"
inline std::string hhmm(double hours) {
    int m = static_cast<int>(hours * 60.0 + 0.5);
    m = ((m % 1440) + 1440) % 1440;
    char b[8];
    std::snprintf(b, sizeof b, "%02d:%02d", m / 60, m % 60);
    return b;
}

inline const char* who_text(Who w) {
    switch (w) {
        case Who::All: return "everyone must be in bed";
        case Who::Share: return "each sleeper speeds it up";
        case Who::Percent: return "enough players in bed";
    }
    return "";
}

// The answer to /sleepers.
inline std::string sleepers_text(int sleeping, int players, const Settings& s, bool day, double tod_hours) {
    char b[200];
    if (players <= 0) return "Sleep Through the Day: no players";
    const double want = wanted_speed(s, sleeping, players);
    const char* noun = players == 1 ? "player" : "players";
    if (sleeping <= 0) {
        std::snprintf(b, sizeof b, "Nobody is asleep (%d %s). Sleep by day to skip to %s (%s).", players, noun,
                      hhmm(s.wake_hour).c_str(), who_text(s.who));
    } else if (day && tod_hours < s.wake_hour && want > 1.0) {
        std::snprintf(b, sizeof b, "%d of %d asleep: the day passes %.0fx faster until %s.", sleeping, players, want,
                      hhmm(s.wake_hour).c_str());
    } else if (day && tod_hours < s.wake_hour) {
        std::snprintf(b, sizeof b, "%d of %d asleep: not enough to skip the day (%s).", sleeping, players, who_text(s.who));
    } else {
        std::snprintf(b, sizeof b, "%d of %d asleep (%s: the game's own night skip decides).", sleeping, players,
                      day ? "evening" : "night");
    }
    return b;
}

}  // namespace sleep
