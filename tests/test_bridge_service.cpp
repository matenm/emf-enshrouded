// The debug bridge's commands against fake sources: hello, errors and the bad-line limit, console, state, mods, log
// (since and follow), wait, events (rate limit), screenshot, mod_key, the per-client limits and late answers.
#include <doctest.h>

#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "common/log_ring.h"
#include "core/bridge/protocol.h"
#include "core/bridge/service.h"
#include "core/json.h"

using namespace emf::core;
using namespace emf::core::bridge;

namespace {

struct Sent {
    ClientId client;
    std::string line;
    bool droppable;
};

struct Fake {
    uint64_t now = 100000;
    std::vector<Sent> sent;
    std::vector<ClientId> disconnected;
    emf::LogRing ring{100};
    bool world = false, player = false;
    // console: what the fake "typed line" answers (synchronously unless deferred)
    std::function<void(const std::string&)> console_line;
    std::function<void(const std::string&)> console_done;
    std::string console_text;
    bool console_defer = false;
    // state
    std::function<void(std::string)> state_done;
    // screenshot
    std::function<void(const ShotResult&)> shot_done;
    ShotOptions shot_options;
    std::string shot_refuse;
    bool shot_taken = false;
    int shot_cancels = 0;
    // keys
    std::vector<std::tuple<uint32_t, KeyAction, uint32_t>> keys;
    std::function<void(const KeyResult&)> key_done;
    int quits = 0;
    std::string quit_refuse;
    // load_world
    int loads = 0;
    LoadWorldParams load_params;
    std::function<void(const LoadWorldReply&)> load_done;
    Refusal load_refuse;
    std::string mods_json = R"([{"id":"clock","status":"loaded"}])";

    Sources sources() {
        Sources s;
        s.hello = HelloInfo{"1.2.3", 16, 1273043, false, 4242};
        s.now_ms = [this] { return now; };
        s.console = [this](const std::string& text, std::function<void(const std::string&)> line,
                           std::function<void(const std::string&)> done) {
            console_text = text;
            console_line = line;
            console_done = done;
            if (console_defer) return;
            line("> " + text);
            line("first");
            line("second");
            done("ran");
        };
        s.state = [this](std::function<void(std::string)> done) { state_done = done; };
        s.mods = [this] { return mods_json; };
        s.log = &ring;
        s.world_running = [this] { return world; };
        s.player_exists = [this] { return player; };
        s.shot_start = [this](const ShotOptions& o, std::function<void(const ShotResult&)> done) -> Refusal {
            shot_options = o;
            if (!shot_refuse.empty()) return Refusal{code::kUnavailable, shot_refuse};
            shot_done = done;
            return {};
        };
        s.shot_cancel = [this] {
            ++shot_cancels;
            return !shot_taken;
        };
        s.mod_key = [this](uint32_t vk, KeyAction a, uint32_t hold,
                           std::function<void(const KeyResult&)> done) -> Refusal {
            keys.emplace_back(vk, a, hold);
            if (a == KeyAction::Press) {
                key_done = done;
            } else {
                KeyResult r;
                r.ok = true;
                r.mod_reads = 3;
                done(r);
            }
            return {};
        };
        s.quit = [this]() -> Refusal {
            ++quits;
            if (!quit_refuse.empty()) return Refusal{code::kUnavailable, quit_refuse};
            return {};
        };
        s.load_world = [this](const LoadWorldParams& p, std::function<void(const LoadWorldReply&)> done) -> Refusal {
            ++loads;
            load_params = p;
            if (load_refuse.refused()) return load_refuse;
            load_done = done;
            return {};
        };
        s.event_names = {"pickup", "kill", "damage", "save", "load"};
        return s;
    }
    Output output() {
        Output o;
        o.send = [this](ClientId c, std::string line, bool droppable) { sent.push_back({c, std::move(line), droppable}); };
        o.disconnect = [this](ClientId c) { disconnected.push_back(c); };
        return o;
    }
};

json::Value parse(const std::string& line) {
    auto r = json::parse(line);
    REQUIRE_MESSAGE(r.value, line);
    return *r.value;
}
json::Value last(const Fake& f) {
    REQUIRE_FALSE(f.sent.empty());
    return parse(f.sent.back().line);
}
std::string str(const json::Value& v, const char* k) {
    const json::Value* m = v.get(k);
    return m && m->is(json::Value::Type::String) ? m->s : "<none>";
}
double num(const json::Value& v, const char* k) {
    const json::Value* m = v.get(k);
    return m && m->is(json::Value::Type::Number) ? m->n : -999;
}
bool ok(const json::Value& v) {
    const json::Value* m = v.get("ok");
    return m && m->is(json::Value::Type::Bool) && m->b;
}
bool flag(const json::Value& v, const char* k) {
    const json::Value* m = v.get(k);
    return m && m->is(json::Value::Type::Bool) && m->b;
}

}  // namespace

