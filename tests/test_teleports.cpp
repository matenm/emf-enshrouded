// Waypoint Teleports (mods/teleports): the decisions over a fake game (tp_logic.h): the countdown and its cancel, the
// cooldowns, the combat window, /unstuck falling back to the spawn point, /home's cost and the shroud, admin arguments.
#include <doctest.h>

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "../mods/teleports/tp_logic.h"

using namespace tp;

namespace {
struct FakeGame : Game {
    uint64_t now = 100000;
    std::vector<uint32_t> list{1, 2, 3};
    std::map<uint32_t, Vec> pos{{1, {4000, 700, 1200}}, {2, {4100, 700, 1300}}, {3, {3000, 650, 900}}};
    std::set<uint32_t> dead_ones, shrouded;
    std::map<uint32_t, uint32_t> spawn{{1, 1025}, {2, 1025}};
    struct Tp {
        uint32_t player;
        TeleportAsk ask;
        std::function<void(const Outcome&)> done;
    };
    std::vector<Tp> tps;
    bool teleport_ok = true;
    struct Take {
        uint32_t player;
        std::string item;
        int count;
        std::function<void(bool)> done;
    };
    std::vector<Take> takes;
    struct Given {
        uint32_t player;
        std::string item;
        int count;
    };
    std::vector<Given> given_back;
    uint32_t sess = 1;
    std::map<uint32_t, std::string> names_of;  // player -> name (player_name)
    std::vector<std::pair<uint32_t, std::string>> tells;
    std::vector<std::string> logs;

    uint64_t now_ms() override { return now; }
    std::vector<uint32_t> players() override { return list; }
    std::vector<uint32_t> players_named(const std::string& n) override {
        std::vector<uint32_t> out;
        for (const auto& [id, name] : names_of)
            if (lower(name) == lower(n)) out.push_back(id);
        return out;
    }
    uint32_t session() override { return sess; }
    std::string name_of(uint32_t p) override {
        auto it = names_of.find(p);
        return it == names_of.end() ? "" : it->second;
    }
    bool names = false;
    bool names_available() override { return names; }
    std::optional<Vec> position(uint32_t p) override {
        auto it = pos.find(p);
        if (it == pos.end()) return std::nullopt;
        return it->second;
    }
    uint64_t account(uint32_t p) override { return 0xACC0 + p; }
    bool dead(uint32_t p) override { return dead_ones.count(p) != 0; }
    bool in_shroud(uint32_t p) override { return shrouded.count(p) != 0; }
    uint32_t spawn_point(uint32_t p) override {
        auto it = spawn.find(p);
        return it == spawn.end() ? 0 : it->second;
    }
    bool teleport(uint32_t p, const TeleportAsk& ask, std::function<void(const Outcome&)> done) override {
        if (!teleport_ok) return false;
        tps.push_back({p, ask, std::move(done)});
        return true;
    }
    bool take_cost(uint32_t p, const std::string& item, int count, std::function<void(bool)> done) override {
        takes.push_back({p, item, count, std::move(done)});
        return true;
    }
    void give_back(uint32_t p, const std::string& item, int count) override { given_back.push_back({p, item, count}); }
    void tell(uint32_t p, const std::string& t) override { tells.push_back({p, t}); }
    void log(const std::string& t) override { logs.push_back(t); }

    // the game moves the last teleport's player to `at` and answers
    void answer(bool moved, Vec at, bool raw = false, const std::string& reason = "") {
        REQUIRE(!tps.empty());
        Tp t = tps.back();
        if (moved) pos[t.player] = at;
        Outcome o;
        o.moved = moved;
        o.pos = at;
        o.raw_target = raw;
        o.reason = reason;
        o.session_ended = ended_next;
        ended_next = false;
        t.done(o);
    }
    std::string last_tell() const { return tells.empty() ? "" : tells.back().second; }
    bool ended_next = false;  // the next answer says the session ended first
};

struct Rig {
    FakeGame g;
    Teleports t{g};
    Rig() { t.set(Settings{}); }
    void wait(uint64_t ms) {
        for (uint64_t done = 0; done < ms; done += 100) {
            g.now += 100;
            t.tick();
        }
    }
};
}  // namespace

