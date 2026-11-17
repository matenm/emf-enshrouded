// Lua 1.37 breaking blocks (API v44): emf.break_blocks(world, x, y, z [, opts], fn), emf.block_at(world, x, y, z) and
// emf.find_wall(world, from, to [, opts]). A break's answer comes once, after a server step, to fn(world, result); a
// script paused or reloaded in between hears nothing. Lua 1.40 (API v47): terrain a player placed (break_blocks'
// terrain option, find_wall's {terrain = true}, a terrain block's world).
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "host_impl.h"

namespace emf::lua {

namespace {

bool has_v44(const EmfApi* a) {
    return a && a->version >= 44 && a->size >= offsetof(EmfApi, find_wall) + sizeof(a->find_wall) && a->break_blocks &&
           a->block_at && a->find_wall;
}
bool has_v47(const EmfApi* a) {
    return has_v44(a) && a->version >= 47 && a->size >= offsetof(EmfApi, find_wall_ex) + sizeof(a->find_wall_ex) &&
           a->find_wall_ex;
}

// {x, y, z} or {x = .., y = .., z = ..}
void read_pos(lua_State* L, int index, float out[3], const char* fn, const char* what) {
    luaL_checktype(L, index, LUA_TTABLE);
    static const char* const kKeys[3] = {"x", "y", "z"};
    for (int i = 0; i < 3; ++i) {
        lua_rawgeti(L, index, i + 1);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_getfield(L, index, kKeys[i]);
        }
        if (lua_type(L, -1) != LUA_TNUMBER) luaL_error(L, "%s: %s needs three numbers ({x, y, z})", fn, what);
        out[i] = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
}

int32_t whole_damage(lua_State* L, int index) {
    if (lua_type(L, index) != LUA_TNUMBER) luaL_error(L, "emf.break_blocks: damage must be a number or a list of five");
    const double v = lua_tonumber(L, index);
    if (!(v >= 0 && v <= 100000) || std::floor(v) != v)
        luaL_error(L, "emf.break_blocks: damage must be whole numbers from 0 to 100000");
    return static_cast<int32_t>(v);
}

// The options table at `t` into r (and the material list into mats, which r points at).
void read_options(lua_State* L, int t, EmfBreakBlocks& r, std::vector<uint8_t>& mats) {
    lua_pushnil(L);
    while (lua_next(L, t)) {
        if (lua_type(L, -2) != LUA_TSTRING) luaL_error(L, "emf.break_blocks: options are named (radius, height, ...)");
        const std::string key = lua_tostring(L, -2);
        const int v = lua_gettop(L);
        if (key == "radius" || key == "height") {
            if (lua_type(L, v) != LUA_TNUMBER) luaL_error(L, "emf.break_blocks: %s must be a number", key.c_str());
            (key == "radius" ? r.radius : r.height) = static_cast<float>(lua_tonumber(L, v));
        } else if (key == "mode") {
            const char* m = lua_type(L, v) == LUA_TSTRING ? lua_tostring(L, v) : "";
            if (std::strcmp(m, "break") == 0)
                r.flags |= EMF_BREAK_INSTANT;
            else if (std::strcmp(m, "damage") != 0)
                luaL_error(L, "emf.break_blocks: mode must be 'damage' or 'break'");
        } else if (key == "world") {
            if (!lua_isboolean(L, v)) luaL_error(L, "emf.break_blocks: world must be true or false");
            if (lua_toboolean(L, v)) r.flags |= EMF_BREAK_WORLD;
        } else if (key == "drops") {
            if (!lua_isboolean(L, v)) luaL_error(L, "emf.break_blocks: drops must be true or false");
            if (lua_toboolean(L, v)) r.flags |= EMF_BREAK_DROPS;
        } else if (key == "terrain") {  // 1.40 (v47)
            if (!lua_isboolean(L, v)) luaL_error(L, "emf.break_blocks: terrain must be true or false");
            if (lua_toboolean(L, v)) r.flags |= EMF_BREAK_TERRAIN;
        } else if (key == "damage") {
            if (lua_istable(L, v)) {
                if (lua_rawlen(L, v) != 5) luaL_error(L, "emf.break_blocks: damage must be a number or a list of five");
                for (int i = 0; i < 5; ++i) {
                    lua_rawgeti(L, v, i + 1);
                    r.damage[i] = whole_damage(L, lua_gettop(L));
                    lua_pop(L, 1);
                }
            } else {
                const int32_t d = whole_damage(L, v);
                for (int i = 0; i < 5; ++i) r.damage[i] = d;
            }
        } else if (key == "materials") {
            if (!lua_istable(L, v)) luaL_error(L, "emf.break_blocks: materials must be building material ids 0 to 127");
            const size_t n = lua_rawlen(L, v);
            if (n == 0)  // {} or a table with names: every material would be broken, surely not what was meant
                luaL_error(L, "emf.break_blocks: materials must list at least one building material id (leave it out for every material)");
            if (n > 128) luaL_error(L, "emf.break_blocks: at most 128 materials");
            for (size_t i = 1; i <= n; ++i) {
                lua_rawgeti(L, v, static_cast<lua_Integer>(i));
                const double m = lua_type(L, -1) == LUA_TNUMBER ? lua_tonumber(L, -1) : -1;
                if (!(m >= 0 && m <= 127) || std::floor(m) != m)
                    luaL_error(L, "emf.break_blocks: materials must be building material ids 0 to 127");
                mats.push_back(static_cast<uint8_t>(m));
                lua_pop(L, 1);
            }
        } else {
            luaL_error(L, "emf.break_blocks: unknown option '%s' (radius, height, mode, damage, drops, materials, world, terrain)",
                       key.c_str());
        }
        lua_pop(L, 1);
    }
    r.material_count = static_cast<uint32_t>(mats.size());
    r.materials = mats.empty() ? nullptr : mats.data();
}

const char* status_name(uint32_t s) {
    switch (s) {
        case EMF_BREAK_DONE: return "done";
        case EMF_BREAK_CANCELLED: return "cancelled";
        default: return "failed";
    }
}

struct BreakSlot {
    Host::Impl* host;
    std::string id;
    int ref;
    uint64_t load;  // the script's load counter when asked (a reload drops the answer)
};

void break_cb_body(EmfWorld* w, uint32_t, const EmfBreakResult* r, void* user) {
    const std::unique_ptr<BreakSlot> slot(static_cast<BreakSlot*>(user));
    Script* s = slot->host->find(slot->id);
    if (!s || !r) return;
    std::lock_guard<std::recursive_mutex> l(s->m);
    if (!s->L || s->paused || slot->load != s->loads) return;
    lua_State* L = s->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, slot->ref);
    push_world(L, *s, w);
    lua_createtable(L, 0, 4);
    lua_pushstring(L, status_name(r->status));
    lua_setfield(L, -2, "status");
    lua_pushinteger(L, r->damaged);
    lua_setfield(L, -2, "damaged");
    lua_pushinteger(L, r->removed);
    lua_setfield(L, -2, "removed");
    lua_pushinteger(L, r->dropped);
    lua_setfield(L, -2, "dropped");
    call_lua(*s, 2);
    ++s->generation;
    if (s->L) luaL_unref(s->L, LUA_REGISTRYINDEX, slot->ref);
}

void break_cb(EmfWorld* w, uint32_t id, const EmfBreakResult* r, void* user) {
    try {
        break_cb_body(w, id, r, user);
    } catch (...) {
    }
}

// version: the core's (a v44 one never writes world; one older than v47 writes 0 for every terrain cell)
void push_block(lua_State* L, const EmfBlockInfo& b, uint32_t version) {
    lua_createtable(L, 0, 10);
    lua_pushstring(L, b.kind == EMF_BLOCK_BUILDING ? "building" : b.kind == EMF_BLOCK_TERRAIN ? "terrain" : "empty");
    lua_setfield(L, -2, "kind");
    lua_pushinteger(L, b.material);
    lua_setfield(L, -2, "material");
    lua_pushinteger(L, b.fill);
    lua_setfield(L, -2, "fill");
    lua_pushinteger(L, b.hardness);
    lua_setfield(L, -2, "hardness");
    lua_pushinteger(L, b.health);
    lua_setfield(L, -2, "health");
    if (b.damage >= 0) {
        lua_pushinteger(L, b.damage);
        lua_setfield(L, -2, "damage");
    }
    const bool world_known = (b.kind == EMF_BLOCK_BUILDING && version >= 45) || (b.kind == EMF_BLOCK_TERRAIN && version >= 47);
    if (world_known && b.world >= 0) {  // 1.38 (v45), terrain 1.40 (v47): nil when not known
        lua_pushboolean(L, b.world == 1);
        lua_setfield(L, -2, "world");
    }
    lua_pushnumber(L, b.x);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, b.y);
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, b.z);
    lua_setfield(L, -2, "z");
}

}  // namespace