TEST_CASE("bridge service: hello names EMF, the API, the game build, the process and the protocol") {
    Fake f;
    Service s(f.sources(), f.output());
    s.on_connect(1);
    s.on_line(1, R"({"id":1,"cmd":"hello"})");
    const auto r = last(f);
    CHECK(f.sent.back().client == 1);
    CHECK_FALSE(f.sent.back().droppable);
    CHECK(num(r, "id") == 1);
    CHECK(ok(r));
    CHECK(str(r, "emf") == "1.2.3");
    CHECK(num(r, "api") == 16);
    CHECK(num(r, "game_build") == 1273043);
    CHECK(str(r, "process") == "client");
    CHECK(num(r, "pid") == 4242);
    CHECK(num(r, "protocol") == 1);
    REQUIRE(r.get("commands"));
    CHECK(r.get("commands")->arr.size() == command_names().size());
}

TEST_CASE("bridge service: hello on an unknown build and in the dedicated server") {
    Fake f;
    auto src = f.sources();
    src.hello.game_build = 0;
    src.hello.dedicated = true;
    Service s(src, f.output());
    s.on_connect(1);
    s.on_line(1, R"({"id":"h","cmd":"hello"})");
    const auto r = last(f);
    CHECK(str(r, "id") == "h");
    CHECK(str(r, "game_build") == "unknown");
    CHECK(str(r, "process") == "dedicated server");
}

TEST_CASE("bridge service: an unknown command, a missing id and bad JSON get errors; five bad lines in a row disconnect") {
    Fake f;
    Service s(f.sources(), f.output());
    s.on_connect(1);
    s.on_line(1, R"({"id":2,"cmd":"fly"})");
    auto r = last(f);
    CHECK_FALSE(ok(r));
    CHECK(str(r, "error").find("unknown command 'fly'") == 0);
    CHECK(str(r, "code") == "unknown_command");

    s.on_line(1, R"({"cmd":"hello"})");
    r = last(f);
    CHECK(r.get("id")->is(json::Value::Type::Null));
    CHECK(str(r, "error") == "the request has no id");
    CHECK(str(r, "code") == "bad_request");
    s.on_line(1, "garbage");
    s.on_line(1, "");  // blank lines are ignored, not bad
    s.on_line(1, R"({"id":3,"cmd":"hello"})");  // a good line resets the count
    CHECK(ok(last(f)));
    for (int i = 0; i < 4; ++i) s.on_line(1, "{nope");
    CHECK(f.disconnected.empty());
    s.on_too_long(1);
    r = last(f);
    CHECK(str(r, "error").find("disconnected") != std::string::npos);
    REQUIRE(f.disconnected.size() == 1);
    CHECK(f.disconnected[0] == 1);
}

TEST_CASE("bridge service: a line over 64 KiB is answered with an error") {
    Fake f;
    Service s(f.sources(), f.output());
    s.on_connect(1);
    s.on_too_long(1);
    CHECK(str(last(f), "error") == "the line is longer than 64 KiB");
}

TEST_CASE("bridge service: console answers with what happened, the output (without the echo) and the newest log line") {
    Fake f;
    f.ring.push("a");
    f.ring.push("b");
    Service s(f.sources(), f.output());
    s.on_connect(1);
    s.on_line(1, R"({"id":5,"cmd":"console","text":"near 10"})");
    CHECK(f.console_text == "near 10");
    CHECK(f.sent.empty());  // it ran: late lines may still come for 300 ms
    f.now += kSettleDefaultMs - 1;
    s.poll();
    CHECK(f.sent.empty());
    f.now += 1;
    s.poll();
    const auto r = last(f);
    CHECK(ok(r));
    CHECK(str(r, "status") == "ran");
    REQUIRE(r.get("output"));
    REQUIRE(r.get("output")->arr.size() == 2);
    CHECK(r.get("output")->arr[0].s == "first");
    CHECK(r.get("output")->arr[1].s == "second");
    CHECK(num(r, "log_seq") == 2);
}