TEST_CASE("teleports: parsing: enabled commands, durations, players and positions") {
    CHECK(parse_enabled("unstuck, HOME tp.pos nonsense") == std::set<std::string>{"unstuck", "home", "tp.pos"});
    CHECK(parse_enabled("").empty());
    CHECK(parse_enabled(kDefaultEnabled).size() == 7);
    CHECK(duration(500) == "1 s");
    CHECK(duration(45000) == "45 s");
    CHECK(duration(125000) == "2 min 5 s");
    CHECK(duration(600000) == "10 min");
    CHECK(parse_who("#2").type == Who::Index);
    CHECK(parse_who("#2").index == 2);
    CHECK(parse_who("3").index == 3);
    CHECK(parse_who("#0").type == Who::None);
    CHECK(parse_who("Ember").type == Who::Name);
    CHECK(parse_who("Ember").name == "Ember");
    PosArgs p = parse_pos_args({"4000", "720.5", "1200", "ground"});
    CHECK(p.ok);
    CHECK(p.ground);
    CHECK(p.at.y == 720.5);
    CHECK(p.who.type == Who::None);
    p = parse_pos_args({"#2", "1", "2", "3"});
    CHECK(p.ok);
    CHECK(p.who.index == 2);
    CHECK_FALSE(p.ground);
    CHECK_FALSE(parse_pos_args({"1", "2"}).ok);
    CHECK_FALSE(parse_pos_args({"1", "x", "3"}).ok);
    CHECK_FALSE(parse_pos_args({"1", "2", "nan"}).ok);
    CHECK_FALSE(parse_pos_args({"#0", "1", "2", "3"}).ok);
}

TEST_CASE("teleports: /unstuck counts down standing still, then hops up with the ground search; a cooldown follows") {
    Rig r;
    const std::string said = r.t.unstuck(1);
    CHECK(said.find("Unstuck in 3 s: stand still") == 0);
    CHECK(r.t.pending(1));
    r.wait(900);
    CHECK(r.g.tells.empty());
    r.wait(100);
    CHECK(r.g.last_tell() == "Unstuck in 2...");
    r.wait(1000);
    CHECK(r.g.last_tell() == "Unstuck in 1...");
    CHECK(r.g.tps.empty());
    r.wait(1000);
    REQUIRE(r.g.tps.size() == 1);
    CHECK_FALSE(r.t.pending(1));
    const TeleportAsk& a = r.g.tps[0].ask;
    CHECK(a.to.x == 4000);
    CHECK(a.to.y == 704);  // 4 m up: the ground search starts there
    CHECK(a.ground);
    CHECK(a.spawn_point == 0u);
    r.g.answer(true, {4003, 701, 1201});  // free ground beside the hole
    CHECK(r.g.last_tell() == "Unstuck");
    CHECK(r.g.tps.size() == 1);  // no fallback
    CHECK(r.t.unstuck(1).find("/unstuck again in 1 min") == 0);
    r.g.now += 60000;
    CHECK(r.t.unstuck(1).find("Unstuck in 3 s") == 0);
}

TEST_CASE("teleports: moving 2 m during the countdown cancels it; a player who leaves is dropped") {
    Rig r;
    r.t.unstuck(1);
    r.wait(1000);
    r.g.pos[1].x += 1.5;  // a small shuffle is fine
    r.wait(500);
    CHECK(r.t.pending(1));
    r.g.pos[1].x += 1.0;  // 2.5 m from the start
    r.wait(100);
    CHECK_FALSE(r.t.pending(1));
    CHECK(r.g.last_tell() == "/unstuck cancelled: you moved");
    r.wait(5000);
    CHECK(r.g.tps.empty());
    // no cooldown after a cancel
    CHECK(r.t.home(1).find("Home in 3 s") == 0);
    r.g.pos.erase(1);  // left the game
    r.wait(4000);
    CHECK(r.g.tps.empty());
    CHECK(r.t.pending_count() == 0);
}

