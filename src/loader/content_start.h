#pragma once
// The content loader's start sequence (docs/engine_parts/content_loader.md), with every side effect injected so the
// tests can check the order. The licence rules it keeps:
//   1. emf_content.dll is loaded only on the held entry (the main thread, the loader lock free, before any game code),
//      before any chained DLL, EMF's core or a mod;
//   2. it is unloaded before they start; when it cannot be (a thread of its own still runs, the module is still there,
//      or the build does not come back), this start runs without the chained DLLs and without EMF's core, and says why
//      (a message box: the player gets no mods this time);
//   3. it is never loaded for a build when the fingerprint and the state say the shadow copy is current, nor after a
//      build with the same files failed (until a mod, the game or the DLL changes). It can still be loaded at such a
//      start, on the held entry and under rules 1 and 2, to draw the creature pictures mods ask for (portraits_plan.h);
//      never with [content] mode=off, on the dedicated server, or in Play without EMF mods.
//
// In DllMain: plan_content (the facts, the decision, the fingerprint and the state), then start_in_dllmain:
//   Off          today's order: the chained DLLs, the file hooks of the redirect spike's switches, the core
//   UseCurrent   the same, with the hooks sending the game's data opens to the shadow copy
//   KnownFailed  the same with the redirect forced off, and the core told (a build with these files failed before)
//   BuildHeld    only the hold of the exe's entry; everything else runs there (run_held_content). If the entry
//                cannot be held, content is off for this start and the order is today's.
// On the held entry, run_held_content: the splash, a state that is never current, the shadow and the build (on a
// worker thread while the splash is kept alive), the state, shadow.txt after a good build, then the chained DLLs, the
// hooks (redirect on only after a good build) and the core. The pictures come after the build's state is written (a
// pictures call never changes it), each call in a work of its own with its own time (draw_pictures), and with the
// attempt recorded as failed before the call, so that a crash or a hang there is remembered. Without a build the entry
// is held for them alone (run_held_portraits).
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "content_build.h"
#include "content_owner.h"
#include "content_portraits.h"
#include "portraits_plan.h"
#include "content_shadow.h"
#include "content_state.h"
#include "content_status_env.h"

namespace emf::loader {

enum class ContentPath { Off, UseCurrent, BuildHeld, KnownFailed };

struct ContentPlan {
    ContentPath path = ContentPath::Off;
    std::string why;          // the decision's reason
    bool emf_owns = false;    // EMF applies the content at this start (UseCurrent, BuildHeld or KnownFailed)
    bool mode_on = false;     // [content] mode=on: the core shows why content is off
    std::string fingerprint;  // when EMF owns it
    size_t state_mods = 0;    // UseCurrent: the earlier build's mods and time
    uint64_t state_ms = 0;
    std::vector<std::string> state_warnings;  // UseCurrent: what its mods warned (the core says it again)
    int attempts = 1;                    // BuildHeld: the how-many-th start in a row that begins this build
    std::optional<ContentState> failed;  // KnownFailed: the earlier failure with the same files
    // Pictures of creatures mods asked for that the cache lacks (portraits_plan.h; the loader fills it, never on the
    // dedicated server): drawn at this start, after the build or on a start held only for them (portraits_hold).
    std::vector<PortraitItem> portraits;
    bool portraits_hold = false;
};

// ---- the facts, gathered in the order of the checks: the first that says no ends it ----

struct FactsIo {
    std::function<ContentMode()> mode;
    ToolLook look;
    std::function<bool()> dll_present;
    std::function<ModScan()> scan_mods;
    std::function<BaseChoice()> base;
};

// Asks only what the decision needs: a start without emf_content.dll looks for the DLL, Kiln and another tool's
// dbghelp.dll
// and reads no DLL and no mod folder. Reading the chained DLLs' text comes after the mods, just before the base.
// `game`: the exe is the game client or the dedicated server.
inline ContentFacts collect_facts(const FactsIo& io, PlayOnce play, bool server, bool game = true) {
    ContentFacts f;
    f.server = server;
    f.play = play;
    f.game = game;
    f.mode = io.mode ? io.mode() : ContentMode::Auto;
    if (f.mode == ContentMode::Off || play == PlayOnce::NoMods || !game) return f;
    f.dll_present = io.dll_present && io.dll_present();
    f.other_tool = other_content_tool(io.look, f.dll_present, false);  // dbghelp.dll read only with the DLL there
    if (f.other_tool || !f.dll_present) return f;
    if (io.scan_mods) f.mods = io.scan_mods();
    if (f.mods.usable.empty()) return f;
    f.other_tool = chained_tool_by_text(io.look);
    if (f.other_tool) return f;
    if (io.base) f.base = io.base();
    return f;
}

// ---- in DllMain ----

struct PlanSteps {
    std::function<ContentFacts()> facts;
    std::function<std::string()> fingerprint;  // asked only when EMF owns the content
    std::function<std::optional<ContentState>()> read_state;
    // the shadow is whole and its record (shadow.txt) passes the redirect's own check (file_redirect.h); asked only for
    // a current state, so a "current" start is one the redirect will really take (null: not asked)
    std::function<bool()> record_ok;
    std::function<void(const std::string&)> log;
};

inline std::string seconds_text(uint64_t ms) {
    if (ms < 10000) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.1f s", static_cast<double>(ms) / 1000.0);
        return buf;
    }
    return std::to_string((ms + 500) / 1000) + " s";
}

