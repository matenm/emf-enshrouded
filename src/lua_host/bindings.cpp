#include "host_impl.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>

#include "emf/emf.hpp"
#include "emf/keys.hpp"
#include "fields.h"

namespace emf::lua {

namespace {

constexpr const char* kWorldMeta = "emf.world";
constexpr const char* kProxyMeta = "emf.proxy";
constexpr const char* kArrayMeta = "emf.array";
constexpr double kFixed = 4294967296.0;  // world positions: 1 m = 2^32

struct WorldUD {
    EmfWorld* world;
    Script* script;
    uint64_t generation;  // valid while it equals script->generation (the callback it was made for)
};
struct ProxyUD {
    uint8_t* base;
    const reflect::Type* type;
    Script* script;
    uint64_t generation;
    bool read_only;  // template data (world:template): writes are refused
};
struct ArrayUD {
    uint8_t* base;  // the struct holding the array
    const reflect::Field* field;
    Script* script;
    uint64_t generation;
    bool read_only;
};

const char* const kStale = "%s can only be used inside the callback that received it";

WorldUD* check_world(lua_State* L, int index) {
    auto* w = static_cast<WorldUD*>(luaL_checkudata(L, index, kWorldMeta));
    if (w->generation != w->script->generation) luaL_error(L, kStale, "a world");
    return w;
}

// A struct that only wraps one scalar (EntityId, Time, HashKey32): read and written as that scalar.
const reflect::Field* single_scalar(const reflect::Type* t) {
    if (!t || t->field_count != 1) return nullptr;
    const auto& f = reflect::kFields[t->first_field];
    if (f.kind == reflect::Kind::Struct || f.kind == reflect::Kind::Bytes || f.count != 1 || f.offset != 0) return nullptr;
    return &f;
}

void push_value(lua_State* L, const Value& v) {
    switch (v.k) {
        case Value::Bool: lua_pushboolean(L, v.b); break;
        case Value::Int: lua_pushinteger(L, v.i); break;
        case Value::Number: lua_pushnumber(L, v.n); break;
        case Value::String: lua_pushlstring(L, v.s.data(), v.s.size()); break;
        default: lua_pushnil(L);
    }
}

Value to_value(lua_State* L, int index) {
    if (lua_isboolean(L, index)) return Value::boolean(lua_toboolean(L, index) != 0);
    if (lua_isinteger(L, index)) return Value::integer(lua_tointeger(L, index));
    if (lua_type(L, index) == LUA_TNUMBER) return Value::number(lua_tonumber(L, index));
    if (lua_type(L, index) == LUA_TSTRING) return Value::string(lua_tostring(L, index));
    return {};
}

void push_proxy(lua_State* L, Script& s, uint8_t* base, const reflect::Type* t, bool read_only = false) {
    auto* p = static_cast<ProxyUD*>(lua_newuserdatauv(L, sizeof(ProxyUD), 0));
    *p = {base, t, &s, s.generation, read_only};
    luaL_setmetatable(L, kProxyMeta);
}

// Pushes field `f` of the struct at `base` (element 0 for scalars; arrays as an array proxy).
void push_field(lua_State* L, Script& s, uint8_t* base, const reflect::Field& f, bool read_only = false) {
    if (f.count > 1) {
        auto* a = static_cast<ArrayUD*>(lua_newuserdatauv(L, sizeof(ArrayUD), 0));
        *a = {base, &f, &s, s.generation, read_only};
        luaL_setmetatable(L, kArrayMeta);
        return;
    }
    if (f.kind == reflect::Kind::Struct) {
        const reflect::Type* inner = type_at(f.type);
        if (const reflect::Field* one = single_scalar(inner)) {
            push_value(L, read_field(base + f.offset, *one, 0));
            return;
        }
        push_proxy(L, s, base + f.offset, inner, read_only);
        return;
    }
    push_value(L, read_field(base, f, 0));
}

void write_value(lua_State* L, uint8_t* base, const reflect::Field& f, uint32_t element, int value_index) {
    std::string why;
    const Value v = to_value(L, value_index);
    if (f.kind == reflect::Kind::Struct) {
        const reflect::Type* inner = type_at(f.type);
        const reflect::Field* one = single_scalar(inner);
        if (!one) luaL_error(L, "%s is a struct: write its fields", f.name);
        const uint32_t elem = f.count ? f.size / f.count : f.size;
        if (!write_field(base + f.offset + element * elem, *one, 0, v, &why)) luaL_error(L, "%s: %s", f.name, why.c_str());
        return;
    }
    if (!write_field(base, f, element, v, &why)) luaL_error(L, "%s: %s", f.name, why.c_str());
}

int proxy_index(lua_State* L) {
    auto* p = static_cast<ProxyUD*>(luaL_checkudata(L, 1, kProxyMeta));
    if (p->generation != p->script->generation) return luaL_error(L, kStale, "a component");
    const char* key = luaL_checkstring(L, 2);
    const reflect::Field* f = find_field(*p->type, key);
    if (!f) return luaL_error(L, "%s has no field %s", p->type->short_name, key);  // a typo would read nil
    push_field(L, *p->script, p->base, *f, p->read_only);
    return 1;
}

int proxy_newindex(lua_State* L) {
    auto* p = static_cast<ProxyUD*>(luaL_checkudata(L, 1, kProxyMeta));
    if (p->generation != p->script->generation) return luaL_error(L, kStale, "a component");
    const char* key = luaL_checkstring(L, 2);
    const reflect::Field* f = find_field(*p->type, key);
    if (!f) return luaL_error(L, "%s has no field %s", p->type->short_name, key);
    if (p->read_only) return luaL_error(L, "template data is read-only (%s.%s)", p->type->short_name, key);
    if (f->count > 1) return luaL_error(L, "%s is an array: write its elements (%s[1] = ...)", key, key);
    write_value(L, p->base, *f, 0, 3);
    return 0;
}

int proxy_tostring(lua_State* L) {
    auto* p = static_cast<ProxyUD*>(luaL_checkudata(L, 1, kProxyMeta));
    lua_pushfstring(L, "%s (component data)", p->type->short_name);
    return 1;
}

ArrayUD* check_array(lua_State* L) {
    auto* a = static_cast<ArrayUD*>(luaL_checkudata(L, 1, kArrayMeta));
    if (a->generation != a->script->generation) luaL_error(L, kStale, "a component");
    return a;
}

int array_index(lua_State* L) {
    ArrayUD* a = check_array(L);
    const lua_Integer i = luaL_checkinteger(L, 2);
    if (i < 1 || i > static_cast<lua_Integer>(a->field->count)) {
        lua_pushnil(L);
        return 1;
    }
    const uint32_t element = static_cast<uint32_t>(i - 1);
    if (a->field->kind == reflect::Kind::Struct) {
        const reflect::Type* inner = type_at(a->field->type);
        const uint32_t elem = a->field->size / a->field->count;
        uint8_t* at = a->base + a->field->offset + element * elem;
        if (const reflect::Field* one = single_scalar(inner)) push_value(L, read_field(at, *one, 0));
        else push_proxy(L, *a->script, at, inner, a->read_only);
        return 1;
    }
    push_value(L, read_field(a->base, *a->field, element));
    return 1;
}

int array_newindex(lua_State* L) {
    ArrayUD* a = check_array(L);
    if (a->read_only) return luaL_error(L, "template data is read-only (%s)", a->field->name);
    const lua_Integer i = luaL_checkinteger(L, 2);
    if (i < 1 || i > static_cast<lua_Integer>(a->field->count))
        return luaL_error(L, "index %d is outside 1..%d", static_cast<int>(i), static_cast<int>(a->field->count));
    write_value(L, a->base, *a->field, static_cast<uint32_t>(i - 1), 3);
    return 0;
}

int array_len(lua_State* L) {
    ArrayUD* a = check_array(L);
    lua_pushinteger(L, a->field->count);
    return 1;
}

// ---- world methods ----
int w_is_server(lua_State* L) {
    WorldUD* w = check_world(L, 1);
    lua_pushboolean(L, w->script->host->api->world_kind(w->world) == EMF_WORLD_SERVER);
    return 1;
}

int w_is_client(lua_State* L) {
    WorldUD* w = check_world(L, 1);
    lua_pushboolean(L, w->script->host->api->world_kind(w->world) == EMF_WORLD_CLIENT);
    return 1;
}

int w_tick(lua_State* L) {
    WorldUD* w = check_world(L, 1);
    const EmfApi* a = w->script->host->api;
    lua_pushinteger(L, a->world_tick ? a->world_tick(w->world) : 0);
    return 1;
}

int w_get(lua_State* L) {
    WorldUD* w = check_world(L, 1);
    const char* name = luaL_checkstring(L, 2);
    const auto entity = static_cast<uint32_t>(luaL_checkinteger(L, 3));
    const reflect::Type* t = find_type(name);
    if (!t) return luaL_error(L, "no type named %s", name);
    if (!(t->flags & 1)) return luaL_error(L, "%s is not a component", name);
    uint64_t size = 0;
    void* data = w->script->host->api->get_component(w->world, t->hash, entity, &size);
    if (!data) {
        lua_pushnil(L);
        return 1;
    }
    if (size != t->size)
        return luaL_error(L, "%s has %d bytes in the game, not %d (a game update?)", name, static_cast<int>(size),
                          static_cast<int>(t->size));
    push_proxy(L, *w->script, static_cast<uint8_t*>(data), t);
    return 1;
}

// world:template(name, id): the entity's template half of a component (shared by every entity of its template, in
// both worlds), read-only; nil when it has none (or the core has no template access).
int w_template(lua_State* L) {
    WorldUD* w = check_world(L, 1);
    const char* name = luaL_checkstring(L, 2);
    const auto entity = static_cast<uint32_t>(luaL_checkinteger(L, 3));
    const reflect::Type* t = find_type(name);
    if (!t) return luaL_error(L, "no type named %s", name);
    if (!(t->flags & 1)) return luaL_error(L, "%s is not a component", name);
    const EmfApi* api = w->script->host->api;
    uint64_t size = 0;
    void* data = api->get_template_component ? api->get_template_component(w->world, t->hash, entity, &size) : nullptr;
    if (!data) {
        lua_pushnil(L);
        return 1;
    }
    if (size && size != t->size)
        return luaL_error(L, "%s has %d bytes in the game, not %d (a game update?)", name, static_cast<int>(size),
                          static_cast<int>(t->size));
    push_proxy(L, *w->script, static_cast<uint8_t*>(data), t, true);
    return 1;
}

int w_entities(lua_State* L) {
    WorldUD* w = check_world(L, 1);
    std::vector<uint32_t> ids;
    w->script->host->api->for_each_entity(w->world, [](uint32_t e, void* u) { static_cast<std::vector<uint32_t>*>(u)->push_back(e); },
                                          &ids);
    lua_createtable(L, static_cast<int>(ids.size()), 0);
    for (size_t i = 0; i < ids.size(); ++i) {
        lua_pushinteger(L, ids[i]);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

// A copy that a bad address cannot crash (game memory read through a pointer the API gave).
bool copy_guarded(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// world:account(id): the player's account hash as "0x" + 16 hex digits (as emf.sender() prints it), or nil. From the
// BasicPlayerInfos singleton: entry id - 1 of 0xA0 bytes, entity id at +0, account at +8 (confirmed in game).
int w_account(lua_State* L) {
    WorldUD* w = check_world(L, 1);
    const lua_Integer id = luaL_checkinteger(L, 2);
    const EmfApi* api = w->script->host->api;
    const auto* infos = id >= 1 && id <= 1023 && api->singleton
                            ? static_cast<const uint8_t*>(api->singleton(w->world, "BasicPlayerInfos"))
                            : nullptr;
    uint8_t head[16];
    uint32_t held = 0;
    uint64_t account = 0;
    if (infos && copy_guarded(head, infos + static_cast<size_t>(id - 1) * 0xA0, sizeof head)) {
        std::memcpy(&held, head, 4);
        std::memcpy(&account, head + 8, 8);
    }
    if (held != static_cast<uint32_t>(id) || account == 0) {
        lua_pushnil(L);
        return 1;
    }
    char text[24];
    std::snprintf(text, sizeof text, "0x%016llx", static_cast<unsigned long long>(account));
    lua_pushstring(L, text);
    return 1;
}

int w_position(lua_State* L) {
    WorldUD* w = check_world(L, 1);
    const auto entity = static_cast<uint32_t>(luaL_checkinteger(L, 2));
    static const reflect::Type* t = find_type("CurrentTransform");
    uint64_t size = 0;
    void* data = t ? w->script->host->api->get_component(w->world, t->hash, entity, &size) : nullptr;
    if (!data || size != t->size) {
        lua_pushnil(L);
        return 1;
    }
    int64_t xyz[3];
    std::memcpy(xyz, data, sizeof xyz);  // CurrentTransform.transform.position (x, y, z) sits at offset 0
    lua_pushnumber(L, xyz[0] / kFixed);
    lua_pushnumber(L, xyz[1] / kFixed);
    lua_pushnumber(L, xyz[2] / kFixed);
    return 3;
}

// ---- items, spawning ----
EmfWorld* world_arg(lua_State* L, int index) { return check_world(L, index)->world; }

uint32_t item_arg(lua_State* L, Script& s, EmfWorld* w, int index) {
    if (lua_isinteger(L, index)) return static_cast<uint32_t>(lua_tointeger(L, index));
    const char* name = luaL_checkstring(L, index);
    const uint32_t id = s.host->api->find_item(w, name);
    if (!id) luaL_error(L, "no item named %s", name);
    return id;
}

// The world of the callback running now (emf.item without a world argument); an error outside callbacks.
EmfWorld* current_world(lua_State* L, const char* fn) {
    lua_getfield(L, LUA_REGISTRYINDEX, "emf.world_now");
    auto* w = static_cast<EmfWorld*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!w)
        luaL_error(L, "%s needs a world: call it inside a callback (on_tick, an event, a command), not at the top of "
                      "main.lua", fn);
    return w;
}

int l_item(lua_State* L) {
    Script* s = script_of(L);
    const char* name = luaL_checkstring(L, 1);
    EmfWorld* w = lua_gettop(L) >= 2 ? world_arg(L, 2) : current_world(L, "emf.item");
    const uint32_t id = s->host->api->find_item(w, name);
    if (id) lua_pushinteger(L, id);
    else lua_pushnil(L);
    return 1;
}

int l_item_info(lua_State* L) {
    Script* s = script_of(L);
    const auto id = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    EmfItemInfo info{};
    info.size = sizeof info;
    if (!s->host->api->item_info(current_world(L, "emf.item_info"), id, &info)) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, info.item_id);
    lua_setfield(L, -2, "id");
    lua_pushstring(L, info.name);
    lua_setfield(L, -2, "name");
    lua_pushinteger(L, info.max_stack);
    lua_setfield(L, -2, "max_stack");
    lua_pushstring(L, emf::guid_to_text(info.guid).c_str());
    lua_setfield(L, -2, "guid");
    return 1;
}

// emf.item_name(item [, "game"]) (lua 1.21, API v29): item is an id or a debug name. The game's name and true; the debug
// name and false when the game has none here (the dedicated server, before its text is known, an older core); nil and
// false for an unknown item. "game": the player's language even where EMF's screens cannot draw it (for the chat).
int l_item_name(lua_State* L) {
    Script* s = script_of(L);
    EmfWorld* w = current_world(L, "emf.item_name");
    const EmfApi* api = s->host->api;
    uint32_t id = 0;
    if (lua_isinteger(L, 1))
        id = static_cast<uint32_t>(lua_tointeger(L, 1));
    else
        id = api->find_item(w, luaL_checkstring(L, 1));
    const char* how = luaL_optstring(L, 2, "");
    if (*how && std::strcmp(how, "game") != 0) return luaL_error(L, "emf.item_name: the second argument is \"game\" or nothing");
    const bool v29 = api->size >= offsetof(EmfApi, game_language) + sizeof(api->game_language) && api->item_name;
    if (id && v29) {
        char buf[256];
        if (api->item_name(w, id, *how ? EMF_ITEM_NAME_GAME_TEXT : 0u, buf, sizeof buf)) {
            lua_pushstring(L, buf);
            lua_pushboolean(L, 1);
            return 2;
        }
    }
    EmfItemInfo info{};
    info.size = sizeof info;
    if (id && api->item_info(w, id, &info) && info.name[0])
        lua_pushstring(L, info.name);
    else
        lua_pushnil(L);
    lua_pushboolean(L, 0);
    return 2;
}

// emf.language() (lua 1.21, API v29): "en_us", "de_de", ... the language the game shows text in; nil on the dedicated
// server, before the game's text is known, or on an older core.
int l_language(lua_State* L) {
    const EmfApi* api = script_of(L)->host->api;
    char code[16] = {};
    if (api->size >= offsetof(EmfApi, game_language) + sizeof(api->game_language) && api->game_language &&
        api->game_language(code, sizeof code) && code[0])
        lua_pushstring(L, code);
    else
        lua_pushnil(L);
    return 1;
}

void handed_on(Script& s, uint32_t item, uint32_t n);

int l_give(lua_State* L) {
    Script* s = script_of(L);
    EmfWorld* w = world_arg(L, 1);
    const auto player = static_cast<uint32_t>(luaL_checkinteger(L, 2));
    const uint32_t item = item_arg(L, *s, w, 3);
    const auto count = static_cast<uint32_t>(luaL_optinteger(L, 4, 1));
    const uint32_t id = s->host->api->give_item(w, player, item, count);
    if (id) handed_on(*s, item, count);
    lua_pushinteger(L, id);
    return 1;
}

uint32_t session_of(const EmfApi* a) { return a && a->server_session ? a->server_session() : 0u; }

constexpr uint32_t kPlayerStateHash = 0x6ff967bbu;      // PlayerState (emf/generated/components.hpp)
constexpr uint32_t kCurrentTransformHash = 0x027ac564u;  // CurrentTransform

bool entity_pos(const EmfApi* a, EmfWorld* w, uint32_t e, double out[3]) {
    uint64_t size = 0;
    const void* t = e && a->get_component ? a->get_component(w, kCurrentTransformHash, e, &size) : nullptr;
    int64_t p[3];
    if (!t || size < sizeof p || !copy_guarded(p, t, sizeof p)) return false;
    for (int i = 0; i < 3; ++i) out[i] = static_cast<double>(p[i]) / kFixed;
    return true;
}

// Drops `n` of `item` at `first`, else `second`, else `third`, else at the nearest player still there (a give needs its
// receiver's transform: a player who left has none). The entity it went to, or 0: nobody could take it.
uint32_t drop_somewhere(const EmfApi* a, EmfWorld* w, uint32_t first, uint32_t second, uint32_t third, uint32_t item,
                        uint32_t n) {
    if (!a->give_item || !n) return 0;
    for (uint32_t e : {first, second, third})
        if (e && a->give_item(w, e, item, n)) return e;
    double at[3] = {0, 0, 0};
    const bool have = entity_pos(a, w, first, at) || entity_pos(a, w, second, at) || entity_pos(a, w, third, at);
    std::vector<std::pair<double, uint32_t>> players;
    for (uint32_t id = 1; id <= 1023; ++id) {
        if (id == first || id == second || id == third || !a->get_component(w, kPlayerStateHash, id, nullptr)) continue;
        double p[3];
        if (!entity_pos(a, w, id, p)) continue;
        const double d = have ? (p[0] - at[0]) * (p[0] - at[0]) + (p[1] - at[1]) * (p[1] - at[1]) + (p[2] - at[2]) * (p[2] - at[2]) : 0.0;
        players.emplace_back(d, id);
    }
    std::sort(players.begin(), players.end());
    for (const auto& pl : players)
        if (a->give_item(w, pl.second, item, n)) return pl.second;
    return 0;
}

void log_drop(Host::Impl* h, const std::string& id, uint32_t where, uint32_t intended, uint32_t item, uint32_t n) {
    if (!where)
        h->api->log(id.c_str(), "LOST %u of item %08x: %u and everyone else were gone (please report)", n, item, intended);
    else if (where != intended)
        h->api->log(id.c_str(), "%u of item %08x dropped at %u (%u was gone)", n, item, where, intended);
    else
        h->api->log(id.c_str(), "%u of item %08x dropped at %u (no room in it)", n, item, intended);
}

struct ItemsSlot {
    Host::Impl* host;
    std::string id;
    int ref;  // the Lua function, or LUA_NOREF
    uint64_t load;  // the script's load counter when asked (a reload drops the answer)
    bool take = false;  // emf.take: taken items whose answer its script cannot hear go back (take_back)
    uint32_t session = 0;  // the hosted session it was asked in: an answer from another one touches nothing
};

// Where taken items go back to: the entity they came from (a put there, the rest dropped at it).
struct TakeBack {
    Host::Impl* host;
    std::string id;
    uint32_t session;
};
void take_back_cb(EmfWorld* w, uint32_t entity, uint32_t item, uint32_t requested, uint32_t added, void* user) {
    auto* tb = static_cast<TakeBack*>(user);
    if (added < requested && session_of(tb->host->api) == tb->session)
        log_drop(tb->host, tb->id, drop_somewhere(tb->host->api, w, entity, 0, 0, item, requested - added), entity, item,
                 requested - added);
    delete tb;
}
// Taken items its script will not hand on (it cannot hear the answer any more: paused by an error or its time budget,
// or reloaded, since it asked; or its callback raised before handing them on): `done` items would be nowhere, so they
// go back into the entity they came from (the rest dropped there). Only in the session they were taken in.
void take_back(Host::Impl* h, const std::string& id, EmfWorld* w, uint32_t entity, uint32_t item, uint32_t done,
               uint32_t session, const char* why) {
    if (session_of(h->api) != session) {
        h->api->log(id.c_str(), "a take of %u x item %08x from %u ended after the world changed: nothing done with it (the "
                                "old world's save decides)", done, item, entity);
        return;
    }
    h->api->log(id.c_str(), "a take of %u x item %08x from %u %s: put back into %u", done, item, entity, why, entity);
    EmfItemsRequest r{};
    r.size = sizeof r;
    r.entity = entity;
    r.item = item;
    r.count = done;
    r.cb = take_back_cb;
    r.user = new TakeBack{h, id, session};
    if (h->api->put_items_ex && h->api->put_items_ex(w, &r)) return;
    delete static_cast<TakeBack*>(r.user);
    log_drop(h, id, drop_somewhere(h->api, w, entity, 0, 0, item, done), entity, item, done);
}

void items_cb_body(EmfWorld* w, uint32_t entity, uint32_t item, uint32_t requested, uint32_t done, void* user) {
    auto* slot = static_cast<ItemsSlot*>(user);
    Script* s = slot->host->find(slot->id);
    bool heard = false;
    if (s && slot->ref != LUA_NOREF) {
        std::lock_guard<std::recursive_mutex> l(s->m);
        if (s->L && !s->paused && slot->load == s->loads) {
            heard = true;
            lua_rawgeti(s->L, LUA_REGISTRYINDEX, slot->ref);
            push_world(s->L, *s, w);
            lua_pushinteger(s->L, done);
            const Script::TakeHand outer = s->hand;
            if (slot->take) s->hand = {true, item, done};
            const bool ok = call_lua(*s, 2);
            const uint32_t left = slot->take ? s->hand.left : 0;
            s->hand = outer;
            ++s->generation;
            if (s->L) luaL_unref(s->L, LUA_REGISTRYINDEX, slot->ref);
            if (!ok && left)  // the callback raised before handing everything on
                take_back(slot->host, slot->id, w, entity, item, left, slot->session, "whose callback raised an error");
        }
    }
    if (!heard && slot->ref != LUA_NOREF) {
        if (slot->take && done)
            take_back(slot->host, slot->id, w, entity, item, done, slot->session,
                      "was answered after the script was paused or reloaded");
        else if (!slot->take && done < requested) {  // a put's rest the script would have dealt with
            if (session_of(slot->host->api) == slot->session) {
                slot->host->api->log(slot->id.c_str(), "a put of %u x item %08x into %u was answered after the script was "
                                                       "paused or reloaded; %u did not fit: dropped",
                                     requested, item, entity, requested - done);
                log_drop(slot->host, slot->id, drop_somewhere(slot->host->api, w, entity, 0, 0, item, requested - done),
                         entity, item, requested - done);
            } else {
                slot->host->api->log(slot->id.c_str(), "a put into %u was answered after the world changed: nothing done "
                                                       "with it", entity);
            }
        }
    }
    delete slot;
}

// Called by EMF: nothing may escape into the game (a Lua error is a C++ exception).
void items_cb(EmfWorld* w, uint32_t entity, uint32_t item, uint32_t requested, uint32_t done, void* user) {
    try {
        items_cb_body(w, entity, item, requested, done, user);
    } catch (...) {
    }
}

// A take callback handed `n` of `item` on (emf.put, emf.give): what it raises with no longer goes back.
void handed_on(Script& s, uint32_t item, uint32_t n) {
    if (s.hand.active && s.hand.item == item) s.hand.left -= (std::min)(s.hand.left, n);
}

// A rarity for emf.put: 0-4 or its name ("common" .. "legendary").
uint8_t rarity_arg(lua_State* L, int index) {
    static const char* const kNames[] = {"common", "uncommon", "rare", "epic", "legendary"};
    if (lua_type(L, index) == LUA_TSTRING && !lua_isnumber(L, index)) {
        std::string n = lua_tostring(L, index);
        for (auto& c : n)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        for (uint8_t i = 0; i < 5; ++i)
            if (n == kNames[i]) return i;
        return luaL_error(L, "rarity \"%s\" is not common, uncommon, rare, epic or legendary (or 0-4)", lua_tostring(L, index)),
               uint8_t{0};
    }
    const lua_Integer r = luaL_checkinteger(L, index);
    if (r < 0 || r > 4) return luaL_error(L, "rarity %d is not 0-4 (common .. legendary)", static_cast<int>(r)), uint8_t{0};
    return static_cast<uint8_t>(r);
}

// The options table of emf.put (level, rarity), emf.take (slot, exact) and emf.move (all of them, and fallback).
struct ItemOpts {
    uint32_t level = 0;
    bool has_rarity = false;
    uint8_t rarity = 0;
    uint64_t slot = 0;
    bool exact = false;
    uint32_t fallback = 0;
};
ItemOpts item_opts(lua_State* L, int index, bool put_fields, bool take_fields) {
    ItemOpts o;
    if (!index) return o;
    if (put_fields) {
        lua_getfield(L, index, "level");
        if (!lua_isnil(L, -1)) {
            const lua_Integer level = luaL_checkinteger(L, -1);
            if (level < 0 || level > 1000) luaL_error(L, "level %d is not 0-1000", static_cast<int>(level));
            o.level = static_cast<uint32_t>(level);
        }
        lua_pop(L, 1);
        lua_getfield(L, index, "rarity");
        if (!lua_isnil(L, -1)) {
            o.rarity = rarity_arg(L, lua_gettop(L));
            o.has_rarity = true;
        }
        lua_pop(L, 1);
    }
    if (take_fields) {
        lua_getfield(L, index, "slot");
        if (!lua_isnil(L, -1)) o.slot = static_cast<uint64_t>(luaL_checkinteger(L, -1));
        lua_pop(L, 1);
        lua_getfield(L, index, "exact");
        o.exact = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        if (o.exact && !o.slot) luaL_error(L, "exact = true needs a slot");
    }
    return o;
}

int items_request(lua_State* L, bool take) {
    Script* s = script_of(L);
    EmfWorld* w = world_arg(L, 1);
    EmfItemsRequest r{};
    r.size = sizeof r;
    r.entity = static_cast<uint32_t>(luaL_checkinteger(L, 2));
    r.item = item_arg(L, *s, w, 3);
    r.count = static_cast<uint32_t>(luaL_optinteger(L, 4, 1));
    // after the count: the callback and a table, in either order: put (lua 1.4) {level = n, rarity = 0-4 or a name},
    // take (lua 1.6) {slot = InventorySlotId, exact = true}
    int fn_index = 0, opts_index = 0;
    for (int i = 5; i <= 6; ++i) {
        if (lua_isfunction(L, i) && !fn_index) fn_index = i;
        else if (lua_istable(L, i) && !opts_index) opts_index = i;
    }
    const ItemOpts o = item_opts(L, opts_index, !take, take);
    r.level = o.level;
    if (o.has_rarity) {
        r.rarity = o.rarity;
        r.flags |= EMF_ITEMS_RARITY;
    }
    r.slot = o.slot;
    if (o.exact) r.flags |= EMF_ITEMS_EXACT_SLOT;
    int ref = LUA_NOREF;
    if (fn_index) {
        lua_pushvalue(L, fn_index);
        ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    auto* slot = new ItemsSlot{s->host, s->id, ref, s->loads, take, session_of(s->host->api)};
    r.cb = items_cb;
    r.user = slot;
    const bool ok = take ? s->host->api->take_items(w, &r) : s->host->api->put_items_ex(w, &r);
    if (!ok) {
        if (ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, ref);
        delete slot;
    } else if (!take) {
        handed_on(*s, r.item, r.count);
    }
    lua_pushboolean(L, ok);
    return 1;
}

int l_put(lua_State* L) { return items_request(L, false); }
int l_take(lua_State* L) { return items_request(L, true); }

// ---- emf.move (lua 1.6): take -> put -> give back -> drop, done here so a script's pause or reload loses nothing ----
constexpr int kMaxMoves = 24;  // of the core's 64 waiting item requests (each move holds one at a time)

struct MoveJob {
    Host::Impl* host;
    std::string id;
    int ref;  // the Lua function, or LUA_NOREF
    uint64_t load;
    uint32_t session;
    uint32_t from, to, fallback, item, want;
    ItemOpts o;
    uint32_t taken = 0, moved = 0, returned = 0, dropped = 0, back_want = 0;
};

// The end of a move: its place in the queue comes free; the script hears the result if it still can (and the world is
// the one the move was made in).
void move_finish(MoveJob* j, EmfWorld* w, bool report) {
    std::unique_ptr<MoveJob> owned(j);
    j->host->moves.fetch_sub(1);
    Script* s = j->host->find(j->id);
    if (!s || j->ref == LUA_NOREF) return;
    std::lock_guard<std::recursive_mutex> l(s->m);
    if (!s->L || j->load != s->loads) return;  // reloaded: the reference went with the old state
    if (report && !s->paused) {
        lua_rawgeti(s->L, LUA_REGISTRYINDEX, j->ref);
        push_world(s->L, *s, w);
        lua_pushinteger(s->L, j->moved);
        lua_pushinteger(s->L, j->returned);
        lua_pushinteger(s->L, j->dropped);
        lua_pushinteger(s->L, j->taken);
        call_lua(*s, 5);
        ++s->generation;
    }
    if (s->L) luaL_unref(s->L, LUA_REGISTRYINDEX, j->ref);
}

bool move_same_session(MoveJob* j, const char* step) {
    if (session_of(j->host->api) == j->session) return true;
    j->host->api->log(j->id.c_str(), "a move of item %08x from %u to %u: its %s was answered after the world changed: "
                                     "nothing done with it (the old world's save decides)", j->item, j->from, j->to, step);
    return false;
}

void move_drop(MoveJob* j, EmfWorld* w, uint32_t n) {
    const uint32_t where = drop_somewhere(j->host->api, w, j->from, j->to, j->fallback, j->item, n);
    log_drop(j->host, j->id, where, j->from, j->item, n);
    if (where) j->dropped += n;
}

void move_back_cb(EmfWorld* w, uint32_t, uint32_t, uint32_t, uint32_t back, void* user) {
    auto* j = static_cast<MoveJob*>(user);
    try {
        if (!move_same_session(j, "give back")) {
            move_finish(j, w, false);
            return;
        }
        j->returned = back;
        if (back < j->back_want) move_drop(j, w, j->back_want - back);
    } catch (...) {
    }
    move_finish(j, w, true);
}

void move_give_back(MoveJob* j, EmfWorld* w, uint32_t n) {
    j->back_want = n;
    EmfItemsRequest r{};
    r.size = sizeof r;
    r.entity = j->from;
    r.item = j->item;
    r.count = n;
    r.level = j->o.level;
    if (j->o.has_rarity) {
        r.rarity = j->o.rarity;
        r.flags = EMF_ITEMS_RARITY;
    }
    r.cb = move_back_cb;
    r.user = j;
    if (j->host->api->put_items_ex && j->host->api->put_items_ex(w, &r)) return;
    move_drop(j, w, n);  // refused at once: straight to the feet
    move_finish(j, w, true);
}

void move_put_cb(EmfWorld* w, uint32_t, uint32_t, uint32_t, uint32_t added, void* user) {
    auto* j = static_cast<MoveJob*>(user);
    try {
        if (!move_same_session(j, "put")) {
            move_finish(j, w, false);
            return;
        }
        j->moved = added;
        if (added < j->taken) {
            move_give_back(j, w, j->taken - added);
            return;
        }
    } catch (...) {
    }
    move_finish(j, w, true);
}

void move_take_cb(EmfWorld* w, uint32_t, uint32_t, uint32_t, uint32_t taken, void* user) {
    auto* j = static_cast<MoveJob*>(user);
    try {
        if (!move_same_session(j, "take")) {
            move_finish(j, w, false);
            return;
        }
        j->taken = taken;
        if (!taken) {
            move_finish(j, w, true);
            return;
        }
        EmfItemsRequest r{};
        r.size = sizeof r;
        r.entity = j->to;
        r.item = j->item;
        r.count = taken;
        r.level = j->o.level;
        if (j->o.has_rarity) {
            r.rarity = j->o.rarity;
            r.flags = EMF_ITEMS_RARITY;
        }
        r.cb = move_put_cb;
        r.user = j;
        if (j->host->api->put_items_ex && j->host->api->put_items_ex(w, &r)) return;
        move_give_back(j, w, taken);  // the put was refused at once: everything goes back
    } catch (...) {
        move_finish(j, w, false);
    }
}

// emf.move(world, from, to, item, count [, {slot =, exact =, fallback =, level =, rarity =}] [, fn]): takes count of item
// from `from` (all or nothing; exactly that slot with exact = true), puts it into `to`, gives back into `from` what does
// not fit, and drops what still does not fit at `from`, else at `to`, else at `fallback`, else at the nearest player
// (logged; LOST only when none of them can take it). fn(world, moved, returned, dropped, taken) at the end, when the script can
// still hear it. True when the take was asked for; false (nothing done) when refused, or 24 moves already wait.
int l_move(lua_State* L) {
    Script* s = script_of(L);
    EmfWorld* w = world_arg(L, 1);
    const auto from = static_cast<uint32_t>(luaL_checkinteger(L, 2));
    const auto to = static_cast<uint32_t>(luaL_checkinteger(L, 3));
    const uint32_t item = item_arg(L, *s, w, 4);
    const lua_Integer count = luaL_checkinteger(L, 5);
    luaL_argcheck(L, count >= 1 && count <= 65535, 5, "the count is 1-65535");
    int fn_index = 0, opts_index = 0;
    for (int i = 6; i <= 7; ++i) {
        if (lua_isfunction(L, i) && !fn_index) fn_index = i;
        else if (lua_istable(L, i) && !opts_index) opts_index = i;
    }
    ItemOpts o = item_opts(L, opts_index, true, true);
    if (opts_index) {
        lua_getfield(L, opts_index, "fallback");
        if (!lua_isnil(L, -1)) o.fallback = static_cast<uint32_t>(luaL_checkinteger(L, -1));
        lua_pop(L, 1);
    }
    Host::Impl* h = s->host;
    if (!h->api->take_items || !h->api->put_items_ex || h->moves.fetch_add(1) >= kMaxMoves) {
        if (h->api->take_items && h->api->put_items_ex) h->moves.fetch_sub(1);
        lua_pushboolean(L, false);
        return 1;
    }
    int ref = LUA_NOREF;
    if (fn_index) {
        lua_pushvalue(L, fn_index);
        ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    auto* j = new MoveJob{h, s->id, ref, s->loads, session_of(h->api), from, to, o.fallback, item,
                          static_cast<uint32_t>(count), o};
    EmfItemsRequest r{};
    r.size = sizeof r;
    r.entity = from;
    r.item = item;
    r.count = static_cast<uint32_t>(count);
    r.slot = o.slot;
    if (o.exact) r.flags = EMF_ITEMS_EXACT_SLOT;
    r.cb = move_take_cb;
    r.user = j;
    if (!h->api->take_items(w, &r)) {
        if (ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, ref);
        delete j;
        h->moves.fetch_sub(1);
        lua_pushboolean(L, false);
        return 1;
    }
    lua_pushboolean(L, true);
    return 1;
}

std::vector<uint8_t> component_data(lua_State* L, const reflect::Type& t, const char* name, int data_index);

// emf.spawn(world, guid, x, y, z [, components]) (components: lua 1.12): components = {Name = {field = value}, ...}
// are spawned with the entity (spawn_ex, at most 15; never CurrentTransform: x, y, z give it). A component the game
// reads when the entity is made (ScaleComponent: the size it is drawn at) only works this way.
int l_spawn(lua_State* L) {
    Script* s = script_of(L);
    EmfWorld* w = world_arg(L, 1);
    const char* text = luaL_checkstring(L, 2);
    uint8_t guid[16];
    if (!emf::parse_guid(text, guid)) return luaL_error(L, "%s is not a GUID (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx)", text);
    const double x = luaL_checknumber(L, 3), y = luaL_checknumber(L, 4), z = luaL_checknumber(L, 5);
    if (lua_isnoneornil(L, 6)) {
        lua_pushinteger(L, s->host->api->spawn(w, guid, x, y, z));
        return 1;
    }
    luaL_checktype(L, 6, LUA_TTABLE);
    std::vector<std::vector<uint8_t>> data;
    std::vector<EmfComponentInit> inits;
    lua_pushnil(L);
    while (lua_next(L, 6)) {
        if (lua_type(L, -2) != LUA_TSTRING) return luaL_error(L, "emf.spawn: the components' keys are component names");
        const char* name = lua_tostring(L, -2);
        const reflect::Type* t = find_type(name);
        if (!t) return luaL_error(L, "no type named %s", name);
        if (!(t->flags & 1)) return luaL_error(L, "%s is not a component", name);
        if (t == find_type("CurrentTransform"))
            return luaL_error(L, "emf.spawn: x, y, z give CurrentTransform");
        if (inits.size() >= 15) return luaL_error(L, "emf.spawn: at most 15 components");
        if (!lua_isnil(L, -1)) luaL_checktype(L, -1, LUA_TTABLE);
        data.push_back(component_data(L, *t, name, lua_isnil(L, -1) ? 0 : lua_gettop(L)));
        if (t == find_type("ScaleComponent") && data.back().size() == sizeof(float)) {
            // the same range as change_component's (components_api.cpp check_data): 0 is not drawn and cannot be hit
            float v = 0;
            std::memcpy(&v, data.back().data(), sizeof v);
            if (!std::isfinite(v) || v < 0.1f || v > 10.0f)
                return luaL_error(L, "ScaleComponent needs a size from 0.1 to 10, not %f", static_cast<lua_Number>(v));
        }
        inits.push_back({t->hash, static_cast<uint32_t>(data.back().size()), nullptr});
        lua_pop(L, 1);
    }
    for (size_t i = 0; i < inits.size(); ++i) inits[i].data = data[i].data();
    const EmfApi* a = s->host->api;
    if (!a->spawn_ex) {
        lua_pushinteger(L, 0);
        return 1;
    }
    lua_pushinteger(L, a->spawn_ex(w, guid, x, y, z, inits.data(), static_cast<uint32_t>(inits.size())));
    return 1;
}

int l_destroy(lua_State* L) {
    Script* s = script_of(L);
    EmfWorld* w = world_arg(L, 1);
    lua_pushboolean(L, s->host->api->destroy(w, static_cast<uint32_t>(luaL_checkinteger(L, 2))));
    return 1;
}

// emf.remove_building(world, id [, {refund = false | "drop" | player_id, force = false}]) (lua 1.28, API v36): a placed
// object removed like a pick-up. True plus the item id given back (or nil), or false plus the reason.
int l_remove_building(lua_State* L) {
    Script* s = script_of(L);
    EmfWorld* w = world_arg(L, 1);
    EmfRemoveBuilding r{};
    r.size = sizeof r;
    r.entity = static_cast<uint32_t>(luaL_checkinteger(L, 2));
    if (lua_istable(L, 3)) {
        lua_pushnil(L);  // only refund and force: a typo would quietly lose the item
        while (lua_next(L, 3)) {
            lua_pop(L, 1);
            const char* k = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : nullptr;
            if (!k || (std::string(k) != "refund" && std::string(k) != "force")) {
                lua_pop(L, 1);
                return luaL_error(L, "emf.remove_building: unknown option (refund and force only)");
            }
        }
        lua_getfield(L, 3, "refund");
        int is_int = 0;
        const lua_Integer as_int = lua_type(L, -1) == LUA_TNUMBER ? lua_tointegerx(L, -1, &is_int) : 0;
        if (lua_type(L, -1) == LUA_TSTRING && std::string(lua_tostring(L, -1)) == "drop") {
            r.refund = EMF_REFUND_DROP;
        } else if (is_int && as_int > 0 && as_int <= 0xffffffff) {  // 7.0 is player 7
            r.refund = EMF_REFUND_PLAYER;
            r.player = static_cast<uint32_t>(as_int);
        } else if (!lua_isnil(L, -1) && !(lua_isboolean(L, -1) && !lua_toboolean(L, -1))) {
            return luaL_error(L, "emf.remove_building: refund must be false, 'drop' or a player id");
        }
        lua_pop(L, 1);
        lua_getfield(L, 3, "force");
        if (lua_toboolean(L, -1)) r.flags |= EMF_REMOVE_FORCE;
        lua_pop(L, 1);
    } else if (!lua_isnoneornil(L, 3)) {
        return luaL_error(L, "emf.remove_building: the third argument is an options table");
    }
    const EmfApi* a = s->host->api;
    if (!a || a->size < offsetof(EmfApi, remove_building) + sizeof(a->remove_building) || !a->remove_building) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "this EMF core is too old (remove_building needs API v36)");
        return 2;
    }
    if (!a->remove_building(w, &r)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, r.why[0] ? r.why : "refused (see the log)");
        return 2;
    }
    lua_pushboolean(L, true);
    if (r.refunded) lua_pushinteger(L, r.refunded);
    else lua_pushnil(L);
    return 2;
}

// emf.kill(world, id) (lua 1.4): the entity's current health becomes 0 and the game's death program does the rest; no
// attacker, no kill credit; never a player (emf::kill in emf.hpp: through attribute_add since lua 1.5 / API v18).
int l_kill(lua_State* L) {
    Script* s = script_of(L);
    EmfWorld* w = world_arg(L, 1);
    lua_pushboolean(L, emf::detail::kill_with(s->host->api, w, static_cast<uint32_t>(luaL_checkinteger(L, 2))));
    return 1;
}

// ---- lua 1.5: attributes and damage (API v18) ----
bool has_v18(const EmfApi* a) { return a && a->size >= offsetof(EmfApi, damage) + sizeof(a->damage); }

// emf.health(world, id): current, max (the attribute registry in a world this game hosts, else NetworkHealth), or nil.
int l_health(lua_State* L) {
    Script* s = script_of(L);
    EmfWorld* w = world_arg(L, 1);
    int32_t cur = 0, max = 0;
    if (!emf::detail::health_with(s->host->api, w, static_cast<uint32_t>(luaL_checkinteger(L, 2)), &cur, &max)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, cur);
    lua_pushinteger(L, max);
    return 2;
}

// An attribute by name (health, max_health, stamina, mana) or by its id.
uint32_t attribute_arg(lua_State* L, int index) {
    if (lua_isinteger(L, index)) return static_cast<uint32_t>(lua_tointeger(L, index));
    const std::string n = luaL_checkstring(L, index);
    if (n == "health") return EMF_ATTR_HEALTH;
    if (n == "max_health") return EMF_ATTR_MAX_HEALTH;
    if (n == "stamina") return EMF_ATTR_STAMINA;
    if (n == "mana") return EMF_ATTR_MANA;
    if (n == "max_stamina") return EMF_ATTR_MAX_STAMINA;
    if (n == "max_mana") return EMF_ATTR_MAX_MANA;
    // a player's XP and level (EMF API v22): give XP with emf.attribute_add(w, id, "xp_pending", n); the rest read-only
    if (n == "xp_pending") return EMF_ATTR_XP_PENDING;
    if (n == "xp") return EMF_ATTR_XP;
    if (n == "xp_required") return EMF_ATTR_XP_REQUIRED;
    if (n == "level") return EMF_ATTR_LEVEL;
    luaL_error(L,
               "unknown attribute '%s' (health, max_health, stamina, max_stamina, mana, max_mana, xp_pending, xp, "
               "xp_required, level, or an attribute id)",
               n.c_str());
    return 0;
}

int32_t int32_arg(lua_State* L, int index) {
    const lua_Integer v = luaL_checkinteger(L, index);
    luaL_argcheck(L, v >= INT32_MIN && v <= INT32_MAX, index, "out of range");
    return static_cast<int32_t>(v);
}

// emf.attribute(world, id, name_or_id): the attribute's value as the game reads it, or nil.
int l_attribute(lua_State* L) {
    Script* s = script_of(L);
    EmfWorld* w = world_arg(L, 1);
    const auto id = static_cast<uint32_t>(luaL_checkinteger(L, 2));
    const uint32_t attr = attribute_arg(L, 3);
    const EmfApi* a = s->host->api;
    int32_t v = 0;
    if (has_v18(a) && a->attribute_get && a->attribute_get(w, id, attr, &v)) lua_pushinteger(L, v);
    else lua_pushnil(L);
    return 1;
}

// emf.attribute_add(world, id, name_or_id, delta): true when done (or queued); clamped, not a hit (attribute_add).
int l_attribute_add(lua_State* L) {
    Script* s = script_of(L);
    EmfWorld* w = world_arg(L, 1);
    const auto id = static_cast<uint32_t>(luaL_checkinteger(L, 2));
    const uint32_t attr = attribute_arg(L, 3);
    const int32_t delta = int32_arg(L, 4);
    const EmfApi* a = s->host->api;
    lua_pushboolean(L, has_v18(a) && a->attribute_add && a->attribute_add(w, id, attr, delta));
    return 1;
}

// emf.damage(world, attacker, target, amount): a game hit (credit to the attacker, 0 = none) on the next step.
int l_damage(lua_State* L) {
    Script* s = script_of(L);
    EmfWorld* w = world_arg(L, 1);
    const auto attacker = static_cast<uint32_t>(luaL_checkinteger(L, 2));
    const auto target = static_cast<uint32_t>(luaL_checkinteger(L, 3));
    const int32_t amount = int32_arg(L, 4);
    const EmfApi* a = s->host->api;
    lua_pushboolean(L, has_v18(a) && a->damage && a->damage(w, attacker, target, amount));
    return 1;
}

// ---- lua 1.11: a player's level (API v22) ----
bool has_v22(const EmfApi* a) {
    return a && a->version >= 22 && a->size >= offsetof(EmfApi, player_level_set) + sizeof(a->player_level_set);
}

uint32_t level_arg(lua_State* L, int index) {
    const lua_Integer v = luaL_checkinteger(L, index);
    luaL_argcheck(L, v >= 0 && v <= UINT32_MAX, index, "out of range");
    return static_cast<uint32_t>(v);
}

// emf.xp_required(world, level): the XP that level needs before the next one, or nil (no such level, unknown).
int l_xp_required(lua_State* L) {
    Script* s = script_of(L);
    EmfWorld* w = world_arg(L, 1);
    const uint32_t level = level_arg(L, 2);
    const EmfApi* a = s->host->api;
    const uint32_t need = has_v22(a) && a->player_xp_required ? a->player_xp_required(w, level) : 0u;
    if (need) lua_pushinteger(L, need);
    else lua_pushnil(L);
    return 1;
}

// emf.level_cap(world): the highest level a player can reach, or nil when unknown.
int l_level_cap(lua_State* L) {
    Script* s = script_of(L);
    EmfWorld* w = world_arg(L, 1);
    const EmfApi* a = s->host->api;
    const uint32_t cap = has_v22(a) && a->player_level_cap ? a->player_level_cap(w) : 0u;
    if (cap) lua_pushinteger(L, cap);
    else lua_pushnil(L);
    return 1;
}

// emf.set_level(world, player, level [, xp]): true when set (or queued); raises or lowers, no level-up events.
int l_set_level(lua_State* L) {
    Script* s = script_of(L);
    EmfWorld* w = world_arg(L, 1);
    const auto player = static_cast<uint32_t>(luaL_checkinteger(L, 2));
    const uint32_t level = level_arg(L, 3);
    const uint32_t xp = lua_isnoneornil(L, 4) ? 0u : level_arg(L, 4);
    const EmfApi* a = s->host->api;
    lua_pushboolean(L, has_v22(a) && a->player_level_set && a->player_level_set(w, player, level, xp));
    return 1;
}

// ---- lua 1.12: add and remove components (API v23) ----
bool has_v23(const EmfApi* a) {
    return a && a->version >= 23 && a->size >= offsetof(EmfApi, change_component) + sizeof(a->change_component);
}

struct ComponentSlot {
    Host::Impl* host;
    std::string id;
    int ref;        // the Lua function
    uint64_t load;  // the script's load counter when asked (a reload drops the answer)
};

const char* component_status_name(uint32_t s) {
    return s == EMF_COMPONENT_DONE ? "done" : s == EMF_COMPONENT_ALREADY ? "already" : "failed";
}

void component_cb_body(EmfWorld* w, uint32_t status, const char* why, void* user) {
    auto* slot = static_cast<ComponentSlot*>(user);
    Script* s = slot->host->find(slot->id);
    if (s) {
        std::lock_guard<std::recursive_mutex> l(s->m);
        const bool same_state = s->L && slot->load == s->loads;
        // a NULL world: the world ended first (its ids mean nothing); the script is not asked
        if (same_state && !s->paused && w) {
            lua_rawgeti(s->L, LUA_REGISTRYINDEX, slot->ref);
            push_world(s->L, *s, w);
            lua_pushstring(s->L, component_status_name(status));
            lua_pushstring(s->L, why ? why : "");
            call_lua(*s, 3);
            ++s->generation;
        }
        if (same_state) luaL_unref(s->L, LUA_REGISTRYINDEX, slot->ref);
    }
    delete slot;
}

// Called by EMF: nothing may escape into the game (a Lua error is a C++ exception).
void component_cb(EmfWorld* w, uint32_t, uint32_t, uint32_t status, const char* why, void* user) {
    try {
        component_cb_body(w, status, why, user);
    } catch (...) {
    }
}

// The fields of `data` (a table: field name = value) written by name into a zeroed component of type t.
std::vector<uint8_t> component_data(lua_State* L, const reflect::Type& t, const char* name, int data_index) {
    std::vector<uint8_t> data(t.size, 0);
    if (!data_index) return data;
    lua_pushnil(L);
    while (lua_next(L, data_index)) {
        if (lua_type(L, -2) != LUA_TSTRING) luaL_error(L, "%s: the data's keys are field names", name);
        const char* key = lua_tostring(L, -2);
        const reflect::Field* f = find_field(t, key);
        if (!f) luaL_error(L, "%s has no field %s", name, key);
        if (f->count > 1) luaL_error(L, "%s.%s is an array: not supported here", name, key);
        write_value(L, data.data(), *f, 0, lua_gettop(L));
        lua_pop(L, 1);
    }
    return data;
}

// add: emf.add_component(world, id, name [, data] [, opts] [, fn]); remove: emf.remove_component(world, id, name [, opts]
// [, fn]). data = {field = value} (absent fields are zero); opts: persistent = true (one the world save keeps);
// fn(world, status, why) once: status "done", "already" or "failed". True when queued.
int change_component_lua(lua_State* L, uint32_t op) {
    Script* s = script_of(L);
    EmfWorld* w = world_arg(L, 1);
    const auto entity = static_cast<uint32_t>(luaL_checkinteger(L, 2));
    const char* name = luaL_checkstring(L, 3);
    const reflect::Type* t = find_type(name);
    if (!t) return luaL_error(L, "no type named %s", name);
    if (!(t->flags & 1)) return luaL_error(L, "%s is not a component", name);
    const bool add = op == EMF_COMPONENT_ADD;
    int data_index = 0, fn_index = 0, opts_index = 0;
    if (add && !lua_isnoneornil(L, 4)) {
        luaL_checktype(L, 4, LUA_TTABLE);
        data_index = 4;
    }
    for (int i = add ? 5 : 4; i <= (add ? 6 : 5); ++i) {
        if (lua_isfunction(L, i) && !fn_index) fn_index = i;
        else if (lua_istable(L, i) && !opts_index) opts_index = i;
    }
    EmfComponentChange c{};
    c.size = sizeof c;
    c.op = op;
    c.entity = entity;
    c.hash = t->hash;
    if (opts_index) {
        lua_getfield(L, opts_index, "persistent");
        if (lua_toboolean(L, -1)) c.flags |= EMF_COMPONENT_PERSISTENT;
        lua_pop(L, 1);
    }
    std::vector<uint8_t> data;
    if (add) {
        data = component_data(L, *t, name, data_index);
        c.data = data.data();
        c.data_size = static_cast<uint32_t>(data.size());
    }
    const EmfApi* a = s->host->api;
    if (!has_v23(a) || !a->change_component) {
        lua_pushboolean(L, false);
        return 1;
    }
    ComponentSlot* slot = nullptr;
    if (fn_index) {
        lua_pushvalue(L, fn_index);
        slot = new ComponentSlot{s->host, s->id, luaL_ref(L, LUA_REGISTRYINDEX), s->loads};
        c.cb = component_cb;
        c.user = slot;
    }
    const bool ok = a->change_component(w, &c);
    if (!ok && slot) {  // refused at once: the callback is never called
        luaL_unref(L, LUA_REGISTRYINDEX, slot->ref);
        delete slot;
    }
    lua_pushboolean(L, ok);
    return 1;
}

int l_add_component(lua_State* L) { return change_component_lua(L, EMF_COMPONENT_ADD); }
int l_remove_component(lua_State* L) { return change_component_lua(L, EMF_COMPONENT_REMOVE); }

// ---- lua 1.6: chat lines and saves (API v19) ----
bool has_v19(const EmfApi* a) { return a && a->size >= offsetof(EmfApi, request_save) + sizeof(a->request_save); }

constexpr int kScriptChatLines = 30;           // per script and 10 s (the core also limits the lua mod as a whole)
constexpr uint64_t kScriptChatWindowMs = 10000;

// The lines a text makes in the chat ('\n' splits; the core sends at most 10 and a "..." line).
int chat_line_count(const std::string& text) {
    int n = 1;
    for (char c : text)
        if (c == '\n' && n <= static_cast<int>(EMF_CHAT_MAX_LINES)) ++n;
    return n;
}

// True if the script may send `text` now; false, logged once per window, when over its limit. Only lines the core
// queued count (chat_spent).
bool chat_budget(Script& s, const std::string& text, const char* fn) {
    const uint64_t now = s.host->now_ms ? s.host->now_ms() : GetTickCount64();
    if (s.chat_lines == 0 || now - s.chat_window >= kScriptChatWindowMs) {
        s.chat_window = now;
        s.chat_lines = 0;
    }
    const int n = chat_line_count(text);
    if (s.chat_lines + n > kScriptChatLines) {
        if (s.chat_lines <= kScriptChatLines) {
            s.host->api->log(s.id.c_str(), "%s refused: more than %d chat lines in 10 s", fn, kScriptChatLines);
            s.chat_lines = kScriptChatLines + 1;  // said once for this window
        }
        return false;
    }
    return true;
}
bool chat_spent(Script& s, const std::string& text, bool queued) {
    if (queued) s.chat_lines += chat_line_count(text);
    return queued;
}

// emf.tell(world, id, text): a chat line only that player sees (the host of a game you host: a note); true when queued.
bool has_v30_chat(const EmfApi* a) {
    return a && a->size >= offsetof(EmfApi, chat_broadcast_ex) + sizeof(a->chat_broadcast_ex) && a->chat_tell_ex &&
           a->chat_broadcast_ex;
}

// The optional last argument of emf.tell / emf.broadcast (lua 1.22): {system = true} for a line without a speaker's
// name. -1: no options; else the flags. An unknown key is an error.
int64_t chat_options(lua_State* L, int idx, const char* fn) {
    if (lua_isnoneornil(L, idx)) return -1;
    luaL_checktype(L, idx, LUA_TTABLE);
    int64_t flags = 0;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        const char* key = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : nullptr;
        if (!key || std::strcmp(key, "system") != 0)
            luaL_error(L, "%s: unknown option \"%s\" (the options: system)", fn, key ? key : luaL_typename(L, -2));
        flags = lua_toboolean(L, -1) ? EMF_CHAT_SYSTEM : 0;
        lua_pop(L, 1);
    }
    return flags;
}

int l_tell(lua_State* L) {
    Script* s = script_of(L);
    EmfWorld* w = world_arg(L, 1);
    const auto id = static_cast<uint32_t>(luaL_checkinteger(L, 2));
    const std::string text = to_text(L, 3);
    const int64_t flags = chat_options(L, 4, "emf.tell");
    const EmfApi* a = s->host->api;
    // with options on a v30 core: chat_tell_ex; else chat_tell (an older core shows a system line under the player's name)
    const bool ex = flags >= 0 && has_v30_chat(a);
    lua_pushboolean(L, has_v19(a) && a->chat_tell && chat_budget(*s, text, "emf.tell") &&
                           chat_spent(*s, text, ex ? a->chat_tell_ex(w, id, text.c_str(), static_cast<uint32_t>(flags))
                                                   : a->chat_tell(w, id, text.c_str())));
    return 1;
}

// emf.broadcast(text [, opts]): the line for every connected player; true when queued.
int l_broadcast(lua_State* L) {
    Script* s = script_of(L);
    const std::string text = to_text(L, 1);
    const int64_t flags = chat_options(L, 2, "emf.broadcast");
    const EmfApi* a = s->host->api;
    const bool ex = flags >= 0 && has_v30_chat(a);
    lua_pushboolean(L, has_v19(a) && a->chat_broadcast && chat_budget(*s, text, "emf.broadcast") &&
                           chat_spent(*s, text, ex ? a->chat_broadcast_ex(text.c_str(), static_cast<uint32_t>(flags))
                                                   : a->chat_broadcast(text.c_str())));
    return 1;
}

// world:name(id): the player's name as the game knows it, or nil.
int w_name(lua_State* L) {
    WorldUD* w = check_world(L, 1);
    const lua_Integer id = luaL_checkinteger(L, 2);
    const EmfApi* a = w->script->host->api;
    char buf[256] = {};
    if (id < 1 || id > 1023 || !has_v19(a) || !a->player_name || !a->player_name(w->world, static_cast<uint32_t>(id), buf, sizeof buf) ||
        !buf[0]) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, buf);
    return 1;
}