TEST_CASE("bridge service: console keeps collecting late lines until none came for settle_ms") {
    Fake f;
    f.console_defer = true;
    Service s(f.sources(), f.output());
    s.on_connect(1);
    s.on_line(1, R"({"id":1,"cmd":"console","text":"put Wood 1","settle_ms":500})");
    f.console_line("> put Wood 1");
    f.console_done("ran");  // the command returned; its result comes one step later
    f.now += 400;
    s.poll();
    CHECK(f.sent.empty());
    f.console_line("put 1 x Wood into entity 1: 1 added");  // through current_reply(), later
    f.now += 400;
    s.poll();
    CHECK(f.sent.empty());  // the late line opened another 500 ms
    f.now += 100;
    s.poll();
    const auto r = last(f);
    CHECK(str(r, "status") == "ran");
    REQUIRE(r.get("output")->arr.size() == 1);
    CHECK(r.get("output")->arr[0].s == "put 1 x Wood into entity 1: 1 added");

    f.sent.clear();
    s.on_line(1, R"({"id":2,"cmd":"console","text":"pos","settle_ms":0})");
    f.console_done("ran");
    CHECK(num(last(f), "id") == 2);  // settle_ms 0: at once

    s.on_line(1, R"({"id":3,"cmd":"console","text":"nosuch"})");
    f.console_line("unknown command 'nosuch' (type help)");
    f.console_done("unknown");
    const auto u = last(f);  // a command that did not run sends nothing later: at once
    CHECK(num(u, "id") == 3);
    CHECK(ok(u));
    CHECK(str(u, "status") == "unknown");

    s.on_line(1, R"({"id":4,"cmd":"console","text":"pos","settle_ms":5001})");
    CHECK(str(last(f), "code") == "bad_request");
    CHECK(str(last(f), "error") == "settle_ms must be a whole number from 0 to 5000");
}

TEST_CASE("bridge service: console checks its text and time limit") {
    Fake f;
    Service s(f.sources(), f.output());
    s.on_connect(1);
    s.on_line(1, R"({"id":1,"cmd":"console"})");
    CHECK(str(last(f), "error") == "console needs \"text\": the command line to run");
    CHECK(str(last(f), "code") == "bad_request");
    s.on_line(1, R"({"id":1,"cmd":"console","text":"  "})");
    CHECK(str(last(f), "error") == "console needs \"text\": the command line to run");
    s.on_line(1, R"({"id":1,"cmd":"console","text":"help\nmods"})");
    CHECK(str(last(f), "error") == "one command per request: text has a line break");
    s.on_line(1, R"({"id":1,"cmd":"console","text":"help","timeout_ms":0})");
    CHECK(str(last(f), "error") == "timeout_ms must be a whole number from 1 to 120000");
    s.on_line(1, R"({"id":1,"cmd":"console","text":"help","timeout_ms":120001})");
    CHECK_FALSE(ok(last(f)));
    CHECK(f.console_text.empty());  // nothing ran
}

TEST_CASE("bridge service: console times out (ok false, code timeout) with the lines so far; the late end is dropped") {
    Fake f;
    f.console_defer = true;
    Service s(f.sources(), f.output());
    s.on_connect(1);
    s.on_line(1, R"({"id":9,"cmd":"console","text":"put Wood 5","timeout_ms":3000})");
    CHECK(f.sent.empty());
    f.console_line("> put Wood 5");
    f.console_line("queued");
    f.now += 2999;
    s.poll();
    CHECK(f.sent.empty());
    f.now += 2;
    s.poll();
    REQUIRE(f.sent.size() == 1);
    const auto r = last(f);
    CHECK_FALSE(ok(r));
    CHECK(str(r, "code") == "timeout");
    REQUIRE(r.get("output")->arr.size() == 1);
    CHECK(r.get("output")->arr[0].s == "queued");
    CHECK(r.get("log_seq"));
    f.console_line("put: 5 given");
    f.console_done("ran");
    f.now += 1000;
    s.poll();
    CHECK(f.sent.size() == 1);  // answered once
}

TEST_CASE("bridge service: state answers with the snapshot's members, or an error after 2 s") {
    Fake f;
    Service s(f.sources(), f.output());
    s.on_connect(1);
    s.on_line(1, R"({"id":1,"cmd":"state"})");
    REQUIRE(f.state_done);
    CHECK(f.sent.empty());
    f.ring.push("x");
    f.state_done(R"({"world":{"status":"running"},"player":null})");
    auto r = last(f);
    CHECK(ok(r));
    CHECK(num(r, "log_seq") == 1);
    REQUIRE(r.get("world"));
    CHECK(str(*r.get("world"), "status") == "running");
    CHECK(r.get("player")->is(json::Value::Type::Null));

    f.state_done = nullptr;
    s.on_line(1, R"({"id":2,"cmd":"state"})");
    f.now += 2001;
    s.poll();
    r = last(f);
    CHECK(num(r, "id") == 2);
    CHECK_FALSE(ok(r));
    CHECK(str(r, "code") == "timeout");
    CHECK(str(r, "error").find("2 s") != std::string::npos);
}

