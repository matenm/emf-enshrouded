// Server Herald (mods/herald): joins and leaves, announcements, the restart countdown, playtime, finding a player, the
// chat log's lines and the Discord webhook's filter and queue, without the game or the network.
#include <doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../mods/herald/herald_logic.h"
#include "core/manifest.h"

using namespace herald;

namespace {
constexpr uint64_t kSec = 1000;
constexpr uint64_t kA = 0xa1b2c3d4e5f60718ull, kB = 0x00000000000000b2ull, kC = 0x00000000000000c3ull;

Seen seen(uint32_t entity, uint64_t account, bool ready = true, const char* name = "") {
    Seen s;
    s.entity = entity;
    s.account = account;
    s.ready = ready;
    s.name = name;
    return s;
}

struct FakeTransport : Transport {
    std::vector<std::string> bodies;
    int status = 204;
    int post(const std::string& body) override {
        bodies.push_back(body);
        return status;
    }
};
}  // namespace

// ---- joins and leaves ----
TEST_CASE("herald roster: a player joins once ready, leaves when the entity is gone; counts after each change") {
    Roster r;
    auto c = r.update({seen(1, kA, false, "Ember")}, 0, 5 * kSec);
    CHECK(c.empty());  // there, but still loading
    c = r.update({seen(1, kA, true, "Ember")}, 1 * kSec, 5 * kSec);
    REQUIRE(c.size() == 1);
    CHECK(c[0].kind == Change::Joined);
    CHECK(c[0].name == "Ember");
    CHECK(c[0].online == 1);
    c = r.update({seen(1, kA, true, "Ember"), seen(2, kB, true, "Ash")}, 2 * kSec, 5 * kSec);
    REQUIRE(c.size() == 1);
    CHECK(c[0].entity == 2);
    CHECK(c[0].online == 2);
    CHECK(r.update({seen(1, kA), seen(2, kB)}, 3 * kSec, 5 * kSec).empty());  // nothing changed
    c = r.update({seen(2, kB, true, "Ash")}, 61 * kSec, 5 * kSec);
    REQUIRE(c.size() == 1);
    CHECK(c[0].kind == Change::Left);
    CHECK(c[0].entity == 1);
    CHECK(c[0].session_ms == 60 * kSec);
    CHECK(c[0].online == 1);
}

TEST_CASE("herald roster: no account yet is not a join; a joined player whose ready flag drops stays online") {
    Roster r;
    CHECK(r.update({seen(1, 0, true)}, 0, 0).empty());
    CHECK(r.update({seen(1, kA, true)}, kSec, 0).size() == 1);
    CHECK(r.update({seen(1, kA, false)}, 2 * kSec, 0).empty());  // still there (a loading screen), not a leave
    CHECK(r.online().size() == 1);
}

TEST_CASE("herald roster: a reconnect with a new entity id is a leave and a join; an entity id reused by another account too") {
    Roster r;
    r.update({seen(1, kA)}, 0, 0);
    auto c = r.update({seen(3, kA)}, 10 * kSec, 0);
    REQUIRE(c.size() == 2);
    CHECK(c[0].kind == Change::Left);
    CHECK(c[0].entity == 1);
    CHECK(c[1].kind == Change::Joined);
    CHECK(c[1].entity == 3);
    c = r.update({seen(3, kB)}, 20 * kSec, 0);  // the id now holds another player
    REQUIRE(c.size() == 2);
    CHECK(c[0].account == kA);
    CHECK(c[1].account == kB);
}

TEST_CASE("herald roster: the MOTD is due after the delay, once per join; online lists lowest entity first") {
    Roster r;
    r.update({seen(5, kA), seen(2, kB)}, 0, 5 * kSec);
    CHECK(r.motd_due(4 * kSec).empty());
    auto due = r.motd_due(5 * kSec);
    CHECK(due.size() == 2);
    for (const auto& o : due) r.motd_result(o.entity, true, 5 * kSec);
    CHECK(r.motd_due(9 * kSec).empty());
    REQUIRE(r.online().size() == 2);
    CHECK(r.online()[0].entity == 2);
    CHECK(r.online()[1].entity == 5);
    r.update({seen(5, kA)}, 10 * kSec, 5 * kSec);  // 2 leaves and comes back: a new MOTD
    r.update({seen(5, kA), seen(2, kB)}, 11 * kSec, 5 * kSec);
    due = r.motd_due(16 * kSec);
    REQUIRE(due.size() == 1);
    CHECK(due[0].entity == 2);
}

