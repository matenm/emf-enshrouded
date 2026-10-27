// Lua host 1.9: wheel groups. Several mods publish their sections of a group; emf.wheel_group opens one wheel with all
// of them, and a pick runs only the owning mod's function, with the index into that mod's own labels.
#include <windows.h>

#include <doctest.h>

#include <string>
#include <vector>

#include "lua_host/host.h"
#include "lua_test_fake.h"

using emf::lua::Host;

namespace {
bool in_log(const Fake& f, const std::string& s) {
    for (const auto& x : f.log)
        if (x.find(s) != std::string::npos) return true;
    return false;
}
const char* kBeasts =
    "emf.wheel_section('summons', 'Beasts', {'Call Wolf'}, function(w, i) emf.log('beasts picked ' .. i) end)\n"
    "emf.command('clear', '', function() emf.wheel_section('summons', 'Beasts', {}) end)\n"
    "emf.command('open', '', function() emf.log('opened ' .. tostring(emf.wheel_group('summons', 'Summons', 'B'))) end)\n";
const char* kUndead =
    "emf.wheel_section('summons', 'Undead', {'Raise', 'Spirits'}, function(w, i, l) emf.log('undead picked ' .. i .. ' ' .. tostring(l)) end)\n"
    "emf.command('open', '', function() emf.log('opened ' .. tostring(emf.wheel_group('summons', 'Summons', 'V'))) end)\n";
}  // namespace

TEST_CASE("lua 1.9: two mods' sections open as one wheel; a pick runs the owner's function with its own index") {
    Rig r(L"wgroup");
    add_script(r.dir, "beasts", kBeasts);
    add_script(r.dir, "undead", kUndead);
    Host host(&r.api, r.dir);
    host.load_all();
    REQUIRE(host.status("undead") == "running");
    r.run_command("undead.open");
    CHECK(in_log(r.fake, "opened true"));
    REQUIRE(r.fake.wheels.size() == 1);
    const auto w = r.fake.wheels[0];
    CHECK(w.title == "Summons");
    CHECK(w.items == std::vector<std::string>{"Beasts: Call Wolf", "Undead: Raise", "Undead: Spirits"});
    CHECK(w.hold == 'V');
    w.cb(kClient, 2, w.user);  // the third item: Undead's second label
    CHECK(in_log(r.fake, "undead picked 2 Spirits"));  // the index into its own labels, and the label shown
    CHECK(!in_log(r.fake, "beasts picked"));
    // the other mod opens the same group with its own key; its pick reaches it
    r.run_command("beasts.open");
    REQUIRE(r.fake.wheels.size() == 2);
    CHECK(r.fake.wheels[1].hold == 'B');
    r.fake.wheels[1].cb(kClient, 0, r.fake.wheels[1].user);
    CHECK(in_log(r.fake, "beasts picked 1"));
    // a cancelled wheel calls nothing
    r.run_command("undead.open");
    r.fake.log.clear();
    r.fake.wheels[2].cb(kClient, -1, r.fake.wheels[2].user);
    CHECK(!in_log(r.fake, "picked"));
}

TEST_CASE("lua 1.9: one section shows its labels as they are; an empty list removes it; a reloaded mod's section is gone and an old pick is dropped") {
    Rig r(L"wgroup2");
    add_script(r.dir, "beasts", kBeasts);
    add_script(r.dir, "undead", kUndead);
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("beasts.clear");
    r.run_command("undead.open");
    REQUIRE(r.fake.wheels.size() == 1);
    CHECK(r.fake.wheels[0].items == std::vector<std::string>{"Raise", "Spirits"});  // one section: no prefix
    // undead is reloaded with code that publishes nothing: its section is gone, and its old wheel's pick goes nowhere
    Sleep(20);
    write(r.dir + L"\\undead\\main.lua", "emf.log('undead v2')\n");
    host.check_reload_now();
    REQUIRE(host.status("undead") == "running");
    r.fake.log.clear();
    r.fake.wheels[0].cb(kClient, 0, r.fake.wheels[0].user);
    CHECK(!in_log(r.fake, "picked"));
    r.run_command("beasts.open");
    CHECK(in_log(r.fake, "opened false"));  // no section left
    CHECK(r.fake.wheels.size() == 1);
}

TEST_CASE("lua 1.9: a wheel opened before a mod reloads drops its pick, even when the reloaded mod publishes again") {
    Rig r(L"wgroup3");
    add_script(r.dir, "beasts", kBeasts);
    add_script(r.dir, "undead", kUndead);
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("undead.open");
    REQUIRE(r.fake.wheels.size() == 1);
    Sleep(20);
    write(r.dir + L"\\undead\\main.lua",
          "emf.wheel_section('summons', 'Undead', {'Dismiss all', 'Raise'}, function(w, i, l) emf.log('v2 picked ' .. i .. ' ' .. tostring(l)) end)\n"
          "emf.command('open', '', function() emf.log('opened ' .. tostring(emf.wheel_group('summons', 'Summons', 'V'))) end)\n");
    host.check_reload_now();
    REQUIRE(host.status("undead") == "running");
    r.fake.log.clear();
    r.fake.wheels[0].cb(kClient, 1, r.fake.wheels[0].user);  // "Undead: Raise" on the old wheel
    CHECK(!in_log(r.fake, "picked"));                          // not run as v2's "Dismiss all"
    r.run_command("undead.open");
    REQUIRE(r.fake.wheels.size() == 2);
    r.fake.wheels[1].cb(kClient, 1, r.fake.wheels[1].user);  // the new wheel works
    CHECK(in_log(r.fake, "v2 picked 1 Dismiss all"));
}

TEST_CASE("lua 1.9: a pick that raises pauses only its mod; its section leaves the wheel and the other mod's picks still work") {
    Rig r(L"wgroup4");
    add_script(r.dir, "beasts", kBeasts);
    add_script(r.dir, "undead",
               "emf.wheel_section('summons', 'Undead', {'Raise'}, function(w, i) error('broken pick') end)\n"
               "emf.command('open', '', function() emf.log('opened ' .. tostring(emf.wheel_group('summons', 'Summons', 'V'))) end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("beasts.open");
    REQUIRE(r.fake.wheels.size() == 1);
    CHECK(r.fake.wheels[0].items.size() == 2);
    r.fake.wheels[0].cb(kClient, 1, r.fake.wheels[0].user);  // undead's pick raises
    CHECK(host.status("undead").find("paused") == 0);
    CHECK(host.status("beasts") == "running");
    r.run_command("beasts.open");
    REQUIRE(r.fake.wheels.size() == 2);
    CHECK(r.fake.wheels[1].items == std::vector<std::string>{"Call Wolf"});  // the paused mod's section is gone
    r.fake.wheels[1].cb(kClient, 0, r.fake.wheels[1].user);
    CHECK(in_log(r.fake, "beasts picked 1"));
}