TEST_CASE("bridge service: mods answers with the mod list; a reply over 1 MiB becomes an error") {
    Fake f;
    Service s(f.sources(), f.output());
    s.on_connect(1);
    s.on_line(1, R"({"id":1,"cmd":"mods"})");
    auto r = last(f);
    CHECK(ok(r));
    REQUIRE(r.get("mods"));
    CHECK(str(r.get("mods")->arr[0], "id") == "clock");

    f.mods_json = "[\"" + std::string(kMaxLineOut, 'x') + "\"]";
    s.on_line(1, R"({"id":2,"cmd":"mods"})");
    r = last(f);
    CHECK_FALSE(ok(r));
    CHECK(str(r, "error") == "the reply would be larger than 1 MiB");
    CHECK(str(r, "code") == "refused");
    CHECK(f.sent.back().line.size() < 200);
}

TEST_CASE("bridge service: log gives the newest lines, or those after `since`, with their numbers") {
    Fake f;
    for (int i = 1; i <= 5; ++i) f.ring.push("line " + std::to_string(i));
    Service s(f.sources(), f.output());
    s.on_connect(1);
    s.on_line(1, R"({"id":1,"cmd":"log","since":3})");
    auto r = last(f);
    CHECK(ok(r));
    auto lines = r.get("lines")->arr;
    REQUIRE(lines.size() == 2);
    CHECK(num(lines[0], "seq") == 4);
    CHECK(str(lines[0], "line") == "line 4");
    CHECK(num(r, "last") == 5);
    CHECK_FALSE(flag(r, "more"));
    CHECK_FALSE(flag(r, "following"));

    s.on_line(1, R"({"id":2,"cmd":"log","max":2})");  // no since: the newest
    r = last(f);
    lines = r.get("lines")->arr;
    REQUIRE(lines.size() == 2);
    CHECK(num(lines[0], "seq") == 4);

    s.on_line(1, R"({"id":3,"cmd":"log","since":0,"max":2})");
    r = last(f);
    CHECK(r.get("lines")->arr.size() == 2);
    CHECK(flag(r, "more"));
    CHECK(num(r, "last") == 2);

    s.on_line(1, R"({"id":4,"cmd":"log","max":0})");
    CHECK(str(last(f), "error") == "max must be a whole number from 1 to 5000");
    CHECK(str(last(f), "code") == "bad_request");
}

TEST_CASE("bridge service: a log read from a line the ring no longer holds says how many lines are gone") {
    Fake f;
    for (int i = 1; i <= 150; ++i) f.ring.push("line " + std::to_string(i));  // keeps 51..150
    Service s(f.sources(), f.output());
    s.on_connect(1);
    s.on_line(1, R"({"id":1,"cmd":"log","since":10,"max":5})");
    auto r = last(f);
    CHECK(num(r, "missed") == 40);  // 11..50
    CHECK(num(r.get("lines")->arr[0], "seq") == 51);
    s.on_line(1, R"({"id":2,"cmd":"log","since":60,"max":5})");
    CHECK(last(f).get("missed") == nullptr);
}

TEST_CASE("bridge service: log follow pushes new lines as events until it is switched off") {
    Fake f;
    f.ring.push("old");
    Service s(f.sources(), f.output());
    s.on_connect(1);
    s.on_connect(2);
    s.on_line(1, R"({"id":1,"cmd":"log","since":1,"follow":true})");
    auto r = last(f);
    CHECK(flag(r, "following"));
    CHECK(r.get("lines")->arr.empty());
    f.sent.clear();
    f.ring.push("new 1");
    f.ring.push("new 2");
    s.poll();
    REQUIRE(f.sent.size() == 2);
    CHECK(f.sent[0].client == 1);
    CHECK(f.sent[0].droppable);
    auto e = parse(f.sent[0].line);
    CHECK(str(e, "event") == "log");
    CHECK(num(e, "seq") == 2);
    CHECK(str(e, "line") == "new 1");
    s.poll();
    CHECK(f.sent.size() == 2);  // nothing twice

    s.on_line(1, R"({"id":2,"cmd":"log","follow":false,"max":1})");
    CHECK_FALSE(flag(last(f), "following"));
    f.sent.clear();
    f.ring.push("new 3");
    s.poll();
    CHECK(f.sent.empty());
}

TEST_CASE("bridge service: a follower that fell behind the ring is told how many lines it missed") {
    Fake f;
    Service s(f.sources(), f.output());
    s.on_connect(1);
    s.on_line(1, R"({"id":1,"cmd":"log","since":0,"follow":true})");
    f.sent.clear();
    for (int i = 0; i < 150; ++i) f.ring.push("x" + std::to_string(i));  // the ring keeps 100
    s.poll();
    REQUIRE_FALSE(f.sent.empty());
    auto e = parse(f.sent[0].line);
    CHECK(num(e, "seq") == 51);
    CHECK(num(e, "missed") == 50);
}