TEST_CASE("herald: the MOTD fills in {name} and {online}; lines get the prefix") {
    CHECK(expand_motd("Welcome {name}! {online} online. Bye {name}", "Ember", 3) == "Welcome Ember! 3 online. Bye Ember");
    CHECK(with_prefix("[Server]", "hi") == "[Server] hi");
    CHECK(with_prefix("", "hi") == "hi");
    CHECK(player_label(std::nullopt, 7) == "player #7");
    CHECK(player_label(std::string("A\nB"), 7) == "A?B");
}

TEST_CASE("herald: /players numbers the players and wraps long lists") {
    Roster r;
    r.update({seen(1, kA, true, "Ember"), seen(2, kB, true, "Ash")}, 0, 0);
    auto lines = players_lines(r.online(), 125 * kSec);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "2 online:");
    CHECK(lines[1] == "#1 Ember (2 min), #2 Ash (2 min)");
    CHECK(players_lines({}, 0)[0] == "nobody is online");
    std::vector<Online> many;
    for (uint32_t i = 1; i <= 16; ++i) many.push_back({i, i, "Player" + std::to_string(i), 0, 0, true});
    lines = players_lines(many, 0, 60);
    CHECK(lines.size() > 3);
    for (const auto& l : lines) CHECK(l.size() <= 60);
}

// ---- finding a player ----
TEST_CASE("herald target: #k, an exact name with spaces, a unique start, an ambiguous start, nobody") {
    std::vector<Online> on = {{1, kA, "Character 2", 0, 0, true}, {4, kB, "Alice", 0, 0, true}, {7, kC, "Alina", 0, 0, true}};
    auto t = find_target(on, {"#2", "hello"});
    CHECK(t.entity == 4);
    CHECK(t.words == 1);
    t = find_target(on, {"character", "2", "hello", "there"});
    CHECK(t.entity == 1);
    CHECK(t.words == 2);  // "hello there" is the message
    t = find_target(on, {"alice"});
    CHECK(t.entity == 4);
    t = find_target(on, {"char", "x"});
    CHECK(t.entity == 1);
    t = find_target(on, {"ali", "x"});
    CHECK(t.entity == 0);
    CHECK(t.error.find("several") != std::string::npos);
    CHECK(t.error.find("Alice") != std::string::npos);
    t = find_target(on, {"#9"});
    CHECK(t.entity == 0);
    CHECK(t.error.find("#9") != std::string::npos);
    t = find_target(on, {"#x"});
    CHECK(t.entity == 0);
    t = find_target(on, {"bob"});
    CHECK(t.error.find("no player named") != std::string::npos);
    CHECK(find_target(on, {}).entity == 0);
}

// ---- announcements ----
TEST_CASE("herald announcements: the file's lines, without empty and # lines, a BOM or CR; control characters as '?'") {
    const auto lines = parse_announcements("\xEF\xBB\xBF" "First\r\n\r\n# a note\n  Second  \nThird\x1b[31m\n");
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "First");
    CHECK(lines[1] == "Second");
    CHECK(lines[2] == "Third?[31m");
    CHECK(parse_announcements("").empty());
}

TEST_CASE("herald announcements: in turn, one per interval, only while someone is online (the clock waits otherwise)") {
    Announcer a;
    a.set_lines({"one", "two", "three"});
    const uint64_t every = 10 * 60 * kSec;
    CHECK(!a.tick(0, every, true));             // the clock starts
    CHECK(!a.tick(every - 1, every, true));
    CHECK(a.tick(every, every, true) == std::optional<std::string>("one"));
    CHECK(a.tick(2 * every, every, true) == std::optional<std::string>("two"));
    CHECK(!a.tick(3 * every, every, false));    // nobody online: nothing, and the clock waits
    CHECK(!a.tick(3 * every + 1, every, true));  // a full interval after someone is back
    CHECK(!a.tick(4 * every, every, true));
    CHECK(a.tick(4 * every + 1, every, true) == std::optional<std::string>("three"));
    CHECK(a.tick(5 * every + 1, every, true) == std::optional<std::string>("one"));  // wraps
    CHECK(!a.tick(9 * every, 0, true));  // 0 = off
    a.set_lines({"only"});              // a reload with fewer lines starts over inside the new list
    CHECK(!a.tick(10 * every, every, true));
    CHECK(a.tick(11 * every, every, true) == std::optional<std::string>("only"));
}

