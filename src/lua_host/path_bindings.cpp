// Lua 1.29 paths on the game's path grid (API v37; 1.35: points[i].door, API v42; 1.41: as, result.used, API v48): emf.path_find(world, from, to [, opts], fn) and emf.path_cancel(id).
// The answer comes once, after a server step or more, to fn(world, result, points); a script paused or reloaded in
// between hears nothing.
#include <cstddef>
#include <memory>
#include <string>

#include "host_impl.h"

namespace emf::lua {

namespace {

bool has_v37(const EmfApi* a) {
    return a && a->version >= 37 && a->size >= offsetof(EmfApi, path_cancel) + sizeof(a->path_cancel) && a->path_find;
}

// {x, y, z} or {x = .., y = .., z = ..}
void read_pos(lua_State* L, int index, float out[3], const char* what) {
    luaL_checktype(L, index, LUA_TTABLE);
    static const char* const kKeys[3] = {"x", "y", "z"};
    for (int i = 0; i < 3; ++i) {
        lua_rawgeti(L, index, i + 1);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_getfield(L, index, kKeys[i]);
        }
        if (!lua_isnumber(L, -1)) luaL_error(L, "emf.path_find: %s needs three numbers ({x, y, z})", what);
        out[i] = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
}

double opt_number(lua_State* L, int t, const char* key, double def, double lo, double hi) {
    lua_getfield(L, t, key);
    double v = def;
    if (!lua_isnil(L, -1)) {
        if (!lua_isnumber(L, -1)) luaL_error(L, "emf.path_find: %s must be a number", key);
        v = lua_tonumber(L, -1);
        if (!(v >= lo && v <= hi)) luaL_error(L, "emf.path_find: %s must be %g to %g", key, lo, hi);
    }
    lua_pop(L, 1);
    return v;
}

bool opt_bool(lua_State* L, int t, const char* key) {
    lua_getfield(L, t, key);
    const bool v = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return v;
}

const char* status_name(uint32_t s) {
    switch (s) {
        case EMF_PATH_FOUND: return "found";
        case EMF_PATH_NO_PATH: return "no_path";
        case EMF_PATH_TOO_LONG: return "too_long";
        case EMF_PATH_NO_START: return "no_start";
        case EMF_PATH_NO_TARGET: return "no_target";
        case EMF_PATH_NO_MEMORY: return "no_memory";
        case EMF_PATH_CANCELLED: return "cancelled";
        default: return "other";
    }
}

struct PathSlot {
    Host::Impl* host;
    std::string id;
    int ref;
    uint64_t load;  // the script's load counter when asked (a reload drops the answer)
};

void path_cb_body(EmfWorld* w, uint32_t, const EmfPathResult* r, const EmfPathPoint* pts, void* user) {
    const std::unique_ptr<PathSlot> slot(static_cast<PathSlot*>(user));
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
    lua_pushboolean(L, r->reaches != 0);
    lua_setfield(L, -2, "reaches");
    lua_pushnumber(L, r->length);
    lua_setfield(L, -2, "length");
    lua_pushinteger(L, r->count);
    lua_setfield(L, -2, "count");
    lua_createtable(L, static_cast<int>(r->count), 0);
    const uint32_t* doors = r->size >= offsetof(EmfPathResult, doors) + sizeof(r->doors) ? r->doors : nullptr;
    for (uint32_t i = 0; pts && i < r->count; ++i) {
        lua_createtable(L, 0, 4);
        lua_pushnumber(L, pts[i].x);
        lua_setfield(L, -2, "x");
        lua_pushnumber(L, pts[i].y);
        lua_setfield(L, -2, "y");
        lua_pushnumber(L, pts[i].z);
        lua_setfield(L, -2, "z");
        lua_pushinteger(L, pts[i].move);
        lua_setfield(L, -2, "move");
        if (doors && doors[i]) {  // 1.35 (API v42): the door entity of a door point
            lua_pushinteger(L, doors[i]);
            lua_setfield(L, -2, "door");
        }
        lua_rawseti(L, -2, static_cast<lua_Integer>(i) + 1);
    }
    if (r->size >= offsetof(EmfPathResult, height) + sizeof(r->height)) {  // 1.41 (API v48): what it planned with
        lua_pushvalue(L, -2);                                               // the result table
        lua_createtable(L, 0, 8);
        lua_pushinteger(L, r->moves);
        lua_setfield(L, -2, "moves");
        lua_pushboolean(L, (r->moves & (EMF_PATH_JUMP_UP | EMF_PATH_JUMP_DOWN | EMF_PATH_JUMP_ACROSS)) != 0);
        lua_setfield(L, -2, "jumps");
        lua_pushboolean(L, (r->moves & EMF_PATH_OPEN_DOOR) != 0);
        lua_setfield(L, -2, "doors");
        lua_pushboolean(L, (r->moves & EMF_PATH_SWIM) != 0);
        lua_setfield(L, -2, "swim");
        lua_pushinteger(L, r->jump_up);
        lua_setfield(L, -2, "max_jump_up");
        lua_pushinteger(L, r->jump_down);
        lua_setfield(L, -2, "max_jump_down");
        lua_pushnumber(L, r->width);
        lua_setfield(L, -2, "width");
        lua_pushnumber(L, r->height);
        lua_setfield(L, -2, "height");
        lua_setfield(L, -2, "used");
        lua_pop(L, 1);
    }
    call_lua(*s, 3);
    ++s->generation;
    if (s->L) luaL_unref(s->L, LUA_REGISTRYINDEX, slot->ref);
}

void path_cb(EmfWorld* w, uint32_t id, const EmfPathResult* r, const EmfPathPoint* pts, void* user) {
    try {
        path_cb_body(w, id, r, pts, user);
    } catch (...) {
    }
}

}  // namespace

// emf.path_find(world, from, to [, {jumps, doors, swim, closest, width, height, max_jump_up, max_jump_down, nodes,
// owner, as}], fn): the query id, or nil when refused (the reason is in the log).
int l_path_find(lua_State* L) {
    Script* s = script_of(L);
    EmfWorld* w = lua_world_arg(L, 1);
    EmfPathQuery q{};
    q.size = sizeof q;
    read_pos(L, 2, q.from, "from");
    read_pos(L, 3, q.to, "to");
    int fn_index = 4;
    if (lua_istable(L, 4)) {
        fn_index = 5;
        if (opt_bool(L, 4, "jumps")) q.moves |= EMF_PATH_JUMP_UP | EMF_PATH_JUMP_DOWN | EMF_PATH_JUMP_ACROSS;
        if (opt_bool(L, 4, "doors")) q.moves |= EMF_PATH_OPEN_DOOR;
        if (opt_bool(L, 4, "swim")) q.moves |= EMF_PATH_SWIM;
        q.closest = opt_bool(L, 4, "closest") ? 1 : 0;
        q.width = static_cast<float>(opt_number(L, 4, "width", 0, 0, 64));
        q.height = static_cast<float>(opt_number(L, 4, "height", 0, 0, 64));
        q.max_jump_up = static_cast<uint8_t>(opt_number(L, 4, "max_jump_up", 0, 0, 255));
        q.max_jump_down = static_cast<uint8_t>(opt_number(L, 4, "max_jump_down", 0, 0, 255));
        q.max_nodes = static_cast<uint32_t>(opt_number(L, 4, "nodes", 0, 0, 20000));
        q.owner = static_cast<uint32_t>(opt_number(L, 4, "owner", 0, 0, 4294967295.0));
        q.as_entity = static_cast<uint32_t>(opt_number(L, 4, "as", 0, 0, 4294967295.0));  // 1.41 (API v48)
    }
    luaL_checktype(L, fn_index, LUA_TFUNCTION);
    const EmfApi* a = s->host->api;
    if (q.as_entity && a && a->version < 48)  // an older core ignores it: it would plan as a player
        luaL_error(L, "emf.path_find: as needs EMF 0.6.20 or newer (core API v48)");
    if (!has_v37(a)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushvalue(L, fn_index);
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    auto* slot = new PathSlot{s->host, s->id, ref, s->loads};
    const uint32_t id = a->path_find(w, &q, path_cb, slot);
    if (!id) {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        delete slot;
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, id);
    return 1;
}

// emf.path_cancel(id): true when the query was open (its function still gets status "cancelled").
int l_path_cancel(lua_State* L) {
    Script* s = script_of(L);
    const lua_Integer id = luaL_checkinteger(L, 1);
    const EmfApi* a = s->host->api;
    lua_pushboolean(L, has_v37(a) && id > 0 && id <= 0xffffffff && a->path_cancel(static_cast<uint32_t>(id)));
    return 1;
}

}  // namespace emf::lua
