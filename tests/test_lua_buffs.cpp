// Lua 1.24 buffs: emf.buff_types, emf.buff_type, emf.buffs, emf.apply_buff, emf.remove_buff (their answer after the
// step) and emf.on_buff, over the fake core API.
#include <windows.h>

#include <doctest.h>

#include <string>

#include "lua_host/host.h"
#include "lua_test_fake.h"

using emf::lua::Host;

namespace {

constexpr uint32_t kWet = 0xAC13C6C3u;
constexpr uint32_t kElixir = 0xFF24634Fu;

void types(Rig& r) {
    EmfBuffType wet{};
    wet.size = sizeof wet;
    wet.id = kWet;
    wet.slot = EMF_BUFF_DEBUFF;
    wet.apply_type = 1;
    wet.default_ns = 900'000'000'000LL;
    wet.flags = EMF_BUFF_SAVED;
    EmfBuffType elixir = wet;
    elixir.id = kElixir;
    elixir.slot = EMF_BUFF_BUFF;
    elixir.apply_type = 3;
    elixir.default_ns = 1'800'000'000'000LL;
    elixir.flags = EMF_BUFF_HIDDEN;
    elixir.follow_up = kWet;
    r.fake.buff_types = {wet, elixir};
    r.fake.buff_names = {{kWet, "Wet"}};
}

void answer(const Rig& r, size_t i, uint32_t kind, const char* why = nullptr, uint32_t buff_entity = 0) {
    const auto& a = r.fake.buff_asks[i];
    EmfBuffEvent e{};
    e.size = sizeof e;
    e.kind = kind;
    e.target = a.target;
    e.type = a.type;
    e.buff_entity = buff_entity;
    e.total_ns = e.remaining_ns = -1;
    e.why = why;
    a.done(a.world, &e, a.user);
}

}  // namespace