// ---- the restart countdown ----
namespace {
std::vector<std::string> run_countdown(Countdown& c, uint64_t from_ms, uint64_t to_ms, bool* saved) {
    std::vector<std::string> said;
    for (uint64_t t = from_ms; t <= to_ms; t += kSec)
        for (const auto& e : c.tick(t)) {
            said.push_back(e.text);
            if (e.now && saved) *saved = true;
        }
    return said;
}
}  // namespace

TEST_CASE("herald restart: from 10 minutes it warns at 5, 2, 1 min and 30 s, then 'now' with the save") {
    Countdown c;
    CHECK(c.start(600, "nightly", 0) == "Server restart in 10 minutes: nightly");
    bool saved = false;
    const auto said = run_countdown(c, kSec, 601 * kSec, &saved);
    REQUIRE(said.size() == 5);
    CHECK(said[0] == "Server restart in 5 minutes: nightly");
    CHECK(said[1] == "Server restart in 2 minutes: nightly");
    CHECK(said[2] == "Server restart in 1 minute: nightly");
    CHECK(said[3] == "Server restart in 30 seconds: nightly");
    CHECK(said[4] == "Server restarting now: nightly");
    CHECK(saved);
    CHECK(!c.running());
    CHECK(c.tick(700 * kSec).empty());
}

TEST_CASE("herald restart: from 3 minutes only the marks below it; from 45 s only 30 s; a late tick says only 'now'") {
    Countdown c;
    CHECK(c.start(180, "", 0) == "Server restart in 3 minutes");
    auto said = run_countdown(c, kSec, 181 * kSec, nullptr);
    REQUIRE(said.size() == 4);
    CHECK(said[0] == "Server restart in 2 minutes");
    CHECK(said[3] == "Server restarting now");
    CHECK(c.start(45, "update", 0) == "Server restart in 45 seconds: update");
    said = run_countdown(c, kSec, 46 * kSec, nullptr);
    REQUIRE(said.size() == 2);
    CHECK(said[0] == "Server restart in 30 seconds: update");
    CHECK(said[1] == "Server restarting now: update");
    c.start(120, "", 0);
    const auto late = c.tick(500 * kSec);  // the server stalled past the end
    REQUIRE(late.size() == 1);
    CHECK(late[0].now);
    c.start(600, "", 0);
    const auto jump = c.tick(590 * kSec);  // a stall over several marks: only the last passed one
    REQUIRE(jump.size() == 1);
    CHECK(jump[0].text == "Server restart in 30 seconds");
}

TEST_CASE("herald restart: cancel stops it; a new start replaces the old one; minutes parse") {
    Countdown c;
    c.start(600, "x", 0);
    CHECK(c.remaining_s(100 * kSec) == 500);
    CHECK(c.cancel());
    CHECK(!c.cancel());
    CHECK(c.tick(600 * kSec).empty());
    c.start(600, "a", 0);
    c.start(60, "b", 0);
    const auto said = run_countdown(c, kSec, 61 * kSec, nullptr);
    REQUIRE(said.size() == 2);
    CHECK(said[1] == "Server restarting now: b");
    CHECK(parse_minutes("10") == std::optional<uint64_t>(600));
    CHECK(parse_minutes("0.75") == std::optional<uint64_t>(45));
    CHECK(!parse_minutes("0"));
    CHECK(!parse_minutes("-1"));
    CHECK(!parse_minutes("ten"));
    CHECK(!parse_minutes("5m"));
    CHECK(!parse_minutes("100000"));
    CHECK(restart_text(90, "") == "Server restart in 1 min 30 s");
}

