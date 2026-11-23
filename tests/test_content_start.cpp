// The content loader's start sequence (src/loader/content_start.h), with recording fakes: what runs in DllMain and
// what runs on the held entry, in which order. The licence rules: emf_content.dll is loaded only on the held entry
// before any chained DLL or EMF's core, it is unloaded before they start (or they do not start), and it is not
// loaded at all when the shadow copy is current.
#include <doctest.h>

#include <windows.h>

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "loader/content_splash.h"
#include "loader/content_start.h"

using emf::loader::ContentPath;
using emf::loader::Hooks;

namespace {
struct Recorder {
    std::vector<std::string> calls;
    std::vector<std::string> log;
    std::vector<emf::loader::ContentState> states;
    void add(const std::string& c) { calls.push_back(c); }
    std::string joined() const {
        std::string s;
        for (const auto& c : calls) s += (s.empty() ? "" : ", ") + c;
        return s;
    }
    bool logged(const std::string& part) const {
        for (const auto& l : log)
            if (l.find(part) != std::string::npos) return true;
        return false;
    }
};

const char* hooks_name(Hooks h) {
    switch (h) {
        case Hooks::Spike: return "hooks(spike)";
        case Hooks::ContentOn: return "hooks(content on)";
        default: return "hooks(content off)";
    }
}

// Everything there for a build: auto, the DLL, one mod, a good base, the client.
emf::loader::ContentFacts owned_facts() {
    emf::loader::ContentFacts f;
    f.dll_present = true;
    emf::loader::EmlMod m;
    m.folder = L"beasts";
    m.id = "beasts";
    m.version = "1.0.0";
    f.mods.usable.push_back(m);
    f.mods.skipped.emplace_back(L"rt", "needs EML's runtime loader, which EMF does not run");
    f.base.ok = true;
    f.base.dat_count = 55;
    f.base.why = "the base is enshrouded.kfc (55 containers)";
    return f;
}

emf::loader::ContentState good_state(const std::string& fp) {
    emf::loader::ContentState s;
    s.fingerprint = fp;
    s.code = 0;
    s.mods = {"beasts 1.0.0"};
    s.ms = 28000;
    s.built_at = "2026-11-01T20:00:00Z";
    return s;
}

emf::loader::PlanSteps plan_steps(Recorder& r, const emf::loader::ContentFacts& facts,
                                  std::optional<emf::loader::ContentState> state) {
    emf::loader::PlanSteps p;
    p.facts = [&r, facts] { return r.add("facts"), facts; };
    p.fingerprint = [&r] { return r.add("fingerprint"), std::string("sha256:now"); };
    p.read_state = [&r, state] { return r.add("read_state"), state; };
    p.log = [&r](const std::string& line) { r.log.push_back(line); };
    return p;
}

emf::loader::BuildOutcome built_ok() {
    emf::loader::BuildOutcome o;
    o.code = 0;
    o.message = "built";
    o.mods = {"beasts 1.0.0"};
    o.ms = 1500;
    return o;
}

emf::loader::StartSteps start_steps(Recorder& r, bool holdable = true, emf::loader::BuildOutcome outcome = built_ok(),
                                    bool shadow_ok = true, bool finishes = true) {
    emf::loader::StartSteps s;
    s.hold_entry = [&r, holdable] { return r.add("hold"), holdable; };
    s.run_chain = [&r] { r.add("chain"); };
    s.hooks = [&r](Hooks h) { r.add(hooks_name(h)); };
    s.start_core = [&r] { r.add("core"); };
    s.set_status = [&r](const emf::loader::ContentStatus& st) {
        r.add("status(" + st.kind + ")");
        r.log.push_back("status " + st.kind + " " + std::to_string(st.mods) + " " + std::to_string(st.ms) + " " + st.message);
    };
    s.log = [&r](const std::string& line) { r.log.push_back(line); };
    s.splash_begin = [&r](const std::string&) { r.add("splash"); };
    s.splash_end = [&r] { r.add("splash end"); };
    s.run_work = [&r, finishes](const std::function<void()>& work) {
        if (!finishes) return false;  // the work never came back in time
        work();
        return true;
    };
    s.build_shadow = [&r, shadow_ok] {
        r.add("build_shadow");
        emf::loader::ShadowResult res;
        res.ok = shadow_ok;
        res.why = shadow_ok ? "" : "the game folder's drive does not allow hard links";
        return res;
    };
    s.run_build = [&r, outcome](const std::atomic<bool>&) { return r.add("run_build"), outcome; };
    s.write_state = [&r](const emf::loader::ContentState& st) {
        r.add("write_state");
        r.states.push_back(st);
    };
    s.write_record = [&r](const std::string&) { r.add("write_record"); };
    s.drop_record = [&r] { r.add("drop_record"); };
    s.dll_gone = [] { return true; };
    s.alert = [&r](const std::string& text) {
        r.add("alert");
        r.log.push_back("alert " + text);
    };
    s.now = [] { return std::string("2026-11-01T21:00:00Z"); };
    return s;
}
}  // namespace

TEST_CASE("content start: the status text the core reads") {
    emf::loader::ContentStatus st;
    st.kind = "failed";
    st.mods = 3;
    st.ms = 28000;
    st.message = "EML could not\tpatch\r\nthe bear";
    const auto back = emf::loader::parse_content_status(emf::loader::content_status_text(st));
    REQUIRE(back.has_value());
    CHECK(back->kind == "failed");
    CHECK(back->mods == 3);
    CHECK(back->ms == 28000);
    CHECK(back->message == "EML could not patch the bear");  // one line, no tabs
    CHECK_FALSE(emf::loader::parse_content_status("").has_value());
    CHECK_FALSE(emf::loader::parse_content_status("built").has_value());
}

