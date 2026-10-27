// Lua host 1.8: world:template(component, id) reads an entity's template half (read-only), through the fake API.
#include <windows.h>

#include <doctest.h>

#include <string>
#include <vector>

#include "emf/generated/components.hpp"
#include "lua_host/host.h"
#include "lua_test_fake.h"

using emf::lua::Host;
namespace ecs = emf::gen::ecs;

namespace {
// ModelResource of the game's wolf (char_enemy_wolf_01): 91e2d4cc-01c9-4203-a92e-b08769364522, as the game stores it
std::vector<uint8_t> wolf_model() {
    return {0xcc, 0xd4, 0xe2, 0x91, 0xc9, 0x01, 0x03, 0x42, 0xa9, 0x2e, 0xb0, 0x87, 0x69, 0x36, 0x45, 0x22};
}
bool said(const Fake& f, const std::string& s) {
    for (const auto& x : f.replies)
        if (x.find(s) != std::string::npos) return true;
    return false;
}
}  // namespace

TEST_CASE("lua 1.8: world:template gives a creature's ModelResource.model as text") {
    Rig r(L"tpl_read");
    r.fake.templates[{ecs::ModelResource::kHash, 50}] = wolf_model();
    add_script(r.dir, "tp",
               "emf.command('model', '', function(w)\n"
               "  emf.reply('model ' .. w:template('ModelResource', 50).model)\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("tp.model");
    CHECK(said(r.fake, "model 91e2d4cc-01c9-4203-a92e-b08769364522"));
}

TEST_CASE("lua 1.8: template data cannot be written") {
    Rig r(L"tpl_write");
    r.fake.templates[{ecs::ModelResource::kHash, 50}] = wolf_model();
    add_script(r.dir, "tp",
               "emf.command('write', '', function(w)\n"
               "  local t = w:template('ModelResource', 50)\n"
               "  local ok, err = pcall(function() t.model = 'x' end)\n"
               "  emf.reply('write ' .. tostring(ok) .. ' ' .. tostring(err))\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("tp.write");
    CHECK(said(r.fake, "write false"));
    CHECK(said(r.fake, "read-only"));
    CHECK(r.fake.templates[{ecs::ModelResource::kHash, 50}] == wolf_model());  // untouched
}

TEST_CASE("lua 1.8: no template half gives nil") {
    Rig r(L"tpl_none");
    add_script(r.dir, "tp",
               "emf.command('none', '', function(w)\n"
               "  emf.reply('none ' .. tostring(w:template('ModelResource', 51)))\n"
               "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("tp.none");
    CHECK(said(r.fake, "none nil"));
}