// ---- playtime ----
TEST_CASE("herald playtime: time adds up per account, with first and last seen and the newest name; rank by total") {
    Playtime p;
    p.add(kA, 90 * kSec, 1000, "Ember");
    p.add(kA, 30 * kSec, 1100, "Ember II");
    p.add(kB, 300 * kSec, 1050, "Ash");
    p.add(0, 999 * kSec, 1, "nobody");  // no account: not kept
    REQUIRE(p.find(kA));
    CHECK(p.find(kA)->total_ms == 120 * kSec);
    CHECK(p.find(kA)->first_seen == 1000);
    CHECK(p.find(kA)->last_seen == 1100);
    CHECK(p.find(kA)->name == "Ember II");
    CHECK(p.rank(kB) == 1);
    CHECK(p.rank(kA) == 2);
    CHECK(p.rank(kC) == 0);
    CHECK(p.size() == 2);
    CHECK(p.by_name("ash") == kB);
    CHECK(p.by_name("emb") == kA);
    CHECK(p.by_name("") == 0);
}

TEST_CASE("herald playtime: the save text round-trips, and a name with ';', '%' or a newline cannot break a line") {
    Playtime p;
    p.add(kA, 3723 * kSec, 1000, "Ev;il\nName 100%");
    p.add(kB, 5 * kSec, 2000, "Ash");
    const std::string text = p.text();
    CHECK(std::count(text.begin(), text.end(), '\n') == 2);
    Playtime q;
    q.merge_text(text);
    REQUIRE(q.find(kA));
    CHECK(q.find(kA)->total_ms == 3723 * kSec);
    CHECK(q.find(kA)->name == "Ev;il\nName 100%");
    CHECK(q.find(kA)->first_seen == 1000);
    CHECK(q.find(kB)->name == "Ash");
    CHECK(q.text() == text);
    Playtime r;
    r.merge_text("garbage\n0xzz;1;2;3;x\n;;;;\n" + text + "0x01;5;6\n");  // broken lines are skipped
    CHECK(r.size() == 2);
}

TEST_CASE("herald playtime: time counted before the world's file was read is added to it (merge)") {
    Playtime p;
    p.add(kA, 10 * kSec, 5000, "Ember");  // this session, before the file arrived
    Playtime saved;
    saved.add(kA, 100 * kSec, 1000, "Old Name");
    p.merge_text(saved.text());
    CHECK(p.find(kA)->total_ms == 110 * kSec);
    CHECK(p.find(kA)->first_seen == 1000);
    CHECK(p.find(kA)->last_seen == 5000);
    CHECK(p.find(kA)->name == "Ember");  // the newer name stays
    CHECK(duration_text(45) == "45 s");
    CHECK(duration_text(12 * 60 + 5) == "12 min");
    CHECK(duration_text(3 * 3600 + 5 * 60) == "3 h 05 min");
}

// ---- the chat log ----
TEST_CASE("herald chat log: file name per day, 'HH:MM:SS 0x<account> <name>: <text>', control characters as '?'") {
    CHECK(chat_log_name(2026, 10, 7) == "chat-2026-10-07.log");
    CHECK(chat_log_line(9, 5, 3, kA, "Ember", "hello") == "09:05:03 0xa1b2c3d4e5f60718 Ember: hello");
    CHECK(chat_log_line(23, 59, 59, kB, "Ev\nil", "fake\r\n10:00:00 line") ==
          "23:59:59 0x00000000000000b2 Ev?il: fake??10:00:00 line");
    CHECK(chat_log_event(1, 2, 3, "Ember joined (1 online)") == "01:02:03 -- Ember joined (1 online)");
}