TEST_CASE("content start: off leaves today's order") {
    Recorder r;
    auto facts = owned_facts();
    facts.dll_present = false;
    auto plan = emf::loader::plan_content(plan_steps(r, facts, std::nullopt));
    CHECK(plan.path == ContentPath::Off);
    CHECK(r.joined() == "facts");  // no fingerprint, no state read
    CHECK(r.log == std::vector<std::string>{"EML content: off (emf_content.dll is not installed)"});
    r.calls.clear();
    auto steps = start_steps(r);
    CHECK_FALSE(emf::loader::start_in_dllmain(plan, steps));
    CHECK(r.joined() == "chain, hooks(spike), core");  // no hold; no status unless mode=on
    plan.mode_on = true;
    r.calls.clear();
    emf::loader::start_in_dllmain(plan, steps);
    CHECK(r.joined() == "chain, hooks(spike), status(off), core");
}

TEST_CASE("content start: current shadow redirects without loading the dll") {
    Recorder r;
    auto plan = emf::loader::plan_content(plan_steps(r, owned_facts(), good_state("sha256:now")));
    CHECK(plan.path == ContentPath::UseCurrent);
    CHECK(r.logged("the shadow copy is current"));
    r.calls.clear();
    auto steps = start_steps(r);
    CHECK_FALSE(emf::loader::start_in_dllmain(plan, steps));
    CHECK(r.joined() == "chain, hooks(content on), status(current), core");  // no hold, no build_shadow, no run_build
    CHECK(r.logged("status current 1 28000"));  // the state's mods and build time, for the core's line
}

TEST_CASE("content start: a state from other files, or none, builds again") {
    auto failed_before = good_state("sha256:before");
    failed_before.code = 2;
    for (const auto& state : {std::optional<emf::loader::ContentState>(), std::optional(good_state("sha256:before")),
                              std::optional(failed_before)}) {
        Recorder r;
        const auto plan = emf::loader::plan_content(plan_steps(r, owned_facts(), state));
        CHECK(plan.path == ContentPath::BuildHeld);
        CHECK(plan.fingerprint == "sha256:now");
        CHECK(plan.attempts == 1);
        CHECK(r.joined() == "facts, fingerprint, read_state");
        CHECK(r.logged("left out rt: needs EML's runtime loader"));
    }
}

TEST_CASE("content start: a build that failed with the same files is not tried again at every start") {
    auto failed = good_state("sha256:now");
    failed.code = 2;
    failed.message = "EML could not patch the bear";
    Recorder r;
    auto plan = emf::loader::plan_content(plan_steps(r, owned_facts(), failed));
    CHECK(plan.path == ContentPath::KnownFailed);
    CHECK(r.logged("EML content could not be built at an earlier start with the same files: EML could not patch the bear"));
    // the core was told at that start (and backed up the saves): now the warning only, no build, no redirect
    failed.reported = true;
    plan = emf::loader::plan_content(plan_steps(r, owned_facts(), failed));
    r.calls.clear();
    auto steps = start_steps(r);
    CHECK_FALSE(emf::loader::start_in_dllmain(plan, steps));
    CHECK(r.joined() == "chain, hooks(content off), status(failed_again), core");
    // a failure the core never heard of (the DLL stayed loaded, so the core did not start): told now, once
    failed.reported = false;
    failed.unloaded = false;
    plan = emf::loader::plan_content(plan_steps(r, owned_facts(), failed));
    REQUIRE(plan.path == ContentPath::KnownFailed);
    r.calls.clear();
    r.states.clear();
    emf::loader::start_in_dllmain(plan, steps);
    CHECK(r.joined() == "chain, hooks(content off), write_state, status(failed), core");
    CHECK(r.states.at(0).reported);
    CHECK(r.states.at(0).fingerprint == "sha256:now");
}

TEST_CASE("content start: a build the game stopped in is tried once more, then left alone") {
    auto running = good_state("sha256:now");
    running.code = emf::loader::kBuildRunning;
    running.attempts = 1;
    Recorder r;
    auto plan = emf::loader::plan_content(plan_steps(r, owned_facts(), running));
    CHECK(plan.path == ContentPath::BuildHeld);
    CHECK(plan.attempts == 2);
    CHECK(r.logged("the last build did not finish"));
    running.attempts = 2;
    plan = emf::loader::plan_content(plan_steps(r, owned_facts(), running));
    CHECK(plan.path == ContentPath::KnownFailed);
    CHECK(r.logged("stopped the game"));
}

TEST_CASE("content start: build runs before chain and core") {
    Recorder r;
    auto plan = emf::loader::plan_content(plan_steps(r, owned_facts(), std::nullopt));
    REQUIRE(plan.path == ContentPath::BuildHeld);
    r.calls.clear();
    auto steps = start_steps(r);
    CHECK(emf::loader::start_in_dllmain(plan, steps));
    CHECK(r.joined() == "hold");  // nothing else in DllMain: no chained DLL, no core
    r.calls.clear();
    emf::loader::run_held_content(plan, steps);
    // a state that is never current before anything is touched (a crash leaves it), shadow.txt only after a good build
    CHECK(r.joined() == "splash, write_state, build_shadow, run_build, write_state, write_record, splash end, chain, "
                        "hooks(content on), status(built), core");
    REQUIRE(r.states.size() == 2);
    CHECK(r.states[0].fingerprint == "sha256:now");
    CHECK(r.states[0].code == emf::loader::kBuildRunning);
    CHECK(r.states[0].attempts == 1);
    CHECK(r.states[1].fingerprint == "sha256:now");
    CHECK(r.states[1].code == 0);
    CHECK(r.states[1].reported);
    CHECK(r.states[1].built_at == "2026-11-01T21:00:00Z");
    CHECK(r.logged("EML content: built 1 mod in 1.5 s"));
    CHECK(r.logged("status built 1 1500"));
}

