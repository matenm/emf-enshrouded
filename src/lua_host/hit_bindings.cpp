// Lua 1.23 hits (API v31): emf.on_hit, emf.hit_rule, emf.hit_rule_remove. Lua never runs before a hit lands (the
// detour is on the game's impact thread and the script lock is shared by the world threads): a script changes hits
// only through rules, which the core applies; its rules go when it reloads, unloads or pauses.
#include "host_impl.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace emf::lua {

namespace {

bool has_v31(const EmfApi* a) {
    return a && a->version >= 31 && a->size >= offsetof(EmfApi, hit_rule_remove) + sizeof(a->hit_rule_remove);
}

// EmfHit.damage_types bits (bit 7, healing, is never set: a heal has the healing flag)
const char* const kTypes[] = {"cut",  "pierce", "blunt", "fire",  "ice",   "shroud",   "lightning",
                              nullptr, "poison", "wood",  "stone", "metal", "explosion"};
constexpr int kTypeCount = static_cast<int>(sizeof kTypes / sizeof kTypes[0]);
const char* const kAttacks[] = {"melee", "ranged", "magic", "tool", "other", "aoe", "skillshot"};
// EmfHit.kind (EMF_HIT_KIND_*)
const char* const kKinds[] = {"hit", "fall", "shroud", "cold", "drowning"};
constexpr int kKindCount = static_cast<int>(sizeof kKinds / sizeof kKinds[0]);
constexpr int kAttackCount = static_cast<int>(sizeof kAttacks / sizeof kAttacks[0]);

void set_int(lua_State* L, const char* key, lua_Integer v) {
    lua_pushinteger(L, v);
    lua_setfield(L, -2, key);
}
void set_bool(lua_State* L, const char* key, bool v) {
    lua_pushboolean(L, v);
    lua_setfield(L, -2, key);
}
// -1 (not known) is nil
void set_health(lua_State* L, const char* key, int32_t v) {
    if (v < 0) return;
    set_int(L, key, v);
}

void push_hit(lua_State* L, const EmfHit& h) {
    lua_createtable(L, 0, 24);
    lua_pushstring(L, h.kind < kKindCount ? kKinds[h.kind] : "hit");
    lua_setfield(L, -2, "kind");
    set_int(L, "attacker", h.attacker);
    set_int(L, "source", h.source);
    set_int(L, "target", h.target);
    set_int(L, "part", h.part);
    set_int(L, "amount", h.amount);
    set_int(L, "stamina", h.stamina);
    set_bool(L, "crit", (h.flags & EMF_HIT_CRIT) != 0);
    set_bool(L, "blocked", (h.flags & EMF_HIT_BLOCKED) != 0);
    set_bool(L, "parried", (h.flags & EMF_HIT_PARRIED) != 0);
    set_bool(L, "backstab", (h.flags & EMF_HIT_BACKSTAB) != 0);
    set_bool(L, "weakspot", (h.flags & EMF_HIT_WEAKSPOT) != 0);
    set_bool(L, "healing", (h.flags & EMF_HIT_HEALING) != 0 || h.amount < 0);
    set_bool(L, "killed", (h.flags & EMF_HIT_KILLED) != 0);
    set_bool(L, "instant", (h.flags & EMF_HIT_INSTANT) != 0);
    lua_createtable(L, 4, 0);
    int n = 0;
    for (int b = 0; b < kTypeCount; ++b)
        if (kTypes[b] && (h.damage_types & (1u << b))) {
            lua_pushstring(L, kTypes[b]);
            lua_rawseti(L, -2, ++n);
        }
    lua_setfield(L, -2, "types");
    lua_createtable(L, 1, 0);  // attacks: every kind; attack: the first (most hits have one), nil for none
    int k = 0;
    for (int b = 0; b < kAttackCount; ++b)
        if (h.attacks & (1u << b)) {
            if (k == 0) {
                lua_pushstring(L, kAttacks[b]);
                lua_setfield(L, -3, "attack");
            }
            lua_pushstring(L, kAttacks[b]);
            lua_rawseti(L, -2, ++k);
        }
    lua_setfield(L, -2, "attacks");
    set_int(L, "weapon_item", h.weapon_item);
    set_int(L, "weapon_category", h.weapon_category);
    set_int(L, "program", h.program);
    set_int(L, "impact", h.impact);
    set_int(L, "reaction", h.reaction);
    set_health(L, "health_before", h.health_before);
    set_health(L, "health_after", h.health_after);
    set_bool(L, "from_mod", h.from_mod != 0);
    lua_createtable(L, 0, 3);
    lua_pushnumber(L, h.position[0]);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, h.position[1]);
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, h.position[2]);
    lua_setfield(L, -2, "z");
    lua_setfield(L, -2, "position");
}