// ---- the Discord webhook ----
TEST_CASE("herald webhook: only https to discord.com or discordapp.com under /api/webhooks/, unless any host") {
    CHECK(webhook_url_allowed("https://discord.com/api/webhooks/123/abc", false));
    CHECK(webhook_url_allowed("https://discordapp.com/api/webhooks/123/abc", false));
    CHECK(webhook_url_allowed("https://ptb.discord.com/api/webhooks/1/x", false));
    CHECK(webhook_url_allowed("HTTPS://Discord.com/api/webhooks/1/x", false));
    CHECK(webhook_url_allowed("https://discord.com:443/api/webhooks/1/x?wait=true", false));
    std::string why;
    CHECK(!webhook_url_allowed("http://discord.com/api/webhooks/1/x", false, &why));
    CHECK(why.find("https") != std::string::npos);
    CHECK(why.find("discord.com/api") == std::string::npos);  // never repeats the URL
    CHECK(!webhook_url_allowed("https://evil.com/api/webhooks/1/x", false));
    CHECK(webhook_url_allowed("https://evil.com/api/webhooks/1/x", true));
    CHECK(!webhook_url_allowed("http://evil.com/x", true));  // https even with any host
    CHECK(!webhook_url_allowed("https://discord.com.evil.com/api/webhooks/1/x", false));
    CHECK(!webhook_url_allowed("https://evildiscord.com/api/webhooks/1/x", false));
    CHECK(!webhook_url_allowed("https://discord.com@evil.com/api/webhooks/1/x", false));
    CHECK(!webhook_url_allowed("https://discord.com@evil.com/api/webhooks/1/x", true));  // userinfo never
    CHECK(!webhook_url_allowed("https://evil.com\\@discord.com/api/webhooks/1/x", false));
    CHECK(!webhook_url_allowed("https://discord.com/other/1/x", false));
    CHECK(!webhook_url_allowed("https://discord.com /api/webhooks/1/x", false));
    CHECK(!webhook_url_allowed("https://discord.com:99999/api/webhooks/1/x", false));
    CHECK(!webhook_url_allowed("", false));
    const auto p = split_https_url("https://Discord.com:8443/api/webhooks/1/x?wait=1#frag");
    REQUIRE(p);
    CHECK(p->host == "discord.com");
    CHECK(p->port == 8443);
    CHECK(p->path == "/api/webhooks/1/x?wait=1");
}

TEST_CASE("herald webhook: the body is escaped JSON with no pings") {
    CHECK(json_escape("a\"b\\c\nd\x01") == "a\\\"b\\\\c\\nd\\u0001");
    CHECK(webhook_body("@everyone hi") == "{\"content\":\"@everyone hi\",\"allowed_mentions\":{\"parse\":[]}}");
}

TEST_CASE("herald webhook: lines batch into one post, at most one post per 2 s, a 429 waits 10 s") {
    WebhookQueue q;
    FakeTransport t;
    CHECK(pump_webhook(q, t, 0) == 0);  // nothing waits
    q.push("Ember joined");
    q.push("Ash joined");
    CHECK(pump_webhook(q, t, 1000) == 204);
    REQUIRE(t.bodies.size() == 1);
    CHECK(t.bodies[0].find("Ember joined\\nAsh joined") != std::string::npos);
    q.push("Ember fell");
    CHECK(pump_webhook(q, t, 2000) == 0);  // only 1 s later
    CHECK(pump_webhook(q, t, 2999) == 0);
    CHECK(pump_webhook(q, t, 3000) == 204);
    CHECK(t.bodies.size() == 2);
    t.status = 429;
    q.push("x");
    CHECK(pump_webhook(q, t, 6000) == 429);
    q.push("y");
    CHECK(pump_webhook(q, t, 9000) == 0);  // backing off
    t.status = 204;
    CHECK(pump_webhook(q, t, 16000) == 204);
}

TEST_CASE("herald webhook: a batch stays under Discord's size; too many lines drop the oldest and say so") {
    WebhookQueue q;
    for (int i = 0; i < 100; ++i) q.push(std::string(50, 'a' + static_cast<char>(i % 26)));
    const auto first = q.next(0);
    REQUIRE(first);
    CHECK(first->size() <= WebhookQueue::kBatch);
    CHECK(q.waiting() > 0);  // the rest waits for the next posts
    WebhookQueue full;
    for (size_t i = 0; i < WebhookQueue::kMax + 5; ++i) full.push("line " + std::to_string(i));
    CHECK(full.dropped() == 5);
    const auto batch = full.next(0);
    REQUIRE(batch);
    CHECK(batch->rfind("(5 older line(s) dropped)", 0) == 0);
    CHECK(batch->find("line 0\n") == std::string::npos);
    CHECK(batch->find("line 5") != std::string::npos);
    WebhookQueue one;
    one.push(std::string(5000, 'z'));  // one huge line is cut
    CHECK(one.next(0)->size() == WebhookQueue::kBatch);
}