// A creature pack caught its own failure and only logged it: the build said "up to date" and nothing reached the
// player. Its warning now goes into the reply, the state, the loader's log and the core's status.
TEST_CASE("content start: a build whose mods warned keeps the warnings, logs each, and the status carries a summary") {
    auto warned = built_ok();
    warned.warnings = {"emf_beastmaster_bear: Bear: not added: out of references", "emf_snakes: Grass Snake: not added"};
    Recorder r;
    auto plan = emf::loader::plan_content(plan_steps(r, owned_facts(), std::nullopt));
    REQUIRE(plan.path == ContentPath::BuildHeld);
    auto steps = start_steps(r, true, warned);
    CHECK(emf::loader::start_in_dllmain(plan, steps));
    emf::loader::run_held_content(plan, steps);
    REQUIRE(r.states.size() == 2);
    CHECK(r.states[1].code == 0);  // still a good build: the rest of the content is in
    CHECK(r.states[1].warnings == warned.warnings);
    CHECK(r.joined().find("hooks(content on), status(built)") != std::string::npos);
    CHECK(r.logged("EML content: a mod warned: emf_beastmaster_bear: Bear: not added: out of references"));
    CHECK(r.logged("EML content: a mod warned: emf_snakes: Grass Snake: not added"));
    CHECK(r.logged("status built 1 1500 emf_beastmaster_bear: Bear: not added: out of references (and 1 more)"));
}

TEST_CASE("content start: a current start sends the earlier build's warning again (the creature is still missing)") {
    auto state = good_state("sha256:now");
    state.warnings = {"emf_beastmaster_bear: Bear: not added"};
    Recorder r;
    auto plan = emf::loader::plan_content(plan_steps(r, owned_facts(), state));
    REQUIRE(plan.path == ContentPath::UseCurrent);
    auto steps = start_steps(r);
    CHECK_FALSE(emf::loader::start_in_dllmain(plan, steps));
    CHECK(r.logged("status current 1 28000 emf_beastmaster_bear: Bear: not added"));
}

TEST_CASE("content build: the reply's mod_warnings are read as '<id>: <warning>'") {
    emf::loader::BuildOutcome out;
    REQUIRE(emf::loader::detail::read_reply(
        R"({"code": 0, "message": "up to date", "mod_warnings": [{"id": "bear", "warning": "Bear: not added"},)"
        R"( {"id": 5, "warning": "no id"}, {"warning": "x"}]})",
        out));
    REQUIRE(out.warnings.size() == 3);
    CHECK(out.warnings[0] == "bear: Bear: not added");
    CHECK(out.warnings[1] == "?: no id");
    CHECK(out.warnings[2] == "?: x");
    emf::loader::BuildOutcome none;
    REQUIRE(emf::loader::detail::read_reply(R"({"code": 0, "message": "up to date"})", none));
    CHECK(none.warnings.empty());
}

TEST_CASE("content state: the warnings are written and read back") {
    auto st = good_state("sha256:now");
    st.warnings = {"bear: Bear: not added", "a: \"quoted\""};
    const auto back = emf::loader::parse_state(emf::loader::state_json(st));
    REQUIRE(back.has_value());
    CHECK(back->warnings == st.warnings);
    const auto old = emf::loader::parse_state(R"({"fingerprint": "f", "code": 0})");  // a state from before
    REQUIRE(old.has_value());
    CHECK(old->warnings.empty());
}

TEST_CASE("content warnings: the summary is the first warning and how many more") {
    CHECK(emf::loader::warnings_summary({}).empty());
    CHECK(emf::loader::warnings_summary({"a: b"}) == "a: b");
    CHECK(emf::loader::warnings_summary({"a: b", "c: d", "e: f"}) == "a: b (and 2 more)");
}

// The status message holds 512 bytes: a long first warning in letters of 2 bytes each lost the count at its end.
TEST_CASE("content warnings: a long first warning is cut between letters so the count still fits") {
    std::string first = "bear: ";
    for (int i = 0; i < 300; ++i) first += "\xc3\xa9";  // 600 bytes of letters
    const std::string s = emf::loader::warnings_summary({first, "c: d"});
    CHECK(s.size() <= emf::loader::kWarningsSummaryMaxBytes);
    CHECK(s.size() + 64 < emf::loader::kContentStatusMaxMessage);  // room to spare in the status
    CHECK(s.substr(s.size() - std::string(" (and 1 more)").size()) == " (and 1 more)");
    const std::string kept = s.substr(0, s.size() - std::string(" (and 1 more)").size());
    CHECK(kept.rfind("bear: ", 0) == 0);
    CHECK((static_cast<unsigned char>(kept.back()) & 0xC0) != 0xC0);  // not the first byte of a letter left alone
    CHECK((kept.size() - 6) % 2 == 0);                                  // whole two-byte letters
}

TEST_CASE("content start: a failed build logs its mods' warnings too") {
    auto failed = built_ok();
    failed.code = 2;
    failed.message = "EML could not patch the bear";
    failed.warnings = {"emf_beastmaster_bear: Bear: not added"};
    Recorder r;
    auto plan = emf::loader::plan_content(plan_steps(r, owned_facts(), std::nullopt));
    auto steps = start_steps(r, true, failed);
    CHECK(emf::loader::start_in_dllmain(plan, steps));
    emf::loader::run_held_content(plan, steps);
    CHECK(r.logged("EML content: a mod warned: emf_beastmaster_bear: Bear: not added"));
}