bool has_v30(const EmfApi* a) {
    return a && a->size >= offsetof(EmfApi, player_character_name) + sizeof(a->player_character_name);
}

// world:character_name(id) (lua 1.22, API v30): the character's name (the Social tab's), or nil until the player's game
// has sent it (a few seconds after joining), on an older core, or for anything but a player of this server world
int w_character_name(lua_State* L) {
    WorldUD* w = check_world(L, 1);
    const lua_Integer id = luaL_checkinteger(L, 2);
    const EmfApi* a = w->script->host->api;
    char buf[260] = {};
    if (id < 1 || id > 0xFFFFFFFF || !has_v30(a) || !a->player_character_name ||
        !a->player_character_name(w->world, static_cast<uint32_t>(id), buf, sizeof buf) || !buf[0]) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, buf);
    return 1;
}

bool has_v43(const EmfApi* a) {
    return a && a->size >= offsetof(EmfApi, player_permissions) + sizeof(a->player_permissions);
}

// world:permissions(id) (lua 1.36, API v43): the player's own game permissions, {admin, access_inventories, edit_base,
// edit_world, extend_base, receive_exp, mask}; admin = the game's admin (a dedicated server's Admin group, a host's
// admins). nil for anything but a player of this server world, on a client world or an older core.
int w_permissions(lua_State* L) {
    WorldUD* w = check_world(L, 1);
    const lua_Integer id = luaL_checkinteger(L, 2);
    const EmfApi* a = w->script->host->api;
    uint32_t perms = 0;
    if (id < 1 || id > 0xFFFFFFFF || !has_v43(a) || !a->player_permissions ||
        !a->player_permissions(w->world, static_cast<uint32_t>(id), &perms)) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 7);
    const struct {
        const char* name;
        uint32_t bit;
    } bits[] = {{"admin", EMF_PERM_KICK_BAN},       {"access_inventories", EMF_PERM_ACCESS_INVENTORIES},
                {"edit_base", EMF_PERM_EDIT_BASE},  {"edit_world", EMF_PERM_EDIT_WORLD},
                {"extend_base", EMF_PERM_EXTEND_BASE}, {"receive_exp", EMF_PERM_RECEIVE_EXP}};
    for (const auto& b : bits) {
        lua_pushboolean(L, (perms & b.bit) != 0);
        lua_setfield(L, -2, b.name);
    }
    lua_pushinteger(L, static_cast<lua_Integer>(perms));
    lua_setfield(L, -2, "mask");
    return 1;
}

