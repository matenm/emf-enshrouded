// Lua 1.30 doors (API v38): emf.door, emf.door_open, emf.door_close, emf.door_lock. Changes land at the next server
// step; a lock is kept with the world (from EMF 0.6.8). Lua 1.32 enemy orders (API v39): emf.enemy_order, emf.enemy_order_cancel,
// emf.enemy_target.
#include <cstring>
#include <string>

#include "host_impl.h"

namespace emf::lua {

namespace {
bool has_v38(const EmfApi* a) {
    return a && a->version >= 38 && a->size >= offsetof(EmfApi, door_lock) + sizeof(a->door_lock) && a->door_state;
}
}  // namespace

// emf.door(world, id): "open" or "closed" plus {locked = bool, logic = bool}, or nil (not a door).
int l_door(lua_State* L) {
    Script* s = script_of(L);
    EmfWorld* w = lua_world_arg(L, 1);
    const uint32_t id = static_cast<uint32_t>(luaL_checkinteger(L, 2));
    const EmfApi* a = s->host->api;
    const uint32_t st = has_v38(a) ? a->door_state(w, id) : EMF_DOOR_NONE;
    if (!(st & (EMF_DOOR_OPEN | EMF_DOOR_CLOSED))) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, (st & EMF_DOOR_OPEN) ? "open" : "closed");
    lua_createtable(L, 0, 2);
    lua_pushboolean(L, (st & EMF_DOOR_LOCKED) != 0);
    lua_setfield(L, -2, "locked");
    lua_pushboolean(L, (st & EMF_DOOR_LOGIC) != 0);
    lua_setfield(L, -2, "logic");
    return 2;
}

// emf.door_open(world, id [, "front" | "back"]) / emf.door_close(world, id): true when it will change at the next step.
int door_set(lua_State* L, bool open) {
    Script* s = script_of(L);
    EmfWorld* w = lua_world_arg(L, 1);
    const uint32_t id = static_cast<uint32_t>(luaL_checkinteger(L, 2));
    uint32_t side = 0;
    if (!open && !lua_isnoneornil(L, 3))  // a side given to a close would be dropped without a word
        return luaL_error(L, "emf.door_close(world, id) takes no side: a side is for emf.door_open");
    if (open && !lua_isnoneornil(L, 3)) {
        const std::string word = luaL_checkstring(L, 3);
        if (word == "front") side = 1;
        else if (word == "back") side = 2;
        else return luaL_error(L, "emf.door_open: the side is 'front' or 'back'");
    }
    const EmfApi* a = s->host->api;
    lua_pushboolean(L, has_v38(a) && a->door_set(w, id, open ? 1u : 0u, side));
    return 1;
}
int l_door_open(lua_State* L) { return door_set(L, true); }
int l_door_close(lua_State* L) { return door_set(L, false); }

// emf.door_lock(world, id, locked): true when it will be (un)locked at the next step.
int l_door_lock(lua_State* L) {
    Script* s = script_of(L);
    EmfWorld* w = lua_world_arg(L, 1);
    const uint32_t id = static_cast<uint32_t>(luaL_checkinteger(L, 2));
    luaL_checktype(L, 3, LUA_TBOOLEAN);
    const bool locked = lua_toboolean(L, 3) != 0;
    const EmfApi* a = s->host->api;
    lua_pushboolean(L, has_v38(a) && a->door_lock(w, id, locked ? 1u : 0u));
    return 1;
}

namespace {
bool has_v39(const EmfApi* a) {
    return a && a->version >= 39 && a->size >= offsetof(EmfApi, enemy_target) + sizeof(a->enemy_target) && a->enemy_order;
}
}  // namespace

// emf.enemy_order(world, enemy, target [, {seconds = n, damage = n, allies = bool}]): the order's id, or nil and why.
int l_enemy_order(lua_State* L) {
    Script* s = script_of(L);
    EmfWorld* w = lua_world_arg(L, 1);
    EmfEnemyOrder o{};
    o.size = sizeof o;
    o.enemy = static_cast<uint32_t>(luaL_checkinteger(L, 2));
    o.target = static_cast<uint32_t>(luaL_checkinteger(L, 3));
    if (!lua_isnoneornil(L, 4)) {  // read strictly: a typo errors at once, not an order that never ends
        luaL_checktype(L, 4, LUA_TTABLE);
        lua_pushnil(L);
        while (lua_next(L, 4)) {
            const char* k = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : "";
            if (!std::strcmp(k, "seconds")) {
                if (lua_type(L, -1) != LUA_TNUMBER) return luaL_error(L, "emf.enemy_order: seconds is a number");
                o.seconds = static_cast<float>(lua_tonumber(L, -1));
            } else if (!std::strcmp(k, "damage")) {
                if (!lua_isinteger(L, -1)) return luaL_error(L, "emf.enemy_order: damage is a whole number");
                const lua_Integer d = lua_tointeger(L, -1);
                if (d < 0 || d > 100000) return luaL_error(L, "emf.enemy_order: damage must be 0 to 100000");
                o.damage = static_cast<int32_t>(d);
            } else if (!std::strcmp(k, "allies")) {
                if (!lua_isboolean(L, -1)) return luaL_error(L, "emf.enemy_order: allies is true or false");
                if (lua_toboolean(L, -1)) o.flags |= EMF_ORDER_ALLIES;
            } else {
                return luaL_error(L, "emf.enemy_order: unknown option '%s' (seconds, damage, allies)", k);
            }
            lua_pop(L, 1);
        }
    }
    const EmfApi* a = s->host->api;
    if (!has_v39(a)) {
        lua_pushnil(L);
        lua_pushstring(L, "this EMF has no enemy orders (EMF API v39)");
        return 2;
    }
    const uint32_t id = a->enemy_order(w, &o);
    if (!id) {
        lua_pushnil(L);
        lua_pushstring(L, o.why);
        return 2;
    }
    s->enemy_orders.insert(id);
    lua_pushinteger(L, id);
    return 1;
}

// emf.enemy_order_cancel(id): true when the order was there.
int l_enemy_order_cancel(lua_State* L) {
    Script* s = script_of(L);
    const uint32_t id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    s->enemy_orders.erase(id);
    const EmfApi* a = s->host->api;
    lua_pushboolean(L, has_v39(a) && a->enemy_order_cancel(id));
    return 1;
}

// A script that reloads or pauses: its orders end (the core ends them at the next step).
void drop_enemy_orders(Host::Impl& h, Script& s) {
    for (const uint32_t id : s.enemy_orders)
        if (has_v39(h.api)) h.api->enemy_order_cancel(id);
    s.enemy_orders.clear();
}

// emf.enemy_target(world, enemy): what it goes for now (its pick), or nil.
int l_enemy_target(lua_State* L) {
    Script* s = script_of(L);
    EmfWorld* w = lua_world_arg(L, 1);
    const uint32_t enemy = static_cast<uint32_t>(luaL_checkinteger(L, 2));
    const EmfApi* a = s->host->api;
    const uint32_t t = has_v39(a) ? a->enemy_target(w, enemy) : 0;
    if (t) lua_pushinteger(L, t);
    else lua_pushnil(L);
    return 1;
}

}  // namespace emf::lua