TEST_CASE("content start: failed build starts chain and core without redirect") {
    auto failed = built_ok();
    failed.code = 2;
    failed.message = "EML could not patch the bear";
    Recorder r;
    auto plan = emf::loader::plan_content(plan_steps(r, owned_facts(), std::nullopt));
    auto steps = start_steps(r, true, failed);
    r.calls.clear();
    emf::loader::run_held_content(plan, steps);
    CHECK(r.joined() == "splash, write_state, build_shadow, run_build, write_state, splash end, drop_record, chain, "
                        "hooks(content off), status(failed), core");
    CHECK(r.states.at(1).code == 2);
    CHECK(r.states.at(1).reported);  // the core is told at this start
    CHECK(r.logged("EML content could not be built: EML could not patch the bear"));
    // a cut reply keeps code 0 but its result is unknown: no redirect either
    auto cut = built_ok();
    cut.truncated = true;
    Recorder c;
    auto cut_steps = start_steps(c, true, cut);
    emf::loader::run_held_content(plan, cut_steps);
    CHECK(c.joined().find("hooks(content off)") != std::string::npos);
    CHECK(c.states.at(1).truncated);
    // the shadow could not be made: the DLL is never loaded
    Recorder s;
    auto shadow_steps = start_steps(s, true, built_ok(), false);
    emf::loader::run_held_content(plan, shadow_steps);
    CHECK(s.joined() == "splash, write_state, build_shadow, write_state, splash end, drop_record, chain, "
                        "hooks(content off), status(failed), core");
    CHECK(s.states.at(1).code == emf::loader::kBuildShadowFailed);
    CHECK(s.states.at(1).message.find("hard links") != std::string::npos);
}

TEST_CASE("content start: dll not unloaded starts neither chain nor core") {
    auto stuck = built_ok();
    stuck.unloaded = false;
    stuck.message = "1 thread started by emf_content.dll still run, so it stays loaded";
    Recorder r;
    auto plan = emf::loader::plan_content(plan_steps(r, owned_facts(), std::nullopt));
    auto steps = start_steps(r, true, stuck);
    r.calls.clear();
    emf::loader::run_held_content(plan, steps);
    CHECK(r.joined() == "splash, write_state, build_shadow, run_build, write_state, splash end, drop_record, alert");
    CHECK(r.logged("without the chained DLLs and without EMF's core"));
    CHECK(r.logged("alert EMF could not finish preparing EML content"));  // the player sees why EMF's mods are missing
    CHECK_FALSE(r.states.at(1).unloaded);
    CHECK_FALSE(r.states.at(1).reported);  // the core does not start: it is told at the next start
}

TEST_CASE("content start: the DLL still in the process before the chained DLLs: neither starts") {
    Recorder r;
    auto plan = emf::loader::plan_content(plan_steps(r, owned_facts(), std::nullopt));
    auto steps = start_steps(r);
    steps.dll_gone = [] { return false; };  // the outcome said unloaded, but the module is still there
    r.calls.clear();
    emf::loader::run_held_content(plan, steps);
    CHECK(r.joined() == "splash, write_state, build_shadow, run_build, write_state, splash end, drop_record, alert");
    CHECK_FALSE(r.states.at(1).unloaded);
}

TEST_CASE("content start: a build that does not come back in time starts neither chain nor core") {
    Recorder r;
    auto plan = emf::loader::plan_content(plan_steps(r, owned_facts(), std::nullopt));
    auto steps = start_steps(r, true, built_ok(), true, false);
    r.calls.clear();
    emf::loader::run_held_content(plan, steps);
    CHECK(r.joined() == "splash, write_state, write_state, splash end, drop_record, alert");
    CHECK(r.logged("did not finish"));
    CHECK(r.states.at(1).code == emf::loader::kBuildTimedOut);
    CHECK_FALSE(r.states.at(1).unloaded);
}

TEST_CASE("content start: a build that comes back too late never loads the DLL") {
    Recorder r;
    auto plan = emf::loader::plan_content(plan_steps(r, owned_facts(), std::nullopt));
    auto steps = start_steps(r);
    std::function<void()> late;
    steps.run_work = [&late](const std::function<void()>& work) {
        late = work;  // the worker is still on its way when the time is up
        return false;
    };
    r.calls.clear();
    emf::loader::run_held_content(plan, steps);
    REQUIRE(late);
    late();  // the worker goes on after the game started
    CHECK(r.joined().find("run_build") == std::string::npos);
}

TEST_CASE("content start: client entry not holdable turns content off") {
    Recorder r;
    auto plan = emf::loader::plan_content(plan_steps(r, owned_facts(), std::nullopt));
    auto steps = start_steps(r, false);
    r.calls.clear();
    CHECK_FALSE(emf::loader::start_in_dllmain(plan, steps));
    // the player's EML mods are missing at this start: the core warns and backs up the saves, as after a failed build
    CHECK(r.joined() == "hold, chain, hooks(content off), status(failed), core");
    CHECK(plan.path == ContentPath::Off);
    CHECK(r.logged("EML content: off for this start"));
}