void chat_cb_body(EmfWorld* w, uint64_t account, uint32_t entity, const char* text, void* user) {
    auto* h = static_cast<Host::Impl*>(user);
    char hex[24];
    std::snprintf(hex, sizeof hex, "0x%016llx", static_cast<unsigned long long>(account));
    for (auto& sp : h->scripts) {
        Script& s = *sp;
        std::lock_guard<std::recursive_mutex> l(s.m);
        if (!s.L || s.paused || s.chat_cbs.empty()) continue;
        const std::vector<int> refs = s.chat_cbs;
        for (int ref : refs) {
            lua_rawgeti(s.L, LUA_REGISTRYINDEX, ref);
            push_world(s.L, s, w);
            lua_pushstring(s.L, hex);
            lua_pushinteger(s.L, entity);
            lua_pushstring(s.L, text ? text : "");
            if (!call_lua(s, 4)) break;
        }
        ++s.generation;
    }
}

// Called by EMF: nothing may escape into the game (a Lua error is a C++ exception).
void chat_cb(EmfWorld* w, uint64_t account, uint32_t entity, const char* text, void* user) {
    try {
        chat_cb_body(w, account, entity, text, user);
    } catch (...) {
    }
}

// emf.on_chat(fn): fn(world, account, entity, text) for every player's chat line that is no EMF command (read-only), on
// the server world after its next step. False when players' chat is off here (the core logs why) or on an older core.
int l_on_chat(lua_State* L) {
    Script* s = script_of(L);
    luaL_checktype(L, 1, LUA_TFUNCTION);
    Host::Impl* h = s->host;
    const EmfApi* a = h->api;
    int state = h->chat_hooked.load();
    if (state == 0) {  // the first emf.on_chat of any script subscribes the host once, for the process
        const bool ok = has_v19(a) && a->on_chat && a->on_chat(chat_cb, h);
        int expected = 0;
        h->chat_hooked.compare_exchange_strong(expected, ok ? 1 : -1);
        state = h->chat_hooked.load();
    }
    if (state != 1) {
        lua_pushboolean(L, false);
        return 1;
    }
    lua_pushvalue(L, 1);
    s->chat_cbs.push_back(luaL_ref(L, LUA_REGISTRYINDEX));
    lua_pushboolean(L, true);
    return 1;
}

