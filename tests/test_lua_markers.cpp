// Lua 1.25 markers: emf.marker, emf.remove_marker, emf.clear_markers, emf.marker_types, emf.world_to_screen,
// emf.send_marker, emf.unsend_marker over the fake core API; a script's markers leave with it.
#include <windows.h>

#include <doctest.h>

#include <string>

#include "lua_host/host.h"
#include "lua_test_fake.h"

using emf::lua::Host;

TEST_CASE("lua 1.25: emf.marker builds the C marker under the script's own id; defaults world and distance") {
    Rig r(L"marker_set");
    add_script(r.dir, "mk",
               "emf.log('set ' .. tostring(emf.marker('home', {x = 1, y = 2, z = 3, text = 'Home', colour = 0xffcc33ff,\n"
               "  shape = 'square', game_map = true, edge_arrow = true, height = 2.5, max_distance = 200, game_icon = 0x11,\n"
               "  icon = 7})))\n"
               "emf.marker('beast', {entity = 42, world = false, distance = false})\n");
    Host host(&r.api, r.dir);
    host.load_all();
    CHECK(logged("set true"));
    REQUIRE(r.fake.markers.count("mk:home") == 1);
    const auto& h = r.fake.markers["mk:home"];
    CHECK(h.text == "Home");
    CHECK(h.m.size == sizeof(EmfMarker));
    CHECK(h.m.x == 1);
    CHECK(h.m.y == 2);
    CHECK(h.m.z == 3);
    CHECK(h.m.rgba == 0xffcc33ffu);
    CHECK(h.m.shape == EMF_MARKER_SQUARE);
    CHECK(h.m.flags == (EMF_MARKER_WORLD | EMF_MARKER_DISTANCE | EMF_MARKER_GAME_MAP | EMF_MARKER_EDGE_ARROW));
    CHECK(h.m.height == doctest::Approx(2.5f));
    CHECK(h.m.max_distance == doctest::Approx(200.0f));
    CHECK(h.m.game_icon == 0x11u);
    CHECK(h.m.icon == 7u);
    REQUIRE(r.fake.markers.count("mk:beast") == 1);
    CHECK(r.fake.markers["mk:beast"].m.entity == 42u);
    CHECK(r.fake.markers["mk:beast"].m.flags == 0u);
}

TEST_CASE("lua 1.25: emf.marker raises on an unknown key, a bad shape or a bad id") {
    Rig r(L"marker_bad");
    add_script(r.dir, "mb",
               "local ok1 = pcall(emf.marker, 'a', {x = 1, colour_ = 3})\n"
               "local ok2 = pcall(emf.marker, 'a', {x = 1, shape = 'star'})\n"
               "local ok3 = pcall(emf.marker, 'has space', {x = 1})\n"
               "local ok4 = pcall(emf.marker, string.rep('a', 60), {x = 1})\n"
               "emf.log('bad ' .. tostring(ok1) .. tostring(ok2) .. tostring(ok3) .. tostring(ok4))\n");
    Host host(&r.api, r.dir);
    host.load_all();
    CHECK(logged("bad falsefalsefalsefalse"));
    CHECK(r.fake.markers.empty());
}

TEST_CASE("lua 1.25: emf.marker's entity, icon and game_icon must be whole numbers that fit; a colour beyond any number is an error") {
    Rig r(L"marker_numbers");
    add_script(r.dir, "mn",
               "local function try(what, spec) local ok, err = pcall(emf.marker, 'a', spec)\n"
               "  emf.log(what .. ' ' .. tostring(ok) .. ' ' .. tostring(err)) end\n"
               "try('neg', {entity = -1})\n"
               "try('big', {entity = 4294967296})\n"
               "try('frac', {entity = 1.5})\n"
               "try('nan', {icon = 0/0})\n"
               "try('inf', {game_icon = math.huge})\n"
               "try('colour', {colour = 1e300})\n"
               "emf.marker('ok', {entity = 4294967295, icon = 3.0, game_icon = 0, colour = -1})\n");
    Host host(&r.api, r.dir);
    host.load_all();
    CHECK(logged("neg false emf.marker: entity must be a whole number from 0 to 4294967295"));
    CHECK(logged("big false emf.marker: entity must be a whole number from 0 to 4294967295"));
    CHECK(logged("frac false emf.marker: entity must be a whole number from 0 to 4294967295"));
    CHECK(logged("nan false emf.marker: icon must be a whole number from 0 to 4294967295"));
    CHECK(logged("inf false emf.marker: game_icon must be a whole number from 0 to 4294967295"));
    CHECK(logged("colour false emf.marker: colour must be 0xRRGGBBAA"));
    REQUIRE(r.fake.markers.count("mn:ok") == 1);
    REQUIRE(r.fake.markers.size() == 1);
    CHECK(r.fake.markers["mn:ok"].m.entity == 4294967295u);
    CHECK(r.fake.markers["mn:ok"].m.icon == 3u);
    CHECK(r.fake.markers["mn:ok"].m.rgba == 0xFFFFFFFFu);  // as before: -1 is every bit
}