TEST_CASE("content start: facts stop at the first no (a start without emf_content.dll reads no DLL and no mod)") {
    std::vector<std::string> asked;
    emf::loader::FactsIo io;
    io.mode = [&] { return asked.push_back("mode"), emf::loader::ContentMode::Auto; };
    io.look.exists = [&](const std::wstring& name) { return asked.push_back("exists " + emf::loader::cio::to_utf8(name)), false; };
    io.look.tool_name = [&](const std::wstring& name) {
        return asked.push_back("read " + emf::loader::cio::to_utf8(name)), std::string();
    };
    io.look.chain = {L"creative.dll"};
    bool dll = false;
    io.dll_present = [&] { return asked.push_back("dll"), dll; };
    io.scan_mods = [&] { return asked.push_back("mods"), owned_facts().mods; };
    io.base = [&] { return asked.push_back("base"), owned_facts().base; };

    auto f = emf::loader::collect_facts(io, emf::loader::PlayOnce::Normal, false);
    CHECK_FALSE(emf::loader::decide_content(f).emf_owns);
    std::string seen;
    for (const auto& a : asked) seen += a + "; ";
    CHECK(seen == "mode; dll; exists kiln\\kiln.json; exists dbghelp.dll; ");

    asked.clear();
    dll = true;
    f = emf::loader::collect_facts(io, emf::loader::PlayOnce::Normal, false);
    CHECK(emf::loader::decide_content(f).emf_owns);
    seen.clear();
    for (const auto& a : asked) seen += a + "; ";
    CHECK(seen == "mode; dll; exists kiln\\kiln.json; exists dbghelp.dll; mods; read creative.dll; base; ");

    asked.clear();
    f = emf::loader::collect_facts(io, emf::loader::PlayOnce::NoMods, false);
    CHECK(asked == std::vector<std::string>{"mode"});
    asked.clear();
    f = emf::loader::collect_facts(io, emf::loader::PlayOnce::Normal, true);  // the server: the same questions
    CHECK(f.server);
    CHECK(emf::loader::decide_content(f).emf_owns);
    seen.clear();
    for (const auto& a : asked) seen += a + "; ";
    CHECK(seen == "mode; dll; exists kiln\\kiln.json; exists dbghelp.dll; mods; read creative.dll; base; ");

    // EML's or ShroudForge's dbghelp.dll there but no emf_content.dll: named, never read (it is up to 32 MB)
    asked.clear();
    dll = false;
    io.look.exists = [&](const std::wstring& name) { return asked.push_back("exists " + emf::loader::cio::to_utf8(name)), name == L"dbghelp.dll"; };
    f = emf::loader::collect_facts(io, emf::loader::PlayOnce::Normal, false);
    CHECK(f.other_tool == std::optional<std::string>("dbghelp.dll (EML or ShroudForge)"));
    CHECK(asked == std::vector<std::string>{"mode", "dll", "exists kiln\\kiln.json", "exists dbghelp.dll"});
    asked.clear();
    dll = true;
    f = emf::loader::collect_facts(io, emf::loader::PlayOnce::Normal, false);
    CHECK(asked == std::vector<std::string>{"mode", "dll", "exists kiln\\kiln.json", "exists dbghelp.dll", "read dbghelp.dll"});

    // another program that loads EMF's loader: nothing more is asked
    asked.clear();
    f = emf::loader::collect_facts(io, emf::loader::PlayOnce::Normal, false, false);
    CHECK(asked == std::vector<std::string>{"mode"});
    CHECK(emf::loader::decide_content(f).why.find("not the game") != std::string::npos);
}

TEST_CASE("content splash: a window while the work runs on another thread, with its last line; gone afterwards") {
    emf::loader::ContentSplash splash;
    int shown = 0;
    splash.show_window = [&](HWND) { ++shown; };  // nothing on the screen during the tests
    REQUIRE(splash.begin("EMF is preparing EML content (first start after a change)..."));
    const HWND w = splash.window();
    CHECK(IsWindow(w));
    CHECK_FALSE(IsWindowVisible(w));  // hidden until the work has run for a second
    CHECK((GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_NOACTIVATE) != 0);  // never takes the game's focus
    DWORD worker = 0;
    const bool done = emf::loader::run_with_splash(&splash,
                                                   [&] {
                                                       worker = GetCurrentThreadId();
                                                       splash.line("standin: building");
                                                       Sleep(300);
                                                       splash.line("the last line");
                                                   },
                                                   10000);
    CHECK(done);
    CHECK(worker != 0);
    CHECK(worker != GetCurrentThreadId());
    CHECK(splash.last_line() == "the last line");
    CHECK(shown == 0);  // a work of 0.3 s never shows it: a quick start shows nothing
    CHECK_FALSE(splash.shown());
    splash.end();
    CHECK_FALSE(IsWindow(w));
    // a work that does not come back in time: false, and the work keeps what it needs
    CHECK_FALSE(emf::loader::run_with_splash(nullptr, [] { Sleep(1500); }, 100));
}

TEST_CASE("content splash: a work longer than the delay shows the window once, after the delay") {
    emf::loader::ContentSplash splash;
    std::vector<ULONGLONG> shown_at;
    splash.show_window = [&](HWND) { shown_at.push_back(GetTickCount64()); };
    const ULONGLONG start = GetTickCount64();
    REQUIRE(splash.begin("EMF is preparing EML content (first start after a change)...", 150));
    CHECK(emf::loader::run_with_splash(&splash, [] { Sleep(700); }, 10000));
    REQUIRE(shown_at.size() == 1u);
    CHECK(shown_at[0] - start >= 140u);  // not before the delay
    CHECK(shown_at[0] - start < 650u);   // but while the work still ran
    CHECK(splash.shown());
    splash.end();
}

TEST_CASE("content splash: without a splash (the dedicated server) it only waits: no message loop, no window") {
    MSG m;
    PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE);  // this thread gets its message queue
    REQUIRE(PostThreadMessageW(GetCurrentThreadId(), WM_APP + 7, 1, 2));
    DWORD worker = 0;
    CHECK(emf::loader::run_with_splash(nullptr, [&] { worker = GetCurrentThreadId(); Sleep(200); }, 10000));
    CHECK(worker != 0);
    CHECK(worker != GetCurrentThreadId());
    // the message is still there: nothing pumped this thread's queue
    CHECK(PeekMessageW(&m, nullptr, WM_APP + 7, WM_APP + 7, PM_REMOVE));
}