TEST_CASE("bridge service: wait for a world, a player or a log line, or time out") {
    Fake f;
    Service s(f.sources(), f.output());
    s.on_connect(1);
    s.on_line(1, R"({"id":1,"cmd":"wait","for":"world","timeout_ms":5000})");
    CHECK(f.sent.empty());
    s.poll();
    CHECK(f.sent.empty());
    f.world = true;
    s.poll();
    CHECK(ok(last(f)));
    CHECK(num(last(f), "id") == 1);

    f.player = true;  // already true: answered at once
    s.on_line(1, R"({"id":2,"cmd":"wait","for":"player"})");
    CHECK(num(last(f), "id") == 2);
    CHECK(ok(last(f)));

    f.ring.push("[life_skills] old line");
    s.on_line(1, R"({"id":3,"cmd":"wait","for":"log","match":"[life_skills]","timeout_ms":1000})");
    CHECK(num(last(f), "id") == 2);  // old lines do not count by default
    f.ring.push("[other] x");
    f.ring.push("[life_skills] Woodcutting reached level 2");
    s.poll();
    auto r = last(f);
    CHECK(num(r, "id") == 3);
    CHECK(ok(r));
    CHECK(str(r, "line") == "[life_skills] Woodcutting reached level 2");
    CHECK(num(r, "seq") == 3);

    s.on_line(1, R"({"id":4,"cmd":"wait","for":"log","match":"old line","since":0})");  // since 0: the whole ring
    CHECK(num(last(f), "id") == 4);
    CHECK(ok(last(f)));

    f.world = false;
    s.on_line(1, R"({"id":5,"cmd":"wait","for":"world","timeout_ms":1000})");
    f.now += 1001;
    s.poll();
    r = last(f);
    CHECK(num(r, "id") == 5);
    CHECK_FALSE(ok(r));
    CHECK(str(r, "error") == "timed out");
    CHECK(str(r, "code") == "timeout");
}

TEST_CASE("bridge service: wait checks what it waits for") {
    Fake f;
    Service s(f.sources(), f.output());
    s.on_connect(1);
    s.on_line(1, R"({"id":1,"cmd":"wait"})");
    CHECK(str(last(f), "error") == "wait needs \"for\": world, player or log");
    s.on_line(1, R"({"id":1,"cmd":"wait","for":"rain"})");
    CHECK(str(last(f), "error") == "wait needs \"for\": world, player or log");
    s.on_line(1, R"({"id":1,"cmd":"wait","for":"log"})");
    CHECK(str(last(f), "error") == "wait for log needs \"match\": the text to look for");
    s.on_line(1, R"({"id":1,"cmd":"wait","for":"world","timeout_ms":600001})");
    CHECK(str(last(f), "error") == "timeout_ms must be a whole number from 1 to 600000");
}

TEST_CASE("bridge service: events pushes chosen game events, at most 200 a second per client, with a dropped count") {
    Fake f;
    Service s(f.sources(), f.output());
    s.on_connect(1);
    s.on_connect(2);
    CHECK_FALSE(s.wants("pickup"));
    s.on_line(1, R"({"id":1,"cmd":"events","names":["pickup"]})");
    auto r = last(f);
    CHECK(ok(r));
    CHECK(r.get("names")->arr.size() == 1);
    CHECK(s.wants("pickup"));
    CHECK_FALSE(s.wants("kill"));
    s.on_line(2, R"({"id":1,"cmd":"events","names":["teleport"]})");
    CHECK(str(last(f), "error").find("unknown event 'teleport'") == 0);
    CHECK(str(last(f), "code") == "bad_request");
    f.sent.clear();

    s.publish("kill", R"({"entity":5})");
    CHECK(f.sent.empty());  // nobody asked for kills
    s.publish("pickup", R"({"count":5})");
    REQUIRE(f.sent.size() == 1);
    CHECK(f.sent[0].client == 1);
    CHECK(f.sent[0].droppable);
    auto e = parse(f.sent[0].line);
    CHECK(str(e, "event") == "game");
    CHECK(str(e, "name") == "pickup");
    CHECK(num(*e.get("fields"), "count") == 5);

    f.sent.clear();
    f.now += 1000;  // a new second
    for (int i = 0; i < 250; ++i) s.publish("pickup", R"({"count":1})");
    CHECK(f.sent.size() == 200);
    f.now += 1000;
    s.publish("pickup", R"({"count":1})");
    REQUIRE(f.sent.size() == 201);
    CHECK(num(parse(f.sent.back().line), "dropped") == 50);
    s.publish("pickup", R"({"count":1})");
    CHECK(parse(f.sent.back().line).get("dropped") == nullptr);  // reported once

    s.on_line(1, R"({"id":2,"cmd":"events","names":[]})");  // none: stop
    f.sent.clear();
    s.publish("pickup", R"({"count":1})");
    CHECK(f.sent.empty());
}