void hit_cb_body(EmfWorld* w, const EmfHit* hit, void* user) {
    auto* h = static_cast<Host::Impl*>(user);
    if (!hit) return;
    for (auto& sp : h->scripts) {
        Script& s = *sp;
        // never wait for a script that does not listen to this kind (fall, shroud, cold and drowning come 60 a second)
        if (!s.hit_cb_count.load(std::memory_order_acquire)) continue;
        if (!((s.hit_cb_kinds_any.load(std::memory_order_acquire) >> hit->kind) & 1u)) continue;
        std::lock_guard<std::recursive_mutex> l(s.m);
        if (!s.L || s.paused || s.hit_cbs.empty()) continue;
        const std::vector<int> refs = s.hit_cbs;
        const std::vector<uint8_t> kinds = s.hit_cb_kinds;
        for (size_t i = 0; i < refs.size(); ++i) {
            if (i < kinds.size() && !((kinds[i] >> hit->kind) & 1u)) continue;
            const int ref = refs[i];
            lua_rawgeti(s.L, LUA_REGISTRYINDEX, ref);
            push_world(s.L, s, w);
            push_hit(s.L, *hit);
            if (!call_lua(s, 2)) break;
        }
        ++s.generation;
    }
}

// Called by EMF: nothing may escape into the game (a Lua error is a C++ exception).
void hit_cb(EmfWorld* w, const EmfHit* hit, void* user) {
    try {
        hit_cb_body(w, hit, user);
    } catch (...) {
    }
}

// from / to: nil (anyone), an entity id, or "players", "creatures", "objects"
uint32_t who_of(lua_State* L, const char* key) {
    lua_getfield(L, 1, key);
    uint32_t who = 0;
    if (lua_isinteger(L, -1)) {
        const lua_Integer v = lua_tointeger(L, -1);
        if (v < 1 || v >= 0xFFFFFFF0LL) luaL_error(L, "emf.hit_rule: %s = %I is not an entity id", key, v);
        who = static_cast<uint32_t>(v);
    } else if (lua_type(L, -1) == LUA_TSTRING) {
        const std::string t = lua_tostring(L, -1);
        if (t == "players") who = EMF_HIT_PLAYERS;
        else if (t == "creatures") who = EMF_HIT_CREATURES;
        else if (t == "objects") who = EMF_HIT_OBJECTS;
        else luaL_error(L, "emf.hit_rule: %s = '%s': use an entity id, 'players', 'creatures' or 'objects'", key, t.c_str());
    } else if (!lua_isnil(L, -1)) {
        luaL_error(L, "emf.hit_rule: %s must be an entity id or 'players', 'creatures' or 'objects'", key);
    }
    lua_pop(L, 1);
    return who;
}

// a name, or a list of names, from `names` -> a bit mask
uint32_t names_of(lua_State* L, const char* key, const char* const* names, int count, bool list_only) {
    lua_getfield(L, 1, key);
    uint32_t mask = 0;
    auto one = [&](int idx) {
        if (lua_type(L, idx) != LUA_TSTRING) luaL_error(L, "emf.hit_rule: %s takes names", key);
        const char* t = lua_tostring(L, idx);
        for (int i = 0; i < count; ++i)
            if (names[i] && std::strcmp(names[i], t) == 0) {
                mask |= 1u << i;
                return;
            }
        luaL_error(L, "emf.hit_rule: %s has no '%s'", key, t);
    };
    if (lua_istable(L, -1)) {
        const lua_Integer n = luaL_len(L, -1);
        for (lua_Integer i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            one(-1);
            lua_pop(L, 1);
        }
    } else if (!list_only && lua_type(L, -1) == LUA_TSTRING) {
        one(-1);
    } else if (!lua_isnil(L, -1)) {
        luaL_error(L, list_only ? "emf.hit_rule: %s takes a list of names" : "emf.hit_rule: %s takes a name or a list of names",
                   key);
    }
    lua_pop(L, 1);
    return mask;
}