TEST_CASE("content start: a current state but a shadow whose record does not match the game builds again") {
    Recorder r;
    auto steps = plan_steps(r, owned_facts(), good_state("sha256:now"));
    steps.record_ok = [&r] { return r.add("record"), false; };  // shadow.txt gone, or the game's files changed
    const auto plan = emf::loader::plan_content(steps);
    CHECK(plan.path == ContentPath::BuildHeld);
    CHECK(r.joined() == "facts, fingerprint, read_state, record");
    CHECK(r.logged("its record, shadow.txt, is missing"));
    Recorder ok;
    auto good = plan_steps(ok, owned_facts(), good_state("sha256:now"));
    good.record_ok = [&ok] { return ok.add("record"), true; };
    CHECK(emf::loader::plan_content(good).path == ContentPath::UseCurrent);
}

TEST_CASE("content splash: its size follows the screen's DPI") {
    CHECK(emf::loader::splash_scaled(620, 96) == 620);
    CHECK(emf::loader::splash_scaled(620, 144) == 930);  // 150 %
    CHECK(emf::loader::splash_scaled(110, 192) == 220);  // 200 %
    emf::loader::ContentSplash splash;
    REQUIRE(splash.begin("EMF is preparing EML content (first start after a change)..."));
    RECT r{};
    GetWindowRect(splash.window(), &r);
    CHECK(r.right - r.left == emf::loader::splash_scaled(620, emf::loader::screen_dpi()));
    splash.end();
}

namespace {
emf::loader::PortraitItem wolf_item() {
    emf::loader::PortraitItem p;
    p.mod = "beastmaster";
    p.key = "wolf";
    p.template_guid = "503fa1d1-dce2-45a1-85a5-185a22666d33";
    return p;
}
void with_portraits(Recorder& r, emf::loader::StartSteps& s, bool unloads = true) {
    s.run_portraits = [&r, unloads](const std::vector<emf::loader::PortraitItem>& items, bool shadow,
                                    const std::atomic<bool>&) {
        r.add(std::string("portraits(") + (shadow ? "shadow" : "game") + ")");
        emf::loader::PortraitsOutcome o;
        o.call.code = 0;
        o.call.unloaded = unloads;
        for (const auto& i : items) o.drawn.push_back(i.key);
        return o;
    };
}
}  // namespace

TEST_CASE("content start: missing portraits on a current shadow hold the start, are drawn from it, then all goes on") {
    Recorder r;
    auto plan = emf::loader::plan_content(plan_steps(r, owned_facts(), good_state("sha256:now")));
    REQUIRE(plan.path == ContentPath::UseCurrent);
    plan.portraits = {wolf_item()};
    r.calls.clear();
    auto steps = start_steps(r);
    with_portraits(r, steps);
    CHECK(emf::loader::start_in_dllmain(plan, steps));
    CHECK(r.joined() == "hold");  // the chained DLLs and the core wait for the pictures
    r.calls.clear();
    emf::loader::run_held_content(plan, steps);
    CHECK(r.joined() == "splash, portraits(shadow), splash end, chain, hooks(content on), status(current), core");
    CHECK(r.logged("drew 1"));
}

TEST_CASE("content start: no portraits missing: no hold (a start with every picture cached is not slower)") {
    Recorder r;
    auto plan = emf::loader::plan_content(plan_steps(r, owned_facts(), good_state("sha256:now")));
    r.calls.clear();
    auto steps = start_steps(r);
    with_portraits(r, steps);
    CHECK_FALSE(emf::loader::start_in_dllmain(plan, steps));
    CHECK(r.joined() == "chain, hooks(content on), status(current), core");
}

TEST_CASE("content start: a build start draws the portraits after the build: from the shadow when it built, else the game") {
    for (const bool ok : {true, false}) {
        Recorder r;
        auto plan = emf::loader::plan_content(plan_steps(r, owned_facts(), std::nullopt));
        REQUIRE(plan.path == ContentPath::BuildHeld);
        plan.portraits = {wolf_item()};
        emf::loader::BuildOutcome failed = built_ok();
        failed.code = 2;
        auto steps = start_steps(r, true, ok ? built_ok() : failed);
        with_portraits(r, steps);
        CHECK(emf::loader::start_in_dllmain(plan, steps));
        r.calls.clear();
        emf::loader::run_held_content(plan, steps);
        CHECK(r.joined().find(ok ? "portraits(shadow)" : "portraits(game)") != std::string::npos);
        CHECK(r.joined().find("core") != std::string::npos);
    }
}

TEST_CASE("content start: portraits that leave emf_content.dll in the game start neither the chained DLLs nor the core") {
    Recorder r;
    auto plan = emf::loader::plan_content(plan_steps(r, owned_facts(), good_state("sha256:now")));
    plan.portraits = {wolf_item()};
    auto steps = start_steps(r);
    with_portraits(r, steps, false);
    CHECK(emf::loader::start_in_dllmain(plan, steps));
    r.calls.clear();
    emf::loader::run_held_content(plan, steps);
    CHECK(r.joined().find("chain") == std::string::npos);
    CHECK(r.joined().find("core") == std::string::npos);
    CHECK(r.joined().find("alert") != std::string::npos);
}

TEST_CASE("content start: a start that cannot be held draws no portraits and goes on as before") {
    Recorder r;
    auto plan = emf::loader::plan_content(plan_steps(r, owned_facts(), good_state("sha256:now")));
    plan.portraits = {wolf_item()};
    r.calls.clear();
    auto steps = start_steps(r, false);
    with_portraits(r, steps);
    CHECK_FALSE(emf::loader::start_in_dllmain(plan, steps));
    CHECK(r.joined() == "hold, chain, hooks(content on), status(current), core");
    CHECK(r.logged("later start"));
}