// ---- lua 1.20: player events (API v28 on_player) ----
bool has_v28(const EmfApi* a) {
    return a && a->version >= 28 && a->size >= offsetof(EmfApi, on_player) + sizeof(a->on_player) && a->on_player;
}

// The info table of a player event: {player, account, name, host [, killer]}.
void push_player_info(lua_State* L, uint32_t player, uint64_t account, const char* name, bool host, bool died,
                      uint32_t killer) {
    char hex[24];
    std::snprintf(hex, sizeof hex, "0x%016llx", static_cast<unsigned long long>(account));
    lua_createtable(L, 0, 5);
    lua_pushinteger(L, player);
    lua_setfield(L, -2, "player");
    lua_pushstring(L, hex);
    lua_setfield(L, -2, "account");
    lua_pushstring(L, name);
    lua_setfield(L, -2, "name");
    lua_pushboolean(L, host);
    lua_setfield(L, -2, "host");
    if (died) {
        lua_pushinteger(L, killer);
        lua_setfield(L, -2, "killer");
    }
}


void player_cb_body(EmfWorld* w, const EmfPlayerEvent* e, void* user) {
    auto* h = static_cast<Host::Impl*>(user);
    if (!e || e->kind < EMF_PLAYER_JOINED || e->kind > EMF_PLAYER_DIED) return;
    const size_t k = e->kind - EMF_PLAYER_JOINED;
    char name[sizeof e->name + 1];
    std::memcpy(name, e->name, sizeof e->name);
    name[sizeof e->name] = 0;
    {  // who is in: told to a function registered later (a reload)
        std::lock_guard<std::mutex> l(h->present_m);
        const uint32_t session = session_of(h->api);
        if (session != h->present_session) {
            h->present.clear();
            h->present_session = session;
        }
        if (e->kind == EMF_PLAYER_JOINED) h->present[e->account] = {e->player, name, e->host != 0};
        if (e->kind == EMF_PLAYER_LEFT) h->present.erase(e->account);
    }
    for (auto& sp : h->scripts) {
        Script& s = *sp;
        std::lock_guard<std::recursive_mutex> l(s.m);
        if (!s.L || s.paused || s.player_cbs[k].empty()) continue;
        const std::vector<int> refs = s.player_cbs[k];
        for (int ref : refs) {
            lua_rawgeti(s.L, LUA_REGISTRYINDEX, ref);
            push_world(s.L, s, w);
            push_player_info(s.L, e->player, e->account, name, e->host != 0, e->kind == EMF_PLAYER_DIED, e->killer);
            if (!call_lua(s, 2)) break;
        }
        ++s.generation;
    }
}

// Called by EMF: nothing may escape into the game (a Lua error is a C++ exception).
void player_cb(EmfWorld* w, const EmfPlayerEvent* e, void* user) {
    try {
        player_cb_body(w, e, user);
    } catch (...) {
    }
}

// emf.on_player_joined / _left / _died(fn): fn(world, {player, account, name, host, killer (died)}) on the server world
// after its step. The first of any script subscribes the host once, for the process. False on an older core.
int on_player_kind(lua_State* L, size_t k) {
    Script* s = script_of(L);
    luaL_checktype(L, 1, LUA_TFUNCTION);
    Host::Impl* h = s->host;
    int state = h->player_hooked.load();
    const bool was_on = state == 1;  // the core already told the host who came: a new joined function hears it from it
    if (state == 0) {
        const bool ok = has_v28(h->api) && h->api->on_player(player_cb, h);
        int expected = 0;
        h->player_hooked.compare_exchange_strong(expected, ok ? 1 : -1);
        state = h->player_hooked.load();
    }
    if (state != 1) {
        lua_pushboolean(L, false);
        return 1;
    }
    lua_pushvalue(L, 1);
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    s->player_cbs[k].push_back(ref);
    if (k == 0 && was_on) s->player_intro.push_back(ref);
    lua_pushboolean(L, true);
    return 1;
}
int l_on_player_joined(lua_State* L) { return on_player_kind(L, 0); }
int l_on_player_left(lua_State* L) { return on_player_kind(L, 1); }
int l_on_player_died(lua_State* L) { return on_player_kind(L, 2); }

void player_intro_body(Host::Impl& h, Script& s, EmfWorld* w) {
    const std::vector<int> refs = std::move(s.player_intro);
    s.player_intro.clear();
    std::vector<std::pair<uint64_t, Host::Impl::PresentPlayer>> list;
    {
        std::lock_guard<std::mutex> l(h.present_m);
        if (h.present_session != session_of(h.api)) return;  // another world: its joins come by themselves
        list.assign(h.present.begin(), h.present.end());
    }
    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
        return a.second.host != b.second.host ? a.second.host : a.second.player < b.second.player;
    });
    for (int ref : refs)
        for (const auto& [account, p] : list) {
            if (!s.L || s.paused) return;
            lua_rawgeti(s.L, LUA_REGISTRYINDEX, ref);
            push_world(s.L, s, w);
            push_player_info(s.L, p.player, account, p.name.c_str(), p.host, false, 0);
            if (!call_lua(s, 2)) return;
        }
}

// emf.request_save(): asks the game to save the world now; true when asked (or queued).
int l_request_save(lua_State* L) {
    Script* s = script_of(L);
    const EmfApi* a = s->host->api;
    lua_pushboolean(L, has_v19(a) && a->request_save && a->request_save());
    return 1;
}

// ---- lua 1.7: teleport (API v20) ----
bool has_v20(const EmfApi* a) { return a && a->size >= offsetof(EmfApi, teleport) + sizeof(a->teleport); }

struct TeleportSlot {
    Host::Impl* host;
    std::string id;
    int ref;        // the Lua function
    uint64_t load;  // the script's load counter when asked (a reload drops the answer)
};

const char* teleport_status_name(uint32_t s) {
    return s == EMF_TELEPORT_MOVED ? "moved" : s == EMF_TELEPORT_READY ? "ready" : "failed";
}

void teleport_cb_body(EmfWorld* w, const EmfTeleportResult* r, void* user) {
    auto* slot = static_cast<TeleportSlot*>(user);
    const bool last = (r->flags & EMF_TELEPORT_LAST) != 0;
    // The session ended first (a new world, the menu): its world and ids mean nothing; the script is not asked.
    const bool ended = (r->flags & EMF_TELEPORT_SESSION_ENDED) != 0;
    Script* s = slot->host->find(slot->id);
    if (s) {
        std::lock_guard<std::recursive_mutex> l(s->m);
        // the reference lives in the state of the load that asked: after a reload it went with the old state
        const bool same_state = s->L && slot->load == s->loads;
        if (same_state && !s->paused && !ended) {
            lua_rawgeti(s->L, LUA_REGISTRYINDEX, slot->ref);
            lua_pushboolean(s->L, r->status != EMF_TELEPORT_FAILED);
            lua_pushnumber(s->L, r->x);
            lua_pushnumber(s->L, r->y);
            lua_pushnumber(s->L, r->z);
            lua_createtable(s->L, 0, 6);
            lua_pushstring(s->L, teleport_status_name(r->status));
            lua_setfield(s->L, -2, "status");
            lua_pushnumber(s->L, r->distance);
            lua_setfield(s->L, -2, "distance");
            lua_pushboolean(s->L, (r->flags & EMF_TELEPORT_OFF_TARGET) != 0);
            lua_setfield(s->L, -2, "off_target");
            lua_pushboolean(s->L, (r->flags & EMF_TELEPORT_RAW_TARGET) != 0);
            lua_setfield(s->L, -2, "raw_target");
            lua_pushstring(s->L, r->reason ? r->reason : "");
            lua_setfield(s->L, -2, "reason");
            lua_pushinteger(s->L, r->entity);
            lua_setfield(s->L, -2, "entity");
            push_world(s->L, *s, w);
            call_lua(*s, 6);
            ++s->generation;
        }
        if (last && same_state) luaL_unref(s->L, LUA_REGISTRYINDEX, slot->ref);
    }
    if (last) delete slot;
}

// Called by EMF: nothing may escape into the game (a Lua error is a C++ exception).
void teleport_cb(EmfWorld* w, const EmfTeleportResult* r, void* user) {
    try {
        teleport_cb_body(w, r, user);
    } catch (...) {
    }
}

// emf.teleport(world, id, x, y, z [, opts] [, fn(moved, x, y, z, info, world)]): true when queued. opts: ground = true
// (the game's ground search), yaw = degrees, spawn_point = id (land on its pads; x, y, z may then be nil), checkpoint =
// true, ready = true (fn runs again with info.status "ready" once the player can play). info: status ("moved", "failed",
// "ready"), distance, off_target, raw_target, reason, entity.
int l_teleport(lua_State* L) {
    Script* s = script_of(L);
    EmfWorld* w = world_arg(L, 1);
    EmfTeleport t{};
    t.size = sizeof t;
    t.entity = static_cast<uint32_t>(luaL_checkinteger(L, 2));
    int fn_index = 0, opts_index = 0;
    for (int i = 6; i <= 7; ++i) {
        if (lua_isfunction(L, i) && !fn_index) fn_index = i;
        else if (lua_istable(L, i) && !opts_index) opts_index = i;
    }
    if (opts_index) {
        lua_getfield(L, opts_index, "ground");
        if (lua_toboolean(L, -1)) t.flags |= EMF_TELEPORT_FIND_GROUND;
        lua_pop(L, 1);
        lua_getfield(L, opts_index, "checkpoint");
        if (lua_toboolean(L, -1)) t.flags |= EMF_TELEPORT_CHECKPOINT;
        lua_pop(L, 1);
        lua_getfield(L, opts_index, "ready");
        if (lua_toboolean(L, -1)) t.flags |= EMF_TELEPORT_NOTIFY_READY;
        lua_pop(L, 1);
        lua_getfield(L, opts_index, "yaw");
        if (!lua_isnil(L, -1)) {
            t.yaw_deg = static_cast<float>(luaL_checknumber(L, -1));
            t.flags |= EMF_TELEPORT_FACE_YAW;
        }
        lua_pop(L, 1);
        lua_getfield(L, opts_index, "spawn_point");
        if (!lua_isnil(L, -1)) t.spawn_point = static_cast<uint32_t>(luaL_checkinteger(L, -1));
        lua_pop(L, 1);
    }
    if (t.spawn_point) {
        t.x = luaL_optnumber(L, 3, 0);
        t.y = luaL_optnumber(L, 4, 0);
        t.z = luaL_optnumber(L, 5, 0);
    } else {
        t.x = luaL_checknumber(L, 3);
        t.y = luaL_checknumber(L, 4);
        t.z = luaL_checknumber(L, 5);
    }
    const EmfApi* a = s->host->api;
    if (!has_v20(a) || !a->teleport) {
        lua_pushboolean(L, false);
        return 1;
    }
    TeleportSlot* slot = nullptr;
    if (fn_index) {
        lua_pushvalue(L, fn_index);
        slot = new TeleportSlot{s->host, s->id, luaL_ref(L, LUA_REGISTRYINDEX), s->loads};
        t.cb = teleport_cb;
        t.user = slot;
    }
    const bool ok = a->teleport(w, &t);
    if (!ok && slot) {
        luaL_unref(L, LUA_REGISTRYINDEX, slot->ref);
        delete slot;
    }
    lua_pushboolean(L, ok);
    return 1;
}