// `kinds` of the table at `index`: nil -> 0, "all", a kind's name, or a list of names -> a bit per kind
uint8_t kinds_at(lua_State* L, int index, const char* fn) {
    lua_getfield(L, index, "kinds");
    uint8_t mask = 0;
    auto one = [&](int idx) {
        if (lua_type(L, idx) != LUA_TSTRING) luaL_error(L, "%s: kinds takes names", fn);
        const char* t = lua_tostring(L, idx);
        if (!std::strcmp(t, "all")) {
            mask = EMF_HIT_KINDS_ALL;
            return;
        }
        for (int i = 0; i < kKindCount; ++i)
            if (!std::strcmp(kKinds[i], t)) {
                mask |= static_cast<uint8_t>(1u << i);
                return;
            }
        luaL_error(L, "%s: kinds has no '%s' (hit, fall, shroud, cold, drowning or all)", fn, t);
    };
    if (lua_type(L, -1) == LUA_TSTRING) {
        one(-1);
    } else if (lua_istable(L, -1)) {
        const lua_Integer n = luaL_len(L, -1);
        // an empty list would read as no kinds, which means entity hits only: never what a script that names kinds wants
        if (n == 0) luaL_error(L, "%s: kinds = {} names no kind (hit, fall, shroud, cold, drowning or all)", fn);
        for (lua_Integer i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            one(-1);
            lua_pop(L, 1);
        }
    } else if (!lua_isnil(L, -1)) {
        luaL_error(L, "%s: kinds takes a name, a list of names or \"all\"", fn);
    }
    lua_pop(L, 1);
    return mask;
}

// A whole number field of the rule table that must fit in [lo, hi] (an int32 of the C rule): one that does not is an
// error, not a value that wraps. `def` when absent.
int32_t int32_of(lua_State* L, const char* key, lua_Integer lo, lua_Integer hi, int32_t def) {
    lua_getfield(L, 1, key);
    int32_t out = def;
    if (!lua_isnil(L, -1)) {
        const lua_Integer v = luaL_checkinteger(L, -1);
        if (v < lo || v > hi) luaL_error(L, "emf.hit_rule: %s = %I does not fit (%I to %I)", key, v, lo, hi);
        out = static_cast<int32_t>(v);
    }
    lua_pop(L, 1);
    return out;
}

bool flag_of(lua_State* L, const char* key) {
    lua_getfield(L, 1, key);
    const bool on = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return on;
}

bool known_key(const char* k) {
    static const char* const keys[] = {"from",     "to",    "crit",   "backstab", "weakspot", "types",  "attack",
                                       "healing",  "scale", "add",    "min_health", "cancel", "kinds"};
    for (const char* x : keys)
        if (std::strcmp(x, k) == 0) return true;
    return false;
}

}  // namespace

// emf.on_hit(fn): fn(world, hit) after the server world's step for each hit the server applied to an entity. The first
// of any script subscribes the host once. False on an older core.
int l_on_hit(lua_State* L) {
    Script* s = script_of(L);
    luaL_checktype(L, 1, LUA_TFUNCTION);
    Host::Impl* h = s->host;
    int state = h->hit_hooked.load();
    if (state == 0) {
        const bool ok = has_v31(h->api) && h->api->on_hit && h->api->on_hit(hit_cb, h);
        int expected = 0;
        h->hit_hooked.compare_exchange_strong(expected, ok ? 1 : -1);
        state = h->hit_hooked.load();
    }
    if (state != 1) {
        lua_pushboolean(L, false);
        return 1;
    }
    uint8_t kinds = 1u << EMF_HIT_KIND_HIT;  // entity hits only unless asked (env kinds come 60 a second)
    if (lua_istable(L, 2)) {
        lua_pushnil(L);
        while (lua_next(L, 2)) {
            const char* k = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : "";
            if (std::strcmp(k, "kinds") != 0) return luaL_error(L, "emf.on_hit: unknown option '%s' (kinds)", k);
            lua_pop(L, 1);
        }
        if (const uint8_t asked = kinds_at(L, 2, "emf.on_hit")) kinds = asked;
    }
    lua_pushvalue(L, 1);
    s->hit_cbs.push_back(luaL_ref(L, LUA_REGISTRYINDEX));
    s->hit_cb_kinds.push_back(kinds);
    s->hit_cb_kinds_any.fetch_or(kinds, std::memory_order_release);
    s->hit_cb_count.store(static_cast<uint32_t>(s->hit_cbs.size()), std::memory_order_release);
    lua_pushboolean(L, true);
    return 1;
}