TEST_CASE("teleports: /unstuck waits combat_seconds after a hit on the player, and never runs while dead") {
    Rig r;
    r.t.on_hit(1);
    CHECK(r.t.unstuck(1).find("You were hit just now: /unstuck works 10 s after") == 0);
    r.g.now += 4000;
    CHECK(r.t.unstuck(1).find("6 s") != std::string::npos);
    r.g.now += 6000;
    CHECK(r.t.unstuck(1).find("Unstuck in") == 0);
    r.t.on_hit(5000);  // not a player
    CHECK(r.t.unstuck(2).find("Unstuck in") == 0);
    r.g.dead_ones.insert(3);
    CHECK(r.t.unstuck(3) == "You are dead: respawn first");
    CHECK(r.t.home(3) == "You are dead: respawn first");
    // dying during the countdown: no teleport
    r.g.dead_ones.insert(1);
    r.wait(3100);
    CHECK(r.g.last_tell().find("dead") != std::string::npos);
    REQUIRE(r.g.tps.size() == 1);  // player 2's only
    CHECK(r.g.tps[0].player == 2u);
    // the shroud does not stop /unstuck
    Rig s;
    s.g.shrouded.insert(1);
    CHECK(s.t.unstuck(1).find("Unstuck in") == 0);
}

TEST_CASE("teleports: /unstuck falls back to the spawn point when the ground search found nothing or left you in place") {
    Rig r;
    r.t.unstuck(1);
    r.wait(3100);
    REQUIRE(r.g.tps.size() == 1);
    r.g.answer(true, {4000, 704, 1200}, /*raw*/ true);  // hanging at the raw target
    REQUIRE(r.g.tps.size() == 2);
    CHECK(r.g.tps[1].ask.spawn_point == 1025u);
    r.g.answer(true, {3900, 690, 1152});
    CHECK(r.g.last_tell() == "No free ground nearby: you are at your spawn point");
    CHECK(r.t.unstuck(1).find("again in") != std::string::npos);  // the cooldown started

    Rig same;  // the search put the player back into the same hole
    same.t.unstuck(1);
    same.wait(3100);
    same.g.answer(true, {4000.5, 700.2, 1200});
    REQUIRE(same.g.tps.size() == 2);
    CHECK(same.g.tps[1].ask.spawn_point == 1025u);
    same.g.answer(false, {}, false, "entity 1 is dead");
    CHECK(same.g.last_tell().find("/unstuck did not work: entity 1 is dead") != std::string::npos);
    CHECK(same.t.unstuck(1).find("Unstuck in") == 0);  // no cooldown: it did not work

    Rig none;  // no spawn point known
    none.g.spawn.clear();
    none.t.unstuck(1);
    none.wait(3100);
    none.g.answer(true, {4000, 704, 1200}, true);
    CHECK(none.g.tps.size() == 1);
    CHECK(none.g.last_tell() == "No free ground found nearby, and no spawn point known");

    Rig failed;  // EMF refused when it ran
    failed.t.unstuck(1);
    failed.wait(3100);
    failed.g.answer(false, {}, false, "a game teleport of entity 1 is still under way (waited 5 s)");
    CHECK(failed.g.tps.size() == 1);
    CHECK(failed.g.last_tell().find("/unstuck did not work: a game teleport") == 0);
    failed.g.teleport_ok = false;  // refused at once
    failed.t.unstuck(1);
    failed.wait(3100);
    CHECK(failed.g.last_tell() == "/unstuck was refused (see the log)");
}

TEST_CASE("teleports: /home goes to the spawn point's pads after the countdown; not inside the shroud by default") {
    Rig r;
    r.g.shrouded.insert(1);
    CHECK(r.t.home(1).find("Not inside the shroud") == 0);
    Settings s;
    s.home_in_shroud = true;
    r.t.set(s);
    CHECK(r.t.home(1).find("Home in 3 s") == 0);
    r.wait(3100);
    REQUIRE(r.g.tps.size() == 1);
    CHECK(r.g.tps[0].ask.spawn_point == 1025u);
    CHECK_FALSE(r.g.tps[0].ask.ground);
    r.g.answer(true, {3900, 690, 1152});
    CHECK(r.g.last_tell() == "Home");
    CHECK(r.t.home(1).find("/home again in 10 min") == 0);
    // entering the shroud during the countdown (home_in_shroud off): no teleport
    Rig q;
    q.t.home(2);
    q.g.shrouded.insert(2);
    q.wait(3100);
    CHECK(q.g.tps.empty());
    CHECK(q.g.last_tell() == "Not inside the shroud: no teleport");
    // no spawn point: refused at once
    Rig n;
    CHECK(n.t.home(3) == "No spawn point known: rest at a flame altar first");
}

