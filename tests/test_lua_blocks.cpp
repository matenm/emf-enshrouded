// Lua 1.37 breaking blocks (API v44): emf.break_blocks, emf.block_at, emf.find_wall over the fake core API.
#include <windows.h>

#include <doctest.h>

#include <cstddef>
#include <cstring>
#include <string>

#include "lua_host/host.h"
#include "lua_test_fake.h"

using emf::lua::Host;

TEST_CASE("lua 1.37: emf.break_blocks: the options reach the request; fn gets the counts") {
    Rig r(L"lua_blocks");
    add_script(r.dir, "bb",
               "emf.command('go', '', function(w)\n"
               "  id = emf.break_blocks(w, 10, 20, 30, {radius = 1.5, height = 0.7, mode = 'break', damage = 10,\n"
               "    drops = true, materials = {7, 4}}, function(w2, res)\n"
               "    emf.log('got ' .. res.status .. ' ' .. res.damaged .. ' ' .. res.removed .. ' ' .. res.dropped)\n"
               "  end)\n"
               "  emf.log('id ' .. tostring(id))\n"
               "  id2 = emf.break_blocks(w, 1, 2, 3, function(w2, res) emf.log('plain ' .. res.status) end)\n"
               "  emf.log('id2 ' .. tostring(id2))\n"
               "  id3 = emf.break_blocks(w, 1, 2, 3, {damage = {1, 2, 3, 4, 5}}, function() end)\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("bb.go");
    CHECK(logged("id 1"));
    CHECK(logged("id2 2"));
    REQUIRE(r.fake.breaks.size() == 3u);
    const EmfBreakBlocks& q = r.fake.breaks[0].r;
    CHECK(q.size == sizeof(EmfBreakBlocks));
    CHECK(q.x == 10.0f);
    CHECK(q.y == 20.0f);
    CHECK(q.z == 30.0f);
    CHECK(q.radius == 1.5f);
    CHECK(q.height == doctest::Approx(0.7f));
    CHECK(q.flags == (EMF_BREAK_INSTANT | EMF_BREAK_DROPS));
    for (int i = 0; i < 5; ++i) CHECK(q.damage[i] == 10);
    CHECK(r.fake.breaks[0].materials == std::vector<uint8_t>{7, 4});
    const EmfBreakBlocks& plain = r.fake.breaks[1].r;
    CHECK(plain.flags == 0u);
    CHECK(plain.radius == 0.0f);
    for (int i = 0; i < 5; ++i) CHECK(plain.damage[i] == 0);
    CHECK(plain.material_count == 0u);
    const EmfBreakBlocks& listed = r.fake.breaks[2].r;
    CHECK(listed.damage[0] == 1);
    CHECK(listed.damage[4] == 5);
    EmfBreakResult res{};
    res.size = sizeof res;
    res.status = EMF_BREAK_DONE;
    res.damaged = 12;
    res.removed = 9;
    res.dropped = 2;
    r.fake.breaks[0].cb(kServer, 1, &res, r.fake.breaks[0].user);
    CHECK(logged("got done 12 9 2"));
    res.status = EMF_BREAK_CANCELLED;
    r.fake.breaks[1].cb(kServer, 2, &res, r.fake.breaks[1].user);
    CHECK(logged("plain cancelled"));
}

TEST_CASE("lua 1.37: emf.break_blocks: an unknown option or a wrong type is an error; a refusal gives nil") {
    Rig r(L"lua_blocks_bad");
    add_script(r.dir, "bx",
               "emf.command('go', '', function(w)\n"
               "  local function try(o) local ok, e = pcall(emf.break_blocks, w, 1, 2, 3, o, function() end)\n"
               "    emf.log(tostring(ok) .. ' ' .. tostring(e)) end\n"
               "  try({radiuss = 1})\n"
               "  try({mode = 'smash'})\n"
               "  try({drops = 1})\n"
               "  try({damage = {1, 2, 3}})\n"
               "  try({damage = 2.5})\n"
               "  try({materials = {200}})\n"
               "  try({radius = 'big'})\n"
               "end)\n"
               "emf.command('no', '', function(w)\n"
               "  emf.log('refused ' .. tostring(emf.break_blocks(w, 1, 2, 3, function() emf.log('heard') end)))\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("bx.go");
    CHECK(logged("unknown option 'radiuss'"));
    CHECK(logged("mode must be 'damage' or 'break'"));
    CHECK(logged("drops must be true or false"));
    CHECK(logged("damage must be a number or a list of five"));
    CHECK(logged("damage must be whole numbers from 0 to 100000"));
    CHECK(logged("materials must be building material ids 0 to 127"));
    CHECK(logged("radius must be a number"));
    CHECK(r.fake.breaks.empty());
    r.fake.break_ok = false;
    r.run_command("bx.no");
    CHECK(logged("refused nil"));
}

TEST_CASE("lua 1.37: emf.break_blocks: a reloaded script hears nothing") {
    Rig r(L"lua_blocks_reload");
    add_script(r.dir, "br",
               "emf.command('go', '', function(w)\n"
               "  emf.break_blocks(w, 1, 2, 3, function() emf.log('heard') end)\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("br.go");
    REQUIRE(r.fake.breaks.size() == 1u);
    Sleep(20);
    add_script(r.dir, "br", "emf.log('reloaded')\n");
    host.check_reload_now();
    EmfBreakResult res{};
    res.size = sizeof res;
    r.fake.breaks[0].cb(kServer, 1, &res, r.fake.breaks[0].user);
    CHECK_FALSE(logged("heard"));
}

TEST_CASE("lua 1.37: emf.block_at and emf.find_wall give the block as a table, or nil") {
    Rig r(L"lua_block_at");
    add_script(r.dir, "ba",
               "emf.command('go', '', function(w)\n"
               "  local b = emf.block_at(w, 1, 2, 3)\n"
               "  emf.log('at ' .. b.kind .. ' ' .. b.material .. ' ' .. b.fill .. ' ' .. b.hardness .. ' ' .. b.health ..\n"
               "    ' ' .. tostring(b.damage) .. ' ' .. b.x)\n"
               "  local f = emf.find_wall(w, {1, 2, 3}, {x = 4, y = 5, z = 6})\n"
               "  emf.log('wall ' .. tostring(f and f.material))\n"
               "end)\n"
               "emf.command('none', '', function(w)\n"
               "  emf.log('none ' .. tostring(emf.block_at(w, 1, 2, 3)) .. ' ' .. tostring(emf.find_wall(w, {1, 2, 3}, {4, 5, 6})))\n"
               "end)\n"
               "emf.command('terr', '', function(w)\n"
               "  local b = emf.block_at(w, 1, 2, 3)\n"
               "  emf.log('terr ' .. b.kind .. ' ' .. tostring(b.damage))\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.fake.block.kind = EMF_BLOCK_BUILDING;
    r.fake.block.material = 7;
    r.fake.block.fill = 255;
    r.fake.block.hardness = 4;
    r.fake.block.health = 1000;
    r.fake.block.damage = 34;
    r.fake.block.x = 10.25f;
    r.run_command("ba.go");
    CHECK(logged("at building 7 255 4 1000 34 10.25"));
    CHECK(logged("wall 7"));
    CHECK(r.fake.wall_from[0] == 1.0f);
    CHECK(r.fake.wall_to[1] == 5.0f);
    r.fake.block.kind = EMF_BLOCK_TERRAIN;
    r.fake.block.damage = -1;
    r.run_command("ba.terr");
    CHECK(logged("terr terrain nil"));
    r.fake.block_ok = false;
    r.run_command("ba.none");
    CHECK(logged("none nil nil"));
}

TEST_CASE("lua 1.37: nil on a core older than v44") {
    Rig r(L"lua_blocks_old");
    add_script(r.dir, "bo",
               "emf.command('go', '', function(w)\n"
               "  emf.log('old ' .. tostring(emf.break_blocks(w, 1, 2, 3, function() end)) .. ' ' ..\n"
               "    tostring(emf.block_at(w, 1, 2, 3)) .. ' ' .. tostring(emf.find_wall(w, {1, 2, 3}, {4, 5, 6})))\n"
               "end)\n");
    r.api.size = static_cast<uint32_t>(offsetof(EmfApi, break_blocks));  // a v43 core's table
    r.api.version = 43;
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("bo.go");
    CHECK(logged("old nil nil nil"));
    CHECK(r.fake.breaks.empty());
}

TEST_CASE("lua 1.38: a block's world (true: the world's own, false: a player's, nil: not known) and break_blocks' world option; a v44 core gives no world") {
    Rig r(L"lua_block_world");
    add_script(r.dir, "bw",
               "emf.command('go', '', function(w)\n"
               "  local b = emf.block_at(w, 1, 2, 3)\n"
               "  emf.log('world ' .. tostring(b.world))\n"
               "end)\n"
               "emf.command('brk', '', function(w)\n"
               "  emf.break_blocks(w, 1, 2, 3, {world = true}, function() end)\n"
               "  emf.break_blocks(w, 1, 2, 3, {world = false}, function() end)\n"
               "end)\n"
               "emf.command('old', '', function(w)\n"
               "  emf.log('old ' .. tostring(emf.block_at(w, 1, 2, 3).world))\n"
               "end)\n"
               "emf.command('bad', '', function(w)\n"
               "  local ok, err = pcall(emf.break_blocks, w, 1, 2, 3, {world = 1}, function() end)\n"
               "  emf.log('bad ' .. tostring(ok) .. ' ' .. tostring(err))\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.fake.block.kind = EMF_BLOCK_BUILDING;
    r.fake.block.material = 10;
    r.fake.block.world = 1;
    r.run_command("bw.go");
    CHECK(logged("world true"));
    r.fake.block.world = 0;
    r.run_command("bw.go");
    CHECK(logged("world false"));
    r.fake.block.world = -1;
    r.run_command("bw.go");
    CHECK(logged("world nil"));
    r.run_command("bw.brk");
    REQUIRE(r.fake.breaks.size() == 2u);
    CHECK((r.fake.breaks[0].r.flags & EMF_BREAK_WORLD) != 0);
    CHECK((r.fake.breaks[1].r.flags & EMF_BREAK_WORLD) == 0);
    r.run_command("bw.bad");
    CHECK(logged("bad false"));
    CHECK(logged("world must be true or false"));
    // a v44 core never wrote world: nil
    r.api.version = 44;
    r.fake.block.world = 1;
    r.run_command("bw.old");
    CHECK(logged("old nil"));
    // and world = true is no unknown flag for it: ruins break like any block there
    r.fake.breaks.clear();
    r.run_command("bw.brk");
    REQUIRE(r.fake.breaks.size() == 2u);
    CHECK((r.fake.breaks[0].r.flags & EMF_BREAK_WORLD) == 0);
}

TEST_CASE("lua: break_blocks refuses an empty or non-list materials table and numbers given as text") {
    Rig r(L"lua_block_strict");
    add_script(r.dir, "bs",
               "emf.command('go', '', function(w)\n"
               "  local function try(tag, o) local ok, e = pcall(emf.break_blocks, w, 1, 2, 3, o, function() end)\n"
               "    emf.log(tag .. ' ' .. tostring(ok) .. ' ' .. tostring(e)) end\n"
               "  try('m0', {materials = {}})\n"
               "  try('m1', {materials = {a = 1}})\n"
               "  try('m2', {materials = {'7'}})\n"
               "  try('r0', {radius = '2'})\n"
               "  try('d0', {damage = '5'})\n"
               "  try('d1', {damage = {1, 2, '3', 4, 5}})\n"
               "  try('ok', {materials = {7}, radius = 2})\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("bs.go");
    for (const char* tag : {"m0 false", "m1 false", "m2 false", "r0 false", "d0 false", "d1 false", "ok true"})
        CHECK_MESSAGE(logged(tag), tag);
    CHECK(logged("materials must list at least one building material id"));
    REQUIRE(r.fake.breaks.size() == 1u);
    CHECK(r.fake.breaks[0].materials == std::vector<uint8_t>{7});
}
