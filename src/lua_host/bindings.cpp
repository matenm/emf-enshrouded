#include "host_impl.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

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

int l_spawn(lua_State* L) {
    Script* s = script_of(L);
    EmfWorld* w = world_arg(L, 1);
    const char* text = luaL_checkstring(L, 2);
    uint8_t guid[16];
    if (!emf::parse_guid(text, guid)) return luaL_error(L, "%s is not a GUID (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx)", text);
    const double x = luaL_checknumber(L, 3), y = luaL_checknumber(L, 4), z = luaL_checknumber(L, 5);
    lua_pushinteger(L, s->host->api->spawn(w, guid, x, y, z));
    return 1;
}

int l_destroy(lua_State* L) {
    Script* s = script_of(L);
    EmfWorld* w = world_arg(L, 1);
    lua_pushboolean(L, s->host->api->destroy(w, static_cast<uint32_t>(luaL_checkinteger(L, 2))));
    return 1;
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
    luaL_error(L, "unknown attribute '%s' (health, max_health, stamina, max_stamina, mana, max_mana, or an attribute id)",
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
int l_tell(lua_State* L) {
    Script* s = script_of(L);
    EmfWorld* w = world_arg(L, 1);
    const auto id = static_cast<uint32_t>(luaL_checkinteger(L, 2));
    const std::string text = to_text(L, 3);
    const EmfApi* a = s->host->api;
    lua_pushboolean(L, has_v19(a) && a->chat_tell && chat_budget(*s, text, "emf.tell") &&
                           chat_spent(*s, text, a->chat_tell(w, id, text.c_str())));
    return 1;
}

// emf.broadcast(text): the line for every connected player; true when queued.
int l_broadcast(lua_State* L) {
    Script* s = script_of(L);
    const std::string text = to_text(L, 1);
    const EmfApi* a = s->host->api;
    lua_pushboolean(L, has_v19(a) && a->chat_broadcast && chat_budget(*s, text, "emf.broadcast") &&
                           chat_spent(*s, text, a->chat_broadcast(text.c_str())));
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
    for (auto& sp : slot->host->scripts) {
        Script& s = *sp;
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

}  // namespace

void push_world(lua_State* L, Script& s, EmfWorld* world) {
    auto* w = static_cast<WorldUD*>(lua_newuserdatauv(L, sizeof(WorldUD), 0));
    w->world = world;
    w->script = &s;
    w->generation = s.generation;
    luaL_setmetatable(L, kWorldMeta);
    lua_pushlightuserdata(L, world);  // emf.item() without a world uses the callback's world
    lua_setfield(L, LUA_REGISTRYINDEX, "emf.world_now");
}

void open_bindings(lua_State* L, Script&) {
    static const luaL_Reg fns[] = {{"item", l_item},       {"item_info", l_item_info}, {"give", l_give},
                                   {"put", l_put},         {"take", l_take},           {"move", l_move},           {"spawn", l_spawn},
                                   {"destroy", l_destroy}, {"kill", l_kill},           {"time_of_day", l_time_of_day},
                                   {"health", l_health},   {"attribute", l_attribute}, {"attribute_add", l_attribute_add},
                                   {"damage", l_damage},
                                   {"tell", l_tell},       {"broadcast", l_broadcast}, {"on_chat", l_on_chat},
                                   {"request_save", l_request_save}, {"teleport", l_teleport},
                                   {"on_event", l_on_event}, {"emits", l_emits},       {"emit", l_emit},
                                   {"wheel", l_wheel},     {"wheel_section", l_wheel_section},
                                   {"wheel_group", l_wheel_group}, {nullptr, nullptr}};
    luaL_setfuncs(L, fns, 0);  // into the emf table on top of the stack
    if (luaL_newmetatable(L, kWorldMeta)) {
        static const luaL_Reg methods[] = {{"is_server", w_is_server}, {"is_client", w_is_client}, {"tick", w_tick},
                                           {"get", w_get},             {"entities", w_entities},   {"position", w_position},
                                           {"account", w_account},     {"name", w_name},
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

}  // namespace emf::lua