TEST_CASE("bridge service: screenshot takes no path, answers with the picture, or says why not") {
    Fake f;
    Service s(f.sources(), f.output());
    s.on_connect(1);
    s.on_line(1, R"({"id":1,"cmd":"screenshot","path":"C:\\Windows\\x.png"})");
    CHECK(str(last(f), "error") == "screenshot takes no path: pictures always go to emf\\screens");
    CHECK(str(last(f), "code") == "bad_request");
    CHECK_FALSE(f.shot_done);

    f.shot_refuse = "the dedicated server has no screen";
    s.on_line(1, R"({"id":2,"cmd":"screenshot"})");
    CHECK(str(last(f), "error") == "the dedicated server has no screen");
    CHECK(str(last(f), "code") == "unavailable");

    f.shot_refuse.clear();
    s.on_line(1, R"({"id":3,"cmd":"screenshot"})");
    REQUIRE(f.shot_done);
    CHECK(f.shot_options.overlay);
    CHECK(f.shot_options.max_width == 0);
    ShotResult res;
    res.ok = true;
    res.path = "C:\\Game\\emf\\screens\\2026-10-26_20-15-00-123.png";
    res.width = 1920;
    res.height = 1080;
    res.hdr = true;
    f.shot_done(res);
    auto r = last(f);
    CHECK(num(r, "id") == 3);
    CHECK(ok(r));
    CHECK(str(r, "path") == res.path);
    CHECK(num(r, "width") == 1920);
    CHECK(num(r, "height") == 1080);
    CHECK(flag(r, "hdr"));
}

TEST_CASE("bridge service: screenshot options: without the overlay, and scaled down to a width") {
    Fake f;
    Service s(f.sources(), f.output());
    s.on_connect(1);
    s.on_line(1, R"({"id":1,"cmd":"screenshot","overlay":false,"max_width":640})");
    REQUIRE(f.shot_done);
    CHECK_FALSE(f.shot_options.overlay);
    CHECK(f.shot_options.max_width == 640);
    s.on_line(1, R"({"id":2,"cmd":"screenshot","max_width":8})");
    CHECK(str(last(f), "error") == "max_width must be a whole number from 16 to 16384");
    CHECK(str(last(f), "code") == "bad_request");
    s.on_line(1, R"({"id":3,"cmd":"screenshot","overlay":"no"})");
    CHECK(str(last(f), "code") == "bad_request");
    ShotResult busy;
    busy.code = code::kBusy;
    busy.error = "a screenshot is already being taken";
    f.shot_done(busy);
    auto r = last(f);
    CHECK(num(r, "id") == 1);
    CHECK(str(r, "code") == "busy");
}

TEST_CASE("bridge service: a screenshot no frame took in 5 s is cancelled; one a frame took gets more time") {
    Fake f;
    Service s(f.sources(), f.output());
    s.on_connect(1);
    s.on_line(1, R"({"id":1,"cmd":"screenshot"})");
    f.now += 5001;
    s.poll();
    CHECK(f.shot_cancels == 1);
    auto r = last(f);
    CHECK_FALSE(ok(r));
    CHECK(str(r, "error").find("no frame") != std::string::npos);
    CHECK(str(r, "code") == "timeout");

    f.sent.clear();
    s.on_line(1, R"({"id":2,"cmd":"screenshot"})");
    f.shot_taken = true;  // a frame took it: the copy is being written
    f.now += 5001;
    s.poll();
    CHECK(f.sent.empty());
    ShotResult res;
    res.error = "the swapchain format is not supported";
    f.shot_done(res);
    r = last(f);
    CHECK(num(r, "id") == 2);
    CHECK_FALSE(ok(r));
    CHECK(str(r, "error") == "the swapchain format is not supported");
}