inline std::string mods_text(size_t n) { return std::to_string(n) + (n == 1 ? " mod" : " mods"); }

// The status message of a good build whose mods warned: the first warning, and how many more ("" for none). At most
// kWarningsSummaryMaxBytes, well inside the status's message: a long first warning is cut between letters, the count kept.
constexpr size_t kWarningsSummaryMaxBytes = 400;
inline std::string warnings_summary(const std::vector<std::string>& warnings) {
    if (warnings.empty()) return {};
    const std::string more = warnings.size() > 1 ? " (and " + std::to_string(warnings.size() - 1) + " more)" : "";
    std::string first = warnings.front();
    if (first.size() + more.size() > kWarningsSummaryMaxBytes) {
        size_t n = kWarningsSummaryMaxBytes - more.size();
        while (n > 0 && (static_cast<unsigned char>(first[n]) & 0xC0) == 0x80) --n;  // not inside a UTF-8 letter
        first.resize(n);
    }
    return first + more;
}

constexpr int kMaxBuildAttempts = 2;  // starts in a row a build may stop the game in before it is left alone

inline ContentPlan plan_content(const PlanSteps& s) {
    ContentPlan plan;
    const ContentFacts facts = s.facts();
    plan.mode_on = facts.mode == ContentMode::On;
    const ContentDecision d = decide_content(facts);
    plan.why = d.why;
    if (facts.dll_present)  // the mods are looked at only then
        for (const auto& [folder, reason] : facts.mods.skipped)
            s.log("EML content: left out " + cio::to_utf8(folder) + ": " + reason);
    if (!d.emf_owns) {
        s.log("EML content: " + d.why);
        return plan;
    }
    plan.emf_owns = true;
    plan.fingerprint = s.fingerprint();
    const std::optional<ContentState> state = s.read_state();
    const bool same = state && !state->fingerprint.empty() && state->fingerprint == plan.fingerprint;
    const std::string build_needed = "EML content: " + d.why + "; a build is needed at this start (";
    if (state_current(state, plan.fingerprint)) {
        if (s.record_ok && !s.record_ok()) {
            plan.path = ContentPath::BuildHeld;
            s.log(build_needed + "the shadow copy or its record, shadow.txt, is missing or does not match the game's "
                                 "files)");
            return plan;
        }
        plan.path = ContentPath::UseCurrent;
        plan.state_mods = state->mods.size();
        plan.state_ms = state->ms;
        plan.state_warnings = state->warnings;
        s.log("EML content: the shadow copy is current (" + mods_text(state->mods.size()) + ", built " + state->built_at +
              "): emf_content.dll is not loaded");
        return plan;
    }
    if (same && state->code == kBuildRunning) {
        if (state->attempts < kMaxBuildAttempts) {
            plan.path = ContentPath::BuildHeld;
            plan.attempts = state->attempts + 1;
            s.log(build_needed + "the last build did not finish: the game stopped during it)");
            return plan;
        }
        plan.path = ContentPath::KnownFailed;
        plan.failed = state;
        plan.failed->message = "the build stopped the game " + std::to_string(state->attempts) +
                               " times in a row (see emf\\content.log)";
        s.log("EML content could not be built: " + plan.failed->message +
              "; EMF tries again when a mod, the game or emf_content.dll changes");
        return plan;
    }
    if (same) {  // a build with these files failed (or its result is unknown): not again at every start
        plan.path = ContentPath::KnownFailed;
        plan.failed = state;
        s.log("EML content could not be built at an earlier start with the same files: " + state->message +
              " (code " + std::to_string(state->code) + "); EMF tries again when a mod, the game or emf_content.dll "
              "changes");
        return plan;
    }
    plan.path = ContentPath::BuildHeld;
    s.log(build_needed + (!state ? "no earlier build" : "the game, the mods or emf_content.dll changed since the last build") +
          ")");
    return plan;
}

