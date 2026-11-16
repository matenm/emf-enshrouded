// Lua 1.23 hits: emf.on_hit (each landing hit after the server step) and emf.hit_rule / emf.hit_rule_remove (rules
// the core applies before a hit lands; a script's rules go when it reloads or pauses), over the fake core API.
#include <windows.h>

#include <doctest.h>

#include <chrono>
#include <future>
#include <string>
#include <thread>

#include "lua_host/host.h"
#include "lua_test_fake.h"

using emf::lua::Host;

TEST_CASE("lua 1.23: emf.hit_rule builds the C rule") {
    Rig r(L"hit_rule");
    add_script(r.dir, "hr",
               "local id = emf.hit_rule{from = 'creatures', to = 'players', types = {'fire', 'ice'},\n"
               "                        attack = {'melee', 'ranged'}, scale = 0.5}\n"
               "emf.log('rule ' .. tostring(id))\n");
    Host host(&r.api, r.dir);
    host.load_all();
    CHECK(logged("rule 100"));
    REQUIRE(r.fake.hit_rules_added.size() == 1);
    const EmfHitRule& h = r.fake.hit_rules_added[0];
    CHECK(h.size == sizeof(EmfHitRule));
    CHECK(h.attacker == EMF_HIT_CREATURES);
    CHECK(h.target == EMF_HIT_PLAYERS);
    CHECK(h.damage_types_any == ((1u << 3) | (1u << 4)));
    CHECK(h.attacks == 3);
    CHECK(h.scale == doctest::Approx(0.5f));
    CHECK(h.cancel == 0);
    CHECK(h.healing == 0);
}

TEST_CASE("lua 1.23: emf.hit_rule with crit, min_health, an entity, objects, healing and cancel") {
    Rig r(L"hit_rule2");
    add_script(r.dir, "hr2",
               "emf.hit_rule{to = 5, min_health = 1}\n"
               "emf.hit_rule{from = 'players', crit = true, backstab = true, scale = 1.5}\n"
               "emf.hit_rule{from = 'players', to = 'objects', attack = 'tool', add = -3}\n"
               "emf.hit_rule{to = 'players', healing = true, cancel = true}\n");
    Host host(&r.api, r.dir);
    host.load_all();
    REQUIRE(r.fake.hit_rules_added.size() == 4);
    CHECK(r.fake.hit_rules_added[0].target == 5);
    CHECK(r.fake.hit_rules_added[0].min_health == 1);
    CHECK(r.fake.hit_rules_added[0].scale == 1.0f);
    CHECK(r.fake.hit_rules_added[1].flags_all == (EMF_HIT_CRIT | EMF_HIT_BACKSTAB));
    CHECK(r.fake.hit_rules_added[2].target == EMF_HIT_OBJECTS);
    CHECK(r.fake.hit_rules_added[2].attacks == (1u << 3));
    CHECK(r.fake.hit_rules_added[2].add == -3);
    CHECK(r.fake.hit_rules_added[3].healing == 1);
    CHECK(r.fake.hit_rules_added[3].cancel == 1);
}

TEST_CASE("lua 1.23: emf.hit_rule raises on an unknown key or a bad type name") {
    Rig r(L"hit_rule_bad");
    add_script(r.dir, "hb",
               "local ok, err = pcall(emf.hit_rule, {colour = 1})\n"
               "emf.log('key ' .. tostring(ok) .. ' ' .. tostring(err))\n"
               "ok, err = pcall(emf.hit_rule, {types = {'plasma'}, scale = 2})\n"
               "emf.log('type ' .. tostring(ok) .. ' ' .. tostring(err))\n"
               "ok, err = pcall(emf.hit_rule, {attack = 'lasso', scale = 2})\n"
               "emf.log('attack ' .. tostring(ok) .. ' ' .. tostring(err))\n"
               "ok, err = pcall(emf.hit_rule, {from = 'dragons', scale = 2})\n"
               "emf.log('from ' .. tostring(ok) .. ' ' .. tostring(err))\n");
    Host host(&r.api, r.dir);
    host.load_all();
    CHECK(logged("key false"));
    CHECK(logged("colour"));
    CHECK(logged("type false"));
    CHECK(logged("plasma"));
    CHECK(logged("attack false"));
    CHECK(logged("lasso"));
    CHECK(logged("from false"));
    CHECK(logged("dragons"));
    CHECK(r.fake.hit_rules_added.empty());
}