// emf.hit_rule{from, to, crit, backstab, weakspot, types, attack, healing, scale, add, min_health, cancel}: the rule's
// id, or nil when the core refused it (logged) or is older. A wrong field is an error.
int l_hit_rule(lua_State* L) {
    Script* s = script_of(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushnil(L);
    while (lua_next(L, 1)) {
        lua_pop(L, 1);  // the value; the key stays for the next round
        if (lua_type(L, -1) != LUA_TSTRING || !known_key(lua_tostring(L, -1)))
            return luaL_error(L, "emf.hit_rule: unknown field '%s' (from, to, crit, backstab, weakspot, types, attack, healing, "
                                 "scale, add, min_health, cancel, kinds)",
                              luaL_tolstring(L, -1, nullptr));
    }
    EmfHitRule r{};
    r.size = sizeof r;
    r.scale = 1.0f;
    r.attacker = who_of(L, "from");
    r.target = who_of(L, "to");
    if (flag_of(L, "crit")) r.flags_all |= EMF_HIT_CRIT;
    if (flag_of(L, "backstab")) r.flags_all |= EMF_HIT_BACKSTAB;
    if (flag_of(L, "weakspot")) r.flags_all |= EMF_HIT_WEAKSPOT;
    r.damage_types_any = static_cast<uint16_t>(names_of(L, "types", kTypes, kTypeCount, false));
    r.attacks = static_cast<uint8_t>(names_of(L, "attack", kAttacks, kAttackCount, false));
    r.healing = flag_of(L, "healing") ? 1 : 0;
    r.cancel = flag_of(L, "cancel") ? 1 : 0;
    r.kinds = kinds_at(L, 1, "emf.hit_rule");
    lua_getfield(L, 1, "scale");
    if (!lua_isnil(L, -1)) r.scale = static_cast<float>(luaL_checknumber(L, -1));
    lua_pop(L, 1);
    constexpr lua_Integer kMin32 = INT32_MIN, kMax32 = INT32_MAX;
    r.add = int32_of(L, "add", kMin32, kMax32, 0);
    r.min_health = int32_of(L, "min_health", 0, kMax32, 0);  // the core refuses a negative one
    const EmfApi* api = s->host->api;
    const uint32_t id = has_v31(api) && api->hit_rule_add ? api->hit_rule_add(&r) : 0;
    if (!id) {
        lua_pushnil(L);
        return 1;
    }
    s->hit_rules.insert(id);
    lua_pushinteger(L, id);
    return 1;
}

// emf.hit_rule_remove(id): true when it was one of this script's rules.
int l_hit_rule_remove(lua_State* L) {
    Script* s = script_of(L);
    const lua_Integer v = luaL_checkinteger(L, 1);
    const auto it = v > 0 && v <= 0xFFFFFFFFLL ? s->hit_rules.find(static_cast<uint32_t>(v)) : s->hit_rules.end();
    if (it == s->hit_rules.end()) {
        lua_pushboolean(L, false);
        return 1;
    }
    s->hit_rules.erase(it);
    const EmfApi* api = s->host->api;
    lua_pushboolean(L, has_v31(api) && api->hit_rule_remove && api->hit_rule_remove(static_cast<uint32_t>(v)));
    return 1;
}

void drop_hit_rules(Host::Impl& h, Script& s) {
    for (const uint32_t id : s.hit_rules)
        if (has_v31(h.api) && h.api->hit_rule_remove) h.api->hit_rule_remove(id);
    s.hit_rules.clear();
}

}  // namespace emf::lua