TEST_CASE("bridge service: mod_key presses a key for mods and answers after the release") {
    Fake f;
    Service s(f.sources(), f.output());
    s.on_connect(1);
    s.on_line(1, R"({"id":1,"cmd":"mod_key","key":"Banana"})");
    CHECK(str(last(f), "error").find("unknown key 'Banana'") == 0);
    s.on_line(1, R"({"id":1,"cmd":"mod_key"})");
    CHECK(str(last(f), "error").find("mod_key needs \"key\"") == 0);
    s.on_line(1, R"({"id":1,"cmd":"mod_key","key":"L","action":"tap"})");
    CHECK(str(last(f), "error") == "action must be press, down or up");
    f.sent.clear();

    s.on_line(1, R"({"id":2,"cmd":"mod_key","key":"l","hold_ms":200})");
    REQUIRE(f.keys.size() == 1);
    CHECK(std::get<0>(f.keys[0]) == 'L');
    CHECK(std::get<1>(f.keys[0]) == KeyAction::Press);
    CHECK(std::get<2>(f.keys[0]) == 200);
    CHECK(f.sent.empty());  // answered after the release
    KeyResult kr;
    kr.ok = true;
    kr.mod_reads = 12;
    f.key_done(kr);
    auto r = last(f);
    CHECK(ok(r));
    CHECK(str(r, "key") == "L");
    CHECK(num(r, "vk") == 'L');
    CHECK(num(r, "mod_reads") == 12);

    s.on_line(1, R"({"id":3,"cmd":"mod_key","key":"F6","action":"down"})");
    CHECK(ok(last(f)));
    s.on_disconnect(1);  // a key it held down is released
    REQUIRE(f.keys.size() == 3);
    CHECK(std::get<0>(f.keys[2]) == VK_F6);
    CHECK(std::get<1>(f.keys[2]) == KeyAction::Up);
}

TEST_CASE("bridge service: at most 8 requests wait per client; answers for a gone client or service are dropped") {
    Fake f;
    f.console_defer = true;
    auto svc = std::make_unique<Service>(f.sources(), f.output());
    svc->on_connect(1);
    for (int i = 0; i < 8; ++i) svc->on_line(1, R"({"id":1,"cmd":"wait","for":"world"})");
    CHECK(f.sent.empty());
    svc->on_line(1, R"({"id":9,"cmd":"wait","for":"world"})");
    CHECK(str(last(f), "error") == "too many requests waiting (8): wait for some answers first");
    CHECK(str(last(f), "code") == "busy");
    f.sent.clear();

    svc->on_connect(2);
    svc->on_line(2, R"({"id":1,"cmd":"console","text":"help"})");
    svc->on_disconnect(2);
    f.console_line("late");
    f.console_done("ran");
    f.now += 1000;
    svc->poll();
    CHECK(f.sent.empty());

    svc->on_connect(3);
    svc->on_line(3, R"({"id":1,"cmd":"console","text":"help"})");
    svc.reset();  // the core never does this; tests do
    f.console_line("after the end");
    f.console_done("ran");
    CHECK(f.sent.empty());
}

TEST_CASE("bridge service: console lines the world has not run yet count, even after their answer timed out") {
    Fake f;
    f.console_defer = true;
    std::vector<std::function<void()>> dones;
    auto src = f.sources();
    src.console = [&](const std::string&, std::function<void(const std::string&)>,
                      std::function<void(const std::string&)> done) { dones.push_back([done] { done("ran"); }); };
    Service s(src, f.output());
    s.on_connect(1);
    for (int i = 0; i < 8; ++i) s.on_line(1, R"({"id":1,"cmd":"console","text":"help","timeout_ms":1})");
    f.now += 2;
    s.poll();  // all eight answered "timed out", but still queued in the game
    CHECK(f.sent.size() == 8);
    s.on_line(1, R"({"id":9,"cmd":"console","text":"help"})");
    CHECK(str(last(f), "error") == "8 console lines are still waiting for the world to run them: wait for some");
    CHECK(str(last(f), "code") == "busy");
    CHECK(dones.size() == 8);
    dones[0]();  // the world ran one
    s.on_line(1, R"({"id":10,"cmd":"console","text":"help"})");
    CHECK(dones.size() == 9);
}

TEST_CASE("bridge service: quit answers first; the game is asked to close") {
    Fake f;
    Service s(f.sources(), f.output());
    s.on_connect(1);
    s.on_line(1, R"({"id":1,"cmd":"quit"})");
    CHECK(f.quits == 1);
    auto r = last(f);
    CHECK(ok(r));
    CHECK(flag(r, "quitting"));
    f.quit_refuse = "the dedicated server is stopped in its own window";
    s.on_line(1, R"({"id":2,"cmd":"quit"})");
    r = last(f);
    CHECK_FALSE(ok(r));
    CHECK(str(r, "code") == "unavailable");
}

TEST_CASE("bridge service: lines from an unknown client are ignored") {
    Fake f;
    Service s(f.sources(), f.output());
    s.on_line(77, R"({"id":1,"cmd":"hello"})");
    CHECK(f.sent.empty());
    CHECK(s.clients() == 0);
    s.on_connect(1);
    CHECK(s.clients() == 1);
}