TEST_CASE("lua 1.23: a reload removes the script's rules; a pause does too") {
    Rig r(L"hit_rule_reload");
    add_script(r.dir, "hrl", "emf.hit_rule{scale = 2}\nemf.hit_rule{to = 'players', cancel = true}\n");
    add_script(r.dir, "hrp",
               "emf.hit_rule{to = 'players', min_health = 1}\n"
               "emf.command('boom', ': fails', function() error('boom') end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    REQUIRE(r.fake.hit_rules_added.size() == 3);  // ids 100, 101 (hrl), 102 (hrp)
    Sleep(20);
    write(r.dir + L"\\hrl\\main.lua", "emf.log('reloaded without rules')\n");
    host.check_reload_now();
    CHECK(logged("reloaded without rules"));
    REQUIRE(r.fake.hit_rules_removed.size() == 2);
    CHECK(r.fake.hit_rules_removed[0] + r.fake.hit_rules_removed[1] == 201u);
    r.run_command("hrp.boom");
    REQUIRE(host.status("hrp").rfind("paused", 0) == 0);
    REQUIRE(r.fake.hit_rules_removed.size() == 3);
    CHECK(r.fake.hit_rules_removed[2] == 102u);
}

TEST_CASE("lua 1.23: emf.hit_rule_remove only removes the script's own rule") {
    Rig r(L"hit_rule_own");
    add_script(r.dir, "ha", "rule = emf.hit_rule{scale = 2}\n"
                            "emf.command('rm', ': remove', function() emf.reply('own ' .. tostring(emf.hit_rule_remove(rule))) end)\n");
    add_script(r.dir, "hz", "emf.command('rm', ': remove', function() emf.reply('other ' .. tostring(emf.hit_rule_remove(100))) end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("hz.rm");
    r.run_command("ha.rm");
    r.run_command("ha.rm");
    bool other = false, own = false, again = false;
    for (const auto& l : r.fake.replies) {
        other = other || l == "other false";
        own = own || l == "own true";
        again = again || l == "own false";
    }
    CHECK(other);
    CHECK(own);
    CHECK(again);
    REQUIRE(r.fake.hit_rules_removed.size() == 1);
    CHECK(r.fake.hit_rules_removed[0] == 100u);
}

TEST_CASE("lua 1.23: emf.on_hit gets the hit table; one core listener; false on an older core") {
    Rig r(L"on_hit");
    add_script(r.dir, "oh",
               "emf.log('on_hit ' .. tostring(emf.on_hit(function(w, h)\n"
               "  emf.log('hit ' .. h.attacker .. ' ' .. h.target .. ' ' .. h.part .. ' ' .. h.amount .. ' crit ' ..\n"
               "          tostring(h.crit) .. ' blocked ' .. tostring(h.blocked) .. ' ' .. h.types[1] .. ' ' .. #h.types ..\n"
               "          ' ' .. h.attack .. ' ' .. #h.attacks .. ' killed ' .. tostring(h.killed) .. ' after ' .. h.health_after ..\n"
               "          ' mod ' .. tostring(h.from_mod) .. ' weapon ' .. h.weapon_item .. ' x ' .. h.position.x ..\n"
               "          ' server ' .. tostring(w:is_server()))\n"
               "end)))\n");
    add_script(r.dir, "oh2", "emf.on_hit(function(w, h) emf.log('second ' .. h.amount) end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    CHECK(logged("on_hit true"));
    CHECK(r.fake.hit_cbs.size() == 1);
    EmfHit h{};
    h.size = sizeof h;
    h.attacker = 1;
    h.target = 6808;
    h.part = 6809;
    h.amount = 120;
    h.flags = EMF_HIT_CRIT | EMF_HIT_KILLED;
    h.damage_types = (1u << 3) | (1u << 0);
    h.attacks = 1u << 1;  // ranged
    h.health_after = 0;
    h.health_before = -1;
    h.weapon_item = 0xABCDu;
    h.position[0] = 2.5f;
    fire_hit(kServer, h);
    CHECK(logged("hit 1 6808 6809 120 crit true blocked false cut 2 ranged 1 killed true after 0 mod false weapon 43981 x 2.5 server true"));
    CHECK(logged("second 120"));
}

TEST_CASE("lua 1.23: a hit without an attack kind has attack nil and an empty attacks list") {
    Rig r(L"on_hit_none");
    add_script(r.dir, "on", "emf.on_hit(function(w, h) emf.log('kind ' .. tostring(h.attack) .. ' ' .. #h.attacks) end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    EmfHit h{};
    h.size = sizeof h;
    h.amount = 7;
    h.health_before = -1;
    h.health_after = -1;
    fire_hit(kServer, h);
    CHECK(logged("kind nil 0"));
}

TEST_CASE("lua 1.23: on an older core emf.on_hit is false and emf.hit_rule nil") {
    Rig old(L"on_hit_old");
    old.api.on_hit = nullptr;
    old.api.hit_rule_add = nullptr;
    add_script(old.dir, "oo", "emf.log('old ' .. tostring(emf.on_hit(function() end)) .. ' ' .. tostring(emf.hit_rule{scale = 2}))\n");
    Host host2(&old.api, old.dir);
    host2.load_all();
    CHECK(logged("old false nil"));
}

TEST_CASE("lua 1.23: a hit does not wait for a script without emf.on_hit that is busy in a world step") {  // review M11
    Rig r(L"on_hit_busy");
    add_script(r.dir, "listener", "emf.on_hit(function(w, h) emf.log('heard ' .. h.amount) end)\n");
    add_script(r.dir, "slowpoke", "emf.on_tick(function(w) if w:is_server() then emf.log('hold') end end)\n");
    Host host(&r.api, r.dir);
    host.set_clock([] { return uint64_t{1000}; });
    host.load_all();
    HANDLE held = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    r.fake.log_hook = [&](const std::string& line) {
        if (line.find("hold") == std::string::npos) return;
        SetEvent(held);
        WaitForSingleObject(release, 10000);
    };
    std::thread server([&] { host.tick(kServer); });  // slowpoke stays inside its server tick
    REQUIRE(WaitForSingleObject(held, 10000) == WAIT_OBJECT_0);
    EmfHit h{};
    h.size = sizeof h;
    h.amount = 9;
    h.health_before = -1;
    h.health_after = -1;
    auto done = std::async(std::launch::async, [&] { fire_hit(kServer, h); });
    const bool quick = done.wait_for(std::chrono::milliseconds(2000)) == std::future_status::ready;
    SetEvent(release);
    server.join();
    done.wait();
    r.fake.log_hook = nullptr;
    CloseHandle(held);
    CloseHandle(release);
    CHECK(quick);
    CHECK(logged("heard 9"));
}

// ---- kinds: fall, shroud, cold and drowning ----

TEST_CASE("lua 1.23: hit.kind names; emf.on_hit gets fall, shroud, cold and drowning only when asked") {
    Rig r(L"on_hit_kinds");
    add_script(r.dir, "hk",
               "emf.on_hit(function(w, h) emf.log('plain ' .. h.kind) end)\n"
               "emf.on_hit(function(w, h) emf.log('all ' .. h.kind) end, {kinds = 'all'})\n"
               "emf.on_hit(function(w, h) emf.log('wet ' .. h.kind) end, {kinds = {'drowning'}})\n");
    Host host(&r.api, r.dir);
    host.load_all();
    EmfHit h{};
    h.size = sizeof h;
    h.amount = 5;
    h.health_before = 100;
    h.health_after = 95;
    h.kind = EMF_HIT_KIND_FALL;
    fire_hit(kServer, h);
    CHECK_FALSE(logged("plain fall"));
    CHECK(logged("all fall"));
    CHECK_FALSE(logged("wet fall"));
    h.kind = EMF_HIT_KIND_DROWNING;
    fire_hit(kServer, h);
    CHECK(logged("all drowning"));
    CHECK(logged("wet drowning"));
    h.kind = EMF_HIT_KIND_HIT;
    fire_hit(kServer, h);
    CHECK(logged("plain hit"));
    CHECK(logged("all hit"));
    CHECK_FALSE(logged("wet hit"));
}

TEST_CASE("lua 1.23: emf.hit_rule kinds: all, a name, a list; an unknown kind is an error") {
    Rig r(L"hit_rule_kinds");
    add_script(r.dir, "rk",
               "emf.hit_rule{kinds = 'all', to = 'players', min_health = 1}\n"
               "emf.hit_rule{kinds = {'fall', 'drowning'}, cancel = true}\n"
               "emf.hit_rule{kinds = 'cold', scale = 0.5}\n"
               "emf.hit_rule{scale = 2}\n"
               "local ok, err = pcall(emf.hit_rule, {kinds = 'lava', cancel = true})\n"
               "emf.log('lava ' .. tostring(ok) .. ' ' .. tostring(err))\n");
    Host host(&r.api, r.dir);
    host.load_all();
    REQUIRE(r.fake.hit_rules_added.size() == 4);
    CHECK(r.fake.hit_rules_added[0].kinds == EMF_HIT_KINDS_ALL);
    CHECK(r.fake.hit_rules_added[1].kinds == ((1u << EMF_HIT_KIND_FALL) | (1u << EMF_HIT_KIND_DROWNING)));
    CHECK(r.fake.hit_rules_added[2].kinds == (1u << EMF_HIT_KIND_COLD));
    CHECK(r.fake.hit_rules_added[3].kinds == 0);  // hits only, as before
    CHECK(logged("lava false"));
    CHECK(logged("lava"));
}

TEST_CASE("lua 1.23: emf.hit_rule add and min_health that do not fit, and kinds = {}, are errors (no rule that wraps or quietly means hits only)") {
    Rig r(L"hit_rule_numbers");
    add_script(r.dir, "hn",
               "local function try(what, ...) local ok, err = pcall(...) emf.log(what .. ' ' .. tostring(ok) .. ' ' .. tostring(err)) end\n"
               "try('add', emf.hit_rule, {add = 4294967301})\n"
               "try('neg', emf.hit_rule, {add = -2147483649})\n"
               "try('min', emf.hit_rule, {min_health = 4294967297})\n"
               "try('minneg', emf.hit_rule, {min_health = -1})\n"
               "try('kinds', emf.hit_rule, {kinds = {}, cancel = true})\n"
               "try('onhit', emf.on_hit, function() end, {kinds = {}})\n"
               "try('typo', emf.hit_rule, {kind = 'fall'})\n"
               "emf.hit_rule{add = 2147483647}\n"
               "emf.hit_rule{add = -2147483648, min_health = 2147483647}\n");
    Host host(&r.api, r.dir);
    host.load_all();
    CHECK(logged("add false"));
    CHECK(logged("emf.hit_rule: add = 4294967301 does not fit (-2147483648 to 2147483647)"));
    CHECK(logged("emf.hit_rule: add = -2147483649 does not fit (-2147483648 to 2147483647)"));
    CHECK(logged("emf.hit_rule: min_health = 4294967297 does not fit (0 to 2147483647)"));
    CHECK(logged("emf.hit_rule: min_health = -1 does not fit (0 to 2147483647)"));
    CHECK(logged("kinds false"));
    CHECK(logged("emf.hit_rule: kinds = {} names no kind (hit, fall, shroud, cold, drowning or all)"));
    CHECK(logged("onhit false"));
    CHECK(logged("emf.on_hit: kinds = {} names no kind (hit, fall, shroud, cold, drowning or all)"));
    CHECK(logged("scale, add, min_health, cancel, kinds)"));  // the unknown-field list names kinds too
    REQUIRE(r.fake.hit_rules_added.size() == 2);
    CHECK(r.fake.hit_rules_added[0].add == 2147483647);
    CHECK(r.fake.hit_rules_added[1].add == -2147483647 - 1);
    CHECK(r.fake.hit_rules_added[1].min_health == 2147483647);
}

TEST_CASE("lua 1.23: hit.instant says the game's own damage kills at once (deadly shroud, out of air)") {
    Rig r(L"on_hit_instant");
    add_script(r.dir, "hi", "emf.on_hit(function(w, h) emf.log('instant ' .. h.kind .. ' ' .. tostring(h.instant)) end, {kinds = 'all'})\n");
    Host host(&r.api, r.dir);
    host.load_all();
    EmfHit h{};
    h.size = sizeof h;
    h.amount = 400;
    h.health_before = 400;
    h.health_after = 0;
    h.kind = EMF_HIT_KIND_SHROUD;
    h.flags = EMF_HIT_INSTANT;
    fire_hit(kServer, h);
    CHECK(logged("instant shroud true"));
    h.kind = EMF_HIT_KIND_COLD;
    h.flags = 0;
    fire_hit(kServer, h);
    CHECK(logged("instant cold false"));
}

TEST_CASE("lua 1.23: emf.hit_rule types takes one name as attack does; emf.on_hit options refuse unknown keys (polish)") {
    Rig r(L"hit_rule_polish");
    add_script(r.dir, "hp",
               "emf.hit_rule{types = 'fire', cancel = true}\n"
               "local ok = pcall(emf.on_hit, function() end, {kind = 'fall'})\n"
               "emf.log('onhit typo ' .. tostring(ok))\n");
    Host host(&r.api, r.dir);
    host.load_all();
    REQUIRE(r.fake.hit_rules_added.size() == 1);
    CHECK(r.fake.hit_rules_added[0].damage_types_any == (1u << 3));
    CHECK(logged("onhit typo false"));
}