// The file hooks: the redirect spike's switches as emf_core.ini has them (content off), or the redirect to the shadow
// copy on (after a good build, or a current shadow) or forced off (EMF owns the content but has no good build).
enum class Hooks { Spike, ContentOn, ContentOff };

struct StartSteps {
    // DllMain
    std::function<bool()> hold_entry;  // holds the exe's entry for run_held_content; false (and says why) when it cannot
    // DllMain or the held entry
    std::function<void()> run_chain;
    std::function<void(Hooks)> hooks;
    std::function<void()> start_core;
    std::function<void(const ContentStatus&)> set_status;
    std::function<void(const std::string&)> log;
    std::function<void(const ContentState&)> write_state;
    // the held entry
    std::function<void(const std::string&)> splash_begin;
    std::function<void()> splash_end;
    // runs `work` (on a worker thread with a big stack while the splash is kept alive); false when it did not come
    // back in time
    std::function<bool(const std::function<void()>& work)> run_work;
    std::function<ShadowResult()> build_shadow;
    // load, build, unload (content_build.h), on the thread run_work gives; `abandoned`: never load once it is set
    std::function<BuildOutcome(const std::atomic<bool>& abandoned)> run_build;
    std::function<void(const std::string& record)> write_record;  // shadow.txt, after a good build
    std::function<void()> drop_record;  // removes shadow.txt: a shadow without a good build must never be redirected to
    std::function<bool()> dll_gone;     // emf_content.dll is not in the process
    std::function<void(const std::string&)> alert;  // a message box the player must see (OK only)
    std::function<std::string()> now;               // UTC, for built_at
    // draws `items` through emf_content.dll (content_portraits.h) on run_work's thread, from the shadow copy (`shadow`)
    // or the game's own files; null: no portraits
    std::function<PortraitsOutcome(const std::vector<PortraitItem>& items, bool shadow,
                                   const std::atomic<bool>& abandoned)>
        run_portraits;
    // remembers what a pictures call did for each item it asked for (portraits_attempts.h), on the loader's thread
    // after the call, also when it did not come back; null: nothing is remembered
    std::function<void(const std::vector<PortraitItem>& items, const PortraitsOutcome&)> record_portraits;
    // like run_work, for a pictures call, with a time of its own (draw_pictures); null: run_work
    std::function<bool(const std::function<void()>& work, uint32_t timeout_ms)> run_pictures_work;
};

inline std::string portraits_text(const PortraitsOutcome& o) {
    std::string t = "creature pictures: drew " + std::to_string(o.drawn.size());
    if (!o.current.empty()) t += ", " + std::to_string(o.current.size()) + " current";
    if (!o.failed.empty()) t += ", " + std::to_string(o.failed.size()) + " failed";
    t += " in " + seconds_text(o.call.ms);
    if (o.call.code != 0 && o.failed.empty()) t += " (" + o.call.message + ", code " + std::to_string(o.call.code) + ")";
    return t;
}

inline Hooks hooks_for(const ContentPlan& plan) {
    return plan.path == ContentPath::UseCurrent ? Hooks::ContentOn
           : plan.emf_owns                      ? Hooks::ContentOff  // the spike's switch never redirects there
                                                : Hooks::Spike;
}

// The start without a build: the chained DLLs, the file hooks for the plan's path, the core's status, the core.
inline void start_unheld(const ContentPlan& plan, const StartSteps& s, std::optional<ContentStatus> status) {
    std::optional<ContentState> mark;  // a failure the core is told of now: recorded as told
    if (status) {
        // given: the start that could not be held for a build
    } else if (plan.path == ContentPath::UseCurrent) {
        status = ContentStatus{"current", plan.state_mods, plan.state_ms, warnings_summary(plan.state_warnings)};
    } else if (plan.path == ContentPath::KnownFailed && plan.failed) {
        // the core shows the warning at every such start; the save backup only the first time it hears of it
        status = ContentStatus{plan.failed->reported ? "failed_again" : "failed", plan.failed->mods.size(), plan.failed->ms,
                               plan.failed->message};
        if (!plan.failed->reported) {
            mark = plan.failed;
            mark->reported = true;
        }
    } else if (plan.mode_on) {
        status = ContentStatus{"off", 0, 0, plan.why};
    }
    s.run_chain();
    s.hooks(hooks_for(plan));
    if (mark && s.write_state) s.write_state(*mark);
    if (status) s.set_status(*status);
    s.start_core();
}