TEST_CASE("lua 1.25: emf.remove_marker and emf.clear_markers touch only the script's own markers") {
    Rig r(L"marker_remove");
    add_script(r.dir, "ma", "emf.marker('x', {x = 1})\nemf.marker('y', {x = 2})\n");
    add_script(r.dir, "mz",
               "emf.marker('x', {x = 3})\n"
               "emf.command('rm', '', function() emf.log('rm ' .. tostring(emf.remove_marker('x')) .. ' ' ..\n"
               "  tostring(emf.remove_marker('x'))) end)\n"
               "emf.command('cl', '', function() emf.log('cl ' .. emf.clear_markers()) end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    REQUIRE(r.fake.markers.size() == 3);
    r.run_command("mz.rm");
    CHECK(logged("rm true false"));
    CHECK(r.fake.markers.count("ma:x") == 1);
    CHECK(r.fake.markers.count("mz:x") == 0);
    r.run_command("mz.cl");
    CHECK(logged("cl 0"));
    CHECK(r.fake.markers.size() == 2);
}

TEST_CASE("lua 1.25: emf.send_marker and emf.unsend_marker to one player or all; world_to_screen; marker_types") {
    Rig r(L"marker_send");
    r.fake.map_types = {0x11u, 0x22u};
    add_script(r.dir, "ms",
               "emf.command('go', '', function()\n"
               "  emf.log('one ' .. emf.send_marker(5, 'grave', {x = 1, z = 2, text = 'Grave'}))\n"
               "  emf.log('all ' .. emf.send_marker(nil, 'raid', {x = 9}))\n"
               "  emf.log('un ' .. emf.unsend_marker(5, 'grave'))\n"
               "  local x, y = emf.world_to_screen(10, 0, 20)\n"
               "  emf.log('screen ' .. x .. ' ' .. y)\n"
               "  local t = emf.marker_types()\n"
               "  emf.log('types ' .. #t .. ' ' .. t[2])\n"
               "end)\n"
               "emf.command('off', '', function() emf.log('off ' .. tostring(emf.world_to_screen(1, 2, 3))) end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("ms.go");
    CHECK(logged("one 1"));
    CHECK(logged("all 2"));
    CHECK(logged("un 1"));
    CHECK(logged("screen 20.0 60.0"));  // pixel positions are floats
    CHECK(logged("types 2 34"));
    REQUIRE(r.fake.markers_sent.size() == 2);
    CHECK(r.fake.markers_sent[0].first == 5u);
    CHECK(r.fake.markers_sent[0].second.id == "ms:grave");
    CHECK(r.fake.markers_sent[0].second.text == "Grave");
    CHECK(r.fake.markers_sent[1].first == 0u);
    REQUIRE(r.fake.markers_unsent.size() == 1);
    CHECK(r.fake.markers_unsent[0].second == "ms:grave");
    r.fake.screen_ok = false;
    r.run_command("ms.off");
    CHECK(logged("off nil"));
}

TEST_CASE("lua 1.25: a reload takes the script's markers away, here and in the players' games it sent them to") {
    Rig r(L"marker_reload");
    add_script(r.dir, "mr", "emf.marker('a', {x = 1})\nemf.send_marker(7, 'b', {x = 2})\n");
    Host host(&r.api, r.dir);
    host.load_all();
    REQUIRE(r.fake.markers.count("mr:a") == 1);
    Sleep(20);
    write(r.dir + L"\\mr\\main.lua", "emf.log('reloaded without markers')\n");
    host.check_reload_now();
    CHECK(logged("reloaded without markers"));
    CHECK(r.fake.markers.count("mr:a") == 0);
    REQUIRE(r.fake.markers_unsent.size() == 1);
    CHECK(r.fake.markers_unsent[0].first == 7u);
    CHECK(r.fake.markers_unsent[0].second == "mr:b");
}

TEST_CASE("lua 1.40: a marker's bar = 0..1 sets EMF_MARKER_BAR and its share; outside 0..1 or not a number is an error") {
    Rig r(L"marker_bar");
    add_script(r.dir, "mbar",
               "emf.marker('hp', {x = 1, y = 2, z = 3, text = 'Wolf', bar = 0.75})\n"
               "local ok1 = pcall(emf.marker, 'b1', {x = 1, bar = 1.5})\n"
               "local ok2 = pcall(emf.marker, 'b2', {x = 1, bar = 'full'})\n"
               "local ok3 = pcall(emf.marker, 'b3', {x = 1, bar = -1})\n"
               "emf.log('bar ' .. tostring(ok1) .. tostring(ok2) .. tostring(ok3))\n");
    Host host(&r.api, r.dir);
    host.load_all();
    REQUIRE(r.fake.markers.count("mbar:hp") == 1);
    const auto& h = r.fake.markers["mbar:hp"];
    CHECK((h.m.flags & EMF_MARKER_BAR) != 0u);
    CHECK(h.m.bar == doctest::Approx(0.75f));
    CHECK(logged("bar falsefalsefalse"));
    CHECK(r.fake.markers.count("mbar:b1") == 0);
}

TEST_CASE("lua 1.40 (review): a marker's bar on a core older than v47 is an error (so a pcall can send it without)") {
    Rig r(L"marker_bar_old");
    r.api.size = static_cast<uint32_t>(offsetof(EmfApi, find_wall_ex));
    r.api.version = 46;
    add_script(r.dir, "mold",
               "local ok, err = pcall(emf.marker, 'hp', {x = 1, bar = 0.5})\n"
               "emf.log('old ' .. tostring(ok) .. ' ' .. tostring(err))\n"
               "emf.marker('plain', {x = 1})\n");
    Host host(&r.api, r.dir);
    host.load_all();
    CHECK(logged("old false"));
    CHECK(logged("bar needs EMF 0.6.19"));
    CHECK(r.fake.markers.count("mold:hp") == 0);
    CHECK(r.fake.markers.count("mold:plain") == 1);
}
