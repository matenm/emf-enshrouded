// The Lua host's world, component, item, event, emit and wheel functions, through the fake API.
#include <windows.h>

#include <doctest.h>

#include <cstring>
#include <string>
#include <vector>

#include "emf/generated/components.hpp"
#include "lua_host/host.h"
#include "lua_test_fake.h"

using emf::lua::Host;
namespace ecs = emf::gen::ecs;

namespace {
void put_transform(uint32_t entity, double x, double y, double z) {
    std::vector<uint8_t> d(sizeof(ecs::CurrentTransform), 0);
    auto* t = reinterpret_cast<ecs::CurrentTransform*>(d.data());
    t->transform.position.x = static_cast<int64_t>(x * 4294967296.0);
    t->transform.position.y = static_cast<int64_t>(y * 4294967296.0);
    t->transform.position.z = static_cast<int64_t>(z * 4294967296.0);
    g->components[{ecs::CurrentTransform::kHash, entity}] = d;
}
}  // namespace

TEST_CASE("lua bindings: world:get gives a proxy: nested reads, checked writes, useless after its callback") {
    Rig r(L"proxy");
    put_transform(7, 10.5, 20.0, -3.25);
    r.fake.components[{ecs::Faction::kHash, 7}] = std::vector<uint8_t>(1, 4);
    r.fake.entities = {7, 8};
    add_script(r.dir, "px",
               "emf.command('check', '', function(w)\n"
               "  local t = w:get('CurrentTransform', 7)\n"
               "  emf.reply('x ' .. t.transform.position.x)\n"
               "  local x, y, z = w:position(7)\n"
               "  emf.reply(string.format('pos %.2f %.2f %.2f', x, y, z))\n"
               "  local f = w:get('Faction', 7)\n"
               "  emf.reply('faction ' .. f.faction)\n"
               "  f.faction = 11\n"
               "  local ok, err = pcall(function() f.faction = 300 end)\n"
               "  emf.reply('300: ' .. tostring(ok) .. ' ' .. tostring(err))\n"
               "  emf.reply('none ' .. tostring(w:get('Faction', 99)))\n"
               "  emf.reply('entities ' .. #w:entities())\n"
               "  local ok2, err2 = pcall(function() return w:get('HitEvent', 7) end)\n"
               "  emf.reply('event: ' .. tostring(err2))\n"
               "  keep = f\n"
               "end)\n"
               "emf.command('stale', '', function(w) keep.faction = 3 end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("px.check");
    auto said = [&](const std::string& s) {
        for (const auto& x : r.fake.replies)
            if (x.find(s) != std::string::npos) return true;
        return false;
    };
    CHECK(said("x " + std::to_string(static_cast<int64_t>(10.5 * 4294967296.0))));
    CHECK(said("pos 10.50 20.00 -3.25"));
    CHECK(said("faction 4"));
    CHECK(r.fake.components[{ecs::Faction::kHash, 7}][0] == 11);  // written into the component
    CHECK(said("300: false"));
    CHECK(said("0 to 255"));
    CHECK(said("none nil"));
    CHECK(said("entities 2"));
    CHECK(said("not a component"));
    r.run_command("px.stale");
    CHECK(host.status("px").find("inside the callback that received it") != std::string::npos);
    CHECK(r.fake.components[{ecs::Faction::kHash, 7}][0] == 11);  // the stale write did nothing
}

TEST_CASE("lua bindings: reading a field the type does not have is an error naming both (components and events)") {
    Rig r(L"typo");
    r.fake.components[{ecs::Faction::kHash, 7}] = std::vector<uint8_t>(1, 4);
    add_script(r.dir, "ty",
               "emf.on_event('EntityDiedEvent', function(w, e)\n"
               "  emf.log('dead', e.deadEntityId, 'raw', tostring(rawget(e, 'deadEntityID')))\n"
               "  local ok, err = pcall(function() return e.deadEntityID end)\n"
               "  emf.log('event typo:', tostring(ok), err)\n"
               "end)\n"
               "emf.command('check', '', function(w)\n"
               "  local f = w:get('Faction', 7)\n"
               "  local ok, err = pcall(function() return f.factoin end)\n"
               "  emf.reply('component typo: ' .. tostring(ok) .. ' ' .. tostring(err))\n"
               "  local t = w:get('CurrentTransform', 7)\n"
               "end)\n");
    put_transform(7, 1, 2, 3);
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("ty.check");
    REQUIRE_FALSE(r.fake.replies.empty());
    CHECK(r.fake.replies.back().find("component typo: false") == 0);
    CHECK(r.fake.replies.back().find("Faction has no field factoin") != std::string::npos);
    REQUIRE(r.fake.subs.size() == 1);
    std::vector<uint8_t> ev(ecs::EntityDiedEvent::kSize, 0);
    const uint32_t dead = 42;
    std::memcpy(&ev[offsetof(ecs::EntityDiedEvent, deadEntityId)], &dead, 4);
    r.fake.subs[0].cb(kServer, ev.data(), static_cast<uint32_t>(ev.size()), r.fake.subs[0].user);
    CHECK(logged("dead 42 raw nil"));
    CHECK(logged("event typo: false"));
    CHECK(logged("EntityDiedEvent has no field deadEntityID"));
    CHECK(host.status("ty") == "running");
}

TEST_CASE("lua bindings: items, give, put with a callback, spawn and destroy") {
    Rig r(L"items");
    add_script(r.dir, "it",
               "emf.command('go', '', function(w)\n"
               "  local id = emf.item('Wood')\n"
               "  emf.reply('id ' .. tostring(id) .. ' nope ' .. tostring(emf.item('Nope')))\n"
               "  local info = emf.item_info(id)\n"
               "  emf.reply('info ' .. info.name .. ' ' .. info.max_stack)\n"
               "  emf.give(w, 1, 'Wood', 5)\n"
               "  emf.put(w, 1, id, 3, function(w2, done) emf.log('put done', done) end)\n"
               "  emf.reply('spawned ' .. emf.spawn(w, 'd5d05217-52dc-467a-9192-26b4f661d8d9', 1.5, 2, 3))\n"
               "  emf.destroy(w, 77)\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("it.go");
    auto said = [&](const std::string& s) {
        for (const auto& x : r.fake.replies)
            if (x.find(s) != std::string::npos) return true;
        return false;
    };
    CHECK(said("id 17 nope nil"));
    CHECK(said("info Wood 250"));
    REQUIRE(r.fake.gives.size() == 1);
    CHECK(r.fake.gives[0].item == 0x11);
    CHECK(r.fake.gives[0].count == 5);
    REQUIRE(r.fake.puts.size() == 1);
    CHECK(r.fake.puts[0].count == 3);
    r.fake.puts[0].cb(kServer, 1, 0x11, 3, 3, r.fake.puts[0].user);  // the core's answer, one step later
    CHECK(logged("put done 3"));
    REQUIRE(r.fake.spawns.size() == 1);
    CHECK(r.fake.spawns[0].x == 1.5);
    CHECK(r.fake.spawns[0].guid[0] == 0x17);  // Windows GUID layout
    CHECK(said("spawned 1234"));
    REQUIRE(r.fake.destroyed.size() == 1);
    CHECK(r.fake.destroyed[0] == 77);
}

TEST_CASE("lua bindings: on_event delivers a table of the event's fields (id wrappers read as numbers)") {
    Rig r(L"events");
    add_script(r.dir, "ev", "emf.on_event('EntityDiedEvent', function(w, e) emf.log('died', e.deadEntityId) end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    REQUIRE(r.fake.subs.size() == 1);
    CHECK(r.fake.subs[0].kind == EMF_WORLD_SERVER);
    CHECK(r.fake.subs[0].name == "EntityDiedEvent");
    std::vector<uint8_t> ev(ecs::EntityDiedEvent::kSize, 0);
    const uint32_t dead = 42;
    std::memcpy(&ev[offsetof(ecs::EntityDiedEvent, deadEntityId)], &dead, 4);
    r.fake.subs[0].cb(kServer, ev.data(), static_cast<uint32_t>(ev.size()), r.fake.subs[0].user);
    CHECK(logged("died 42"));
}

TEST_CASE("lua bindings: an event whose size is not the reflected size is not delivered (logged once); the core checks it too") {
    Rig r(L"eventsize");
    add_script(r.dir, "ev", "emf.on_event('EntityDiedEvent', function(w, e) emf.log('died', e.deadEntityId) end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    REQUIRE(r.fake.subs.size() == 1);
    CHECK(r.fake.subs[0].expected == ecs::EntityDiedEvent::kSize);
    const uint32_t dead = 42;
    for (const uint32_t size : {ecs::EntityDiedEvent::kSize + 8, ecs::EntityDiedEvent::kSize + 8, ecs::EntityDiedEvent::kSize - 4}) {
        std::vector<uint8_t> ev(size, 0);  // a game update changed the layout
        std::memcpy(&ev[offsetof(ecs::EntityDiedEvent, deadEntityId)], &dead, 4);
        r.fake.subs[0].cb(kServer, ev.data(), size, r.fake.subs[0].user);
    }
    CHECK_FALSE(logged("died 42"));
    int lines = 0;
    for (const auto& l : r.fake.log) lines += l.find("EntityDiedEvent") != std::string::npos && l.find("bytes") != std::string::npos;
    CHECK(lines == 1);
    CHECK(host.status("ev") == "running");
}

TEST_CASE("lua bindings: full type names (keen::ecs::HitEvent) work in on_event, emits, emit and world:get") {
    Rig r(L"fullnames");
    r.fake.components[{ecs::Faction::kHash, 7}] = std::vector<uint8_t>(1, 4);
    add_script(r.dir, "fn",
               "emf.on_event('keen::ecs::EntityDiedEvent', function(w, e) emf.log('died', e.deadEntityId) end)\n"
               "emf.emits({'keen::ecs::NpcCommandEvent'})\n"
               "emf.command('go', '', function(w)\n"
               "  emf.emit('server', 'NpcCommandEvent', {npcId = 5})\n"
               "  emf.emit('server', 'keen::ecs::NpcCommandEvent', {npcId = 6})\n"
               "  emf.reply('faction ' .. w:get('keen::ecs::Faction', 7).faction)\n"
               "end)\n");
    add_script(r.dir, "fn2", "emf.on_event('EntityDiedEvent', function(w, e) emf.log('also died', e.deadEntityId) end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    REQUIRE(r.fake.subs.size() == 1);  // one subscription for both spellings, by the name the core knows
    CHECK(r.fake.subs[0].name == "EntityDiedEvent");
    std::vector<uint8_t> ev(ecs::EntityDiedEvent::kSize, 0);
    const uint32_t dead = 42;
    std::memcpy(&ev[offsetof(ecs::EntityDiedEvent, deadEntityId)], &dead, 4);
    r.fake.subs[0].cb(kServer, ev.data(), static_cast<uint32_t>(ev.size()), r.fake.subs[0].user);
    CHECK(logged("[fn] died 42"));
    CHECK(logged("[fn2] also died 42"));
    REQUIRE(r.fake.programs.size() == 1);
    REQUIRE(r.fake.programs[0].out.size() == 1);
    CHECK(r.fake.programs[0].out[0] == "NpcCommandEvent");
    r.run_command("fn.go");
    CHECK(host.status("fn") == "running");
    REQUIRE_FALSE(r.fake.replies.empty());
    CHECK(r.fake.replies.back() == "faction 4");
    r.fake.programs[0].fn(nullptr, r.fake.programs[0].user);
    REQUIRE(r.fake.emitted.size() == 2);
    CHECK(r.fake.emitted[0].first == "NpcCommandEvent");
    CHECK(r.fake.emitted[1].first == "NpcCommandEvent");
}

TEST_CASE("lua bindings: emits + emit queue an event that the host's program writes on the next step") {
    Rig r(L"emit");
    add_script(r.dir, "em",
               "emf.emits({'NpcCommandEvent'})\n"
               "emf.command('go', '', function(w)\n"
               "  emf.emit('server', 'NpcCommandEvent', {type = 2, npcId = 1025})\n"
               "  local ok, err = pcall(function() emf.emit('server', 'HitEvent', {}) end)\n"
               "  emf.reply(tostring(err))\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    REQUIRE(r.fake.programs.size() == 1);
    CHECK(r.fake.programs[0].kind == EMF_WORLD_SERVER);
    REQUIRE(r.fake.programs[0].out.size() == 1);
    CHECK(r.fake.programs[0].out[0] == "NpcCommandEvent");
    r.run_command("em.go");
    REQUIRE(r.fake.replies.size() == 1);
    CHECK(r.fake.replies[0].find("emits") != std::string::npos);  // HitEvent was not declared
    r.fake.programs[0].fn(nullptr, r.fake.programs[0].user);    // the program's next run
    REQUIRE(r.fake.emitted.size() == 1);
    CHECK(r.fake.emitted[0].first == "NpcCommandEvent");
    const auto& b = r.fake.emitted[0].second;
    CHECK(b[offsetof(ecs::NpcCommandEvent, type)] == 2);
    uint32_t npc = 0;
    std::memcpy(&npc, &b[offsetof(ecs::NpcCommandEvent, npcId)], 4);
    CHECK(npc == 1025);
    r.fake.programs[0].fn(nullptr, r.fake.programs[0].user);
    CHECK(r.fake.emitted.size() == 1);  // sent once
}

TEST_CASE("lua bindings: a wheel's choice reaches the script as 1..n, a cancel as nil") {
    Rig r(L"wheel");
    add_script(r.dir, "wh", "emf.command('open', '', function(w)\n"
                            "  emf.wheel('Pick', {'first', 'second'}, 'G', function(w2, c) emf.log('choice', tostring(c)) end)\n"
                            "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("wh.open");
    r.run_command("wh.open");  // each wheel's callback runs once
    REQUIRE(r.fake.wheels.size() == 2);
    CHECK(r.fake.wheels[0].title == "Pick");
    CHECK(r.fake.wheels[0].items.size() == 2);
    CHECK(r.fake.wheels[0].hold == 'G');
    r.fake.wheels[0].cb(kClient, 1, r.fake.wheels[0].user);
    CHECK(logged("choice 2"));
    r.fake.wheels[1].cb(kClient, -1, r.fake.wheels[1].user);
    CHECK(logged("choice nil"));
}

TEST_CASE("lua bindings: an on_event or emits added after the game started is reported, and the rest of the mod runs") {
    Rig r(L"lateevent");
    add_script(r.dir, "late", "error('typo when the game started')\n");
    Host host(&r.api, r.dir);
    host.load_all();  // the events of every script are subscribed now
    CHECK(host.status("late").find("paused") == 0);
    Sleep(20);
    write(r.dir + L"\\late\\main.lua", "emf.on_event('EntityDiedEvent', function() end)\nemf.emits({'NpcCommandEvent'})\n"
                                       "emf.log('rest of the mod ran')\n");
    host.check_reload_now();
    CHECK(host.status("late") == "running");
    CHECK(logged("rest of the mod ran"));
    bool told = false;
    for (const auto& n : r.fake.notes) told = told || n.find("restart") != std::string::npos;
    CHECK(told);
}

TEST_CASE("lua bindings: emf.item works inside callbacks; outside one it says so; on_unload gets no stale world") {
    Rig r(L"itemworld");
    add_script(r.dir, "iw",
               "local ok, err = pcall(emf.item, 'Wood')\n"
               "emf.log('top level:', tostring(ok), tostring(err))\n"
               "emf.on_tick(function() emf.log('in tick', emf.item('Wood')) end)\n"
               "function on_unload() local ok2 = pcall(emf.item, 'Wood') emf.log('in unload ok', tostring(ok2)) end\n");
    Host host(&r.api, r.dir);
    host.load_all();
    CHECK(logged("top level: false"));
    CHECK(logged("inside a callback"));
    host.tick(kServer);
    CHECK(logged("in tick 17"));
    Sleep(20);
    write(r.dir + L"\\iw\\main.lua", "emf.log('v2')\n");
    host.check_reload_now();
    CHECK(logged("in unload ok false"));  // the last tick's world is gone by now
}

TEST_CASE("lua bindings: emf.sender tells who typed the command (a player in chat or the console)") {
    Rig r(L"sender");
    add_script(r.dir, "who",
               "emf.command('me', '', function(w)\n"
               "  local s = emf.sender()\n"
               "  emf.reply(string.format('chat %s entity %d account %s admin %s', tostring(s.chat), s.entity, s.account, tostring(s.admin)))\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.fake.sender.size = sizeof r.fake.sender;
    r.fake.sender.from_chat = 1;
    r.fake.sender.entity = 3;
    r.fake.sender.account = 0x00ab00cd00ef0011ull;
    r.run_command("who.me");
    REQUIRE_FALSE(r.fake.replies.empty());
    CHECK(r.fake.replies.back() == "chat true entity 3 account 0x00ab00cd00ef0011 admin false");
    r.fake.sender.admin = 1;
    r.run_command("who.me");
    CHECK(r.fake.replies.back() == "chat true entity 3 account 0x00ab00cd00ef0011 admin true");
}

TEST_CASE("lua bindings: world:account is the player's account from BasicPlayerInfos, nil when it is not known") {
    Rig r(L"account");
    r.fake.player_info(0, 1, 0xAA);
    r.fake.player_info(1, 7, 0xBB);  // entry 1 holds entity 7: it is not entity 2's
    r.fake.player_info(2, 3, 0xCC);
    add_script(r.dir, "acc",
               "emf.command('show', '', function(w)\n"
               "  local out = {}\n"
               "  for _, id in ipairs({1, 2, 3, 0, 5000}) do out[#out + 1] = tostring(w:account(id)) end\n"
               "  emf.reply(table.concat(out, ' '))\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("acc.show");
    REQUIRE(!r.fake.replies.empty());
    CHECK(r.fake.replies.back() == "0x00000000000000aa nil 0x00000000000000cc nil nil");
    r.fake.player_infos.clear();  // no such singleton (e.g. a client world)
    r.run_command("acc.show");
    CHECK(r.fake.replies.back() == "nil nil nil nil nil");
}

// ---- Lua host 1.4: time of day, put with level and rarity, kill, commands for players ----
namespace {
void put_daytime(int64_t tod_ns) {
    std::vector<uint8_t> d(0x70, 0);
    std::memcpy(d.data() + 0x48, &tod_ns, 8);
    g->singletons["g38_daytime::Daytime"] = d;
}
void put_health(uint32_t entity, std::vector<int32_t> storage, uint32_t current, uint32_t max) {
    std::vector<uint8_t> h(sizeof(ecs::Health), 0);
    auto* hp = reinterpret_cast<ecs::Health*>(h.data());
    hp->storageCount = 8;
    for (size_t i = 0; i < storage.size() && i < 8; ++i) hp->dataStorage[i] = storage[i];
    g->components[{ecs::Health::kHash, entity}] = h;
    std::vector<uint8_t> n(sizeof(ecs::NetworkHealth), 0);
    auto* nh = reinterpret_cast<ecs::NetworkHealth*>(n.data());
    nh->health = current;
    nh->healthMax = max;
    g->components[{ecs::NetworkHealth::kHash, entity}] = n;
}
int32_t health_slot(uint32_t entity, int i) {
    return reinterpret_cast<ecs::Health*>(g->components[{ecs::Health::kHash, entity}].data())->dataStorage[i];
}
}  // namespace

TEST_CASE("lua 1.4: emf.time_of_day gives the hours of the world's Daytime, nil when there is none or it is nonsense") {
    Rig r(L"timeofday");
    add_script(r.dir, "tod",
               "emf.on_tick(function(w)\n"
               "  emf.log('tod', tostring(emf.time_of_day()), tostring(emf.time_of_day(w)))\n"
               "end)\n"
               "local ok, err = pcall(emf.time_of_day)\n"
               "emf.log('top', tostring(ok))\n");
    Host host(&r.api, r.dir);
    host.load_all();
    CHECK(logged("top false"));  // no world outside callbacks
    host.tick(kServer);
    CHECK(logged("tod nil nil"));
    put_daytime(int64_t(19) * 3600 * 1000000000ll + 30 * 60 * 1000000000ll);  // 19:30
    host.tick(kServer);
    CHECK(logged("tod 19.5 19.5"));
    put_daytime(int64_t(25) * 3600 * 1000000000ll);  // not a time of day (a layout change after a game update?)
    host.tick(kServer);
    CHECK(logged("tod nil nil"));
}

TEST_CASE("lua 1.4: emf.put takes an item level and rarity (a table), before or after its callback") {
    Rig r(L"putlevel");
    add_script(r.dir, "pl",
               "emf.command('go', '', function(w)\n"
               "  emf.put(w, 1, 'Wood', 1, {level = 12, rarity = 2})\n"
               "  emf.put(w, 1, 'Wood', 2, function(w2, n) emf.log('done', n) end, {level = 7, rarity = 'epic'})\n"
               "  emf.put(w, 1, 'Wood', 3)\n"
               "  local ok, err = pcall(emf.put, w, 1, 'Wood', 1, {rarity = 9})\n"
               "  emf.reply('bad rarity: ' .. tostring(err))\n"
               "  local ok2, err2 = pcall(emf.put, w, 1, 'Wood', 1, {rarity = 'shiny'})\n"
               "  emf.reply('bad name: ' .. tostring(err2))\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("pl.go");
    REQUIRE(r.fake.puts.size() == 3);
    CHECK(r.fake.puts[0].level == 12u);
    CHECK(r.fake.puts[0].rarity == 2);
    CHECK((r.fake.puts[0].flags & EMF_ITEMS_RARITY) != 0);
    CHECK(r.fake.puts[1].level == 7u);
    CHECK(r.fake.puts[1].rarity == 3);  // epic
    REQUIRE(r.fake.puts[1].cb);
    r.fake.puts[1].cb(kServer, 1, 0x11, 2, 2, r.fake.puts[1].user);
    CHECK(logged("done 2"));
    CHECK(r.fake.puts[2].level == 0u);  // the game's default
    CHECK((r.fake.puts[2].flags & EMF_ITEMS_RARITY) == 0);
    REQUIRE(r.fake.replies.size() == 2);
    CHECK(r.fake.replies[0].find("rarity") != std::string::npos);
    CHECK(r.fake.replies[1].find("shiny") != std::string::npos);
    CHECK(host.status("pl") == "running");
}

TEST_CASE("lua 1.4: emf.kill sets the current-health slot to 0 (the game's death program does the rest); never a player") {
    Rig r(L"kill");
    put_health(500, {1498, 0, 1498, 0, 0, 0, 0, 1498}, 1498, 1498);  // a mini-boss as seen in game
    put_health(501, {700, 0, 1498, 0, 0, 0, 0, 1498}, 700, 1498);    // a hurt one
    put_health(502, {0, 0, 1498, 0, 0, 0, 0, 1498}, 0, 1498);        // already dead
    put_health(1, {100, 0, 100, 0, 0, 0, 0, 100}, 100, 100);         // a player
    r.fake.components[{ecs::PlayerState::kHash, 1}] = std::vector<uint8_t>(sizeof(ecs::PlayerState), 0);
    add_script(r.dir, "kl",
               "emf.command('go', '', function(w)\n"
               "  local out = {}\n"
               "  for _, id in ipairs({500, 501, 502, 1, 999}) do out[#out + 1] = tostring(emf.kill(w, id)) end\n"
               "  emf.reply(table.concat(out, ' '))\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("kl.go");
    REQUIRE_FALSE(r.fake.replies.empty());
    CHECK(r.fake.replies.back() == "true true false false false");
    CHECK(health_slot(500, 0) == 0);
    CHECK(health_slot(500, 2) == 1498);  // the maximum stays
    CHECK(health_slot(501, 0) == 0);
    CHECK(health_slot(1, 0) == 100);  // players are never killed
}

TEST_CASE("lua 1.4: emf.command(name, help, fn, {players = true}) marks it for every player; a reload can take the mark off") {
    Rig r(L"playercmd");
    add_script(r.dir, "pc",
               "emf.command('where', ': where is it', function(w) emf.reply('here') end, {players = true})\n"
               "emf.command('start', '', function(w) end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    REQUIRE(r.fake.commands.size() == 2);
    CHECK(r.fake.commands[0].name == "pc.where");
    CHECK(r.fake.commands[0].flags == EMF_COMMAND_PLAYERS);
    CHECK(r.fake.commands[1].flags == 0u);
    Sleep(20);
    write(r.dir + L"\\pc\\main.lua","emf.command('where', ': where is it', function(w) emf.reply('there') end)\n");
    host.check_reload_now();
    REQUIRE(host.status("pc") == "running");
    REQUIRE(r.fake.commands.size() == 2);
    CHECK(r.fake.commands[0].flags == 0u);  // updated in place, not added twice
    r.run_command("pc.where");
    CHECK(r.fake.replies.back() == "there");
}

TEST_CASE("lua 1.4: emf.session_token is unique to this run of the game and this session (sessions restart at 1 every run)") {
    Rig r(L"token");
    add_script(r.dir, "tk", "emf.command('t', '', function(w) emf.reply(emf.session_token()) end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("tk.t");
    REQUIRE(r.fake.replies.size() == 1);
    const std::string first = r.fake.replies[0];
    CHECK(first.size() > 4);
    CHECK(first.substr(first.size() - 2) == "-1");  // ... and the session
    CHECK(first != "1");
    r.run_command("tk.t");
    CHECK(r.fake.replies[1] == first);  // the same within a session
    r.fake.session = 2;
    r.run_command("tk.t");
    CHECK(r.fake.replies[2] != first);
    CHECK(r.fake.replies[2].substr(0, first.size() - 2) == first.substr(0, first.size() - 2));  // the same run
}

// ---- Lua host 1.7: teleport (API v20) ----

TEST_CASE("lua 1.7: emf.teleport passes the target and options; fn gets moved, the position, an info table and the world") {
    Rig r(L"teleport");
    with_teleport(r.api);
    add_script(r.dir, "tp",
               "local function j(...) local t = {} for i = 1, select('#', ...) do t[#t + 1] = tostring((select(i, ...))) end return table.concat(t, ' ') end\n"
               "emf.command('go', '', function(w)\n"
               "  emf.reply(j(emf.teleport(w, 1, 4010.5, 700, 1200, {ground = true, yaw = 90, checkpoint = true}, function(moved, x, y, z, info, w2)\n"
               "    emf.log(j('done', moved, x, y, z, info.status, info.distance, info.off_target, info.raw_target, info.reason, info.entity, w2:is_server()))\n"
               "  end)))\n"
               "  emf.reply(j(emf.teleport(w, 2, nil, nil, nil, {spawn_point = 1025, ready = true}, function(moved, x, y, z, info)\n"
               "    emf.log(j('home', moved, info.status))\n"
               "  end)))\n"
               "  emf.reply(j(emf.teleport(w, 3, 1, 2, 3)))\n"
               "  emf.reply(j(pcall(emf.teleport, w, 4, nil, 2, 3)))\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("tp.go");
    REQUIRE(r.fake.replies.size() == 4);
    CHECK(r.fake.replies[0] == "true");
    CHECK(r.fake.replies[1] == "true");
    CHECK(r.fake.replies[2] == "true");
    CHECK(r.fake.replies[3].find("false") == 0);  // x is needed without a spawn point
    REQUIRE(r.fake.teleports.size() == 3);
    const EmfTeleport& a = r.fake.teleports[0];
    CHECK(a.size == sizeof(EmfTeleport));
    CHECK(a.entity == 1u);
    CHECK(a.x == 4010.5);
    CHECK(a.z == 1200.0);
    CHECK(a.flags == (EMF_TELEPORT_FIND_GROUND | EMF_TELEPORT_FACE_YAW | EMF_TELEPORT_CHECKPOINT));
    CHECK(a.yaw_deg == 90.0f);
    CHECK(a.cb != nullptr);
    const EmfTeleport& b = r.fake.teleports[1];
    CHECK(b.spawn_point == 1025u);
    CHECK(b.flags == EMF_TELEPORT_NOTIFY_READY);
    CHECK(r.fake.teleports[2].cb == nullptr);  // no function: no callback
    // the core's answers, after later steps
    EmfTeleportResult res{};
    res.size = sizeof res;
    res.entity = 1;
    res.status = EMF_TELEPORT_MOVED;
    res.flags = EMF_TELEPORT_OFF_TARGET | EMF_TELEPORT_LAST;
    res.x = 4012;
    res.y = 701;
    res.z = 1200;
    res.distance = 1.5;
    res.reason = "";
    a.cb(kServer, &res, a.user);
    CHECK(logged("done true 4012.0 701.0 1200.0 moved 1.5 true false  1 true"));
    res.entity = 2;
    res.flags = 0;  // a READY follows
    b.cb(kServer, &res, b.user);
    res.status = EMF_TELEPORT_READY;
    res.flags = EMF_TELEPORT_LAST;
    b.cb(kServer, &res, b.user);
    CHECK(logged("home true moved"));
    CHECK(logged("home true ready"));
}

TEST_CASE("lua 1.7: a teleport answered after a reload never frees a reference of the new script state") {
    Rig r(L"teleport_reload");
    with_teleport(r.api);
    add_script(r.dir, "tr",
               "emf.command('go', '', function(w) emf.teleport(w, 1, 1, 2, 3, function(m) emf.log('old cb') end) end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("tr.go");
    REQUIRE(r.fake.teleports.size() == 1);
    Sleep(20);
    write(r.dir + L"\\tr\\main.lua",
          "emf.command('go', '', function(w) emf.teleport(w, 1, 1, 2, 3, function(m) emf.log('new callback') end) end)\n");
    host.check_reload_now();
    REQUIRE(host.status("tr") == "running");
    r.run_command("tr.go");
    REQUIRE(r.fake.teleports.size() == 2);
    EmfTeleportResult res{};
    res.size = sizeof res;
    res.entity = 1;
    res.status = EMF_TELEPORT_MOVED;
    res.flags = EMF_TELEPORT_LAST;
    res.reason = "";
    r.fake.teleports[0].cb(kServer, &res, r.fake.teleports[0].user);  // the old script's answer: not heard, nothing freed
    CHECK_FALSE(logged("old cb"));
    r.fake.teleports[1].cb(kServer, &res, r.fake.teleports[1].user);
    CHECK(logged("new callback"));
}

TEST_CASE("lua 1.7: an answer after the session ended is not passed to the script (its world is gone), only freed") {
    Rig r(L"teleport_ended");
    with_teleport(r.api);
    add_script(r.dir, "te",
               "emf.command('go', '', function(w) emf.teleport(w, 1, 1, 2, 3, function(m, x, y, z, info) emf.log('heard ' .. info.status) end) end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("te.go");
    REQUIRE(r.fake.teleports.size() == 1);
    EmfTeleportResult res{};
    res.size = sizeof res;
    res.entity = 1;
    res.status = EMF_TELEPORT_FAILED;
    res.flags = EMF_TELEPORT_LAST | EMF_TELEPORT_SESSION_ENDED;
    res.reason = "a new world was set up";
    r.fake.teleports[0].cb(kServer, &res, r.fake.teleports[0].user);
    CHECK_FALSE(logged("heard"));
}

TEST_CASE("lua 1.7: emf.teleport is false on a core without teleport, and when the core refuses (no callback kept)") {
    Rig r(L"teleport_old");
    add_script(r.dir, "tq",
               "emf.command('go', '', function(w)\n"
               "  emf.reply(tostring(emf.teleport(w, 1, 1, 2, 3, function() end)))\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("tq.go");
    REQUIRE(r.fake.replies.size() == 1);
    CHECK(r.fake.replies[0] == "false");
    with_teleport(r.api);
    r.fake.teleport_ok = false;
    r.run_command("tq.go");
    CHECK(r.fake.replies.back() == "false");
    REQUIRE(r.fake.teleports.size() == 1);
}

// ---- Lua host 1.5: health, attributes, attribute_add, damage; kill through the attributes ----

TEST_CASE("lua 1.5: emf.health gives current and max; emf.attribute takes a name or an id") {
    Rig r(L"health");
    with_attributes(r.api);
    r.fake.attrs[{500, EMF_ATTR_HEALTH}] = 700;
    r.fake.attrs[{500, EMF_ATTR_MAX_HEALTH}] = 1498;
    r.fake.attrs[{500, EMF_ATTR_STAMINA}] = 80;
    r.fake.attrs[{500, EMF_ATTR_MANA}] = 30;
    r.fake.attrs[{500, 0x12345678u}] = 5;
    put_health(501, {0, 0, 0}, 320, 400);  // a creature seen through NetworkHealth only
    add_script(r.dir, "hp",
               "local function j(...) local t = {} for i = 1, select('#', ...) do t[#t + 1] = tostring((select(i, ...))) end return table.concat(t, ' ') end\n"
               "emf.command('go', '', function(w)\n"
               "  local cur, max = emf.health(w, 500)\n"
               "  local c2, m2 = emf.health(w, 501)\n"
               "  local none = emf.health(w, 999)\n"
               "  emf.reply(j(cur, max, c2, m2, none))\n"
               "  emf.reply(j(emf.attribute(w, 500, 'health'), emf.attribute(w, 500, 'max_health'), emf.attribute(w, 500, 'stamina'),\n"
               "              emf.attribute(w, 500, 'mana'), emf.attribute(w, 500, 0x12345678), emf.attribute(w, 999, 'health')))\n"
               "  local ok, err = pcall(emf.attribute, w, 500, 'speed')\n"
               "  emf.reply(j(ok, err))\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("hp.go");
    REQUIRE(r.fake.replies.size() == 3);
    CHECK(r.fake.replies[0] == "700 1498 320 400 nil");
    CHECK(r.fake.replies[1] == "700 1498 80 30 5 nil");
    CHECK(r.fake.replies[2].find("false") == 0);
    CHECK(r.fake.replies[2].find("unknown attribute 'speed'") != std::string::npos);
}

TEST_CASE("lua 1.5: emf.attribute_add and emf.damage reach the core; a bad amount is an error") {
    Rig r(L"attradd");
    with_attributes(r.api);
    r.fake.attrs[{500, EMF_ATTR_HEALTH}] = 700;
    add_script(r.dir, "ad",
               "local function j(...) local t = {} for i = 1, select('#', ...) do t[#t + 1] = tostring((select(i, ...))) end return table.concat(t, ' ') end\n"
               "emf.command('go', '', function(w)\n"
               "  emf.reply(j(emf.attribute_add(w, 500, 'health', -200), emf.attribute_add(w, 999, 'health', 1)))\n"
               "  emf.reply(j(emf.damage(w, 1, 500, 250), emf.damage(w, 0, 500, 5)))\n"
               "  local ok, err = pcall(emf.damage, w, 1, 500, 2.5)\n"
               "  emf.reply(tostring(ok))\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("ad.go");
    REQUIRE(r.fake.replies.size() == 3);
    CHECK(r.fake.replies[0] == "true false");
    CHECK((r.fake.attrs[{500, EMF_ATTR_HEALTH}]) == 500);
    REQUIRE(r.fake.attr_adds.size() == 1);
    CHECK(r.fake.attr_adds[0] == std::make_tuple(500u, EMF_ATTR_HEALTH, -200));
    CHECK(r.fake.replies[1] == "true true");
    REQUIRE(r.fake.damages.size() == 2);
    CHECK(r.fake.damages[0] == std::make_tuple(1u, 500u, 250));
    CHECK(r.fake.damages[1] == std::make_tuple(0u, 500u, 5));
    CHECK(r.fake.replies[2] == "false");  // 2.5 is not a whole number
}

TEST_CASE("lua 1.5: emf.kill takes the attribute path where the core has it (never a player)") {
    Rig r(L"killattr");
    with_attributes(r.api);
    r.fake.attrs[{500, EMF_ATTR_HEALTH}] = 1498;
    r.fake.attrs[{1, EMF_ATTR_HEALTH}] = 100;
    put_health(500, {640, 0, 1498, 0, 0, 0, 0, 1498}, 1498, 1498);  // slot 0 would not do for the old path
    r.fake.components[{ecs::PlayerState::kHash, 1}] = std::vector<uint8_t>(sizeof(ecs::PlayerState), 0);
    add_script(r.dir, "ka", "emf.command('go', '', function(w) emf.reply(tostring(emf.kill(w, 500)) .. ' ' .. tostring(emf.kill(w, 1))) end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("ka.go");
    REQUIRE_FALSE(r.fake.replies.empty());
    CHECK(r.fake.replies.back() == "true false");
    CHECK((r.fake.attrs[{500, EMF_ATTR_HEALTH}]) == 0);
    CHECK((r.fake.attrs[{1, EMF_ATTR_HEALTH}]) == 100);
    CHECK(health_slot(500, 0) == 640);  // no raw write
}

// ---- lua 1.6: chat lines and saves (API v19) ----

namespace {
bool replied(const std::string& s) {
    for (const auto& x : g->replies)
        if (x.find(s) != std::string::npos) return true;
    return false;
}
}  // namespace

TEST_CASE("lua 1.6: emf.tell and emf.broadcast reach the core; world:name gives the name or nil") {
    Rig r(L"tell");
    r.fake.names[3] = "Ember";
    add_script(r.dir, "tl",
               "emf.command('go', '', function(w)\n"
               "  emf.reply('tell ' .. tostring(emf.tell(w, 3, 'hi Ember')))\n"
               "  emf.reply('all ' .. tostring(emf.broadcast('hello all')))\n"
               "  emf.reply('name ' .. tostring(w:name(3)) .. ' ' .. tostring(w:name(4)) .. ' ' .. tostring(w:name(5000)))\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("tl.go");
    CHECK(replied("tell true"));
    CHECK(replied("all true"));
    CHECK(replied("name Ember nil nil"));
    REQUIRE(r.fake.tells.size() == 1);
    CHECK(r.fake.tells[0] == std::make_pair(3u, std::string("hi Ember")));
    CHECK(r.fake.broadcasts == std::vector<std::string>{"hello all"});
    r.fake.chat_ok = false;  // the core refuses (players' chat off, a full outbox...)
    r.fake.replies.clear();
    r.run_command("tl.go");
    CHECK(replied("tell false"));
    CHECK(replied("all false"));
}

TEST_CASE("lua 1.6: a script may send 30 chat lines in 10 s (said once in the log), then a new window") {
    Rig r(L"tell_limit");
    add_script(r.dir, "tm",
               "emf.command('go', '', function(w)\n"
               "  local n = 0\n"
               "  for i = 1, 35 do if emf.tell(w, 1, 'line ' .. i) then n = n + 1 end end\n"
               "  emf.reply('sent ' .. n .. ' ' .. tostring(emf.broadcast('x')))\n"
               "end)\n"
               "emf.command('ten', '', function(w) emf.reply('ten ' .. tostring(emf.tell(w, 1, 'a\\nb\\nc\\nd\\ne\\nf\\ng\\nh\\ni\\nj\\nk\\nl'))) end)\n");
    Host host(&r.api, r.dir);
    uint64_t now = 100000;
    host.set_clock([&now] { return now; });
    host.load_all();
    r.run_command("tm.go");
    CHECK(replied("sent 30 false"));
    int said = 0;
    for (const auto& l : r.fake.log) said += l.find("more than 30 chat lines in 10 s") != std::string::npos;
    CHECK(said == 1);
    now += 10000;
    r.fake.replies.clear();
    r.run_command("tm.ten");  // 12 lines count as 11 (the core sends 10 and a "...")
    CHECK(replied("ten true"));
    r.fake.replies.clear();
    r.run_command("tm.go");
    CHECK(replied("sent 19 false"));
}

TEST_CASE("lua 1.6: chat lines the core refused use none of the script's 30 lines") {
    Rig r(L"tell_refused");
    add_script(r.dir, "tr",
               "emf.command('go', '', function(w)\n"
               "  local n = 0\n"
               "  for i = 1, 35 do if emf.tell(w, 1, 'line ' .. i) then n = n + 1 end end\n"
               "  if emf.broadcast('all') then n = n + 1 end\n"
               "  emf.reply('sent ' .. n)\n"
               "end)\n");
    Host host(&r.api, r.dir);
    uint64_t now = 100000;
    host.set_clock([&now] { return now; });
    host.load_all();
    r.fake.chat_ok = false;  // the core refuses every line (players' chat off, a full outbox...)
    r.run_command("tr.go");
    CHECK(replied("sent 0"));
    r.fake.chat_ok = true;  // the same 10 s: the script still has all 30
    r.fake.replies.clear();
    r.run_command("tr.go");
    CHECK(replied("sent 30"));
    CHECK(r.fake.tells.size() == 30u);
}

TEST_CASE("lua 1.6: emf.on_chat gets the world, the account as text, the entity and the line; one core listener") {
    Rig r(L"on_chat");
    add_script(r.dir, "oc1",
               "heard = {}\n"
               "emf.reply(tostring(emf.on_chat(function(w, account, entity, text)\n"
               "  emf.log('heard ' .. account .. ' ' .. entity .. ' ' .. text .. ' server ' .. tostring(w:is_server()))\n"
               "end)))\n");
    add_script(r.dir, "oc2", "emf.on_chat(function(w, a, e, t) emf.log('second ' .. t) end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    CHECK(r.fake.chat_cbs.size() == 1);  // the host listens once for every script
    fire_chat(kServer, 0xa1b2c3d4e5f60718ull, 3, "hello there");
    CHECK(logged("heard 0xa1b2c3d4e5f60718 3 hello there server true"));
    CHECK(logged("second hello there"));
}

TEST_CASE("lua 1.6: emf.on_chat is false where the core refuses (players' chat off), and stays so") {
    Rig r(L"on_chat_off");
    r.fake.chat_ok = false;
    add_script(r.dir, "oc",
               "local ok = emf.on_chat(function() end)\n"
               "emf.log('on_chat ' .. tostring(ok))\n");
    Host host(&r.api, r.dir);
    host.load_all();
    CHECK(logged("on_chat false"));
    CHECK(r.fake.chat_cbs.empty());
}

TEST_CASE("lua 1.6: emf.request_save asks the core; on an older core the chat functions answer false") {
    Rig r(L"save");
    add_script(r.dir, "sv",
               "emf.command('go', '', function(w)\n"
               "  emf.reply('save ' .. tostring(emf.request_save()) .. ' tell ' .. tostring(emf.tell(w, 1, 'x')) ..\n"
               "            ' name ' .. tostring(w:name(1)))\n"
               "end)\n");
    r.fake.names[1] = "Ember";
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("sv.go");
    CHECK(replied("save true tell true name Ember"));
    CHECK(r.fake.saves == 1);
    r.api.size = offsetof(EmfApi, chat_tell);  // a v18 core
    r.fake.replies.clear();
    r.run_command("sv.go");
    CHECK(replied("save false tell false name nil"));
    CHECK(r.fake.saves == 1);
}

TEST_CASE("lua 1.6: a command named exactly like the mod is the bare command; others still get the prefix") {
    Rig r(L"bare");
    add_script(r.dir, "handy",
               "emf.command('handy', ': the mod itself', function() emf.reply('bare') end, {players = true})\n"
               "emf.command('who', '', function() emf.reply('who') end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    bool bare = false, prefixed = false;
    for (const auto& c : r.fake.commands) {
        bare |= c.name == "handy" && c.flags == EMF_COMMAND_PLAYERS;
        prefixed |= c.name == "handy.who";
    }
    CHECK(bare);
    CHECK(prefixed);
    r.run_command("handy");
    CHECK(replied("bare"));
}

// ---- lua 1.6: emf.move, emf.take's slot, and nothing lost when a script cannot hear an answer ----

namespace {
void finish_take(size_t i, uint32_t done) {
    const EmfItemsRequest q = g->takes.at(i);
    q.cb(kServer, q.entity, q.item, q.count, done, q.user);
}
void finish_put(size_t i, uint32_t done) {
    const EmfItemsRequest q = g->puts.at(i);
    q.cb(kServer, q.entity, q.item, q.count, done, q.user);
}
// a player entity at (x, 0, 0)
void a_player(uint32_t id, double x) {
    g->components[{ecs::PlayerState::kHash, id}] = std::vector<uint8_t>(ecs::PlayerState::kSize, 0);
    put_transform(id, x, 0, 0);
}
const char* const kMoveScript =
    "emf.command('go', '', function(w, a)\n"
    "  local n = tonumber(a[1] or '10')\n"
    "  emf.reply('queued ' .. tostring(emf.move(w, 1, 2, 'Wood', n, {slot = 0x200000065, exact = true, fallback = 9},\n"
    "    function(w2, moved, returned, dropped) emf.log('moved ' .. moved .. ' ' .. returned .. ' ' .. dropped) end)))\n"
    "end)\n"
    "emf.command('boom', '', function() error('boom') end)\n";
}  // namespace

TEST_CASE("lua 1.6 emf.move: takes exactly that slot, puts into the receiver, gives back the rest, drops what still does not fit") {
    Rig r(L"move");
    a_player(1, 0);
    a_player(2, 3);
    add_script(r.dir, "mv", kMoveScript);
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("mv.go");
    CHECK(replied("queued true"));
    REQUIRE(r.fake.takes.size() == 1);
    CHECK(r.fake.takes[0].entity == 1u);
    CHECK(r.fake.takes[0].count == 10u);
    CHECK(r.fake.takes[0].slot == 0x200000065ull);
    CHECK((r.fake.takes[0].flags & EMF_ITEMS_EXACT_SLOT) != 0);
    finish_take(0, 10);
    REQUIRE(r.fake.puts.size() == 1);
    CHECK(r.fake.puts[0].entity == 2u);
    CHECK(r.fake.puts[0].count == 10u);
    finish_put(0, 6);  // room for 6
    REQUIRE(r.fake.puts.size() == 2);
    CHECK(r.fake.puts[1].entity == 1u);  // the rest back to the giver
    CHECK(r.fake.puts[1].count == 4u);
    finish_put(1, 1);  // only 1 fits back
    REQUIRE(r.fake.gives.size() == 1);
    CHECK(r.fake.gives[0].player == 1u);  // dropped at the giver's feet
    CHECK(r.fake.gives[0].count == 3u);
    CHECK(logged("moved 6 1 3"));
    CHECK_FALSE(logged("LOST"));
}

TEST_CASE("lua 1.6 emf.move: a giver who is gone: dropped at the receiver, the fallback, the nearest player; LOST only when none can") {
    Rig r(L"move_drop");
    a_player(1, 0);
    a_player(2, 3);
    a_player(5, 40);
    add_script(r.dir, "mv", kMoveScript);
    Host host(&r.api, r.dir);
    host.load_all();
    r.fake.give_fails = {1};
    r.run_command("mv.go");
    finish_take(0, 10);
    finish_put(0, 0);
    finish_put(1, 0);
    REQUIRE(r.fake.gives.size() == 1);
    CHECK(r.fake.gives[0].player == 2u);  // the receiver
    r.fake.give_fails = {1, 2, 9};
    r.run_command("mv.go");
    finish_take(1, 10);
    finish_put(2, 0);
    finish_put(3, 0);
    REQUIRE(r.fake.gives.size() == 2);
    CHECK(r.fake.gives[1].player == 5u);  // the nearest player still there
    CHECK_FALSE(logged("LOST"));
    r.fake.give_fails = {1, 2, 5, 9};
    r.run_command("mv.go");
    finish_take(2, 10);
    finish_put(4, 0);
    finish_put(5, 0);
    CHECK(logged("LOST 10 of item"));
}

TEST_CASE("lua 1.6 emf.move: an answer after the session changed touches nothing (its ids name other entities now)") {
    Rig r(L"move_session");
    a_player(1, 0);
    a_player(2, 3);
    add_script(r.dir, "mv", kMoveScript);
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("mv.go");
    finish_take(0, 10);
    ++r.fake.session;  // a reload: the waiting put is answered with 0 in the new world
    finish_put(0, 0);
    CHECK(r.fake.puts.size() == 1);  // no give back into entity 1 of the new world
    CHECK(r.fake.gives.empty());
    CHECK(logged("the world changed"));
}

TEST_CASE("lua 1.6 emf.move goes on while its script is paused or reloaded: nothing is lost, only the callback is skipped") {
    Rig r(L"move_paused");
    a_player(1, 0);
    a_player(2, 3);
    add_script(r.dir, "mv", kMoveScript);
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("mv.go");
    r.run_command("mv.boom");  // the script errors: it is paused
    REQUIRE(host.status("mv").rfind("paused", 0) == 0);
    finish_take(0, 10);
    REQUIRE(r.fake.puts.size() == 1);
    finish_put(0, 4);
    REQUIRE(r.fake.puts.size() == 2);
    CHECK(r.fake.puts[1].count == 6u);
    finish_put(1, 6);
    CHECK(r.fake.gives.empty());
    CHECK_FALSE(logged("moved "));
}

TEST_CASE("lua 1.6 emf.move: at most 24 moves wait at once (the core's item queue keeps room for other mods)") {
    Rig r(L"move_room");
    a_player(1, 0);
    a_player(2, 3);
    add_script(r.dir, "mv", kMoveScript);
    Host host(&r.api, r.dir);
    host.load_all();
    for (int i = 0; i < 24; ++i) r.run_command("mv.go");
    r.fake.replies.clear();
    r.run_command("mv.go");
    CHECK(replied("queued false"));
    CHECK(r.fake.takes.size() == 24u);
    finish_take(0, 0);  // one ends (nothing taken): room again
    r.fake.replies.clear();
    r.run_command("mv.go");
    CHECK(replied("queued true"));
}

TEST_CASE("lua 1.6 emf.take takes {slot = , exact = } (exactly that slot, never another inventory such as worn equipment)") {
    Rig r(L"take_slot");
    add_script(r.dir, "tk",
               "emf.command('go', '', function(w)\n"
               "  emf.take(w, 1, 'Wood', 3, {slot = 0x100000065, exact = true}, function() end)\n"
               "  emf.take(w, 1, 'Wood', 2)\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("tk.go");
    REQUIRE(r.fake.takes.size() == 2);
    CHECK(r.fake.takes[0].slot == 0x100000065ull);
    CHECK(r.fake.takes[0].flags == EMF_ITEMS_EXACT_SLOT);
    CHECK(r.fake.takes[1].slot == 0u);
    CHECK(r.fake.takes[1].flags == 0u);
}

TEST_CASE("lua 1.6: a put whose answer its script cannot hear drops what did not fit at the put's target (same session only)") {
    Rig r(L"put_unheard");
    a_player(2, 3);
    const char* code = "emf.command('go', '', function(w) emf.put(w, 2, 'Wood', 10, function() end) end)\n"
                       "emf.command('boom', '', function() error('boom') end)\n";
    add_script(r.dir, "pu", code);
    add_script(r.dir, "pv", code);
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("pu.go");
    r.run_command("pu.boom");
    finish_put(0, 7);
    REQUIRE(r.fake.gives.size() == 1);
    CHECK(r.fake.gives[0].player == 2u);
    CHECK(r.fake.gives[0].count == 3u);
    CHECK(logged("dropped"));
    // in another session the id names another entity: nothing is dropped there
    r.run_command("pv.go");
    r.run_command("pv.boom");
    ++r.fake.session;
    finish_put(1, 0);
    CHECK(r.fake.gives.size() == 1u);
}

TEST_CASE("lua 1.6: a take callback that raises puts back into the source what it did not hand on") {
    Rig r(L"take_raise");
    a_player(1, 0);
    a_player(2, 3);
    add_script(r.dir, "ta",
               "emf.command('go', '', function(w) emf.take(w, 1, 'Wood', 10, function(w2, n) error('before the put') end) end)\n");
    add_script(r.dir, "tb",
               "emf.command('go', '', function(w) emf.take(w, 1, 'Wood', 10, function(w2, n)\n"
               "  emf.put(w2, 2, 'Wood', 4)\n"
               "  error('after one put')\n"
               "end) end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("ta.go");
    finish_take(0, 10);
    REQUIRE(r.fake.puts.size() == 1);
    CHECK(r.fake.puts[0].entity == 1u);  // all 10 back into the source
    CHECK(r.fake.puts[0].count == 10u);
    r.run_command("tb.go");
    finish_take(1, 10);
    REQUIRE(r.fake.puts.size() == 3);
    CHECK(r.fake.puts[1].entity == 2u);  // the script's own put
    CHECK(r.fake.puts[1].count == 4u);
    CHECK(r.fake.puts[2].entity == 1u);  // the 6 it did not hand on go back
    CHECK(r.fake.puts[2].count == 6u);
}