TEST_CASE("teleports: /home's cost is taken when it happens and given back when the teleport fails") {
    Rig r;
    Settings s;
    s.home_cost_item = "Rune";
    s.home_cost_count = 2;
    r.t.set(s);
    CHECK(r.t.home(1).find("It costs 2 x Rune.") != std::string::npos);
    r.wait(3100);
    REQUIRE(r.g.takes.size() == 1);
    CHECK(r.g.takes[0].item == "Rune");
    CHECK(r.g.takes[0].count == 2);
    CHECK(r.g.tps.empty());
    r.g.takes[0].done(false);  // not carried
    CHECK(r.g.tps.empty());
    CHECK(r.g.last_tell() == "/home costs 2 x Rune: you do not carry that many");
    CHECK(r.t.home(1).find("Home in") == 0);  // no cooldown
    r.wait(3100);
    REQUIRE(r.g.takes.size() == 2);
    r.g.takes[1].done(true);
    REQUIRE(r.g.tps.size() == 1);
    r.g.answer(false, {}, false, "entity 1 was not moved within 10 s");
    REQUIRE(r.g.given_back.size() == 1);
    CHECK(r.g.given_back[0].player == 1u);
    CHECK(r.g.given_back[0].count == 2);
    CHECK(r.g.last_tell().find("(the cost was given back)") != std::string::npos);
    // refused at once after paying: given back too
    r.g.teleport_ok = false;
    r.t.home(1);
    r.wait(3100);
    r.g.takes.back().done(true);
    CHECK(r.g.given_back.size() == 2);
    // paid and moved: kept
    r.g.teleport_ok = true;
    r.t.home(1);
    r.wait(3100);
    r.g.takes.back().done(true);
    r.g.answer(true, {3900, 690, 1152});
    CHECK(r.g.given_back.size() == 2);
    CHECK(r.g.last_tell() == "Home");
}

TEST_CASE("teleports: countdown 0 teleports at once; switched-off commands and the console get plain answers") {
    Rig r;
    Settings s;
    s.countdown_s = 0;
    r.t.set(s);
    CHECK(r.t.unstuck(1) == "Unsticking you");
    CHECK(r.g.tps.size() == 1);
    CHECK(r.t.unstuck(0).find("is for players") != std::string::npos);
    s.unstuck = false;
    r.t.set(s);
    CHECK(r.t.unstuck(2) == "/unstuck is switched off here");
    s.unstuck = true;
    s.enabled = parse_enabled("unstuck");
    r.t.set(s);
    CHECK(r.t.home(2) == "/home is switched off here");
    CHECK(r.t.tp(1, {"#2"}) == "tp is switched off here");
    CHECK(r.t.bring(1, {"#2"}) == "bring is switched off here");
    CHECK(r.t.tp_pos(1, {"1", "2", "3"}) == "tp.pos is switched off here");
    CHECK(r.t.tp_back(1, {}) == "tp.back is switched off here");
    CHECK(r.t.unstuck(2).find("Unsticking") == 0);
    CHECK(r.t.home(5).find("is switched off") != std::string::npos);
    Rig a;
    CHECK(a.t.unstuck(9).find("no position") != std::string::npos);
    a.t.unstuck(1);
    CHECK(a.t.unstuck(1).find("already about to teleport") != std::string::npos);
}

TEST_CASE("teleports: admins: tp to a player, tp one player to another, bring; #k counts players lowest id first") {
    Rig r;
    std::string said = r.t.tp(1, {"#2"});
    CHECK(said.find("tp: moving player 1 to (4101.0, 700.0, 1300.0)") == 0);
    REQUIRE(r.g.tps.size() == 1);
    CHECK(r.g.tps[0].player == 1u);
    CHECK(r.g.tps[0].ask.ground);
    r.g.answer(true, {4101, 700, 1300});
    CHECK(r.g.last_tell().find("tp: player 1 is at (4101.0, 700.0, 1300.0)") == 0);
    said = r.t.tp(0, {"#3", "#2"});  // from a dedicated server's console
    CHECK(said.find("tp: moving player 3 to") == 0);
    CHECK(r.g.tps.back().player == 3u);
    CHECK(r.t.tp(0, {"#2"}).find("from the console name both players") != std::string::npos);
    CHECK(r.t.tp(1, {"#9"}).find("there is no player #9") == 0);
    CHECK(r.t.tp(1, {"#2", "#2"}).find("the same player") != std::string::npos);
    CHECK(r.t.tp(1, {}).find("usage: tp") == 0);
    CHECK(r.t.tp(1, {"Ember"}).find("player names need a newer EMF") == 0);
    r.g.names = true;
    r.g.names_of[2] = "Ember";
    CHECK(r.t.tp(1, {"Ember"}).find("tp: moving player 1") == 0);
    CHECK(r.t.tp(1, {"Nobody"}).find("no player named Nobody") == 0);
    said = r.t.bring(1, {"#3"});
    CHECK(said.find("bring: moving player 3 to") == 0);
    CHECK(r.t.bring(0, {"#3"}).find("no \"you\" on the console") != std::string::npos);
    CHECK(r.t.bring(1, {"#1"}) == "bring: that is you");
    r.g.dead_ones.insert(2);
    CHECK(r.t.bring(1, {"#2"}).find("is dead") != std::string::npos);
    r.g.teleport_ok = false;
    CHECK(r.t.bring(1, {"#3"}).find("refused") != std::string::npos);
}