// True when the entry is held: the rest runs there (run_held_content), nothing more here.
inline bool start_in_dllmain(ContentPlan& plan, const StartSteps& s) {
    std::optional<ContentStatus> status;
    if (plan.path == ContentPath::BuildHeld) {
        if (s.hold_entry()) return true;
        plan.path = ContentPath::Off;
        plan.why = "off for this start: the game's start could not be held for the build";
        s.log("EML content: " + plan.why);
        status = ContentStatus{"failed", 0, 0, "EMF could not hold the game's start to build the EML content"};
    } else if (!plan.portraits.empty() && s.run_portraits) {
        if (s.hold_entry()) {
            plan.portraits_hold = true;
            return true;
        }
        s.log("creature pictures: the game's start could not be held; they are drawn at a later start");
        plan.portraits.clear();
    }
    start_unheld(plan, s, status);
    return false;
}

// The pictures' own time: they never share the build's (a hang there must not touch the build's state).
constexpr uint32_t kPicturesBaseMs = 2 * 60 * 1000;
constexpr uint32_t kPicturesPerItemMs = 5 * 1000;
inline uint32_t pictures_timeout_ms(size_t items) { return kPicturesBaseMs + static_cast<uint32_t>(items) * kPicturesPerItemMs; }

// One pictures call in its own work with its own time. Before it, every key is recorded as failed ("the game stopped
// while it was drawn"): a crash, a kill or a hang that never comes back then leaves a record, and the next start does
// not hold again for the same keys; the real result replaces it after. The DLL still in the process afterwards counts
// as left loaded.
inline PortraitsOutcome draw_pictures(const std::vector<PortraitItem>& items, bool shadow, const StartSteps& s) {
    if (s.record_portraits) {
        PortraitsOutcome pending;
        pending.call.code = kBuildError;
        pending.call.message = "the game stopped while the pictures were drawn";
        s.record_portraits(items, pending);
    }
    struct Work {
        std::vector<PortraitItem> items;
        PortraitsOutcome out;
        std::atomic<bool> abandoned{false};
    };
    auto work = std::make_shared<Work>();  // the worker owns it: one that does not come back must not write a dead frame
    work->items = items;
    const auto job = [work, &s, shadow] { work->out = s.run_portraits(work->items, shadow, work->abandoned); };
    const uint32_t ms = pictures_timeout_ms(items.size());
    const bool done = s.run_pictures_work ? s.run_pictures_work(job, ms) : s.run_work(job);
    PortraitsOutcome out;
    if (done) {
        out = work->out;
    } else {
        work->abandoned = true;
        out.call.code = kBuildTimedOut;
        out.call.unloaded = false;
        out.call.message = "the pictures did not finish in " + std::to_string(ms / 1000) +
                           " seconds, so emf_content.dll may still run";
    }
    if (out.call.unloaded && s.dll_gone && !s.dll_gone()) out.call.unloaded = false;
    if (done && s.record_portraits) s.record_portraits(items, out);  // not done: the record before the call stays
    return out;
}


inline void log_pictures(const PortraitsOutcome& out, const StartSteps& s) {
    s.log(portraits_text(out));
    for (const auto& f : out.failed) s.log("creature pictures: " + f);
}

inline void pictures_left_dll(const PortraitsOutcome& out, const StartSteps& s) {
    s.log("creature pictures: emf_content.dll is still in the game (" + out.call.message +
          "): this start runs without the chained DLLs and without EMF's core");
    s.alert("EMF could not finish drawing creature pictures; the game starts without EMF's mods this time.\n\n" +
            out.call.message + "\n\nThe details are in emf\\portraits.log.");
}

// On the entry held only for portraits (no build at this start): draw them, then start as without a hold. The DLL
// must be gone before the chained DLLs and the core, as after a build; when it is not, the file hooks still go in
// (the shadow copy of a current start is applied, without EMF's core) and the chained DLLs and the core wait.
inline void run_held_portraits(const ContentPlan& plan, const StartSteps& s) {
    s.splash_begin("EMF is drawing pictures of creatures for mods (first start after a change)...");
    const PortraitsOutcome out = draw_pictures(plan.portraits, plan.path == ContentPath::UseCurrent, s);
    s.splash_end();
    log_pictures(out, s);
    if (!out.call.unloaded) {
        s.hooks(hooks_for(plan));
        pictures_left_dll(out, s);
        return;
    }
    start_unheld(plan, s, std::nullopt);
}