// ---- load_world ----

TEST_CASE("bridge service: load_world hands the LOCAL slots to the game and answers once the world runs") {
    Fake f;
    Service s(f.sources(), f.output());
    s.on_connect(1);
    s.on_line(1, R"({"id":1,"cmd":"load_world","slot":1,"character":1,"expect_name":"Character 2"})");
    REQUIRE(f.loads == 1);
    CHECK(f.load_params.slot == 1u);
    CHECK(f.load_params.character == 1u);
    CHECK(f.load_params.expect_name == "Character 2");
    CHECK(f.load_params.timeout_ms == 180000u);
    CHECK(f.sent.empty());  // no answer until the world runs
    f.now += 20000;
    s.poll();
    CHECK(f.sent.empty());
    LoadWorldReply r;
    r.ok = true;
    r.slot = 1;
    r.character = 1;
    r.world = "My World 1";
    r.character_name = "Character 2";
    r.ms = 20000;
    f.load_done(r);
    const auto v = last(f);
    CHECK(ok(v));
    CHECK(num(v, "slot") == 1);
    CHECK(num(v, "character") == 1);
    CHECK(str(v, "world") == "My World 1");
    CHECK(str(v, "character_name") == "Character 2");
    CHECK(num(v, "ms") == 20000);
    CHECK(f.sent.size() == 1);
}

TEST_CASE("bridge service: load_world needs both slots as whole numbers; refusals and errors come back with their code") {
    Fake f;
    Service s(f.sources(), f.output());
    s.on_connect(1);
    s.on_line(1, R"({"id":1,"cmd":"load_world","slot":1})");
    CHECK(str(last(f), "code") == "bad_request");
    CHECK(str(last(f), "error").find("character") != std::string::npos);
    s.on_line(1, R"({"id":2,"cmd":"load_world","character":1})");
    CHECK(str(last(f), "code") == "bad_request");
    s.on_line(1, R"({"id":3,"cmd":"load_world","slot":"1","character":1})");
    CHECK(str(last(f), "code") == "bad_request");
    s.on_line(1, R"({"id":4,"cmd":"load_world","slot":1,"character":1,"expect_name":5})");
    CHECK(str(last(f), "code") == "bad_request");
    CHECK(f.loads == 0);
    f.load_refuse = Refusal{code::kUnavailable, "load_world needs the game exe of build 1273043"};
    s.on_line(1, R"({"id":5,"cmd":"load_world","slot":1,"character":1})");
    CHECK(str(last(f), "code") == "unavailable");
    CHECK(str(last(f), "error") == "load_world needs the game exe of build 1273043");
    f.load_refuse = Refusal{};
    s.on_line(1, R"({"id":6,"cmd":"load_world","slot":2,"character":1,"timeout_ms":30000})");
    CHECK(f.load_params.timeout_ms == 30000u);
    LoadWorldReply r;
    r.code = "refused";
    r.error = "LOCAL world slot 2 does not exist";
    f.load_done(r);
    const auto v = last(f);
    CHECK_FALSE(ok(v));
    CHECK(num(v, "id") == 6);
    CHECK(str(v, "code") == "refused");
    CHECK(str(v, "error") == "LOCAL world slot 2 does not exist");
}

TEST_CASE("bridge service: load_world times out on its own if the game never answers") {
    Fake f;
    Service s(f.sources(), f.output());
    s.on_connect(1);
    s.on_line(1, R"({"id":1,"cmd":"load_world","slot":1,"character":1,"timeout_ms":5000})");
    f.now += 5000;
    s.poll();
    CHECK(f.sent.empty());  // the game's own timeout answers first; this one is the backstop
    f.now += 20000;
    s.poll();
    CHECK(str(last(f), "code") == "timeout");
    LoadWorldReply late;
    late.ok = true;
    f.load_done(late);  // a late answer goes nowhere
    CHECK(f.sent.size() == 1);
}

TEST_CASE("bridge service: load_world passes test_refuse (the dev check of the guard) and checks its type") {
    Fake f;
    Service s(f.sources(), f.output());
    s.on_connect(1);
    s.on_line(1, R"({"id":1,"cmd":"load_world","slot":1,"character":1,"test_refuse":true})");
    REQUIRE(f.loads == 1);
    CHECK(f.load_params.test_refuse);
    s.on_line(1, R"({"id":2,"cmd":"load_world","slot":1,"character":1,"test_refuse":"yes"})");
    CHECK(str(last(f), "code") == "bad_request");
    CHECK(f.loads == 1);
}
