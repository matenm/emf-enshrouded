// Lua 1.30 doors (API v38) over the fake core API: emf.door, emf.door_open, emf.door_close, emf.door_lock.
#include <windows.h>

#include <doctest.h>

#include <string>

#include "lua_host/host.h"
#include "lua_test_fake.h"

using emf::lua::Host;

TEST_CASE("lua 1.32: emf.enemy_order sends an enemy (seconds, damage, allies), emf.enemy_order_cancel, emf.enemy_target") {
    Rig r(L"lua_orders");
    r.fake.enemy_targets[70] = 900;
    add_script(r.dir, "eo",
               "emf.command('go', '', function(w)\n"
               "  emf.log('id ' .. tostring(emf.enemy_order(w, 70, 900)))\n"
               "  emf.log('id2 ' .. tostring(emf.enemy_order(w, 71, 900, {seconds = 20, damage = 40, allies = true})))\n"
               "  local id, why = emf.enemy_order(w, 13, 900)\n"
               "  emf.log('no ' .. tostring(id) .. ' ' .. tostring(why))\n"
               "  emf.log('bad ' .. tostring(pcall(emf.enemy_order, w, 70, 900, {seconds = 'soon'})))\n"
               "  emf.log('cancel ' .. tostring(emf.enemy_order_cancel(2)) .. ' ' .. tostring(emf.enemy_order_cancel(9)))\n"
               "  emf.log('target ' .. tostring(emf.enemy_target(w, 70)) .. ' ' .. tostring(emf.enemy_target(w, 71)))\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("eo.go");
    CHECK(logged("id 1"));
    CHECK(logged("id2 2"));
    CHECK(logged("no nil it has no enemy AI to send"));
    CHECK(logged("bad false"));
    CHECK(logged("cancel true false"));
    CHECK(logged("target 900 nil"));
    REQUIRE(r.fake.orders.size() == 2u);
    CHECK(r.fake.orders[0].size == sizeof(EmfEnemyOrder));
    CHECK(r.fake.orders[0].enemy == 70u);
    CHECK(r.fake.orders[0].target == 900u);
    CHECK(r.fake.orders[0].seconds == 0.0f);
    CHECK(r.fake.orders[0].damage == 0);
    CHECK(r.fake.orders[0].flags == 0u);
    CHECK(r.fake.orders[1].seconds == 20.0f);
    CHECK(r.fake.orders[1].damage == 40);
    CHECK(r.fake.orders[1].flags == EMF_ORDER_ALLIES);
}

// Review I4: a script's orders end when it reloads or pauses (as its hit rules do).
TEST_CASE("lua 1.32: a reload or a pause cancels the script's enemy orders") {
    Rig r(L"lua_orders_drop");
    add_script(r.dir, "od",
               "emf.command('go', '', function(w) emf.enemy_order(w, 70, 900) emf.enemy_order(w, 71, 900) end)\n"
               "emf.command('boom', '', function() error('boom') end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("od.go");
    REQUIRE(r.fake.orders.size() == 2u);
    r.run_command("od.boom");
    REQUIRE(host.status("od").rfind("paused", 0) == 0);
    CHECK(r.fake.order_cancels.size() == 2u);
}

TEST_CASE("lua 1.30: emf.door reads the state; door_open, door_close and door_lock call the core") {
    Rig r(L"lua_doors");
    r.fake.door_states[500] = EMF_DOOR_CLOSED | EMF_DOOR_LOCKED;
    r.fake.door_states[501] = EMF_DOOR_OPEN | EMF_DOOR_LOGIC;
    add_script(r.dir, "dr",
               "emf.command('go', '', function(w)\n"
               "  local s, f = emf.door(w, 500)\n"
               "  emf.log('d500 ' .. tostring(s) .. ' ' .. tostring(f.locked) .. ' ' .. tostring(f.logic))\n"
               "  s, f = emf.door(w, 501)\n"
               "  emf.log('d501 ' .. tostring(s) .. ' ' .. tostring(f.logic))\n"
               "  emf.log('none ' .. tostring(emf.door(w, 9)))\n"
               "  emf.log('open ' .. tostring(emf.door_open(w, 500, 'front')))\n"
               "  emf.log('close ' .. tostring(emf.door_close(w, 500)))\n"
               "  emf.log('lock ' .. tostring(emf.door_lock(w, 500, false)))\n"
               "  emf.log('no ' .. tostring(emf.door_open(w, 9)))\n"
               "  emf.log('bad ' .. tostring(pcall(emf.door_open, w, 500, 'sideways')))\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("dr.go");
    CHECK(logged("d500 closed true false"));
    CHECK(logged("d501 open true"));
    CHECK(logged("none nil"));
    CHECK(logged("open true"));
    CHECK(logged("close true"));
    CHECK(logged("lock true"));
    CHECK(logged("no false"));
    CHECK(logged("bad false"));
    REQUIRE(r.fake.door_calls.size() == 4u);
    CHECK(r.fake.door_calls[0] == "set 500 1 1");
    CHECK(r.fake.door_calls[1] == "set 500 0 0");
    CHECK(r.fake.door_calls[2] == "lock 500 0");
    CHECK(r.fake.door_calls[3] == "set 9 1 0");
}

TEST_CASE("lua 1.33: emf.enemy_move walks an enemy through points (one point or a list; arrive, seconds, peaceful, allies)") {
    Rig r(L"lua_moves");
    r.fake.moving[70] = 1;
    add_script(r.dir, "mv",
               "emf.command('go', '', function(w)\n"
               "  emf.log('id ' .. tostring(emf.enemy_move(w, 70, {10, 20, 30}, function(world, id, status)\n"
               "    emf.log('done ' .. id .. ' ' .. status .. ' ' .. tostring(world ~= nil))\n"
               "  end)))\n"
               "  emf.log('id2 ' .. tostring(emf.enemy_move(w, 71, {{x = 1, y = 2, z = 3}, {4, 5, 6}},\n"
               "    {arrive = 3, seconds = 40, peaceful = true, allies = true})))\n"
               "  local id, why = emf.enemy_move(w, 13, {1, 2, 3})\n"
               "  emf.log('no ' .. tostring(id) .. ' ' .. tostring(why))\n"
               "  emf.log('bad ' .. tostring(pcall(emf.enemy_move, w, 70, {1, 2})))\n"
               "  emf.log('bad2 ' .. tostring(pcall(emf.enemy_move, w, 70, {1, 2, 3}, {arrive = 'near'})))\n"
               "  emf.log('moving ' .. tostring(emf.enemy_moving(w, 70)) .. ' ' .. tostring(emf.enemy_moving(w, 71)))\n"
               "  emf.log('cancel ' .. tostring(emf.enemy_move_cancel(2)) .. ' ' .. tostring(emf.enemy_move_cancel(9)))\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("mv.go");
    CHECK(logged("id 1"));
    CHECK(logged("id2 2"));
    CHECK(logged("no nil it has no enemy AI to move"));
    CHECK(logged("bad false"));
    CHECK(logged("bad2 false"));
    CHECK(logged("moving 1 nil"));
    CHECK(logged("cancel true false"));
    REQUIRE(r.fake.moves.size() == 2u);
    const auto& a = r.fake.moves[0];
    CHECK(a.o.size == sizeof(EmfMoveOrder));
    CHECK(a.o.enemy == 70u);
    CHECK(a.o.flags == 0u);
    CHECK(a.o.arrive == 0.0f);
    CHECK(a.o.seconds == 0.0f);
    REQUIRE(a.points.size() == 1u);
    CHECK(a.points[0].x == 10.0f);
    CHECK(a.points[0].z == 30.0f);
    const auto& b = r.fake.moves[1];
    REQUIRE(b.points.size() == 2u);
    CHECK(b.points[0].y == 2.0f);
    CHECK(b.points[1].x == 4.0f);
    CHECK(b.o.arrive == 3.0f);
    CHECK(b.o.seconds == 40.0f);
    CHECK(b.o.flags == (EMF_MOVE_PEACEFUL | EMF_MOVE_ALLIES));
    REQUIRE(a.cb != nullptr);
    a.cb(kServer, 1, EMF_MOVE_ARRIVED, a.user);  // after a server step
    CHECK(logged("done 1 arrived true"));
    b.cb(nullptr, 2, EMF_MOVE_CANCELLED, b.user);  // the world closed: no function given, nothing called
}

TEST_CASE("lua 1.33: a reload or a pause cancels the script's moves; their functions are not called") {
    Rig r(L"lua_moves_drop");
    add_script(r.dir, "md",
               "emf.command('go', '', function(w)\n"
               "  emf.enemy_move(w, 70, {1, 2, 3}, function() emf.log('called') end)\n"
               "  emf.enemy_move(w, 71, {1, 2, 3})\n"
               "end)\n"
               "emf.command('boom', '', function() error('boom') end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("md.go");
    REQUIRE(r.fake.moves.size() == 2u);
    r.run_command("md.boom");
    REQUIRE(host.status("md").rfind("paused", 0) == 0);
    CHECK(r.fake.move_cancels.size() == 2u);
    r.fake.moves[0].cb(kServer, 1, EMF_MOVE_CANCELLED, r.fake.moves[0].user);
    CHECK_FALSE(logged("called"));
}

TEST_CASE("lua 1.33 (review M4): emf.enemy_move with nil options still takes the function after them") {
    Rig r(L"lua_moves_nil");
    add_script(r.dir, "mn",
               "emf.command('go', '', function(w)\n"
               "  emf.enemy_move(w, 70, {1, 2, 3}, nil, function(world, id, status) emf.log('end ' .. status) end)\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("mn.go");
    REQUIRE(r.fake.moves.size() == 1u);
    REQUIRE(r.fake.moves[0].cb != nullptr);
    r.fake.moves[0].cb(kServer, 1, EMF_MOVE_STUCK, r.fake.moves[0].user);
    CHECK(logged("end stuck"));
}

// Before 0.6.8 shipped: the option tables are read strictly (a typo errors at once instead of an order that never ends).
TEST_CASE("lua 1.32/1.33: enemy_order and enemy_move refuse an unknown option key and a wrong type") {
    Rig r(L"lua_strict_opts");
    add_script(r.dir, "so",
               "local function bad(f, ...) local ok, err = pcall(f, ...) return ok and 'ok' or err end\n"
               "emf.command('go', '', function(w)\n"
               "  emf.log('o1 ' .. bad(emf.enemy_order, w, 70, 900, {secs = 30}))\n"
               "  emf.log('o2 ' .. bad(emf.enemy_order, w, 70, 900, {allies = 'no'}))\n"
               "  emf.log('o3 ' .. bad(emf.enemy_order, w, 70, 900, {damage = 4294967346}))\n"
               "  emf.log('o4 ' .. bad(emf.enemy_order, w, 70, 900, {damage = 2.5}))\n"
               "  emf.log('o5 ' .. bad(emf.enemy_order, w, 70, 900, {seconds = 10, damage = 40, allies = true}))\n"
               "  emf.log('m1 ' .. bad(emf.enemy_move, w, 70, {1, 2, 3}, {arive = 3}))\n"
               "  emf.log('m2 ' .. bad(emf.enemy_move, w, 70, {1, 2, 3}, {peaceful = 1}))\n"
               "  emf.log('m3 ' .. bad(emf.enemy_move, w, 70, {1, 2, 3}, {arrive = 3, seconds = 9, peaceful = true, allies = false}))\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("so.go");
    CHECK(logged("unknown option 'secs'"));
    CHECK(logged("allies is true or false"));
    CHECK(logged("damage must be 0 to 100000"));
    CHECK(logged("damage is a whole number"));
    CHECK(logged("o5 ok"));
    CHECK(logged("unknown option 'arive'"));
    CHECK(logged("peaceful is true, false"));
    CHECK(logged("m3 ok"));
    REQUIRE(r.fake.orders.size() == 1u);  // only the good one reached the core
    CHECK(r.fake.orders[0].damage == 40);
    REQUIRE(r.fake.moves.size() == 1u);
    CHECK(r.fake.moves[0].o.flags == EMF_MOVE_PEACEFUL);
}

TEST_CASE("lua 1.34: emf.enemy_move takes stuck = seconds and peaceful = \"all\"") {
    Rig r(L"lua_moves_opts");
    add_script(r.dir, "mo",
               "local function bad(f, ...) local ok, err = pcall(f, ...) return ok and 'ok' or err end\n"
               "emf.command('go', '', function(w)\n"
               "  emf.enemy_move(w, 70, {1, 2, 3}, {stuck = 5, peaceful = 'all'})\n"
               "  emf.enemy_move(w, 71, {1, 2, 3}, {peaceful = 'players'})\n"
               "  emf.log('b1 ' .. bad(emf.enemy_move, w, 70, {1, 2, 3}, {peaceful = 'everyone'}))\n"
               "  emf.log('b2 ' .. bad(emf.enemy_move, w, 70, {1, 2, 3}, {stuck = 'soon'}))\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("mo.go");
    REQUIRE(r.fake.moves.size() == 2u);
    CHECK(r.fake.moves[0].o.stuck_seconds == 5.0f);
    CHECK(r.fake.moves[0].o.flags == EMF_MOVE_PEACEFUL_ALL);
    CHECK(r.fake.moves[1].o.flags == EMF_MOVE_PEACEFUL);
    CHECK(r.fake.moves[1].o.stuck_seconds == 0.0f);
    CHECK(logged("b1 "));
    CHECK_FALSE(logged("b1 ok"));
    CHECK_FALSE(logged("b2 ok"));
}

TEST_CASE("lua 1.39: emf.villager_move walks a villager through points (arrive, seconds, stuck, hold seconds or true); villager_moving, villager_move_cancel") {
    Rig r(L"lua_vmoves");
    r.fake.vmoving[80] = 1;
    add_script(r.dir, "vm",
               "emf.command('go', '', function(w)\n"
               "  emf.log('id ' .. tostring(emf.villager_move(w, 80, {10, 20, 30}, {arrive = 3, seconds = 40, stuck = 10, hold = 30},\n"
               "    function(world, id, status) emf.log('vdone ' .. id .. ' ' .. status) end)))\n"
               "  emf.log('id2 ' .. tostring(emf.villager_move(w, 81, {{1, 2, 3}, {4, 5, 6}}, {hold = true})))\n"
               "  local id, why = emf.villager_move(w, 13, {1, 2, 3})\n"
               "  emf.log('no ' .. tostring(id) .. ' ' .. tostring(why))\n"
               "  emf.log('bad ' .. tostring(pcall(emf.villager_move, w, 80, {1, 2, 3}, {peaceful = true})))\n"
               "  emf.log('bad2 ' .. tostring(pcall(emf.villager_move, w, 80, {1, 2, 3}, {hold = 'long'})))\n"
               "  emf.log('moving ' .. tostring(emf.villager_moving(w, 80)) .. ' ' .. tostring(emf.villager_moving(w, 82)))\n"
               "  emf.log('cancel ' .. tostring(emf.villager_move_cancel(2)) .. ' ' .. tostring(emf.villager_move_cancel(9)))\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("vm.go");
    CHECK(logged("id 1"));
    CHECK(logged("id2 2"));
    CHECK(logged("no nil a point is outside the villager's walk area"));
    CHECK(logged("bad false"));
    CHECK(logged("bad2 false"));
    CHECK(logged("moving 1 nil"));
    CHECK(logged("cancel true false"));
    REQUIRE(r.fake.vmoves.size() == 2u);
    const auto& a = r.fake.vmoves[0];
    CHECK(a.o.size == sizeof(EmfMoveOrder));
    CHECK(a.o.enemy == 80u);
    CHECK(a.o.arrive == 3.0f);
    CHECK(a.o.seconds == 40.0f);
    CHECK(a.o.stuck_seconds == 10.0f);
    CHECK(a.o.hold_seconds == 30.0f);
    CHECK(a.o.flags == 0u);
    const auto& b = r.fake.vmoves[1];
    REQUIRE(b.points.size() == 2u);
    CHECK(b.o.flags == EMF_MOVE_HOLD);
    a.cb(kServer, 1, EMF_MOVE_ARRIVED, a.user);
    CHECK(logged("vdone 1 arrived"));
}

TEST_CASE("lua 1.39: a pause cancels the script's villager moves, also one that arrived and holds; on a core older than v46 villager_move is nil and why") {
    Rig r(L"lua_vmoves_drop");
    add_script(r.dir, "vd",
               "emf.command('go', '', function(w)\n"
               "  emf.villager_move(w, 80, {1, 2, 3}, {hold = true}, function() emf.log('arrived') end)\n"
               "end)\n"
               "emf.command('boom', '', function() error('boom') end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("vd.go");
    REQUIRE(r.fake.vmoves.size() == 1u);
    r.fake.vmoves[0].cb(kServer, 1, EMF_MOVE_ARRIVED, r.fake.vmoves[0].user);  // it arrived and holds
    CHECK(logged("arrived"));
    r.run_command("vd.boom");
    REQUIRE(host.status("vd").rfind("paused", 0) == 0);
    REQUIRE(r.fake.vmove_cancels.size() == 1u);  // the hold ends with the script
    CHECK(r.fake.vmove_cancels[0] == 1u);
    // an older core
    Rig o(L"lua_vmoves_old");
    add_script(o.dir, "vo",
               "emf.command('go', '', function(w)\n"
               "  local id, why = emf.villager_move(w, 80, {1, 2, 3})\n"
               "  emf.log('old ' .. tostring(id) .. ' ' .. tostring(why))\n"
               "end)\n");
    o.api.size = static_cast<uint32_t>(offsetof(EmfApi, villager_move));
    o.api.version = 45;
    Host h2(&o.api, o.dir);
    h2.load_all();
    o.run_command("vo.go");
    CHECK(logged("old nil this EMF has no villager moves (EMF API v46)"));
}