TEST_CASE("herald manifest: off by default, a server mod for API v19, every setting clean, the webhook off") {
    std::ifstream f(std::string(EMF_SOURCE_DIR) + "/mods/herald/mod.json", std::ios::binary);
    REQUIRE(f.good());
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    auto r = emf::core::parse_manifest(text);
    REQUIRE_MESSAGE(r.manifest, r.error);
    CHECK(r.manifest->id == "herald");
    CHECK(r.manifest->dll == "herald.dll");
    CHECK(r.manifest->emf == 19);
    CHECK_FALSE(r.manifest->enabled_default);
    CHECK(text.find("\"side\": \"server\"") != std::string::npos);
    bool webhook_off = false;
    for (const auto& s : r.manifest->settings) {
        CAPTURE(s.key);
        CHECK(s.problem.empty());
        CHECK(s.warning.empty());
        if (s.key == "webhook") webhook_off = s.default_value == "0" || s.default_value == "false";
    }
    CHECK(webhook_off);
    CHECK(r.manifest->settings.size() == 13);
}

TEST_CASE("herald webhook: which events go for each choice") {
    CHECK(hook_events("joins") == HookEvents::Joins);
    CHECK(hook_events("joins_deaths") == HookEvents::JoinsDeaths);
    CHECK(hook_events("all_with_chat") == HookEvents::AllWithChat);
    CHECK(hook_events("??") == HookEvents::Joins);
    CHECK(hook_wants(HookEvents::Joins, Hook::Join));
    CHECK(hook_wants(HookEvents::Joins, Hook::Restart));
    CHECK(!hook_wants(HookEvents::Joins, Hook::Death));
    CHECK(hook_wants(HookEvents::JoinsDeaths, Hook::Death));
    CHECK(!hook_wants(HookEvents::JoinsDeaths, Hook::Chat));
    CHECK(hook_wants(HookEvents::AllWithChat, Hook::Chat));
    CHECK(hook_wants(HookEvents::AllWithChat, Hook::Say));
}

TEST_CASE("herald roster: a MOTD whose tell failed (many joins at once: the chat limit) is tried again each second, 10 times") {
    Roster r;
    r.update({seen(5, kA)}, 0, 5 * kSec);
    auto due = r.motd_due(5 * kSec);
    REQUIRE(due.size() == 1);
    CHECK_FALSE(r.motd_result(5, false, 5 * kSec));  // not sent: not given up either
    CHECK(r.motd_due(5 * kSec + 500).empty());       // not before a second has passed
    for (int i = 1; i < 9; ++i) {
        REQUIRE(r.motd_due((5 + i) * kSec).size() == 1);
        CHECK_FALSE(r.motd_result(5, false, (5 + i) * kSec));
    }
    REQUIRE(r.motd_due(14 * kSec).size() == 1);
    CHECK(r.motd_result(5, false, 14 * kSec));  // the 10th failure: given up (the caller logs it)
    CHECK(r.motd_due(30 * kSec).empty());
    r.update({seen(5, kA), seen(6, kB)}, 31 * kSec, 0);
    due = r.motd_due(31 * kSec);
    REQUIRE(due.size() == 1);
    CHECK_FALSE(r.motd_result(6, true, 31 * kSec));  // sent
    CHECK(r.motd_due(40 * kSec).empty());
}

TEST_CASE("herald webhook: Discord markdown in a mirrored line is escaped, so a masked link or a mention trick stays text") {
    CHECK(discord_escape("[gift](https://phish.example)") == R"(\[gift\]\(https://phish.example\))");
    CHECK(discord_escape(R"(**bold** _it_ ~~gone~~ ||spoiler|| > quote `code` a\b)") ==
          R"(\*\*bold\*\* \_it\_ \~\~gone\~\~ \|\|spoiler\|\| \> quote \`code\` a\\b)");
    CHECK(discord_escape("plain text, 3 online") == "plain text, 3 online");
}

