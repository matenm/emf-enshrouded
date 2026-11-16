// Lua 1.24 buffs (API v32): emf.buff_types, emf.buff_type, emf.buffs, emf.apply_buff, emf.remove_buff, emf.on_buff.
// Giving and removing go through the core's emf_buffs program; the answer comes after the server step.
#include "host_impl.h"

#include <cstring>
#include <string>
#include <vector>

namespace emf::lua {

namespace {

bool has_v32(const EmfApi* a) {
    return a && a->version >= 32 && a->size >= offsetof(EmfApi, on_buff) + sizeof(a->on_buff);
}

const char* const kSlots[] = {"food", "buff", "debuff", "rested", "terrain"};
constexpr int kSlotCount = 5;

const char* slot_name(uint8_t s) { return s < kSlotCount ? kSlots[s] : "?"; }
const char* apply_name(uint8_t a) { return a == 1 ? "reset" : a >= 2 ? "replace" : "once"; }
const char* kind_name(uint32_t k) {
    switch (k) {
        case EMF_BUFF_APPLIED: return "applied";
        case EMF_BUFF_REFRESHED: return "refreshed";
        case EMF_BUFF_REMOVED: return "removed";
        default: return "refused";
    }
}

void push_name(lua_State* L, const EmfApi* a, uint32_t id) {
    char buf[256];
    if (a->buff_text && a->buff_text(id, 0, buf, sizeof buf))
        lua_pushstring(L, buf);
    else
        lua_pushnil(L);
}

void push_type(lua_State* L, const EmfApi* a, const EmfBuffType& t) {
    lua_createtable(L, 0, 10);
    lua_pushinteger(L, t.id);
    lua_setfield(L, -2, "id");
    lua_pushstring(L, slot_name(t.slot));
    lua_setfield(L, -2, "slot");
    push_name(L, a, t.id);
    lua_setfield(L, -2, "name");
    char desc[512];
    if (a->buff_text && a->buff_text(t.id, 1, desc, sizeof desc)) {
        lua_pushstring(L, desc);
        lua_setfield(L, -2, "description");
    }
    lua_pushboolean(L, (t.flags & EMF_BUFF_HIDDEN) != 0);
    lua_setfield(L, -2, "hidden");
    lua_pushboolean(L, (t.flags & EMF_BUFF_SAVED) != 0);
    lua_setfield(L, -2, "saved");
    lua_pushboolean(L, (t.flags & EMF_BUFF_DESPAWN_ON_DEATH) != 0);
    lua_setfield(L, -2, "ends_on_death");
    lua_pushinteger(L, t.default_ns / 1'000'000'000LL);
    lua_setfield(L, -2, "seconds");
    lua_pushstring(L, apply_name(t.apply_type));
    lua_setfield(L, -2, "apply");
    lua_pushinteger(L, t.follow_up);
    lua_setfield(L, -2, "follow_up");
}

void push_seconds(lua_State* L, int64_t ns, const char* key) {
    if (ns < 0)
        lua_pushnil(L);
    else
        lua_pushnumber(L, static_cast<double>(ns) / 1e9);
    lua_setfield(L, -2, key);
}

void push_event(lua_State* L, const EmfApi* a, const EmfBuffEvent& e) {
    lua_createtable(L, 0, 9);
    lua_pushstring(L, kind_name(e.kind));
    lua_setfield(L, -2, "kind");
    lua_pushinteger(L, e.target);
    lua_setfield(L, -2, "target");
    lua_pushinteger(L, e.type);
    lua_setfield(L, -2, "type");
    push_name(L, a, e.type);
    lua_setfield(L, -2, "name");
    lua_pushstring(L, slot_name(e.slot));
    lua_setfield(L, -2, "slot");
    lua_pushinteger(L, e.buff_entity);
    lua_setfield(L, -2, "buff_entity");
    lua_pushboolean(L, e.cleansed != 0);
    lua_setfield(L, -2, "cleansed");
    lua_pushboolean(L, e.replaced != 0);
    lua_setfield(L, -2, "replaced");
    push_seconds(L, e.remaining_ns, "seconds_left");
    if (e.why) {
        lua_pushstring(L, e.why);
        lua_setfield(L, -2, "why");
    }
}

// one emf.apply_buff / emf.remove_buff answer: the script's function, while the load that asked still runs
struct BuffSlot {
    Host::Impl* host;
    std::string id;
    int ref;
    uint64_t load;
};

void buff_done_body(EmfWorld* w, const EmfBuffEvent* e, void* user) {
    auto* slot = static_cast<BuffSlot*>(user);
    Script* s = slot->host->find(slot->id);
    if (s && e) {
        std::lock_guard<std::recursive_mutex> l(s->m);
        const bool same_state = s->L && slot->load == s->loads;
        if (same_state && !s->paused) {
            lua_rawgeti(s->L, LUA_REGISTRYINDEX, slot->ref);
            push_world(s->L, *s, w);
            push_event(s->L, slot->host->api, *e);
            call_lua(*s, 2);
            ++s->generation;
        }
        if (same_state) luaL_unref(s->L, LUA_REGISTRYINDEX, slot->ref);
    }
    delete slot;
}

void buff_done(EmfWorld* w, const EmfBuffEvent* e, void* user) {
    try {
        buff_done_body(w, e, user);
    } catch (...) {
    }
}

void buff_event_body(EmfWorld* w, const EmfBuffEvent* e, void* user) {
    auto* h = static_cast<Host::Impl*>(user);
    if (!e) return;
    for (auto& sp : h->scripts) {
        Script& s = *sp;
        if (!s.buff_cb_count.load(std::memory_order_acquire)) continue;  // never wait for a script that does not listen
        std::lock_guard<std::recursive_mutex> l(s.m);
        if (!s.L || s.paused || s.buff_cbs.empty()) continue;
        const std::vector<int> refs = s.buff_cbs;
        for (int ref : refs) {
            lua_rawgeti(s.L, LUA_REGISTRYINDEX, ref);
            push_world(s.L, s, w);
            push_event(s.L, h->api, *e);
            if (!call_lua(s, 2)) break;
        }
        ++s.generation;
    }
}

void buff_event(EmfWorld* w, const EmfBuffEvent* e, void* user) {
    try {
        buff_event_body(w, e, user);
    } catch (...) {
    }
}

int slot_arg(lua_State* L, int index, const char* fn) {
    if (lua_isnoneornil(L, index)) return -1;
    const char* t = luaL_checkstring(L, index);
    for (int i = 0; i < kSlotCount; ++i)
        if (!std::strcmp(t, kSlots[i])) return i;
    return luaL_error(L, "%s: no slot '%s' (food, buff, debuff, rested or terrain)", fn, t);
}

// fn index of the optional function at `index` (0: none)
int fn_at(lua_State* L, int index) {
    if (lua_isnoneornil(L, index)) return 0;
    luaL_checktype(L, index, LUA_TFUNCTION);
    return index;
}

constexpr lua_Number kMaxBuffSeconds = 9e9;  // times 1e9 still fits in int64 nanoseconds (about 285 years)

int ask(lua_State* L, bool remove) {
    Script* s = script_of(L);
    const EmfApi* a = s->host->api;
    if (!has_v32(a) || (remove ? !a->buff_remove : !a->buff_apply)) {
        lua_pushboolean(L, false);
        return 1;
    }
    EmfWorld* w = lua_world_arg(L, 1);
    const uint32_t target = static_cast<uint32_t>(luaL_checkinteger(L, 2));
    const uint32_t type = static_cast<uint32_t>(luaL_checkinteger(L, 3));
    int64_t ns = 0;
    uint32_t source = 0, item = 0;
    int fn = 0;
    if (remove) {
        fn = fn_at(L, 4);
    } else {
        if (lua_istable(L, 4)) {
            lua_pushnil(L);
            while (lua_next(L, 4)) {
                const char* k = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : "";
                if (!std::strcmp(k, "seconds")) {
                    // NaN, infinity, a negative time or one past what nanoseconds hold would wrap in the cast
                    const lua_Number sec = luaL_checknumber(L, -1);
                    if (!(sec >= 0 && sec <= kMaxBuffSeconds))
                        return luaL_error(L, "emf.apply_buff: seconds must be 0 to 9000000000 (0: the type's own time)");
                    ns = static_cast<int64_t>(sec * 1e9);
                } else if (!std::strcmp(k, "source"))
                    source = static_cast<uint32_t>(luaL_checkinteger(L, -1));
                else if (!std::strcmp(k, "item"))
                    item = static_cast<uint32_t>(luaL_checkinteger(L, -1));
                else
                    return luaL_error(L, "emf.apply_buff: unknown option '%s' (seconds, source, item)", k);
                lua_pop(L, 1);
            }
            fn = fn_at(L, 5);
        } else {
            fn = fn_at(L, 4);
        }
    }
    BuffSlot* slot = nullptr;
    if (fn) {
        lua_pushvalue(L, fn);
        slot = new BuffSlot{s->host, s->id, luaL_ref(L, LUA_REGISTRYINDEX), s->loads};
    }
    const bool ok = remove ? a->buff_remove(w, target, type, slot ? buff_done : nullptr, slot)
                           : a->buff_apply(w, target, type, ns, source, item, slot ? buff_done : nullptr, slot);
    if (!ok && slot) {
        luaL_unref(L, LUA_REGISTRYINDEX, slot->ref);
        delete slot;
    }
    lua_pushboolean(L, ok);
    return 1;
}

}  // namespace

// emf.buff_types(world [, slot]): the game's buff types (names on a game client)
int l_buff_types(lua_State* L) {
    Script* s = script_of(L);
    const EmfApi* a = s->host->api;
    EmfWorld* w = lua_world_arg(L, 1);
    const int slot = slot_arg(L, 2, "emf.buff_types");
    lua_newtable(L);
    if (!has_v32(a) || !a->buff_types) return 1;
    std::vector<EmfBuffType> all(a->buff_types(w, nullptr, 0));
    const uint32_t n = a->buff_types(w, all.data(), static_cast<uint32_t>(all.size()));
    lua_Integer i = 0;
    for (uint32_t k = 0; k < n && k < all.size(); ++k) {
        if (slot >= 0 && all[k].slot != slot) continue;
        push_type(L, a, all[k]);
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

// emf.buff_type(world, id): one type, or nil
int l_buff_type(lua_State* L) {
    Script* s = script_of(L);
    const EmfApi* a = s->host->api;
    EmfWorld* w = lua_world_arg(L, 1);
    const uint32_t id = static_cast<uint32_t>(luaL_checkinteger(L, 2));
    EmfBuffType t{};
    if (!has_v32(a) || !a->buff_type || !a->buff_type(w, id, &t)) {
        lua_pushnil(L);
        return 1;
    }
    push_type(L, a, t);
    return 1;
}

// emf.buffs(world, entity): the entity's active buffs
int l_buffs(lua_State* L) {
    Script* s = script_of(L);
    const EmfApi* a = s->host->api;
    EmfWorld* w = lua_world_arg(L, 1);
    const uint32_t entity = static_cast<uint32_t>(luaL_checkinteger(L, 2));
    lua_newtable(L);
    if (!has_v32(a) || !a->buffs_of) return 1;
    EmfBuff b[40];
    const uint32_t n = a->buffs_of(w, entity, b, 40);
    for (uint32_t i = 0; i < n && i < 40; ++i) {
        lua_createtable(L, 0, 8);
        lua_pushinteger(L, b[i].type);
        lua_setfield(L, -2, "type");
        push_name(L, a, b[i].type);
        lua_setfield(L, -2, "name");
        lua_pushstring(L, slot_name(b[i].slot));
        lua_setfield(L, -2, "slot");
        lua_pushinteger(L, b[i].entity);
        lua_setfield(L, -2, "entity");
        lua_pushinteger(L, b[i].level);
        lua_setfield(L, -2, "level");
        push_seconds(L, b[i].total_ns, "seconds");
        push_seconds(L, b[i].remaining_ns, "seconds_left");
        lua_pushboolean(L, b[i].cleansed != 0);
        lua_setfield(L, -2, "cleansed");
        lua_rawseti(L, -2, static_cast<lua_Integer>(i) + 1);
    }
    return 1;
}

// emf.apply_buff(world, target, type [, {seconds, source, item}] [, fn(world, result)]): true when queued
int l_apply_buff(lua_State* L) { return ask(L, false); }
// emf.remove_buff(world, target, type [, fn(world, result)]): true when queued
int l_remove_buff(lua_State* L) { return ask(L, true); }

// emf.on_buff(fn(world, ev)): the players' buff changes (applied, refreshed, removed); false on an older core
int l_on_buff(lua_State* L) {
    Script* s = script_of(L);
    luaL_checktype(L, 1, LUA_TFUNCTION);
    Host::Impl* h = s->host;
    int state = h->buff_hooked.load();
    if (state == 0) {
        const bool ok = has_v32(h->api) && h->api->on_buff && h->api->on_buff(buff_event, h);
        int expected = 0;
        h->buff_hooked.compare_exchange_strong(expected, ok ? 1 : -1);
        state = h->buff_hooked.load();
    }
    if (state != 1) {
        lua_pushboolean(L, false);
        return 1;
    }
    lua_pushvalue(L, 1);
    s->buff_cbs.push_back(luaL_ref(L, LUA_REGISTRYINDEX));
    s->buff_cb_count.store(static_cast<uint32_t>(s->buff_cbs.size()), std::memory_order_release);
    lua_pushboolean(L, true);
    return 1;
}

}  // namespace emf::lua