TEST_CASE("teleports: tp.pos and tp.back (where the player stood before their last teleport, per account, this session)") {
    Rig r;
    CHECK(r.t.tp_back(1, {}).find("no earlier position") != std::string::npos);
    CHECK(r.t.tp_pos(1, {"4500", "760", "1400", "ground"}).find("tp.pos: moving player 1 to (4500.0, 760.0, 1400.0)") == 0);
    CHECK(r.g.tps.back().ask.ground);
    r.g.answer(true, {4500, 758, 1400});
    CHECK(r.t.tp_back(1, {}).find("tp.back: moving player 1 to (4000.0, 700.0, 1200.0)") == 0);
    CHECK_FALSE(r.g.tps.back().ask.ground);
    r.g.answer(true, {4000, 700, 1200});
    CHECK(r.t.tp_back(1, {}).find("to (4500.0, 758.0, 1400.0)") != std::string::npos);  // back and forth
    CHECK(r.t.tp_pos(0, {"#2", "1", "2", "3"}).find("tp.pos: moving player 2") == 0);
    CHECK(r.t.tp_pos(0, {"1", "2", "3"}).find("name the player") != std::string::npos);
    CHECK(r.t.tp_pos(1, {"1", "2"}).find("usage") == 0);
    // a failed move keeps the old tp.back position
    Rig f;
    f.t.tp_pos(1, {"4500", "760", "1400"});
    f.g.answer(false, {}, false, "x");
    CHECK(f.t.tp_back(1, {}).find("no earlier position") != std::string::npos);
    // /unstuck and /home remember too; a new session forgets
    Rig h;
    h.t.home(1);
    h.wait(3100);
    h.g.answer(true, {3900, 690, 1152});
    CHECK(h.t.tp_back(0, {"#1"}).find("to (4000.0, 700.0, 1200.0)") != std::string::npos);
    h.t.new_session();
    CHECK(h.t.tp_back(0, {"#1"}).find("no earlier position") != std::string::npos);
    CHECK(h.t.home(1).find("Home in") == 0);  // the cooldown is gone too
}

TEST_CASE("teleports: tp.list names every player's #k") {
    Rig r;
    const std::string list = r.t.tp_list();
    CHECK(list.find("#1 = player 1 at (4000, 700, 1200)") == 0);
    CHECK(list.find("#3 = player 3 at (3000, 650, 900)") != std::string::npos);
    r.g.list.clear();
    CHECK(r.t.tp_list() == "no players");
}


// ---- edge cases ----

TEST_CASE("teleports: /home gives back exactly what it took, even when the cost setting changed meanwhile") {
    Rig r;
    Settings s;
    s.home_cost_item = "Rune";
    s.home_cost_count = 2;
    r.t.set(s);
    r.t.home(1);
    r.wait(3100);
    REQUIRE(r.g.takes.size() == 1);
    Settings changed = s;  // the admin changes the cost while the take is under way
    changed.home_cost_item = "Gem";
    changed.home_cost_count = 5;
    r.t.set(changed);
    r.g.takes[0].done(true);
    REQUIRE(r.g.tps.size() == 1);
    r.g.answer(false, {}, false, "entity 1 was not moved within 10 s");
    REQUIRE(r.g.given_back.size() == 1);
    CHECK(r.g.given_back[0].item == "Rune");
    CHECK(r.g.given_back[0].count == 2);
}