// emf.time_of_day([world]) (lua 1.4): hours 0 <= h < 24 from the world's Daytime (the callback's world by default), nil
// when it is not known.
int l_time_of_day(lua_State* L) {
    Script* s = script_of(L);
    EmfWorld* w = lua_isnoneornil(L, 1) ? current_world(L, "emf.time_of_day") : world_arg(L, 1);
    const auto h = emf::detail::time_of_day_with(s->host->api, w);
    if (h) lua_pushnumber(L, *h);
    else lua_pushnil(L);
    return 1;
}

// ---- events ----
uint8_t kind_arg(lua_State* L, int index, const char* fallback) {
    const std::string k = luaL_optstring(L, index, fallback);
    if (k == "server") return EMF_WORLD_SERVER;
    if (k == "client") return EMF_WORLD_CLIENT;
    luaL_error(L, "the world kind is \"server\" or \"client\", not \"%s\"", k.c_str());
    return 0;
}

void push_struct_table(lua_State* L, const uint8_t* base, const reflect::Type& t, int depth);

void push_field_value(lua_State* L, const uint8_t* base, const reflect::Field& f, uint32_t element, int depth) {
    if (f.kind == reflect::Kind::Struct) {
        const reflect::Type* inner = type_at(f.type);
        const uint32_t elem = f.count ? f.size / f.count : f.size;
        const uint8_t* at = base + f.offset + element * elem;
        if (const reflect::Field* one = single_scalar(inner)) push_value(L, read_field(at, *one, 0));
        else if (inner && depth < 8) push_struct_table(L, at, *inner, depth + 1);
        else lua_pushnil(L);
        return;
    }
    push_value(L, read_field(base, f, element));
}

// __index of event tables (upvalue: the reflect::Type): only names the type does not have get here (raw bytes fields
// have no value in the table and read nil).
int strict_index(lua_State* L) {
    const auto* t = static_cast<const reflect::Type*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (lua_type(L, 2) != LUA_TSTRING || find_field(*t, lua_tostring(L, 2))) {
        lua_pushnil(L);
        return 1;
    }
    return luaL_error(L, "%s has no field %s", t->short_name, lua_tostring(L, 2));  // a typo would read nil
}

// Sets the metatable of struct type `t` (made once per state and type) on the table on top of the stack.
void set_struct_meta(lua_State* L, const reflect::Type& t) {
    const std::string key = std::string("emf.struct.") + t.name;
    if (luaL_newmetatable(L, key.c_str())) {
        lua_pushstring(L, t.short_name);
        lua_setfield(L, -2, "__name");  // tostring(e): "EntityDiedEvent: 0x..."
        lua_pushlightuserdata(L, const_cast<reflect::Type*>(&t));
        lua_pushcclosure(L, strict_index, 1);
        lua_setfield(L, -2, "__index");
    }
    lua_setmetatable(L, -2);
}

void push_struct_table(lua_State* L, const uint8_t* base, const reflect::Type& t, int depth) {
    lua_createtable(L, 0, static_cast<int>(t.field_count));
    set_struct_meta(L, t);
    for (uint32_t i = 0; i < t.field_count; ++i) {
        const auto& f = reflect::kFields[t.first_field + i];
        if (f.kind == reflect::Kind::Bytes) continue;
        if (f.count > 1) {
            lua_createtable(L, static_cast<int>(f.count), 0);
            for (uint32_t e = 0; e < f.count; ++e) {
                push_field_value(L, base, f, e, depth);
                lua_rawseti(L, -2, static_cast<lua_Integer>(e + 1));
            }
        } else {
            push_field_value(L, base, f, 0, depth);
        }
        lua_setfield(L, -2, f.name);
    }
}

int l_on_event(lua_State* L) {
    Script* s = script_of(L);
    const char* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    const uint8_t kind = kind_arg(L, 3, "server");
    const reflect::Type* t = find_type(name);
    if (!t || !(t->flags & 2)) return luaL_error(L, "%s is not an engine event", name);
    const std::string key = std::to_string(kind) + ":" + t->short_name;  // the core knows events by short name
    {
        std::lock_guard<std::mutex> l(s->host->m);
        if (s->host->load_done && !s->host->subscribed.count(key)) {
            // the rest of the mod still runs; this event only arrives after a restart
            s->host->api->log(s->id.c_str(), "on_event(%s) is new since the game started: restart the game to receive it",
                              name);
            if (s->host->api->notify)
                s->host->api->notify("Lua mod %s: restart the game to receive %s", s->id.c_str(), name);
            return 0;
        }
        s->host->wanted.insert(key);
    }
    lua_pushvalue(L, 2);
    s->events[key].push_back(luaL_ref(L, LUA_REGISTRYINDEX));
    if (kind < 3) ++s->event_kind_subs[kind];
    return 0;
}

struct EventSlot {
    Host::Impl* host;
    std::string key;
    const reflect::Type* type;
    std::atomic<bool> size_logged{false};
};

void event_cb_body(EmfWorld* w, const void* event, uint32_t size, void* user) {
    auto* slot = static_cast<EventSlot*>(user);
    if (size != slot->type->size) {  // a game update changed the layout: the field offsets would be wrong
        if (!slot->size_logged.exchange(true))
            slot->host->api->log("lua", "the game's %s has %u bytes, not %u (a game update?): not given to Lua mods",
                                 slot->type->short_name, size, slot->type->size);
        return;
    }
    const int kind = slot->host->api->world_kind ? slot->host->api->world_kind(w) : EMF_WORLD_SERVER;
    for (auto& sp : slot->host->scripts) {
        Script& s = *sp;
        // A script that listens to no event of this world is not waited for: on the client world it may be busy in
        // the server world's step, and the game's frame waits for the client world.
        if (kind >= 0 && kind < 3 && s.event_kind_subs[kind].load() == 0) continue;
        std::lock_guard<std::recursive_mutex> l(s.m);
        if (!s.L || s.paused) continue;
        auto it = s.events.find(slot->key);
        if (it == s.events.end()) continue;
        const std::vector<int> refs = it->second;
        for (int ref : refs) {
            lua_rawgeti(s.L, LUA_REGISTRYINDEX, ref);
            push_world(s.L, s, w);
            push_struct_table(s.L, static_cast<const uint8_t*>(event), *slot->type, 0);
            if (!call_lua(s, 2)) break;
        }
        ++s.generation;
    }
}

// Called by EMF: nothing may escape into the game (a Lua error is a C++ exception).
void event_cb(EmfWorld* w, const void* event, uint32_t size, void* user) {
    try {
        event_cb_body(w, event, size, user);
    } catch (...) {
    }
}

int l_emits(lua_State* L) {
    Script* s = script_of(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    const uint8_t kind = kind_arg(L, 2, "server");
    std::lock_guard<std::mutex> l(s->host->m);
    const lua_Integer n = luaL_len(L, 1);
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_rawgeti(L, 1, i);
        const std::string given = luaL_checkstring(L, -1);
        lua_pop(L, 1);
        const reflect::Type* t = find_type(given);
        if (!t || !(t->flags & 2)) return luaL_error(L, "%s is not an engine event", given.c_str());
        const std::string name = t->short_name;  // the core knows events by short name
        if (s->host->load_done && !s->host->emits[kind].count(name)) {
            s->host->api->log(s->id.c_str(), "emits(%s) is new since the game started: restart the game to send it",
                              name.c_str());
            if (s->host->api->notify)
                s->host->api->notify("Lua mod %s: restart the game to send %s", s->id.c_str(), name.c_str());
            continue;
        }
        s->host->emits[kind].insert(name);
    }
    return 0;
}

// fields table -> dotted names ("targetTransform.position.x") with values
void flatten(lua_State* L, int index, const std::string& prefix, std::vector<QueuedEvent::Field>& out, int depth) {
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            const std::string name = prefix + lua_tostring(L, -2);
            if (lua_istable(L, -1) && depth < 8) {
                flatten(L, lua_gettop(L), name + ".", out, depth + 1);
            } else {
                QueuedEvent::Field f{name, 0, false, 0, 0.0, ""};
                if (lua_isboolean(L, -1)) f.b = lua_toboolean(L, -1) != 0;
                else if (lua_isinteger(L, -1)) f.kind = 1, f.i = lua_tointeger(L, -1);
                else if (lua_type(L, -1) == LUA_TNUMBER) f.kind = 2, f.n = lua_tonumber(L, -1);
                else if (lua_type(L, -1) == LUA_TSTRING) f.kind = 3, f.s = lua_tostring(L, -1);
                else luaL_error(L, "field %s: give a boolean, number or text", name.c_str());
                out.push_back(std::move(f));
            }
        }
        lua_pop(L, 1);
    }
}

int l_emit(lua_State* L) {
    Script* s = script_of(L);
    const uint8_t kind = kind_arg(L, 1, "server");
    const char* given = luaL_checkstring(L, 2);
    const reflect::Type* t = find_type(given);
    const std::string name = t ? t->short_name : given;  // as emits stored it
    {
        std::lock_guard<std::mutex> l(s->host->m);
        if (!s->host->emits[kind].count(name))
            return luaL_error(L, "%s is not in emf.emits (call emf.emits({'%s'}) at the start of main.lua)", name.c_str(),
                              name.c_str());
    }
    QueuedEvent e;
    e.name = name;
    if (lua_istable(L, 3)) flatten(L, 3, "", e.values, 0);
    std::lock_guard<std::mutex> l(s->host->m);
    if (s->host->outbox.size() >= 256) return luaL_error(L, "too many events waiting (more than 256)");
    s->host->outbox.emplace_back(kind, std::move(e));
    return 0;
}

// Writes `path` ("a.b.c") of the struct at `base` from a queued value; false (and *why) when it does not fit.
bool write_path(uint8_t* base, const reflect::Type& t, const std::string& path, const QueuedEvent::Field& v, std::string* why) {
    const size_t dot = path.find('.');
    const std::string head = path.substr(0, dot);
    const reflect::Field* f = find_field(t, head);
    if (!f) {
        if (why) *why = std::string(t.short_name) + " has no field " + head;
        return false;
    }
    if (dot != std::string::npos) {
        const reflect::Type* inner = f->kind == reflect::Kind::Struct ? type_at(f->type) : nullptr;
        if (!inner) {
            if (why) *why = head + " is not a struct";
            return false;
        }
        return write_path(base + f->offset, *inner, path.substr(dot + 1), v, why);
    }
    Value value;
    switch (v.kind) {
        case 0: value = Value::boolean(v.b); break;
        case 1: value = Value::integer(v.i); break;
        case 2: value = Value::number(v.n); break;
        default: value = Value::string(v.s);
    }
    if (f->kind == reflect::Kind::Struct) {
        const reflect::Field* one = single_scalar(type_at(f->type));
        if (!one) {
            if (why) *why = head + " is a struct: give a table of its fields";
            return false;
        }
        return write_field(base + f->offset, *one, 0, value, why);
    }
    return write_field(base, *f, 0, value, why);
}

struct ProgramSlot {
    Host::Impl* host;
    uint8_t kind;
};