TEST_CASE("content start: pictures that leave emf_content.dll loaded after a good build keep the build's own state") {
    Recorder r;
    auto plan = emf::loader::plan_content(plan_steps(r, owned_facts(), std::nullopt));
    REQUIRE(plan.path == ContentPath::BuildHeld);
    plan.portraits = {wolf_item()};
    auto steps = start_steps(r);
    with_portraits(r, steps, false);  // the build unloads the DLL, the pictures call does not
    CHECK(emf::loader::start_in_dllmain(plan, steps));
    r.calls.clear();
    emf::loader::run_held_content(plan, steps);
    REQUIRE_FALSE(r.states.empty());
    const auto& last = r.states.back();
    CHECK(last.code == 0);
    CHECK(last.unloaded);  // a good build stays a good build: the next start finds the shadow copy current
    CHECK(emf::loader::state_current(last, "sha256:now"));
    CHECK(last.message.find("pictures") == std::string::npos);
    CHECK(r.joined().find("write_record") != std::string::npos);
    CHECK(r.joined().find("drop_record") == std::string::npos);  // the shadow copy is good: it stays redirected next start
    CHECK(r.joined().find("chain") == std::string::npos);  // only this start goes without the chained DLLs and the core
    CHECK(r.joined().find("core") == std::string::npos);
    CHECK(r.joined().find("alert") != std::string::npos);
    CHECK(r.logged("pictures"));
    CHECK(r.logged("without the chained DLLs"));
}

TEST_CASE("content start: every pictures call is recorded for its items, also one that did not come back") {
    for (const bool finishes : {true, false}) {
        Recorder r;
        auto plan = emf::loader::plan_content(plan_steps(r, owned_facts(), good_state("sha256:now")));
        plan.portraits = {wolf_item()};
        auto steps = start_steps(r, true, built_ok(), true, finishes);
        with_portraits(r, steps);
        std::vector<std::string> recorded;
        int code = -1;
        steps.record_portraits = [&](const std::vector<emf::loader::PortraitItem>& items, const emf::loader::PortraitsOutcome& o) {
            for (const auto& i : items) recorded.push_back(i.key);
            code = o.call.code;
        };
        CHECK(emf::loader::start_in_dllmain(plan, steps));
        emf::loader::run_held_content(plan, steps);
        REQUIRE(recorded.size() == (finishes ? 2u : 1u));  // the failed mark before the call, the result after
        CHECK(recorded[0] == "wolf");
        CHECK(code == (finishes ? 0 : emf::loader::kBuildError));  // not done: the mark before the call stays
    }
}

TEST_CASE("content start: the pictures after a build are recorded too") {
    Recorder r;
    auto plan = emf::loader::plan_content(plan_steps(r, owned_facts(), std::nullopt));
    plan.portraits = {wolf_item()};
    auto steps = start_steps(r);
    with_portraits(r, steps);
    int recorded = 0;
    steps.record_portraits = [&](const std::vector<emf::loader::PortraitItem>&, const emf::loader::PortraitsOutcome&) {
        ++recorded;
    };
    CHECK(emf::loader::start_in_dllmain(plan, steps));
    emf::loader::run_held_content(plan, steps);
    CHECK(recorded == 2);
}

namespace {
// A pictures start with its own recorder: what record_portraits was told, in order, and the pictures' work timeouts.
struct PicturesRig {
    Recorder r;
    emf::loader::StartSteps steps;
    std::vector<std::string> told;  // "<key>:<ok|failed> " per record_portraits call
    std::vector<uint32_t> timeouts;
    bool pictures_finish = true;
    bool unloads = true;
    bool dll_there_after_pictures = false;
    int dll_checks = 0;
    explicit PicturesRig(bool build_dll_gone = true, emf::loader::BuildOutcome outcome = built_ok(), bool finishes = true)
        : steps(start_steps(r, true, outcome, true, finishes)) {
        steps.run_portraits = [this](const std::vector<emf::loader::PortraitItem>& items, bool shadow,
                                     const std::atomic<bool>&) {
            r.add(std::string("portraits(") + (shadow ? "shadow" : "game") + ")");
            r.log.push_back(std::string("pending before the call: ") + (told.empty() ? "none" : told.back()));
            emf::loader::PortraitsOutcome o;
            o.call.unloaded = unloads;
            for (const auto& i : items) o.drawn.push_back(i.key);
            return o;
        };
        steps.record_portraits = [this](const std::vector<emf::loader::PortraitItem>& items,
                                        const emf::loader::PortraitsOutcome& o) {
            std::string t;
            for (const auto& i : items) {
                bool ok = false;
                for (const auto& d : o.drawn) ok = ok || d == i.key;
                t += i.key + (ok ? ":ok " : ":failed ");
            }
            told.push_back(t);
            r.add("record");
        };
        steps.run_pictures_work = [this](const std::function<void()>& work, uint32_t timeout_ms) {
            timeouts.push_back(timeout_ms);
            r.add("pictures work");
            if (!pictures_finish) return false;
            work();
            return true;
        };
        // the first check is the build's own, the later ones are after the pictures call
        steps.dll_gone = [this, build_dll_gone] { return ++dll_checks == 1 ? build_dll_gone : !dll_there_after_pictures; };
    }
};
}  // namespace