TEST_CASE("teleports: nothing is given back or told when the session ended first (the take may not have been saved)") {
    Rig r;
    Settings s;
    s.home_cost_item = "Rune";
    s.home_cost_count = 2;
    r.t.set(s);
    r.t.home(1);
    r.wait(3100);
    r.g.takes[0].done(true);
    REQUIRE(r.g.tps.size() == 1);
    const size_t told = r.g.tells.size();
    r.g.ended_next = true;
    r.g.answer(false, {}, false, "a new world was set up");
    CHECK(r.g.given_back.empty());
    CHECK(r.g.tells.size() == told);
    // a take answered in a new session: no teleport, nothing given back
    Rig q;
    q.t.set(s);
    q.t.home(1);
    q.wait(3100);
    q.g.sess = 2;
    q.g.takes[0].done(true);
    CHECK(q.g.tps.empty());
    CHECK(q.g.given_back.empty());
}

TEST_CASE("teleports: /unstuck's way to the spawn point is a /home: its switch, shroud rule, cooldown and cost") {
    auto fallback = [](Rig& r) {  // /unstuck on open ground: the hop changes nothing
        r.t.unstuck(1);
        r.wait(3100);
        REQUIRE(!r.g.tps.empty());
        r.g.answer(true, {4000.2, 700, 1200});
    };
    {
        Rig r;
        Settings s;
        s.home = false;
        r.t.set(s);
        fallback(r);
        CHECK(r.g.tps.size() == 1);  // no second teleport
        CHECK(r.g.last_tell().find("/unstuck could not help here") == 0);
    }
    {
        Rig r;
        r.g.shrouded.insert(1);
        fallback(r);
        CHECK(r.g.tps.size() == 1);
        CHECK(r.g.last_tell().find("could not help here") != std::string::npos);
        CHECK(r.g.last_tell().find("shroud") != std::string::npos);
    }
    {
        Rig r;
        Settings s;
        s.home_cost_item = "Rune";
        s.home_cost_count = 2;
        r.t.set(s);
        fallback(r);
        REQUIRE(r.g.takes.size() == 1);  // the cost of a /home
        CHECK(r.g.takes[0].count == 2);
        CHECK(r.g.tps.size() == 1);
        r.g.takes[0].done(true);
        REQUIRE(r.g.tps.size() == 2);
        CHECK(r.g.tps[1].ask.spawn_point == 1025u);
        r.g.answer(true, {3900, 690, 1152});
        CHECK(r.t.home(1).find("/home again in 10 min") == 0);  // the cooldown is shared
        r.g.now += 60000;
        CHECK(r.t.unstuck(1).find("Unstuck in") == 0);  // /unstuck itself again after its own minute
        r.wait(3100);
        r.g.answer(true, {3900.2, 690, 1152});  // the hop helps nothing again: /home is still cooling down
        CHECK(r.g.tps.size() == 3);
        CHECK(r.g.last_tell().find("could not help here") != std::string::npos);
        CHECK(r.g.last_tell().find("again in") != std::string::npos);
    }
}

TEST_CASE("teleports: the combat window is checked again when the countdown ends, and /home has one too") {
    Rig r;
    r.t.unstuck(1);
    r.wait(1500);
    r.t.on_hit(1);  // hit during the countdown
    r.wait(1600);
    CHECK(r.g.tps.empty());
    CHECK(r.g.last_tell().find("hit") != std::string::npos);
    Rig h;
    h.t.on_hit(1);
    CHECK(h.t.home(1).find("You were hit just now: /home works") == 0);
    h.g.now += 10000;
    CHECK(h.t.home(1).find("Home in") == 0);
    h.wait(1000);
    h.t.on_hit(1);
    h.wait(2100);
    CHECK(h.g.tps.empty());
    Settings s;
    s.home_combat_s = 0;  // /home's own window can be switched off
    Rig z;
    z.t.set(s);
    z.t.on_hit(1);
    CHECK(z.t.home(1).find("Home in") == 0);
}

TEST_CASE("teleports: names: one match is that player, two players with one name are asked for #k") {
    Rig r;
    r.g.names = true;
    r.g.names_of = {{2, "Ember"}, {3, "ember"}};
    CHECK(r.t.tp(1, {"Ember"}).find("2 players are named Ember: use #k") == 0);
    r.g.names_of = {{2, "Ember"}, {3, "Ash"}};
    CHECK(r.t.bring(1, {"ash"}).find("bring: moving player 3") == 0);
    CHECK(r.t.tp_list().find("#2 = Ember (player 2)") != std::string::npos);
}
