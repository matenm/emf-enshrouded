// Lua 1.29 emf.path_find / emf.path_cancel (API v37) over the fake core API: the options become the query, the answer
// reaches the script's function as a result table and a list of points; a reloaded script hears nothing.
#include <windows.h>

#include <doctest.h>

#include <cstddef>
#include <cstring>
#include <string>

#include "lua_host/host.h"
#include "lua_test_fake.h"

using emf::lua::Host;

TEST_CASE("lua 1.29: emf.path_find builds the query from its options; the answer comes as tables") {
    Rig r(L"lua_path");
    add_script(r.dir, "pf",
               "emf.command('go', '', function(w)\n"
               "  id = emf.path_find(w, {1, 50, 2}, {x = 30, y = 51, z = 4}, {jumps = true, doors = true, closest = true,\n"
               "    width = 1.2, height = 2, max_jump_up = 3, nodes = 9000, owner = 7}, function(w2, res, pts)\n"
               "    emf.log('got ' .. res.status .. ' ' .. tostring(res.reaches) .. ' ' .. #pts .. ' ' ..\n"
               "      string.format('%.1f', res.length) .. ' ' .. pts[2].move .. ' ' .. pts[2].x)\n"
               "  end)\n"
               "  emf.log('id ' .. tostring(id))\n"
               "  local bad = pcall(emf.path_find, w, {1, 2}, {3, 4, 5}, function() end)\n"
               "  emf.log('bad ' .. tostring(bad))\n"
               "end)\n"
               "emf.command('stop', '', function(w) emf.log('cancel ' .. tostring(emf.path_cancel(id))) end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("pf.go");
    CHECK(logged("id 1"));
    CHECK(logged("bad false"));  // a position needs three numbers
    REQUIRE(r.fake.paths.size() == 1u);
    const EmfPathQuery& q = r.fake.paths[0].q;
    CHECK(q.size == sizeof(EmfPathQuery));
    CHECK(q.from[0] == 1.0f);
    CHECK(q.from[2] == 2.0f);
    CHECK(q.to[0] == 30.0f);
    CHECK(q.to[1] == 51.0f);
    CHECK(q.moves == (EMF_PATH_JUMP_UP | EMF_PATH_JUMP_DOWN | EMF_PATH_JUMP_ACROSS | EMF_PATH_OPEN_DOOR));
    CHECK(q.closest == 1);
    CHECK(q.width == doctest::Approx(1.2f));
    CHECK(q.height == 2.0f);
    CHECK(q.max_jump_up == 3);
    CHECK(q.max_nodes == 9000u);
    CHECK(q.owner == 7u);
    EmfPathResult res{};
    res.size = sizeof res;
    res.status = EMF_PATH_FOUND;
    res.reaches = 1;
    res.count = 2;
    res.length = 29.04f;
    const EmfPathPoint pts[2] = {{10, 50, 2, 0}, {30, 51, 4, 2}};
    r.fake.paths[0].cb(kServer, 1, &res, pts, r.fake.paths[0].user);
    CHECK(logged("got found true 2 29.0 2 30.0"));
    r.run_command("pf.stop");
    CHECK(logged("cancel true"));
}

TEST_CASE("lua 1.29: a refused path_find gives nil; an answer after a reload is not heard") {
    Rig r(L"lua_path_no");
    add_script(r.dir, "pn",
               "emf.command('go', '', function(w)\n"
               "  emf.log('id ' .. tostring(emf.path_find(w, {1, 50, 2}, {30, 51, 4}, function() emf.log('heard') end)))\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.fake.path_ok = false;
    r.run_command("pn.go");
    CHECK(logged("id nil"));
    r.fake.path_ok = true;
    r.run_command("pn.go");
    CHECK(logged("id 1"));
    REQUIRE(r.fake.paths.size() == 1u);
    Sleep(20);
    add_script(r.dir, "pn", "emf.log('reloaded')\n");  // a new main.lua: the script loads again
    host.check_reload_now();
    EmfPathResult res{};
    res.size = sizeof res;
    res.status = EMF_PATH_NO_PATH;
    r.fake.paths[0].cb(kServer, 1, &res, nullptr, r.fake.paths[0].user);
    CHECK_FALSE(logged("heard"));
}

TEST_CASE("lua 1.35: a door point carries its door entity (points[i].door); none on a core without it") {
    Rig r(L"lua_path_door");
    add_script(r.dir, "pd",
               "emf.command('go', '', function(w)\n"
               "  emf.path_find(w, {1, 50, 2}, {30, 51, 4}, {doors = true}, function(w2, res, pts)\n"
               "    emf.log('door ' .. tostring(pts[1].door) .. ' ' .. tostring(pts[2].door))\n"
               "  end)\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("pd.go");
    r.run_command("pd.go");
    REQUIRE(r.fake.paths.size() == 2u);
    EmfPathResult res{};
    res.size = sizeof res;
    res.status = EMF_PATH_FOUND;
    res.reaches = 1;
    res.count = 2;
    const EmfPathPoint pts[2] = {{10, 50, 2, 0}, {30, 51, 4, 5}};
    const uint32_t doors[2] = {0, 812};
    res.doors = doors;
    r.fake.paths[0].cb(kServer, 1, &res, pts, r.fake.paths[0].user);
    CHECK(logged("door nil 812"));
    EmfPathResult old{};  // a v37-v41 core: the result ends before doors
    std::memcpy(&old, &res, sizeof old);
    old.size = offsetof(EmfPathResult, doors);
    old.doors = doors;  // past its size: must not be read
    r.fake.paths[1].cb(kServer, 2, &old, pts, r.fake.paths[1].user);
    CHECK(logged("door nil nil"));
}

TEST_CASE("lua 1.41: as = entity plans as that creature; result.used says what the search planned with") {
    Rig r(L"lua_path_as");
    add_script(r.dir, "pa",
               "emf.command('go', '', function(w)\n"
               "  emf.path_find(w, {1, 50, 2}, {30, 51, 4}, {as = 812, jumps = true}, function(w2, res, pts)\n"
               "    local u = res.used\n"
               "    emf.log('used ' .. u.moves .. ' ' .. tostring(u.jumps) .. ' ' .. tostring(u.doors) .. ' ' ..\n"
               "      tostring(u.swim) .. ' ' .. u.max_jump_up .. ' ' .. u.max_jump_down .. ' ' ..\n"
               "      string.format('%.1f %.1f', u.width, u.height))\n"
               "  end)\n"
               "  local ok = pcall(emf.path_find, w, {1, 50, 2}, {30, 51, 4}, {as = -1}, function() end)\n"
               "  emf.log('negative ' .. tostring(ok))\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("pa.go");
    CHECK(logged("negative false"));
    REQUIRE(r.fake.paths.size() == 1u);
    CHECK(r.fake.paths[0].q.as_entity == 812u);
    EmfPathResult res{};
    res.size = sizeof res;
    res.status = EMF_PATH_FOUND;
    res.count = 1;
    res.moves = EMF_PATH_WALK | EMF_PATH_OPEN_DOOR;
    res.jump_up = 2;
    res.jump_down = 3;
    res.width = 0.5f;
    res.height = 2.0f;
    const EmfPathPoint pts[1] = {{30, 51, 4, 0}};
    r.fake.paths[0].cb(kServer, 1, &res, pts, r.fake.paths[0].user);
    CHECK(logged("used 33 false true false 2 3 0.5 2.0"));
}

TEST_CASE("lua 1.41: as on a core older than v48 is an error (it would plan as a player); a v47 result has no used") {
    Rig r(L"lua_path_as_old");
    r.api.version = 47;
    add_script(r.dir, "po",
               "emf.command('go', '', function(w)\n"
               "  local ok, err = pcall(emf.path_find, w, {1, 50, 2}, {30, 51, 4}, {as = 812}, function() end)\n"
               "  emf.log('old ' .. tostring(ok) .. ' ' .. tostring(err))\n"
               "  emf.path_find(w, {1, 50, 2}, {30, 51, 4}, function(w2, res) emf.log('used ' .. tostring(res.used)) end)\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("po.go");
    CHECK(logged("old false"));
    CHECK(logged("as needs EMF 0.6.20 or newer (core API v48)"));
    REQUIRE(r.fake.paths.size() == 1u);
    EmfPathResult res{};
    res.size = offsetof(EmfPathResult, moves);  // a v42-v47 result
    res.status = EMF_PATH_NO_PATH;
    res.moves = 1;  // past its size: must not be read
    r.fake.paths[0].cb(kServer, 1, &res, nullptr, r.fake.paths[0].user);
    CHECK(logged("used nil"));
}


TEST_CASE("lua 1.43: state = 'guard' with as plans as the enemy walks in Guard; 'calm' as 1.41; misuse is an error") {
    Rig r(L"lua_path_state");
    add_script(r.dir, "ps",
               "emf.command('go', '', function(w)\n"
               "  emf.path_find(w, {1, 50, 2}, {30, 51, 4}, {as = 812, state = 'guard'}, function() end)\n"
               "  emf.path_find(w, {1, 50, 2}, {30, 51, 4}, {as = 812, state = 'calm'}, function() end)\n"
               "  local ok1, e1 = pcall(emf.path_find, w, {1, 50, 2}, {30, 51, 4}, {as = 812, state = 'combat'}, function() end)\n"
               "  local ok2, e2 = pcall(emf.path_find, w, {1, 50, 2}, {30, 51, 4}, {state = 'guard'}, function() end)\n"
               "  emf.log('bad ' .. tostring(ok1) .. ' ' .. tostring(e1))\n"
               "  emf.log('noas ' .. tostring(ok2) .. ' ' .. tostring(e2))\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("ps.go");
    REQUIRE(r.fake.paths.size() == 2u);
    CHECK(r.fake.paths[0].q.as_state == EMF_PATH_AS_GUARD);
    CHECK(r.fake.paths[1].q.as_state == EMF_PATH_AS_CALM);
    CHECK(logged("bad false"));
    CHECK(logged("state is \"calm\" or \"guard\""));
    CHECK(logged("noas false"));
    CHECK(logged("state needs as"));
}

TEST_CASE("lua 1.43: state on a core older than v49 is an error (it would plan calm)") {
    Rig r(L"lua_path_state_old");
    r.api.version = 48;
    add_script(r.dir, "pso",
               "emf.command('go', '', function(w)\n"
               "  local ok, err = pcall(emf.path_find, w, {1, 50, 2}, {30, 51, 4}, {as = 812, state = 'guard'}, function() end)\n"
               "  emf.log('old ' .. tostring(ok) .. ' ' .. tostring(err))\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("pso.go");
    CHECK(logged("old false"));
    CHECK(logged("state needs EMF 0.6.22 or newer (core API v49)"));
    CHECK(r.fake.paths.empty());
}