TEST_CASE("content start: the build's final state is written before the pictures, and a record marks the keys failed before the call") {
    PicturesRig rig;
    auto plan = emf::loader::plan_content(plan_steps(rig.r, owned_facts(), std::nullopt));
    plan.portraits = {wolf_item()};
    CHECK(emf::loader::start_in_dllmain(plan, rig.steps));
    rig.r.calls.clear();
    rig.r.log.clear();
    emf::loader::run_held_content(plan, rig.steps);
    const std::string j = rig.r.joined();
    CHECK(rig.r.states.back().code == 0);  // what stays if the game dies while drawing is the good state
    REQUIRE(j.find("portraits(shadow)") != std::string::npos);
    CHECK(j.find("write_state", j.find("portraits(shadow)")) == std::string::npos);  // none written after the pictures
    CHECK(rig.r.logged("pending before the call: wolf:failed"));  // overwritten by the real result after
    REQUIRE(rig.told.size() == 2u);
    CHECK(rig.told[1] == "wolf:ok ");
}

TEST_CASE("content start: pictures that hang after a good build time out on their own, short, and leave the build's state alone") {
    PicturesRig rig;
    rig.pictures_finish = false;
    auto plan = emf::loader::plan_content(plan_steps(rig.r, owned_facts(), std::nullopt));
    plan.portraits = {wolf_item()};
    CHECK(emf::loader::start_in_dllmain(plan, rig.steps));
    rig.r.calls.clear();
    emf::loader::run_held_content(plan, rig.steps);
    REQUIRE(rig.timeouts.size() == 1u);
    CHECK(rig.timeouts[0] < emf::loader::kBuildTimeoutMs / 2);
    CHECK(rig.timeouts[0] >= 60u * 1000u);
    CHECK(rig.r.states.back().code == 0);
    CHECK(rig.r.states.back().unloaded);
    CHECK(rig.r.joined().find("drop_record") == std::string::npos);
    REQUIRE_FALSE(rig.told.empty());
    CHECK(rig.told.back() == "wolf:failed ");  // not again until the DLL, the game or the entry changes
    CHECK(rig.r.joined().find("core") == std::string::npos);
}

TEST_CASE("content start: pictures that leave the DLL loaded still put the good shadow copy in place (hooks), without chain or core") {
    for (const bool build : {true, false}) {
        PicturesRig rig;
        rig.unloads = false;
        auto plan = emf::loader::plan_content(
            plan_steps(rig.r, owned_facts(), build ? std::nullopt : std::optional(good_state("sha256:now"))));
        plan.portraits = {wolf_item()};
        CHECK(emf::loader::start_in_dllmain(plan, rig.steps));
        rig.r.calls.clear();
        emf::loader::run_held_content(plan, rig.steps);
        const std::string j = rig.r.joined();
        CHECK(j.find("hooks(content on)") != std::string::npos);
        CHECK(j.find("chain") == std::string::npos);
        CHECK(j.find("core") == std::string::npos);
        CHECK(j.find("alert") != std::string::npos);
    }
}

TEST_CASE("content start: the DLL still in the game after the build: no pictures are asked for, none recorded") {
    PicturesRig rig(false);  // the build's own check finds the DLL
    auto plan = emf::loader::plan_content(plan_steps(rig.r, owned_facts(), std::nullopt));
    plan.portraits = {wolf_item()};
    CHECK(emf::loader::start_in_dllmain(plan, rig.steps));
    rig.r.calls.clear();
    emf::loader::run_held_content(plan, rig.steps);
    CHECK(rig.r.joined().find("portraits(") == std::string::npos);
    CHECK(rig.told.empty());
    CHECK_FALSE(rig.r.states.back().unloaded);
}

TEST_CASE("content start: a build that does not come back in time asks for no pictures and records none") {
    PicturesRig rig(true, built_ok(), false);
    auto plan = emf::loader::plan_content(plan_steps(rig.r, owned_facts(), std::nullopt));
    plan.portraits = {wolf_item()};
    CHECK(emf::loader::start_in_dllmain(plan, rig.steps));
    rig.r.calls.clear();
    emf::loader::run_held_content(plan, rig.steps);
    CHECK(rig.r.joined().find("portraits(") == std::string::npos);
    CHECK(rig.told.empty());
    CHECK(rig.r.joined().find("core") == std::string::npos);
}

TEST_CASE("content start: the DLL found still loaded after the pictures call counts as left loaded") {
    PicturesRig rig;
    rig.dll_there_after_pictures = true;  // the call said unloaded, the process says otherwise
    rig.dll_checks = 1;  // no build at this start: the first check is the pictures'
    auto plan = emf::loader::plan_content(plan_steps(rig.r, owned_facts(), good_state("sha256:now")));
    plan.portraits = {wolf_item()};
    CHECK(emf::loader::start_in_dllmain(plan, rig.steps));
    rig.r.calls.clear();
    emf::loader::run_held_content(plan, rig.steps);
    CHECK(rig.r.joined().find("core") == std::string::npos);
    CHECK(rig.r.joined().find("alert") != std::string::npos);
}

TEST_CASE("content start: a pictures-only hold on a start whose build failed before, or with content off, draws from the game") {
    for (const bool failed : {true, false}) {
        PicturesRig rig;
        emf::loader::ContentPlan plan;
        if (failed) {
            auto bad = good_state("sha256:now");
            bad.code = 3;
            bad.message = "boom";
            plan = emf::loader::plan_content(plan_steps(rig.r, owned_facts(), bad));
            REQUIRE(plan.path == ContentPath::KnownFailed);
        }
        plan.portraits = {wolf_item()};
        rig.dll_checks = 1;  // no build at this start: the first check is the pictures'
        CHECK(emf::loader::start_in_dllmain(plan, rig.steps));
        rig.r.calls.clear();
        emf::loader::run_held_content(plan, rig.steps);
        CHECK(rig.r.joined().find("portraits(game)") != std::string::npos);
        CHECK(rig.r.joined().find(failed ? "hooks(content off)" : "hooks(spike)") != std::string::npos);
        CHECK(rig.r.joined().find("core") != std::string::npos);
    }
}