TEST_CASE("lua 1.24: emf.buff_types and emf.buff_type: id, slot, name, flags, seconds, apply, follow_up") {
    Rig r(L"buff_types");
    types(r);
    add_script(r.dir, "bt",
               "emf.command('bt', '', function(w)\n"
               "  local all = emf.buff_types(w)\n"
               "  local t = all[1]\n"
               "  emf.log('n ' .. #all .. ' ' .. string.format('%X', t.id) .. ' ' .. t.slot .. ' ' .. t.name .. ' ' ..\n"
               "          t.seconds .. ' ' .. t.apply .. ' saved ' .. tostring(t.saved) .. ' hidden ' .. tostring(t.hidden))\n"
               "  local e = emf.buff_type(w, " + std::to_string(kElixir) + ")\n"
               "  emf.log('e ' .. e.slot .. ' ' .. tostring(e.name) .. ' ' .. e.apply .. ' ' .. string.format('%X', e.follow_up))\n"
               "  emf.log('debuffs ' .. #emf.buff_types(w, 'debuff') .. ' none ' .. tostring(emf.buff_type(w, 5)))\n"
               "  local ok, err = pcall(emf.buff_types, w, 'potion')\n"
               "  emf.log('bad ' .. tostring(ok))\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("bt");
    CHECK(logged("n 2 AC13C6C3 debuff Wet 900 reset saved true hidden false"));
    CHECK(logged("e buff nil replace AC13C6C3"));
    CHECK(logged("debuffs 1 none nil"));
    CHECK(logged("bad false"));
}

TEST_CASE("lua 1.24: emf.buffs lists an entity's buffs; time left nil when not known") {
    Rig r(L"buffs_of");
    types(r);
    EmfBuff b{};
    b.size = sizeof b;
    b.type = kWet;
    b.entity = 900;
    b.slot = EMF_BUFF_DEBUFF;
    b.total_ns = 900'000'000'000LL;
    b.remaining_ns = 61'500'000'000LL;
    b.level = 2;
    EmfBuff c = b;
    c.type = kElixir;
    c.slot = EMF_BUFF_BUFF;
    c.total_ns = c.remaining_ns = -1;
    c.cleansed = 1;
    r.fake.buffs[1] = {b, c};
    add_script(r.dir, "bo",
               "emf.command('bo', '', function(w)\n"
               "  local l = emf.buffs(w, 1)\n"
               "  emf.log('b ' .. #l .. ' ' .. l[1].name .. ' ' .. l[1].slot .. ' ' .. l[1].entity .. ' ' .. l[1].level ..\n"
               "          ' ' .. l[1].seconds .. ' ' .. l[1].seconds_left .. ' ' .. tostring(l[1].cleansed))\n"
               "  emf.log('c ' .. tostring(l[2].name) .. ' ' .. tostring(l[2].seconds_left) .. ' ' .. tostring(l[2].cleansed))\n"
               "  emf.log('none ' .. #emf.buffs(w, 77))\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("bo");
    CHECK(logged("b 2 Wet debuff 900 2 900.0 61.5 false"));
    CHECK(logged("c nil nil true"));
    CHECK(logged("none 0"));
}

TEST_CASE("lua 1.24: emf.apply_buff and emf.remove_buff queue through the core; fn gets the answer after the step") {
    Rig r(L"apply_buff");
    types(r);
    add_script(r.dir, "ab",
               "emf.command('ab', '', function(w)\n"
               "  emf.log('q ' .. tostring(emf.apply_buff(w, 1, " + std::to_string(kWet) + ", {seconds = 30, source = 5, item = 7},\n"
               "    function(w2, res) emf.log('got ' .. res.kind .. ' ' .. res.target .. ' ' .. string.format('%X', res.type) ..\n"
               "      ' ' .. tostring(res.buff_entity) .. ' ' .. tostring(res.why)) end)))\n"
               "  emf.remove_buff(w, 1, " + std::to_string(kWet) + ", function(w2, res) emf.log('rm ' .. res.kind .. ' ' .. res.why) end)\n"
               "  emf.apply_buff(w, 2, " + std::to_string(kElixir) + ")\n"
               "  local ok, err = pcall(emf.apply_buff, w, 1, 5, {minutes = 3})\n"
               "  emf.log('bad ' .. tostring(ok))\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("ab");
    CHECK(logged("q true"));
    CHECK(logged("bad false"));
    REQUIRE(r.fake.buff_asks.size() == 3);
    CHECK_FALSE(r.fake.buff_asks[0].remove);
    CHECK(r.fake.buff_asks[0].ns == 30'000'000'000LL);
    CHECK(r.fake.buff_asks[0].source == 5);
    CHECK(r.fake.buff_asks[0].item == 7);
    CHECK(r.fake.buff_asks[1].remove);
    CHECK(r.fake.buff_asks[2].done == nullptr);  // no fn: nothing to answer
    CHECK(r.fake.buff_asks[2].ns == 0);
    answer(r, 0, EMF_BUFF_APPLIED, nullptr, 6874);
    answer(r, 1, EMF_BUFF_REFUSED, "the target does not have it");
    CHECK(logged("got applied 1 AC13C6C3 6874 nil"));
    CHECK(logged("rm refused the target does not have it"));
}

TEST_CASE("lua 1.24: emf.apply_buff seconds that are no time (NaN, infinite, negative, longer than 9e9) are an error, not a wrapped duration") {
    Rig r(L"apply_buff_seconds");
    types(r);
    add_script(r.dir, "abs",
               "emf.command('abs', '', function(w)\n"
               "  for i, s in ipairs({0/0, math.huge, -math.huge, 1e10, -1}) do\n"
               "    local ok, err = pcall(emf.apply_buff, w, 1, " + std::to_string(kWet) + ", {seconds = s})\n"
               "    emf.log('sec ' .. i .. ' ' .. tostring(ok) .. ' ' .. tostring(err))\n"
               "  end\n"
               "  emf.apply_buff(w, 1, " + std::to_string(kWet) + ", {seconds = 9000000000})\n"
               "  emf.apply_buff(w, 1, " + std::to_string(kWet) + ", {seconds = 0})\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("abs");
    for (int i = 1; i <= 5; ++i)
        CHECK(logged(("sec " + std::to_string(i) +
                      " false emf.apply_buff: seconds must be 0 to 9000000000 (0: the type's own time)")
                         .c_str()));
    REQUIRE(r.fake.buff_asks.size() == 2);
    CHECK(r.fake.buff_asks[0].ns == 9'000'000'000'000'000'000LL);
    CHECK(r.fake.buff_asks[1].ns == 0);
}

TEST_CASE("lua 1.24: emf.on_buff hears the players' buff changes; one core listener") {
    Rig r(L"on_buff");
    types(r);
    add_script(r.dir, "ob1", "emf.log('on ' .. tostring(emf.on_buff(function(w, e)\n"
                             "  emf.log('ev ' .. e.kind .. ' ' .. e.target .. ' ' .. tostring(e.name) .. ' ' .. e.slot .. ' ' ..\n"
                             "          e.buff_entity .. ' ' .. tostring(e.cleansed) .. ' ' .. tostring(e.seconds_left))\n"
                             "end)))\n");
    add_script(r.dir, "ob2", "emf.on_buff(function(w, e) emf.log('two ' .. e.kind) end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    CHECK(logged("on true"));
    CHECK(r.fake.buff_cbs.size() == 1);
    EmfBuffEvent e{};
    e.size = sizeof e;
    e.kind = EMF_BUFF_REMOVED;
    e.target = 1;
    e.type = kWet;
    e.buff_entity = 6663;
    e.slot = EMF_BUFF_DEBUFF;
    e.cleansed = 1;
    e.total_ns = -1;
    e.remaining_ns = 0;
    fire_buff(kServer, e);
    CHECK(logged("ev removed 1 Wet debuff 6663 true 0.0"));
    CHECK(logged("two removed"));
    e.kind = EMF_BUFF_REFRESHED;
    e.remaining_ns = -1;
    fire_buff(kServer, e);
    CHECK(logged("ev refreshed 1 Wet debuff 6663 true nil"));
}

TEST_CASE("lua 1.24: on an older core the buff functions answer false or nil") {
    Rig r(L"buff_old");
    r.api.size = offsetof(EmfApi, buff_types);
    add_script(r.dir, "old",
               "emf.log('old ' .. tostring(emf.on_buff(function() end)) .. ' ' ..\n"
               "        tostring(emf.apply_buff(nil, 1, 2)))\n");
    Host host(&r.api, r.dir);
    host.load_all();
    CHECK(logged("old false false"));
    CHECK(r.fake.buff_cbs.empty());
}

TEST_CASE("lua 1.24: a removed event says replaced when a new buff of the type took its place (review I1)") {
    Rig r(L"buff_replaced");
    types(r);
    add_script(r.dir, "rp", "emf.on_buff(function(w, e) emf.log('rp ' .. e.kind .. ' replaced ' .. tostring(e.replaced)) end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    EmfBuffEvent e{};
    e.size = sizeof e;
    e.kind = EMF_BUFF_REMOVED;
    e.target = 1;
    e.type = kElixir;
    e.replaced = 1;
    e.total_ns = e.remaining_ns = -1;
    fire_buff(kServer, e);
    CHECK(logged("rp removed replaced true"));
}