// emf.break_blocks(world, x, y, z [, {radius, height, mode, damage, drops, materials}], fn): the break's id, or nil
// when refused (the reason is in the log).
int l_break_blocks(lua_State* L) {
    Script* s = script_of(L);
    EmfWorld* w = lua_world_arg(L, 1);
    EmfBreakBlocks r{};
    r.size = sizeof r;
    r.x = static_cast<float>(luaL_checknumber(L, 2));
    r.y = static_cast<float>(luaL_checknumber(L, 3));
    r.z = static_cast<float>(luaL_checknumber(L, 4));
    std::vector<uint8_t> mats;
    int fn_index = 5;
    if (lua_istable(L, 5)) {
        fn_index = 6;
        read_options(L, 5, r, mats);
    }
    luaL_checktype(L, fn_index, LUA_TFUNCTION);
    const EmfApi* a = s->host->api;
    if (a->version < 45) r.flags &= ~EMF_BREAK_WORLD;  // a v44 core breaks ruins like any block (and knows no flag)
    if (!has_v47(a)) r.flags &= ~EMF_BREAK_TERRAIN;    // an older core breaks building blocks only (and knows no flag)
    if (!has_v44(a)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushvalue(L, fn_index);
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    auto* slot = new BreakSlot{s->host, s->id, ref, s->loads};
    const uint32_t id = a->break_blocks(w, &r, break_cb, slot);
    if (!id) {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        delete slot;
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, id);
    return 1;
}

// emf.block_at(world, x, y, z): {kind, material, fill, hardness, health, damage, x, y, z, world (1.38)}, or nil.
int l_block_at(lua_State* L) {
    Script* s = script_of(L);
    EmfWorld* w = lua_world_arg(L, 1);
    const auto x = static_cast<float>(luaL_checknumber(L, 2)), y = static_cast<float>(luaL_checknumber(L, 3)),
               z = static_cast<float>(luaL_checknumber(L, 4));
    const EmfApi* a = s->host->api;
    EmfBlockInfo b{};
    b.size = sizeof b;
    if (!has_v44(a) || !a->block_at(w, x, y, z, &b)) {
        lua_pushnil(L);
        return 1;
    }
    push_block(L, b, a->version);
    return 1;
}

// emf.find_wall(world, from, to [, {terrain = true}]): the first building block on the line (the same table as
// block_at), or nil. 1.40 (v47): terrain = true also stops at terrain a player placed (an older core: building blocks).
int l_find_wall(lua_State* L) {
    Script* s = script_of(L);
    EmfWorld* w = lua_world_arg(L, 1);
    float from[3], to[3];
    read_pos(L, 2, from, "emf.find_wall", "from");
    read_pos(L, 3, to, "emf.find_wall", "to");
    bool terrain = false;
    if (!lua_isnoneornil(L, 4)) {
        luaL_checktype(L, 4, LUA_TTABLE);
        lua_pushnil(L);
        while (lua_next(L, 4)) {
            if (lua_type(L, -2) != LUA_TSTRING) luaL_error(L, "emf.find_wall: options are named (terrain)");
            const std::string key = lua_tostring(L, -2);
            if (key != "terrain") luaL_error(L, "emf.find_wall: unknown option '%s' (terrain)", key.c_str());
            if (!lua_isboolean(L, -1)) luaL_error(L, "emf.find_wall: terrain must be true or false");
            terrain = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
        }
    }
    const EmfApi* a = s->host->api;
    EmfBlockInfo b{};
    b.size = sizeof b;
    const bool ok = terrain && has_v47(a) ? a->find_wall_ex(w, from, to, EMF_FIND_TERRAIN, &b)
                                          : has_v44(a) && a->find_wall(w, from, to, &b);
    if (!ok) {
        lua_pushnil(L);
        return 1;
    }
    push_block(L, b, a->version);
    return 1;
}

}  // namespace emf::lua