// ---- the webhook's worker thread ----
namespace {
struct Posted {
    std::mutex m;
    std::vector<std::string> bodies;
    std::atomic<int> connections{0};
    size_t count() {
        std::lock_guard<std::mutex> l(m);
        return bodies.size();
    }
};
struct RecordingTransport : Transport {
    std::shared_ptr<Posted> posted;
    int post(const std::string& body) override {
        std::lock_guard<std::mutex> l(posted->m);
        posted->bodies.push_back(body);
        return 204;
    }
};
template <class F>
bool within(F done, int ms) {
    for (int waited = 0; waited < ms; waited += 5) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}
}  // namespace

TEST_CASE("herald webhook worker: posts while a URL is set; with none its thread ends, and a new URL starts another") {
    auto posted = std::make_shared<Posted>();
    auto clock = std::make_shared<std::atomic<uint64_t>>(1000);  // shared with the thread, which may outlive a failed test
    WebhookWorker worker(
        [posted](const UrlParts&) {
            ++posted->connections;
            auto t = std::make_unique<RecordingTransport>();
            t->posted = posted;
            return std::unique_ptr<Transport>(std::move(t));
        },
        [](int, uint64_t) {}, [clock] { return clock->load(); }, 10);
    CHECK_FALSE(worker.running());
    const auto url = split_https_url("https://discord.com/api/webhooks/1/abc");
    REQUIRE(url);
    worker.set_url(url);
    CHECK(worker.on());
    CHECK(worker.running());
    worker.queue().push("Ember joined");
    worker.wake();
    REQUIRE(within([&] { return posted->count() == 1; }, 5000));

    worker.set_url(std::nullopt);  // the webhook or the herald switched off: the thread ends by itself
    CHECK_FALSE(worker.on());
    CHECK(worker.wait_stopped(5000));
    CHECK_FALSE(worker.running());

    *clock = 10000;  // on again, past the 2 s between posts: a new thread and a new connection
    worker.set_url(url);
    CHECK(worker.running());
    worker.queue().push("Ash joined");
    worker.wake();
    REQUIRE(within([&] { return posted->count() == 2; }, 5000));
    CHECK(posted->connections == 2);
    worker.set_url(std::nullopt);
    CHECK(worker.wait_stopped(5000));
}

TEST_CASE("herald chat log: lines wait and are written together, each day's file once and in order; 64 waiting ask for a write") {
    ChatLogBuffer b;
    CHECK(b.take().empty());
    CHECK_FALSE(b.add("chat-2026-10-27.log", "23:59:58 0x00000000000000b2 Ash: hi"));
    CHECK_FALSE(b.add("chat-2026-10-27.log", "23:59:59 -- Ember joined (2 online)"));
    CHECK_FALSE(b.add("chat-2026-10-28.log", "00:00:01 0x00000000000000b2 Ash: midnight"));
    const auto out = b.take();
    REQUIRE(out.size() == 2u);
    CHECK(out[0].first == "chat-2026-10-27.log");
    CHECK(out[0].second == "23:59:58 0x00000000000000b2 Ash: hi\r\n23:59:59 -- Ember joined (2 online)\r\n");
    CHECK(out[1].first == "chat-2026-10-28.log");
    CHECK(out[1].second == "00:00:01 0x00000000000000b2 Ash: midnight\r\n");
    CHECK(b.take().empty());  // taken once
    // a chat flood: the 64th waiting line asks to write now (the step's write does the rest)
    for (int i = 1; i < 64; ++i) CHECK_FALSE(b.add("chat-2026-10-28.log", "line " + std::to_string(i)));
    CHECK(b.add("chat-2026-10-28.log", "line 64"));
    const auto flood = b.take();
    REQUIRE(flood.size() == 1u);
    CHECK(std::count(flood[0].second.begin(), flood[0].second.end(), '\n') == 64);
    // at the game's exit the lines are taken only if no one holds the buffer (never waits)
    b.add("chat-2026-10-28.log", "last words");
    const auto last = b.try_take();
    REQUIRE(last.size() == 1u);
    CHECK(last[0].second == "last words\r\n");
}