// The host's program (a game worker thread): sends the queued events of its world kind. No Lua here.
void program_cb_body(void* ctx, void* user) {
    auto* slot = static_cast<ProgramSlot*>(user);
    Host::Impl* h = slot->host;
    std::vector<QueuedEvent> mine;
    {
        std::lock_guard<std::mutex> l(h->m);
        for (auto it = h->outbox.begin(); it != h->outbox.end();) {
            if (it->first == slot->kind) {
                mine.push_back(std::move(it->second));
                it = h->outbox.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto& e : mine) {
        const reflect::Type* t = find_type(e.name);
        if (!t) continue;
        auto* ev = static_cast<uint8_t*>(h->api->emit_event(ctx, e.name.c_str(), t->size));
        if (!ev) continue;
        for (const auto& v : e.values) {
            if (v.name == "timeStamp") continue;  // set by the game
            std::string why;
            if (!write_path(ev, *t, v.name, v, &why))
                h->api->log("lua", "%s.%s not sent: %s", e.name.c_str(), v.name.c_str(), why.c_str());
        }
    }
}

// Called by EMF: nothing may escape into the game (a Lua error is a C++ exception).
void program_cb(void* ctx, void* user) {
    try {
        program_cb_body(ctx, user);
    } catch (...) {
    }
}

// ---- the wheel ----
struct WheelSlot {
    Host::Impl* host;
    std::string id;
    int ref;
    uint64_t load;
};

void wheel_cb_body(EmfWorld* w, int choice, void* user) {
    auto* slot = static_cast<WheelSlot*>(user);
    Script* s = slot->host->find(slot->id);
    if (s) {
        std::lock_guard<std::recursive_mutex> l(s->m);
        if (s->L && !s->paused && slot->load == s->loads) {
            lua_rawgeti(s->L, LUA_REGISTRYINDEX, slot->ref);
            push_world(s->L, *s, w);
            if (choice >= 0) lua_pushinteger(s->L, choice + 1);
            else lua_pushnil(s->L);
            call_lua(*s, 2);
            ++s->generation;
            luaL_unref(s->L, LUA_REGISTRYINDEX, slot->ref);
        }
    }
    delete slot;
}

// Called by EMF: nothing may escape into the game (a Lua error is a C++ exception).
void wheel_cb(EmfWorld* w, int choice, void* user) {
    try {
        wheel_cb_body(w, choice, user);
    } catch (...) {
    }
}

int l_wheel(lua_State* L) {
    Script* s = script_of(L);
    const char* title = luaL_optstring(L, 1, "");
    luaL_checktype(L, 2, LUA_TTABLE);
    const char* key = luaL_optstring(L, 3, "");
    luaL_checktype(L, 4, LUA_TFUNCTION);
    std::vector<std::string> labels;
    const lua_Integer n = luaL_len(L, 2);
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_rawgeti(L, 2, i);
        labels.push_back(to_text(L, -1));
        lua_pop(L, 1);
    }
    std::vector<const char*> items;
    for (const auto& x : labels) items.push_back(x.c_str());
    uint32_t vk = 0;
    if (key[0]) {
        const std::string k = key;
        vk = emf::keys::parse_key(std::wstring(k.begin(), k.end()));
        if (!vk) return luaL_error(L, "%s is not a key", key);
    }
    lua_pushvalue(L, 4);
    auto* slot = new WheelSlot{s->host, s->id, luaL_ref(L, LUA_REGISTRYINDEX), s->loads};
    EmfWheel wh{};
    wh.size = sizeof wh;
    wh.title = title;
    wh.items = items.data();
    wh.count = static_cast<uint32_t>(items.size());
    wh.hold_key = vk;
    wh.world_kind = EMF_WORLD_CLIENT;
    wh.cb = wheel_cb;
    wh.user = slot;
    const bool ok = s->host->api->wheel_open && s->host->api->wheel_open(&wh);
    if (!ok) {
        luaL_unref(L, LUA_REGISTRYINDEX, slot->ref);
        delete slot;
    }
    lua_pushboolean(L, ok);
    return 1;
}

// ---- lua 1.13: mod windows (API v24) ----
bool has_v24(const EmfApi* a) {
    return a && a->version >= 24 && a->size >= offsetof(EmfApi, window_close) + sizeof(a->window_close);
}

struct WindowSlot {
    Host::Impl* host;
    std::string id;      // the script
    std::string window;  // the window's full id
    int ref;             // the Lua function
    uint64_t load;       // the script's load counter when opened (a reload drops the events)
};

const char* window_kind_name(uint32_t k) {
    return k == EMF_UI_CLICK ? "click" : k == EMF_UI_RIGHT_CLICK ? "right_click" : k == EMF_UI_CLOSED ? "closed" : "?";
}
const char* window_reason_name(uint32_t r) {
    return r == EMF_UI_BY_PLAYER ? "player" : r == EMF_UI_BY_MOD ? "mod" : "core";
}

void window_cb_body(EmfWorld* w, const EmfUiEvent* e, void* user) {
    auto* slot = static_cast<WindowSlot*>(user);
    const bool closed = e->kind == EMF_UI_CLOSED;
    Script* s = slot->host->find(slot->id);
    if (s) {
        std::lock_guard<std::recursive_mutex> l(s->m);
        const bool same_state = s->L && slot->load == s->loads;
        // a NULL world: no world runs (the main menu); the script is not asked
        if (same_state && !s->paused && w) {
            lua_rawgeti(s->L, LUA_REGISTRYINDEX, slot->ref);
            push_world(s->L, *s, w);
            lua_createtable(s->L, 0, 3);
            lua_pushstring(s->L, window_kind_name(e->kind));
            lua_setfield(s->L, -2, "kind");
            lua_pushstring(s->L, e->key ? e->key : "");
            lua_setfield(s->L, -2, "key");
            if (closed) {
                lua_pushstring(s->L, window_reason_name(e->reason));
                lua_setfield(s->L, -2, "reason");
            }
            call_lua(*s, 2);
            ++s->generation;
        }
        if (closed && same_state) {
            luaL_unref(s->L, LUA_REGISTRYINDEX, slot->ref);
            const auto it = s->windows.find(slot->window);
            if (it != s->windows.end()) s->windows.erase(it);
        }
    }
    if (closed) delete slot;
}

// Called by EMF: nothing may escape into the game (a Lua error is a C++ exception).
void window_cb(EmfWorld* w, const EmfUiEvent* e, void* user) {
    try {
        window_cb_body(w, e, user);
    } catch (...) {
    }
}

// A window description built from a Lua table; the strings live as long as it does.
struct LuaWindow {
    std::string id, title;
    std::deque<std::string> strings;
    std::vector<EmfUiWidget> widgets;
    EmfWindowDef def{};
    const char* keep(const std::string& s) {
        strings.push_back(s);
        return strings.back().c_str();
    }
};

// The full id: the script's id and a dot in front, unless it is there already.
std::string window_full_id(const Script& s, const char* id) {
    const std::string prefix = s.id + ".";
    const std::string given = id;
    return given.compare(0, prefix.size(), prefix) == 0 ? given : prefix + given;
}

uint32_t parse_colour(lua_State* L, const char* text) {
    const size_t n = std::strlen(text);
    if (text[0] != '#' || (n != 7 && n != 9)) luaL_error(L, "color must be \"#rrggbb\" or \"#rrggbbaa\", not %s", text);
    char* end = nullptr;
    const unsigned long v = std::strtoul(text + 1, &end, 16);
    if (!end || *end) luaL_error(L, "color must be \"#rrggbb\" or \"#rrggbbaa\", not %s", text);
    return n == 7 ? static_cast<uint32_t>((v << 8) | 0xFFu) : static_cast<uint32_t>(v);
}

// The window `id` from the table at `t` (title, width, place, close_key, world, rows). A bad row is an error.
void lua_window(lua_State* L, Script& s, const char* id, int t, LuaWindow* out) {
    static const char* const kRowFields[] = {"heading", "text",  "button",  "row",      "slot",  "bar",
                                             "separator", "value", "count", "tooltip",  "color", "same_line",
                                             "selected", "disabled", "dim", "empty", "locked",
                                             "image", "icon", "fps", "height", "key"};  // lua 1.15 pictures
    out->id = window_full_id(s, id);
    lua_getfield(L, t, "title");
    if (!lua_isnil(L, -1)) out->title = luaL_checkstring(L, -1);
    lua_pop(L, 1);
    EmfWindowDef& d = out->def;
    d.size = sizeof d;
    lua_getfield(L, t, "width");
    d.width = lua_isnil(L, -1) ? 0.0f : static_cast<float>(luaL_checknumber(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, t, "place");
    if (!lua_isnil(L, -1)) {
        const std::string p = luaL_checkstring(L, -1);
        if (p == "centre" || p == "center") d.place = EMF_UI_PLACE_CENTRE;
        else if (p == "left") d.place = EMF_UI_PLACE_LEFT;
        else if (p == "right") d.place = EMF_UI_PLACE_RIGHT;
        else luaL_error(L, "place must be \"centre\", \"left\" or \"right\", not %s", p.c_str());
    }
    lua_pop(L, 1);
    lua_getfield(L, t, "close_key");
    if (!lua_isnil(L, -1)) {
        const std::string k = luaL_checkstring(L, -1);  // "": a key setting left unset, no close key
        d.close_key = k.empty() ? 0u : emf::keys::parse_key(std::wstring(k.begin(), k.end()));
        if (!k.empty() && !d.close_key) luaL_error(L, "%s is not a key", k.c_str());
    }
    lua_pop(L, 1);
    const EmfApi* a = s.host->api;
    d.world_kind = (a->server_world && a->server_world()) ? EMF_WORLD_SERVER : EMF_WORLD_CLIENT;
    lua_getfield(L, t, "world");
    if (!lua_isnil(L, -1)) {
        const std::string k = luaL_checkstring(L, -1);
        if (k == "server") d.world_kind = EMF_WORLD_SERVER;
        else if (k == "client") d.world_kind = EMF_WORLD_CLIENT;
        else luaL_error(L, "world must be \"server\" or \"client\", not %s", k.c_str());
    }
    lua_pop(L, 1);
    lua_getfield(L, t, "rows");
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TTABLE);
        const int rows = lua_gettop(L);
        const lua_Integer n = luaL_len(L, rows);
        for (lua_Integer i = 1; i <= n; ++i) {
            lua_rawgeti(L, rows, i);
            if (!lua_istable(L, -1)) luaL_error(L, "row %d is not a table", static_cast<int>(i));
            const int row = lua_gettop(L);
            lua_pushnil(L);
            while (lua_next(L, row)) {  // every field is one EMF knows
                const char* k = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : nullptr;
                bool known = false;
                for (const char* f : kRowFields) known |= k && std::strcmp(k, f) == 0;
                if (!known) luaL_error(L, "row %d: unknown field %s", static_cast<int>(i), k ? k : "(not a name)");
                lua_pop(L, 1);
            }
            EmfUiWidget w{};
            const auto field = [&](const char* name) {
                lua_getfield(L, row, name);
                return !lua_isnil(L, -1);
            };
            const auto flag = [&](const char* name, uint32_t bit) {
                lua_getfield(L, row, name);
                if (lua_toboolean(L, -1)) w.flags |= bit;
                lua_pop(L, 1);
            };
            const char* text_field = "text";
            if (field("heading")) {
                w.type = EMF_UI_HEADING;
                text_field = "heading";
            } else if (lua_pop(L, 1), field("button")) {
                w.type = EMF_UI_BUTTON;
                w.key = out->keep(luaL_checkstring(L, -1));
            } else if (lua_pop(L, 1), field("row")) {
                w.type = EMF_UI_ROW;
                w.key = out->keep(luaL_checkstring(L, -1));
            } else if (lua_pop(L, 1), field("slot")) {
                w.type = EMF_UI_SLOT;
                w.key = out->keep(luaL_checkstring(L, -1));
            } else if (lua_pop(L, 1), field("bar")) {
                w.type = EMF_UI_BAR;
                text_field = "bar";
            } else if (lua_pop(L, 1), field("separator")) {
                w.type = EMF_UI_SEPARATOR;
            } else if (lua_pop(L, 1), field("image")) {  // lua 1.15: {image = emf.image(...), height, fps, key}
                w.type = EMF_UI_IMAGE;
                w.icon = static_cast<uint32_t>(luaL_checkinteger(L, -1));
                lua_getfield(L, row, "key");  // a picture's key is its own field (image holds the picture)
                if (!lua_isnil(L, -1)) w.key = out->keep(luaL_checkstring(L, -1));
                lua_pop(L, 1);
            } else if (lua_pop(L, 1), field("text")) {
                w.type = EMF_UI_TEXT;
            } else {
                luaL_error(L, "row %d has no kind (text, heading, button, row, slot, bar, separator or image)",
                           static_cast<int>(i));
            }
            lua_pop(L, 1);
            lua_getfield(L, row, "icon");  // a slot's or a row's picture (lua 1.15)
            if (!lua_isnil(L, -1)) w.icon = static_cast<uint32_t>(luaL_checkinteger(L, -1));
            lua_pop(L, 1);
            lua_getfield(L, row, "fps");
            if (!lua_isnil(L, -1)) w.fps = static_cast<uint32_t>(luaL_checkinteger(L, -1));
            lua_pop(L, 1);
            lua_getfield(L, row, "height");
            if (!lua_isnil(L, -1)) w.height = static_cast<float>(luaL_checknumber(L, -1));
            lua_pop(L, 1);
            lua_getfield(L, row, text_field);
            if (!lua_isnil(L, -1)) w.text = out->keep(luaL_checkstring(L, -1));
            lua_pop(L, 1);
            lua_getfield(L, row, "tooltip");
            if (!lua_isnil(L, -1)) w.tooltip = out->keep(luaL_checkstring(L, -1));
            lua_pop(L, 1);
            lua_getfield(L, row, "count");
            if (!lua_isnil(L, -1)) w.count = static_cast<uint32_t>(luaL_checkinteger(L, -1));
            lua_pop(L, 1);
            lua_getfield(L, row, "value");  // a bar's fill, 0..1
            if (!lua_isnil(L, -1)) {
                const double v = luaL_checknumber(L, -1);
                w.value = static_cast<uint32_t>((v < 0 ? 0 : v > 1 ? 1 : v) * 1000.0 + 0.5);
            }
            lua_pop(L, 1);
            lua_getfield(L, row, "color");
            if (!lua_isnil(L, -1)) w.rgba = parse_colour(L, luaL_checkstring(L, -1));
            lua_pop(L, 1);
            flag("same_line", EMF_UI_SAME_LINE);
            flag("selected", EMF_UI_SELECTED);
            flag("disabled", EMF_UI_DISABLED);
            flag("dim", EMF_UI_DIM);
            flag("empty", EMF_UI_EMPTY);
            flag("locked", EMF_UI_LOCKED);
            out->widgets.push_back(w);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    d.id = out->id.c_str();
    d.title = out->title.c_str();
    d.widgets = out->widgets.data();
    d.count = static_cast<uint32_t>(out->widgets.size());
    d.widget_size = sizeof(EmfUiWidget);
}

bool has_v26(const EmfApi* a) {
    return a && a->version >= 26 && a->size >= offsetof(EmfApi, image_load) + sizeof(a->image_load) && a->image_load;
}

// emf.image(path) (lua 1.15): a picture for windows, a .png or a folder of frame .pngs inside the script's own mod folder
// ("pics/wolf.png"), or "portrait:<key>" (EMF's portrait cache). The handle, or 0 (refused, or a core before v26). A
// path that leaves the mod's folder is an error.
int l_image(lua_State* L) {
    Script* s = script_of(L);
    const std::string p = luaL_checkstring(L, 1);
    std::string arg;
    if (p.rfind("portrait:", 0) == 0) {
        arg = p;
    } else {
        bool ok = !p.empty() && p[0] != '/' && p[0] != '\\' && p.find(':') == std::string::npos;
        std::string part;
        for (size_t i = 0; ok && i <= p.size(); ++i) {  // no ".." part anywhere
            if (i == p.size() || p[i] == '/' || p[i] == '\\') {
                ok = part != "..";
                part.clear();
            } else {
                part += p[i];
            }
        }
        if (!ok) luaL_error(L, "emf.image: a path inside the mod's folder (\"pics/wolf.png\") or \"portrait:<key>\", not %s", p.c_str());
        std::string rel = p;
        for (char& c : rel)
            if (c == '/') c = '\\';
        arg = utf8(s->folder) + "\\" + rel;
    }
    const EmfApi* a = s->host->api;
    const uint32_t h = has_v26(a) ? a->image_load(arg.c_str()) : 0;
    lua_pushinteger(L, static_cast<lua_Integer>(h));  // 0 when refused: a row with it keeps its place, draws nothing
    return 1;
}

// emf.window(id, {title, width, place, close_key, world, rows = {...}}, fn(world, ev)) (lua 1.13): true when it opened.
// The id gets the mod's id in front ("stable" -> "beastmaster.stable"). ev: kind ("click", "right_click", "closed"),
// key, reason ("player", "mod", "core"; closed only).
int l_window(lua_State* L) {
    Script* s = script_of(L);
    const char* id = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    LuaWindow w;
    lua_window(L, *s, id, 2, &w);
    const EmfApi* a = s->host->api;
    if (!has_v24(a) || !a->window_open) {
        lua_pushboolean(L, false);
        return 1;
    }
    lua_pushvalue(L, 3);
    auto* slot = new WindowSlot{s->host, s->id, w.id, luaL_ref(L, LUA_REGISTRYINDEX), s->loads};
    w.def.cb = window_cb;
    w.def.user = slot;
    const bool ok = a->window_open(&w.def);
    if (ok) {
        s->windows.insert(w.id);
    } else {  // refused at once: no event comes
        luaL_unref(L, LUA_REGISTRYINDEX, slot->ref);
        delete slot;
    }
    lua_pushboolean(L, ok);
    return 1;
}

// emf.window_update(id, {title, ..., rows}) (lua 1.13): new contents for the open window; false when it is not open.
int l_window_update(lua_State* L) {
    Script* s = script_of(L);
    const char* id = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    LuaWindow w;
    lua_window(L, *s, id, 2, &w);
    const EmfApi* a = s->host->api;
    lua_pushboolean(L, has_v24(a) && a->window_update && a->window_update(&w.def));
    return 1;
}

// emf.window_close(id) (lua 1.13): closes the window (fn gets "closed", reason "mod"); false when it is not open.
int l_window_close(lua_State* L) {
    Script* s = script_of(L);
    const std::string id = window_full_id(*s, luaL_checkstring(L, 1));
    const EmfApi* a = s->host->api;
    lua_pushboolean(L, has_v24(a) && a->window_close && a->window_close(id.c_str()));
    return 1;
}

// ---- lua 1.10: world settings (API v21, docs/modding/world_settings.md) ----
bool has_v21(const EmfApi* a) {
    return a && a->size >= offsetof(EmfApi, world_setting_info) + sizeof(a->world_setting_info);
}
constexpr const char* kWsTooOld = "this EMF is too old for world settings (API 21)";

const char* ws_type_name(uint32_t t) {
    switch (t) {
    case EMF_WS_BOOL: return "bool";
    case EMF_WS_ENUM: return "enum";
    case EMF_WS_MINUTES: return "minutes";
    case EMF_WS_WHOLE: return "whole";
    default: return "float";
    }
}

// The setting `key` (key or alias, any case) as the core lists it; false when unknown or no world is hosted.
bool ws_find(const EmfApi* a, const char* key, EmfWorldSettingInfo* out) {
    for (uint32_t i = 0;; ++i) {
        out->size = sizeof *out;
        if (!a->world_setting_info(i, out)) return false;
        if (_stricmp(key, out->key) == 0 || (out->alias[0] && _stricmp(key, out->alias) == 0)) return true;
    }
}

std::string ws_number(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

// emf.world_settings.get(key): the running world's value (durations in minutes), or nil (unknown, no world hosted here).
int l_ws_get(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    const EmfApi* a = script_of(L)->host->api;
    double v = 0;
    if (!has_v21(a) || !a->world_setting_get || !a->world_setting_get(key, &v)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, v);
    return 1;
}

// emf.world_settings.set(key, value [, persist]): true when queued for the next server step; false and why when refused.
int l_ws_set(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    const double value = lua_isboolean(L, 2) ? (lua_toboolean(L, 2) ? 1.0 : 0.0) : luaL_checknumber(L, 2);
    const bool persist = lua_toboolean(L, 3) != 0;
    const EmfApi* a = script_of(L)->host->api;
    if (!has_v21(a) || !a->world_setting_set) {
        lua_pushboolean(L, false);
        lua_pushstring(L, kWsTooOld);
        return 2;
    }
    if (a->world_setting_set(key, value, persist ? EMF_WS_PERSIST : 0u)) {
        lua_pushboolean(L, true);
        return 1;
    }
    std::string why;
    EmfWorldSettingInfo info{};
    info.size = sizeof info;
    if (!a->world_setting_info(0, &info))
        why = "no world is hosted here";
    else if (!ws_find(a, key, &info))
        why = std::string("unknown setting: ") + key;
    else if (value < info.min || value > info.max)
        why = std::string(key) + ": allowed " + ws_number(info.min) + " to " + ws_number(info.max);
    else
        why = std::string(key) + ": refused (see emf_core.log)";
    lua_pushboolean(L, false);
    lua_pushstring(L, why.c_str());
    return 2;
}

// emf.world_settings.reset([key] [, persist]): back to the world's own value; no key (or "all"): every setting.
int l_ws_reset(lua_State* L) {
    const char* key = nullptr;
    int persist_at = 1;
    if (lua_type(L, 1) == LUA_TSTRING) {
        key = lua_tostring(L, 1);
        persist_at = 2;
    } else if (lua_isnil(L, 1) && lua_gettop(L) >= 2) {
        persist_at = 2;
    }
    const bool persist = lua_toboolean(L, persist_at) != 0;
    const EmfApi* a = script_of(L)->host->api;
    lua_pushboolean(L, has_v21(a) && a->world_setting_reset && a->world_setting_reset(key, persist ? EMF_WS_PERSIST : 0u));
    return 1;
}

// emf.world_settings.list(): {{key, alias, group, type, min, max, value, world, changed, needs_client}, ...}; empty
// when no world is hosted here.
int l_ws_list(lua_State* L) {
    const EmfApi* a = script_of(L)->host->api;
    lua_newtable(L);
    if (!has_v21(a) || !a->world_setting_info) return 1;
    EmfWorldSettingInfo info{};
    for (uint32_t i = 0;; ++i) {
        info.size = sizeof info;
        if (!a->world_setting_info(i, &info)) break;
        lua_createtable(L, 0, 10);
        lua_pushstring(L, info.key);
        lua_setfield(L, -2, "key");
        lua_pushstring(L, info.alias);
        lua_setfield(L, -2, "alias");
        lua_pushstring(L, info.group);
        lua_setfield(L, -2, "group");
        lua_pushstring(L, ws_type_name(info.type));
        lua_setfield(L, -2, "type");
        lua_pushnumber(L, info.min);
        lua_setfield(L, -2, "min");
        lua_pushnumber(L, info.max);
        lua_setfield(L, -2, "max");
        lua_pushnumber(L, info.value);
        lua_setfield(L, -2, "value");
        lua_pushnumber(L, info.world);
        lua_setfield(L, -2, "world");
        lua_pushboolean(L, (info.flags & EMF_WS_CHANGED) != 0);
        lua_setfield(L, -2, "changed");
        lua_pushboolean(L, (info.flags & EMF_WS_NEEDS_CLIENT) != 0);
        lua_setfield(L, -2, "needs_client");
        lua_rawseti(L, -2, static_cast<lua_Integer>(i) + 1);
    }
    return 1;
}

void ws_cb_body(const char* key, double value, void* user) {
    auto* h = static_cast<Host::Impl*>(user);
    for (auto& sp : h->scripts) {
        Script& s = *sp;
        std::lock_guard<std::recursive_mutex> l(s.m);
        if (!s.L || s.paused || s.ws_cbs.empty()) continue;
        const std::vector<int> refs = s.ws_cbs;
        for (int ref : refs) {
            lua_rawgeti(s.L, LUA_REGISTRYINDEX, ref);
            lua_pushstring(s.L, key ? key : "");
            lua_pushnumber(s.L, value);
            if (!call_lua(s, 2)) break;
        }
        ++s.generation;
    }
}

// Called by EMF: nothing may escape into the game (a Lua error is a C++ exception).
void ws_cb(const char* key, double value, void* user) {
    try {
        ws_cb_body(key, value, user);
    } catch (...) {
    }
}

// emf.on_world_setting_changed(fn): fn(key, value) for each world setting the core wrote (anyone's change, or a kept
// value applied after a load), on the server world after that step. False on an older core.
int l_on_world_setting_changed(lua_State* L) {
    Script* s = script_of(L);
    luaL_checktype(L, 1, LUA_TFUNCTION);
    Host::Impl* h = s->host;
    const EmfApi* a = h->api;
    int state = h->ws_hooked.load();
    if (state == 0) {  // the first call of any script subscribes the host once, for the process
        const bool ok = has_v21(a) && a->on_world_setting_changed && a->on_world_setting_changed(ws_cb, h);
        int expected = 0;
        h->ws_hooked.compare_exchange_strong(expected, ok ? 1 : -1);
        state = h->ws_hooked.load();
    }
    if (state != 1) {
        lua_pushboolean(L, false);
        return 1;
    }
    lua_pushvalue(L, 1);
    s->ws_cbs.push_back(luaL_ref(L, LUA_REGISTRYINDEX));
    lua_pushboolean(L, true);
    return 1;
}

// ---- wheel groups (lua 1.9): several mods' sections on one wheel ----
// emf.wheel_section(group, title, labels, pick): the calling mod's section of `group` (replaced; an empty list or nil
// removes it). emf.wheel_group(group, title, key): one wheel with every section; a pick runs the owner's `pick` with
// the index into its own labels. The labels are kept as text here, so opening never calls into another mod's state.
int l_wheel_section(lua_State* L) {
    Script* s = script_of(L);
    const std::string group = luaL_checkstring(L, 1);
    const std::string title = luaL_optstring(L, 2, "");
    std::vector<std::string> labels;
    if (lua_istable(L, 3)) {
        const lua_Integer n = luaL_len(L, 3);
        for (lua_Integer i = 1; i <= n; ++i) {
            lua_rawgeti(L, 3, i);
            labels.push_back(to_text(L, -1));
            lua_pop(L, 1);
        }
    } else if (!lua_isnoneornil(L, 3)) {
        return luaL_error(L, "wheel_section: the labels must be a list of texts (or nil to remove the section)");
    }
    if (!labels.empty()) luaL_checktype(L, 4, LUA_TFUNCTION);
    int fresh = LUA_NOREF;
    if (!labels.empty()) {
        lua_pushvalue(L, 4);
        fresh = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    int old = LUA_NOREF;
    {
        Host::Impl& h = *s->host;
        std::lock_guard<std::mutex> l(h.m);
        auto& sections = h.wheel_groups[group];
        auto it = std::find_if(sections.begin(), sections.end(), [&](const WheelSection& x) { return x.script == s->id; });
        if (it != sections.end()) {
            if (it->load == s->loads) old = it->pick;  // a ref of an older load died with its state
            if (labels.empty()) sections.erase(it);
            else *it = WheelSection{s->id, s->loads, title, labels, fresh};
        } else if (!labels.empty()) {
            sections.push_back(WheelSection{s->id, s->loads, title, labels, fresh});
        }
    }
    if (old != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, old);
    lua_pushboolean(L, 1);
    return 1;
}

struct GroupPick {
    std::string script;  // the owning mod
    uint64_t load;       // its load when the wheel opened: a pick after a reload is dropped
    int index;           // 1-based index into the labels it had published then
    std::string label;   // the label shown (without the section's title)
};

struct GroupSlot {
    Host::Impl* host;
    std::string group;
    std::vector<GroupPick> picks;  // per wheel item
};

void group_cb_body(EmfWorld* w, int choice, void* user) {
    std::unique_ptr<GroupSlot> slot(static_cast<GroupSlot*>(user));
    if (choice < 0 || choice >= static_cast<int>(slot->picks.size())) return;  // cancelled
    const GroupPick& p = slot->picks[static_cast<size_t>(choice)];
    Script* s = slot->host->find(p.script);
    if (!s) return;
    std::lock_guard<std::recursive_mutex> l(s->m);
    if (!s->L || s->paused || s->loads != p.load) return;  // the mod reloaded since: the item meant its old code
    int pick = LUA_NOREF;
    {
        std::lock_guard<std::mutex> g(slot->host->m);
        auto it = slot->host->wheel_groups.find(slot->group);
        if (it != slot->host->wheel_groups.end())
            for (const auto& x : it->second)
                if (x.script == p.script && x.load == p.load) pick = x.pick;  // removed since: none
    }
    if (pick == LUA_NOREF) return;
    lua_rawgeti(s->L, LUA_REGISTRYINDEX, pick);
    push_world(s->L, *s, w);
    lua_pushinteger(s->L, p.index);
    lua_pushlstring(s->L, p.label.data(), p.label.size());  // the mod can tell what was shown if its list moved
    call_lua(*s, 3);
    ++s->generation;
}

// Called by EMF: nothing may escape into the game (a Lua error is a C++ exception).
void group_cb(EmfWorld* w, int choice, void* user) {
    try {
        group_cb_body(w, choice, user);
    } catch (...) {
    }
}

int l_wheel_group(lua_State* L) {
    Script* s = script_of(L);
    const std::string group = luaL_checkstring(L, 1);
    const char* title = luaL_optstring(L, 2, "");
    const char* key = luaL_optstring(L, 3, "");
    uint32_t vk = 0;
    if (key[0]) {
        const std::string k = key;
        vk = emf::keys::parse_key(std::wstring(k.begin(), k.end()));
        if (!vk) return luaL_error(L, "%s is not a key", key);
    }
    std::vector<WheelSection> sections;
    {
        std::lock_guard<std::mutex> l(s->host->m);
        auto it = s->host->wheel_groups.find(group);
        if (it != s->host->wheel_groups.end()) sections = it->second;
    }
    // only sections of mods that run on the load that published them
    sections.erase(std::remove_if(sections.begin(), sections.end(),
                                  [&](const WheelSection& x) {
                                      if (x.script == s->id) return x.load != s->loads;
                                      Script* o = s->host->find(x.script);
                                      return !o || o->paused || x.load != o->loads;
                                  }),
                   sections.end());
    auto slot = std::make_unique<GroupSlot>();
    slot->host = s->host;
    slot->group = group;
    std::vector<std::string> labels;
    for (const auto& x : sections)
        for (size_t i = 0; i < x.labels.size(); ++i) {
            labels.push_back(sections.size() > 1 ? x.title + ": " + x.labels[i] : x.labels[i]);
            slot->picks.push_back(GroupPick{x.script, x.load, static_cast<int>(i) + 1, x.labels[i]});
        }
    if (labels.empty() || labels.size() > 64) {  // the wheel takes 1-64 entries
        lua_pushboolean(L, 0);
        return 1;
    }
    std::vector<const char*> items;
    for (const auto& x : labels) items.push_back(x.c_str());
    EmfWheel wh{};
    wh.size = sizeof wh;
    wh.title = title;
    wh.items = items.data();
    wh.count = static_cast<uint32_t>(items.size());
    wh.hold_key = vk;
    wh.world_kind = EMF_WORLD_CLIENT;
    wh.cb = group_cb;
    wh.user = slot.get();
    const bool ok = s->host->api->wheel_open && s->host->api->wheel_open(&wh);
    if (ok) slot.release();  // the callback owns it now
    lua_pushboolean(L, ok);
    return 1;
}

// ---- lua 1.14: E prompts (API v25) ----
bool has_v25(const EmfApi* a) {
    return a && a->version >= 25 && a->size >= offsetof(EmfApi, offer_remove) + sizeof(a->offer_remove) && a->offer;
}

// One offer's Lua function, alive while the core may call it (until the offer is removed or replaced). To the core
// every script is one owner (the Lua host); the host keeps which script offered which entity.
struct OfferSlot {
    Host::Impl* host;
    std::string id;
    int ref;          // the Lua function (LUA_NOREF: none)
    uint64_t load;    // the script's load counter when offered (a reload drops the calls)
    uint32_t session;  // the server session it was offered in
};

uint32_t session_now(const EmfApi* a) { return a && a->server_session ? a->server_session() : 0; }

void offer_cb_body(EmfWorld* w, uint32_t player, uint32_t entity, void* user) {
    auto* slot = static_cast<OfferSlot*>(user);
    if (!slot || slot->ref == LUA_NOREF) return;
    Script* s = slot->host->find(slot->id);
    if (!s) return;
    std::lock_guard<std::recursive_mutex> l(s->m);
    if (!s->L || s->paused || slot->load != s->loads) return;
    lua_rawgeti(s->L, LUA_REGISTRYINDEX, slot->ref);
    push_world(s->L, *s, w);
    lua_pushinteger(s->L, player);
    lua_pushinteger(s->L, entity);
    call_lua(*s, 3);
    ++s->generation;
}

// Called by EMF: nothing may escape into the game.
void offer_cb(EmfWorld* w, uint32_t player, uint32_t entity, void* user) {
    try {
        offer_cb_body(w, player, entity, user);
    } catch (...) {
    }
}

void free_offer_slot(OfferSlot* slot) {
    if (!slot) return;
    if (Script* s = slot->host->find(slot->id)) {
        std::lock_guard<std::recursive_mutex> l(s->m);
        if (s->L && slot->load == s->loads && slot->ref != LUA_NOREF) luaL_unref(s->L, LUA_REGISTRYINDEX, slot->ref);
    }
    delete slot;
}

// emf.offered(world, id) (lua 1.15): true while this script's offer on `id` is in EMF's hands (not removed, not dropped
// by EMF: then emf.offer again takes it back).
int l_offered(lua_State* L) {
    Script* s = script_of(L);
    EmfWorld* w = world_arg(L, 1);
    const auto entity = static_cast<uint32_t>(luaL_checkinteger(L, 2));
    Host::Impl& h = *s->host;
    bool mine = false;
    {
        std::lock_guard<std::mutex> g(h.m);
        const auto it = h.offer_slots.find(entity);
        mine = it != h.offer_slots.end() && static_cast<const OfferSlot*>(it->second)->id == s->id;
    }
    const EmfApi* a = h.api;
    const bool v26 = a && a->version >= 26 && a->size >= offsetof(EmfApi, offer_active) + sizeof(a->offer_active);
    lua_pushboolean(L, mine && v26 && a->offer_active && a->offer_active(w, entity));
    return 1;
}

struct VerbName {
    const char* name;
    uint32_t id;
};
constexpr VerbName kVerbs[] = {
    {"use", EMF_VERB_USE},         {"open", EMF_VERB_OPEN},     {"talk", EMF_VERB_TALK},   {"read", EMF_VERB_READ},
    {"sit", EMF_VERB_SIT},         {"sleep", EMF_VERB_SLEEP},   {"pet", EMF_VERB_PET},     {"touch", EMF_VERB_TOUCH},
    {"activate", EMF_VERB_ACTIVATE}, {"press", EMF_VERB_PRESS}, {"enter", EMF_VERB_ENTER}, {"inspect", EMF_VERB_INSPECT},
    {"take", EMF_VERB_TAKE},       {"collect", EMF_VERB_COLLECT}, {"rest", EMF_VERB_REST}, {"release", EMF_VERB_RELEASE},
};

// The options table {verb = "Open" | id, text = "...", sequence = "touch" | "button" | id, range = m, low_priority = b,
// build_mode = b} as an EmfOffer; an unknown key or verb name is an error. `text` keeps the words alive for the call.
EmfOffer offer_options(lua_State* L, int opts, std::string& text) {
    EmfOffer o{};
    o.size = sizeof o;
    if (!opts) return o;
    lua_pushnil(L);
    while (lua_next(L, opts)) {
        if (lua_type(L, -2) != LUA_TSTRING) luaL_error(L, "emf.offer: the options' keys are names");
        const std::string key = lua_tostring(L, -2);
        if (key == "verb") {
            if (lua_type(L, -1) == LUA_TNUMBER) {
                o.verb = static_cast<uint32_t>(lua_tointeger(L, -1));
            } else {
                std::string name = luaL_checkstring(L, -1);
                for (auto& c : name) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                o.verb = 0;
                for (const auto& v : kVerbs)
                    if (name == v.name) o.verb = v.id;
                if (!o.verb) luaL_error(L, "emf.offer: no verb named %s (Use, Open, Talk, Read, Sit, Sleep, Pet, Touch, Activate, Press, Enter, Inspect, Take, Collect, Rest, Release, or a loca id)", lua_tostring(L, -1));
            }
        } else if (key == "text") {
            size_t n = 0;
            const char* t = luaL_checklstring(L, -1, &n);
            if (n == 0 || n > 63) luaL_error(L, "emf.offer: text must be 1 to 63 bytes");
            text.assign(t, n);
        } else if (key == "sequence") {  // only the neutral ones: a script never plays an arbitrary sequence for players
            const std::string seq = luaL_checkstring(L, -1);
            if (seq == "touch") o.sequence = EMF_SEQUENCE_TOUCH;
            else if (seq == "button") o.sequence = EMF_SEQUENCE_BUTTON;
            else luaL_error(L, "emf.offer: sequence is \"touch\" or \"button\"");
        } else if (key == "range") {
            const lua_Integer m = luaL_checkinteger(L, -1);
            if (m < 1 || m > 255) luaL_error(L, "emf.offer: range is 1 to 255 metres");
            o.range_m = static_cast<uint8_t>(m);
        } else if (key == "low_priority") {
            if (lua_toboolean(L, -1)) o.flags |= EMF_OFFER_LOW_PRIORITY;
        } else if (key == "build_mode") {
            if (lua_toboolean(L, -1)) o.flags |= EMF_OFFER_BUILD_MODE;
        } else {
            luaL_error(L, "emf.offer: no option named %s", key.c_str());
        }
        lua_pop(L, 1);
    }
    return o;
}

// The arguments after the position: an options table and/or a function, in that order (both may be left out).
void offer_tail(lua_State* L, int first, int& opts, int& fn) {
    opts = fn = 0;
    for (int i = first; i <= first + 1; ++i) {
        if (lua_istable(L, i) && !opts && !fn) opts = i;
        else if (lua_isfunction(L, i) && !fn) fn = i;
        else if (!lua_isnoneornil(L, i)) luaL_error(L, "emf.offer: an options table and a function were expected");
    }
}

// A slot for a new offer of script s (the function at fn, if any).
OfferSlot* new_offer_slot(lua_State* L, Script* s, int fn) {
    auto* slot = new OfferSlot{s->host, s->id, LUA_NOREF, s->loads, session_now(s->host->api)};
    if (fn) {
        lua_pushvalue(L, fn);
        slot->ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    return slot;
}

// The slot of `entity` in this session: another script's refuses (nullptr in *other's id), one of an older session is
// forgotten (the core forgot it with that world).
bool entity_free_for(Host::Impl& h, Script* s, uint32_t entity, OfferSlot** mine) {
    std::lock_guard<std::mutex> g(h.m);
    *mine = nullptr;
    auto it = h.offer_slots.find(entity);
    if (it == h.offer_slots.end()) return true;
    auto* os = static_cast<OfferSlot*>(it->second);
    if (os->session != session_now(h.api)) return true;
    if (os->id != s->id) return false;
    *mine = os;
    return true;
}

// The core took the offer: the slot is the entity's now (a slot it replaced is freed; a queued removal of the entity
// from the script's last load no longer applies).
void keep_offer_slot(Host::Impl& h, Script* s, uint32_t entity, OfferSlot* slot) {
    OfferSlot* old = nullptr;
    {
        std::lock_guard<std::mutex> g(h.m);
        auto it = h.offer_slots.find(entity);
        if (it != h.offer_slots.end() && it->second != slot) old = static_cast<OfferSlot*>(it->second);
        h.offer_slots[entity] = slot;
        h.offer_drops.erase(std::remove_if(h.offer_drops.begin(), h.offer_drops.end(),
                                           [&](const std::pair<uint32_t, void*>& d) { return d.first == entity; }),
                            h.offer_drops.end());
    }
    s->offers.insert(entity);
    if (old) free_offer_slot(old);
}

// emf.offer(world, id [, opts] [, fn(world, player, entity)]) (lua 1.14): true when the core took it.
int l_offer(lua_State* L) {
    Script* s = script_of(L);
    EmfWorld* w = world_arg(L, 1);
    const auto entity = static_cast<uint32_t>(luaL_checkinteger(L, 2));
    int opts = 0, fn = 0;
    offer_tail(L, 3, opts, fn);
    std::string text;
    EmfOffer o = offer_options(L, opts, text);
    if (!text.empty()) o.text = text.c_str();
    const EmfApi* a = s->host->api;
    if (!has_v25(a)) {
        lua_pushboolean(L, false);
        return 1;
    }
    OfferSlot* mine = nullptr;
    if (!entity_free_for(*s->host, s, entity, &mine)) {
        if (a->log) a->log(s->id.c_str(), "emf.offer(%u) refused: another Lua mod's offer is on it", entity);
        lua_pushboolean(L, false);
        return 1;
    }
    OfferSlot* slot = new_offer_slot(L, s, fn);
    if (!a->offer(w, entity, &o, offer_cb, slot)) {
        free_offer_slot(slot);
        lua_pushboolean(L, false);
        return 1;
    }
    keep_offer_slot(*s->host, s, entity, slot);
    lua_pushboolean(L, true);
    return 1;
}

// emf.spawn_offer(world, guid, x, y, z [, opts] [, fn]) (lua 1.14): the entity id, or nil.
int l_spawn_offer(lua_State* L) {
    Script* s = script_of(L);
    EmfWorld* w = world_arg(L, 1);
    const char* g = luaL_checkstring(L, 2);
    uint8_t guid[16];
    if (!emf::parse_guid(g, guid)) return luaL_error(L, "%s is not a GUID (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx)", g);
    const double x = luaL_checknumber(L, 3), y = luaL_checknumber(L, 4), z = luaL_checknumber(L, 5);
    int opts = 0, fn = 0;
    offer_tail(L, 6, opts, fn);
    std::string text;
    EmfOffer o = offer_options(L, opts, text);
    if (!text.empty()) o.text = text.c_str();
    const EmfApi* a = s->host->api;
    if (!has_v25(a) || !a->spawn_offer) {
        lua_pushnil(L);
        return 1;
    }
    OfferSlot* slot = new_offer_slot(L, s, fn);
    const uint32_t id = a->spawn_offer(w, guid, x, y, z, &o, offer_cb, slot);
    if (!id) {
        free_offer_slot(slot);
        lua_pushnil(L);
        return 1;
    }
    keep_offer_slot(*s->host, s, id, slot);
    lua_pushinteger(L, id);
    return 1;
}

// emf.offer_remove(world, id) (lua 1.14): true when this script's offer was there.
int l_offer_remove(lua_State* L) {
    Script* s = script_of(L);
    EmfWorld* w = world_arg(L, 1);
    const auto entity = static_cast<uint32_t>(luaL_checkinteger(L, 2));
    const EmfApi* a = s->host->api;
    Host::Impl& h = *s->host;
    OfferSlot* mine = nullptr;
    if (!has_v25(a) || !a->offer_remove || !entity_free_for(h, s, entity, &mine) || !mine) {
        lua_pushboolean(L, false);
        return 1;
    }
    const bool ok = a->offer_remove(w, entity);
    {  // taken off, or the core had dropped it already (the entity went): either way the slot is done
        std::lock_guard<std::mutex> g(h.m);
        h.offer_slots.erase(entity);
    }
    s->offers.erase(entity);
    free_offer_slot(mine);
    lua_pushboolean(L, ok);
    return 1;
}

}  // namespace

// Called by Host::Impl::unload: the script's offers are taken off on the next server step (only a world call may).
void queue_script_offers(Host::Impl& h, Script& s) {
    std::lock_guard<std::mutex> g(h.m);
    for (const uint32_t e : s.offers) {
        auto it = h.offer_slots.find(e);
        if (it != h.offer_slots.end() && static_cast<OfferSlot*>(it->second)->id == s.id)
            h.offer_drops.push_back({e, it->second});
    }
    s.offers.clear();
}

// On every server step (Host::tick_body): the queued removals, and a new session forgets the old one's slots.
void run_offer_drops(Host::Impl& h, EmfWorld* w) {
    const uint32_t session = session_now(h.api);
    std::vector<std::pair<uint32_t, void*>> drops;
    std::vector<OfferSlot*> stale;
    {
        std::lock_guard<std::mutex> g(h.m);
        // the core forgot an ended world's offers: so do we (their entity ids mean other entities now)
        for (auto it = h.offer_slots.begin(); it != h.offer_slots.end();) {
            auto* os = static_cast<OfferSlot*>(it->second);
            if (os->session != session) {
                stale.push_back(os);
                it = h.offer_slots.erase(it);
            } else {
                ++it;
            }
        }
        drops.swap(h.offer_drops);
    }
    for (OfferSlot* os : stale) free_offer_slot(os);
    for (auto& [e, p] : drops) {
        bool still = false;
        {
            std::lock_guard<std::mutex> g(h.m);
            auto it = h.offer_slots.find(e);
            still = it != h.offer_slots.end() && it->second == p;
            if (still) h.offer_slots.erase(it);
        }
        if (!still) continue;
        if (has_v25(h.api) && h.api->offer_remove) h.api->offer_remove(w, e);
        free_offer_slot(static_cast<OfferSlot*>(p));
    }
}


EmfWorld* lua_world_arg(lua_State* L, int index) { return world_arg(L, index); }

void push_world(lua_State* L, Script& s, EmfWorld* world) {
    auto* w = static_cast<WorldUD*>(lua_newuserdatauv(L, sizeof(WorldUD), 0));
    w->world = world;
    w->script = &s;
    w->generation = s.generation;
    luaL_setmetatable(L, kWorldMeta);
    lua_pushlightuserdata(L, world);  // emf.item() without a world uses the callback's world
    lua_setfield(L, LUA_REGISTRYINDEX, "emf.world_now");
}

// ---- lua 1.19: mod messages (API v27) ----
bool has_v27(const EmfApi* a) {
    return a && a->version >= 27 && a->size >= offsetof(EmfApi, net_peer) + sizeof(a->net_peer) && a->net_channel;
}

// A script's channel for the life of the game (the core keeps a channel until the game closes): a reload binds the
// reloaded script's functions to it; its version and directions stay.
struct NetSlot {
    Host::Impl* host;
    std::string id;  // the script
    uint32_t version, flags;
    int ref_server = LUA_NOREF, ref_player = LUA_NOREF;
    uint64_t load = 0;  // the script load the functions belong to
};

void push_net_from(lua_State* L, const EmfNetFrom* f) {
    lua_createtable(L, 0, 5);
    lua_pushinteger(L, f ? f->player : 0);
    lua_setfield(L, -2, "player");
    char account[24];
    std::snprintf(account, sizeof account, "0x%016llx", static_cast<unsigned long long>(f ? f->account : 0));
    lua_pushstring(L, account);
    lua_setfield(L, -2, "account");
    lua_pushboolean(L, f && f->admin);
    lua_setfield(L, -2, "admin");
    lua_pushboolean(L, f && f->local);
    lua_setfield(L, -2, "is_local");
    lua_pushinteger(L, f ? f->peer_version : 0);
    lua_setfield(L, -2, "version");
}

void net_cb_body(EmfWorld* w, const EmfNetFrom* f, const void* data, uint32_t n, NetSlot* slot, bool server) {
    if (!slot) return;
    Script* s = slot->host->find(slot->id);
    if (!s) return;
    std::lock_guard<std::recursive_mutex> l(s->m);
    const int ref = server ? slot->ref_server : slot->ref_player;
    if (ref == LUA_NOREF || !s->L || s->paused || slot->load != s->loads) return;
    lua_rawgeti(s->L, LUA_REGISTRYINDEX, ref);
    push_world(s->L, *s, w);
    push_net_from(s->L, f);
    lua_pushlstring(s->L, static_cast<const char*>(data ? data : ""), n);
    call_lua(*s, 3);
    ++s->generation;
}

// Called by EMF: nothing may escape into the game.
void net_server_cb(EmfWorld* w, const EmfNetFrom* f, const void* d, uint32_t n, void* u) {
    try {
        net_cb_body(w, f, d, n, static_cast<NetSlot*>(u), true);
    } catch (...) {
    }
}
void net_player_cb(EmfWorld* w, const EmfNetFrom* f, const void* d, uint32_t n, void* u) {
    try {
        net_cb_body(w, f, d, n, static_cast<NetSlot*>(u), false);
    } catch (...) {
    }
}

uint32_t opt_uint(lua_State* L, int t, const char* key, uint32_t def) {
    lua_getfield(L, t, key);
    const uint32_t v = lua_isnil(L, -1) ? def : static_cast<uint32_t>(luaL_checkinteger(L, -1));
    lua_pop(L, 1);
    return v;
}

// emf.net.channel(name, {version = 1, to = "server" | "players" | "both", max = 4096, per_10s = 100,
// on_server = fn(world, from, data), on_player = fn(world, from, data)}) (lua 1.19): true when the channel
// "<script id>/<name>" is there. `to` defaults to where the given handlers are.
int l_net_channel(lua_State* L) {
    Script* s = script_of(L);
    const std::string name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    const uint32_t version = opt_uint(L, 2, "version", 1);
    const uint32_t max = opt_uint(L, 2, "max", 0);
    const uint32_t per = opt_uint(L, 2, "per_10s", 0);
    lua_getfield(L, 2, "on_server");
    const bool has_server = lua_isfunction(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 2, "on_player");
    const bool has_player = lua_isfunction(L, -1);
    lua_pop(L, 1);
    uint32_t flags = (has_server ? EMF_NET_TO_SERVER : 0u) | (has_player ? EMF_NET_TO_PLAYERS : 0u);
    lua_getfield(L, 2, "to");
    if (!lua_isnil(L, -1)) {
        const std::string to = luaL_checkstring(L, -1);
        if (to == "server") flags = EMF_NET_TO_SERVER;
        else if (to == "players") flags = EMF_NET_TO_PLAYERS;
        else if (to == "both") flags = EMF_NET_TO_SERVER | EMF_NET_TO_PLAYERS;
        else luaL_error(L, "emf.net.channel: to is \"server\", \"players\" or \"both\", not %s", to.c_str());
    }
    lua_pop(L, 1);
    const std::string full = s->id + "/" + name;
    Host::Impl& h = *s->host;
    const EmfApi* a = h.api;
    if (!has_v27(a)) {
        lua_pushboolean(L, false);
        return 1;
    }
    NetSlot* slot = nullptr;
    {
        std::lock_guard<std::mutex> g(h.m);
        const auto it = h.net_slots.find(full);
        if (it != h.net_slots.end()) slot = static_cast<NetSlot*>(it->second);
    }
    if (slot) {
        if (slot->version != version || slot->flags != flags)
            luaL_error(L, "emf.net.channel: %s changed its version or directions: the game keeps a channel's first "
                          "definition until you restart it", full.c_str());
    } else {
        auto fresh = std::make_unique<NetSlot>(NetSlot{&h, s->id, version, flags});
        EmfNetChannel c{};
        c.size = sizeof c;
        c.version = version;
        c.name = full.c_str();
        c.flags = flags;
        c.max_size = max;
        c.per_10s = per;
        c.on_server = net_server_cb;
        c.on_player = net_player_cb;
        c.user = fresh.get();
        if (!a->net_channel(&c)) {  // refused: the core logged why
            lua_pushboolean(L, false);
            return 1;
        }
        slot = fresh.release();  // the core keeps calling it for the life of the game
        std::lock_guard<std::mutex> g(h.m);
        h.net_slots[full] = slot;
    }
    lua_getfield(L, 2, "on_server");
    slot->ref_server = lua_isfunction(L, -1) ? luaL_ref(L, LUA_REGISTRYINDEX) : (lua_pop(L, 1), LUA_NOREF);
    lua_getfield(L, 2, "on_player");
    slot->ref_player = lua_isfunction(L, -1) ? luaL_ref(L, LUA_REGISTRYINDEX) : (lua_pop(L, 1), LUA_NOREF);
    slot->load = s->loads;
    lua_pushboolean(L, true);
    return 1;
}

// emf.net.send(name, data) (lua 1.19): this game -> the server. true when on its way.
int l_net_send(lua_State* L) {
    Script* s = script_of(L);
    const std::string full = s->id + "/" + luaL_checkstring(L, 1);
    size_t n = 0;
    const char* data = luaL_checklstring(L, 2, &n);
    const EmfApi* a = s->host->api;
    lua_pushboolean(L, has_v27(a) && a->net_to_server(full.c_str(), data, static_cast<uint32_t>(n)));
    return 1;
}

// emf.net.send_to(world, player, name, data) (lua 1.19): the server -> one player's game.
int l_net_send_to(lua_State* L) {
    Script* s = script_of(L);
    EmfWorld* w = world_arg(L, 1);
    const auto player = static_cast<uint32_t>(luaL_checkinteger(L, 2));
    const std::string full = s->id + "/" + luaL_checkstring(L, 3);
    size_t n = 0;
    const char* data = luaL_checklstring(L, 4, &n);
    const EmfApi* a = s->host->api;
    lua_pushboolean(L, has_v27(a) && a->net_to_player(w, player, full.c_str(), data, static_cast<uint32_t>(n)));
    return 1;
}

// emf.net.send_all(world, name, data) (lua 1.19): the server -> every player whose game has the channel; how many.
int l_net_send_all(lua_State* L) {
    Script* s = script_of(L);
    EmfWorld* w = world_arg(L, 1);
    const std::string full = s->id + "/" + luaL_checkstring(L, 2);
    size_t n = 0;
    const char* data = luaL_checklstring(L, 3, &n);
    const EmfApi* a = s->host->api;
    lua_pushinteger(L, has_v27(a) ? a->net_to_players(w, full.c_str(), data, static_cast<uint32_t>(n)) : 0);
    return 1;
}

// emf.net.peer(world, player, name) (lua 1.19): the channel version that player's game has (the server's: nil, 0); 0 =
// none.
int l_net_peer(lua_State* L) {
    Script* s = script_of(L);
    EmfWorld* w = lua_isnil(L, 1) ? nullptr : world_arg(L, 1);
    const auto player = static_cast<uint32_t>(luaL_optinteger(L, 2, 0));
    const std::string full = s->id + "/" + luaL_checkstring(L, 3);
    const EmfApi* a = s->host->api;
    lua_pushinteger(L, has_v27(a) ? a->net_peer(w, player, full.c_str()) : 0);
    return 1;
}

void open_bindings(lua_State* L, Script&) {
    static const luaL_Reg fns[] = {{"item", l_item},       {"item_info", l_item_info}, {"item_name", l_item_name}, {"language", l_language}, {"give", l_give},
                                   {"put", l_put},         {"take", l_take},           {"move", l_move},           {"spawn", l_spawn},
                                   {"destroy", l_destroy}, {"remove_building", l_remove_building}, {"path_find", l_path_find}, {"path_cancel", l_path_cancel}, {"door", l_door}, {"door_open", l_door_open}, {"door_close", l_door_close}, {"door_lock", l_door_lock}, {"enemy_order", l_enemy_order}, {"enemy_order_cancel", l_enemy_order_cancel}, {"enemy_target", l_enemy_target}, {"enemy_move", l_enemy_move}, {"enemy_move_cancel", l_enemy_move_cancel}, {"enemy_moving", l_enemy_moving}, {"kill", l_kill},           {"time_of_day", l_time_of_day},
                                   {"health", l_health},   {"attribute", l_attribute}, {"attribute_add", l_attribute_add},
                                   {"damage", l_damage},
                                   {"xp_required", l_xp_required}, {"level_cap", l_level_cap}, {"set_level", l_set_level},
                                   {"add_component", l_add_component}, {"remove_component", l_remove_component},
                                   {"window", l_window}, {"window_update", l_window_update}, {"window_close", l_window_close},
                                   {"image", l_image}, {"offered", l_offered},
                                   {"offer", l_offer}, {"spawn_offer", l_spawn_offer}, {"offer_remove", l_offer_remove},
                                   {"tell", l_tell},       {"broadcast", l_broadcast}, {"on_chat", l_on_chat},
                                   {"on_player_joined", l_on_player_joined}, {"on_player_left", l_on_player_left},
                                   {"on_player_died", l_on_player_died},
                                   {"on_hit", l_on_hit}, {"hit_rule", l_hit_rule}, {"hit_rule_remove", l_hit_rule_remove},
                                   {"buff_types", l_buff_types}, {"buff_type", l_buff_type}, {"buffs", l_buffs},
                                   {"apply_buff", l_apply_buff}, {"remove_buff", l_remove_buff}, {"on_buff", l_on_buff},
                                   {"marker", l_marker}, {"remove_marker", l_remove_marker},
                                   {"clear_markers", l_clear_markers}, {"marker_types", l_marker_types},
                                   {"world_to_screen", l_world_to_screen}, {"send_marker", l_send_marker},
                                   {"unsend_marker", l_unsend_marker},
                                   {"map_cursor", l_map_cursor}, {"on_map_click", l_on_map_click},
                                   {"map_action", l_map_action}, {"remove_map_action", l_remove_map_action},
                                   {"ground_at", l_ground_at},
                                   {"player_view", l_player_view}, {"fog_at", l_fog_at},
                                   {"map_markers", l_map_markers}, {"canvas", l_canvas},
                                   {"canvas_draw", l_canvas_draw},
                                   {"request_save", l_request_save}, {"teleport", l_teleport},
                                   {"on_world_setting_changed", l_on_world_setting_changed},
                                   {"on_event", l_on_event}, {"emits", l_emits},       {"emit", l_emit},
                                   {"wheel", l_wheel},     {"wheel_section", l_wheel_section},
                                   {"wheel_group", l_wheel_group}, {nullptr, nullptr}};
    luaL_setfuncs(L, fns, 0);  // into the emf table on top of the stack
    static const luaL_Reg ws_fns[] = {{"get", l_ws_get},     {"set", l_ws_set}, {"reset", l_ws_reset},
                                      {"list", l_ws_list},   {nullptr, nullptr}};
    luaL_newlib(L, ws_fns);  // emf.world_settings (lua 1.10)
    lua_setfield(L, -2, "world_settings");
    static const luaL_Reg net_fns[] = {{"channel", l_net_channel}, {"send", l_net_send}, {"send_to", l_net_send_to},
                                       {"send_all", l_net_send_all}, {"peer", l_net_peer}, {nullptr, nullptr}};
    luaL_newlib(L, net_fns);  // emf.net (lua 1.19)
    lua_setfield(L, -2, "net");
    if (luaL_newmetatable(L, kWorldMeta)) {
        static const luaL_Reg methods[] = {{"is_server", w_is_server}, {"is_client", w_is_client}, {"tick", w_tick},
                                           {"get", w_get},             {"entities", w_entities},   {"position", w_position},
                                           {"account", w_account},     {"name", w_name}, {"character_name", w_character_name},
                                           {"permissions", w_permissions},
                                           {"template", w_template},   {nullptr, nullptr}};
        luaL_newlib(L, methods);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);
    if (luaL_newmetatable(L, kProxyMeta)) {
        lua_pushcfunction(L, proxy_index);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, proxy_newindex);
        lua_setfield(L, -2, "__newindex");
        lua_pushcfunction(L, proxy_tostring);
        lua_setfield(L, -2, "__tostring");
    }
    lua_pop(L, 1);
    if (luaL_newmetatable(L, kArrayMeta)) {
        lua_pushcfunction(L, array_index);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, array_newindex);
        lua_setfield(L, -2, "__newindex");
        lua_pushcfunction(L, array_len);
        lua_setfield(L, -2, "__len");
    }
    lua_pop(L, 1);
}

void finish_load(Host::Impl& h) {
    std::set<std::string> wanted;
    {
        std::lock_guard<std::mutex> l(h.m);
        wanted = h.wanted;
    }
    for (const auto& key : wanted) {  // "<kind>:<EventName>"
        const size_t colon = key.find(':');
        const auto kind = static_cast<uint8_t>(std::stoi(key.substr(0, colon)));
        const std::string name = key.substr(colon + 1);
        auto* slot = new EventSlot{&h, key, find_type(name)};  // lives for the process
        if (h.api->on_event && h.api->on_event(kind, name.c_str(), slot->type->size, event_cb, slot)) {
            std::lock_guard<std::mutex> l(h.m);
            h.subscribed.insert(key);
        } else {
            h.api->log("lua", "the engine event %s could not be subscribed (see the lines above)", name.c_str());
            delete slot;
        }
    }
    for (const int k : {EMF_WORLD_SERVER, EMF_WORLD_CLIENT}) {
        const auto kind = static_cast<uint8_t>(k);
        std::vector<std::string> names;
        {
            std::lock_guard<std::mutex> l(h.m);
            names.assign(h.emits[kind].begin(), h.emits[kind].end());
        }
        if (names.empty()) continue;
        auto* store = new std::vector<std::string>(names);  // the definition's strings live for the process
        auto* ptrs = new std::vector<const char*>();
        for (const auto& n : *store) ptrs->push_back(n.c_str());
        ptrs->push_back(nullptr);
        EmfProgramDef def{};
        def.size = sizeof def;
        def.name = kind == EMF_WORLD_SERVER ? "lua_emit_server" : "lua_emit_client";
        def.world_kind = kind;
        def.fn = program_cb;
        def.user = new ProgramSlot{&h, kind};
        def.events_out = ptrs->data();
        if (!h.api->register_program_def || !h.api->register_program_def(&def))
            h.api->log("lua", "the program that sends the scripts' events could not be added: emf.emit will not work");
    }
}

void deliver_player_intro(Host::Impl& h, Script& s, EmfWorld* w) { player_intro_body(h, s, w); }

}  // namespace emf::lua