constexpr uint32_t kBuildTimeoutMs = 15 * 60 * 1000;

// On the held entry (BuildHeld).
inline void run_held_content(const ContentPlan& plan, const StartSteps& s) {
    if (plan.portraits_hold && plan.path != ContentPath::BuildHeld) {
        run_held_portraits(plan, s);
        return;
    }
    s.splash_begin("EMF is preparing EML content (first start after a change)...");
    ContentState state;
    state.fingerprint = plan.fingerprint;
    state.code = kBuildRunning;  // never current: a crash or a kill from here on leaves it
    state.message = "a build was running";
    state.attempts = plan.attempts;
    state.built_at = s.now();
    s.write_state(state);

    struct Work {
        ShadowResult shadow;
        BuildOutcome out;
        std::atomic<bool> abandoned{false};
    };
    // the worker owns its result too: one that does not come back in time must not write into a dead frame
    auto work = std::make_shared<Work>();
    const bool done = s.run_work([work, &s] {
        work->shadow = s.build_shadow();
        if (!work->shadow.ok) {
            work->out.code = kBuildShadowFailed;
            work->out.message = work->shadow.why;
            work->out.unloaded = true;  // never loaded
            return;
        }
        if (work->abandoned) return;  // the time is up: the DLL is never loaded after that
        work->out = s.run_build(work->abandoned);
    });
    BuildOutcome out;
    if (done) {
        out = work->out;
    } else {
        work->abandoned = true;
        out.code = kBuildTimedOut;
        out.unloaded = false;
        out.message = "the build did not finish in " + std::to_string(kBuildTimeoutMs / 60000) +
                      " minutes, so emf_content.dll may still run";
    }
    if (out.unloaded && s.dll_gone && !s.dll_gone()) {
        out.unloaded = false;
        out.message += (out.message.empty() ? "" : "; ") + std::string("emf_content.dll is still in the game");
    }
    const bool ok = out.unloaded && out.usable();
    state.code = out.code;
    state.message = out.message;
    state.mods = out.mods;
    state.errors = out.errors;
    state.warnings = out.warnings;
    state.ms = out.ms;
    state.built_at = s.now();
    state.unloaded = out.unloaded;
    state.truncated = out.truncated;
    state.attempts = 0;
    state.reported = out.unloaded;  // the core starts below and is told; without it, at the next start
    s.write_state(state);  // the build's own, final: written before the pictures, which never change it
    if (ok) s.write_record(work->shadow.record);
    if (!out.unloaded) {
        s.splash_end();
        s.drop_record();
        s.log("EML content: " + out.message + ": this start runs without the chained DLLs and without EMF's core (EML's "
              "code and the mods are never in the game together)");
        s.alert("EMF could not finish preparing EML content; the game starts without EMF's mods this time.\n\n" +
                out.message + "\n\nThe details are in emf\\content.log.");
        return;
    }
    // the pictures, after the build, with their own time: from the shadow when it is good, else the game's files
    std::optional<PortraitsOutcome> pictures;
    if (s.run_portraits && !plan.portraits.empty()) pictures = draw_pictures(plan.portraits, ok, s);
    s.splash_end();
    if (pictures) log_pictures(*pictures, s);
    if (ok) {
        s.log("EML content: built " + mods_text(out.mods.size()) + " in " + seconds_text(out.ms) +
              "; the game reads the shadow copy");
        for (const auto& w : out.warnings) s.log("EML content: a mod warned: " + w);
    } else {
        s.drop_record();
        s.log("EML content could not be built: " + out.message + " (code " + std::to_string(out.code) +
              "); the game starts without it (emf\\content.log has the details)");
        for (const auto& e : out.errors) s.log("EML content: " + e);
        for (const auto& w : out.warnings) s.log("EML content: a mod warned: " + w);
    }
    if (pictures && !pictures->call.unloaded) {
        // the build's state above stays the build's own; only this start goes without the chained DLLs and the core
        s.hooks(ok ? Hooks::ContentOn : Hooks::ContentOff);
        pictures_left_dll(*pictures, s);
        return;
    }
    s.run_chain();
    s.hooks(ok ? Hooks::ContentOn : Hooks::ContentOff);
    s.set_status(ok ? ContentStatus{"built", out.mods.size(), out.ms, warnings_summary(out.warnings)}
                    : ContentStatus{"failed", out.mods.size(), out.ms, out.message});
    s.start_core();
}

}  // namespace emf::loader
