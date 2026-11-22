// Lua 1.25 markers (API v33): emf.marker, emf.remove_marker, emf.clear_markers, emf.marker_types, emf.world_to_screen,
// emf.send_marker, emf.unsend_marker. A script's ids are "<script id>:<id>" in the core (every Lua script is the core's
// mod "lua"); its markers, here and in the players' games it sent them to, leave with it (reload, pause, unload).
#include <cstring>
#include <string>
#include <vector>

#include "host_impl.h"

namespace emf::lua {

namespace {

bool has_v33(const EmfApi* a) {
    return a && a->version >= 33 && a->size >= offsetof(EmfApi, marker_unsend) + sizeof(a->marker_unsend);
}

struct Spec {
    EmfMarker m{};
    std::string id, text;
};

const char* const kKeys[] = {"x",    "y",    "z",          "entity", "height",   "text",       "icon",     "colour",
                             "shape", "world", "game_map", "game_icon", "edge_arrow", "distance", "max_distance",
                             "minimap", "bar"};

bool known_key(const char* k) {
    for (const char* x : kKeys)
        if (std::strcmp(x, k) == 0) return true;
    return false;
}

double number_field(lua_State* L, int t, const char* key, const char* fn, double def) {
    lua_getfield(L, t, key);
    double v = def;
    if (!lua_isnil(L, -1)) {
        if (!lua_isnumber(L, -1)) luaL_error(L, "%s: %s must be a number", fn, key);
        v = lua_tonumber(L, -1);
    }
    lua_pop(L, 1);
    return v;
}

// An id field (an entity, an image handle, a map icon type): a whole number that fits in 32 bits, else an error (a
// double cast to uint32_t out of range is undefined, and a fraction would be cut without a word).
uint32_t id_field(lua_State* L, int t, const char* key, const char* fn) {
    const double v = number_field(L, t, key, fn, 0);
    if (!(v >= 0 && v <= 4294967295.0) || v != static_cast<double>(static_cast<uint32_t>(v)))
        luaL_error(L, "%s: %s must be a whole number from 0 to 4294967295", fn, key);
    return static_cast<uint32_t>(v);
}

bool flag_field(lua_State* L, int t, const char* key, bool def) {
    lua_getfield(L, t, key);
    const bool v = lua_isnil(L, -1) ? def : lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return v;
}

// The marker `id` (arg 1 of the call's id) with the spec table at `t`; raises on an unknown key or a bad value.
Spec read_spec(lua_State* L, Script* s, const char* id, int t, const char* fn) {
    luaL_checktype(L, t, LUA_TTABLE);
    lua_pushnil(L);
    while (lua_next(L, t)) {
        const char* k = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : "";
        if (!known_key(k))
            luaL_error(L, "%s: unknown key '%s' (x, y, z, entity, height, text, icon, colour, shape, world, game_map, "
                          "game_icon, edge_arrow, distance, max_distance, minimap, bar)", fn, k);
        lua_pop(L, 1);
    }
    Spec sp;
    sp.id = s->id + ":" + id;
    EmfMarker& m = sp.m;
    m.size = sizeof(EmfMarker);
    m.x = number_field(L, t, "x", fn, 0);
    m.y = number_field(L, t, "y", fn, 0);
    m.z = number_field(L, t, "z", fn, 0);
    m.entity = id_field(L, t, "entity", fn);
    m.height = static_cast<float>(number_field(L, t, "height", fn, 0));
    m.max_distance = static_cast<float>(number_field(L, t, "max_distance", fn, 0));
    // a colour keeps its low 32 bits as before (-1 is every bit); one beyond any whole number is an error
    const double colour = number_field(L, t, "colour", fn, 0);
    if (!(colour >= -9223372036854775808.0 && colour < 9223372036854775808.0))
        luaL_error(L, "%s: colour must be 0xRRGGBBAA", fn);
    m.rgba = static_cast<uint32_t>(static_cast<int64_t>(colour));
    m.icon = id_field(L, t, "icon", fn);
    m.game_icon = id_field(L, t, "game_icon", fn);
    lua_getfield(L, t, "text");
    if (!lua_isnil(L, -1)) {
        if (lua_type(L, -1) != LUA_TSTRING) luaL_error(L, "%s: text must be a string", fn);
        sp.text = lua_tostring(L, -1);
    }
    lua_pop(L, 1);
    lua_getfield(L, t, "shape");
    if (!lua_isnil(L, -1)) {
        const char* sh = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
        if (std::strcmp(sh, "diamond") == 0) m.shape = EMF_MARKER_DIAMOND;
        else if (std::strcmp(sh, "circle") == 0) m.shape = EMF_MARKER_CIRCLE;
        else if (std::strcmp(sh, "square") == 0) m.shape = EMF_MARKER_SQUARE;
        else if (std::strcmp(sh, "none") == 0) m.shape = EMF_MARKER_NONE;
        else luaL_error(L, "%s: shape '%s' (diamond, circle, square or none)", fn, sh);
    }
    lua_pop(L, 1);
    if (flag_field(L, t, "world", true)) m.flags |= EMF_MARKER_WORLD;
    if (flag_field(L, t, "distance", true)) m.flags |= EMF_MARKER_DISTANCE;
    if (flag_field(L, t, "game_map", false)) m.flags |= EMF_MARKER_GAME_MAP;
    if (flag_field(L, t, "edge_arrow", false)) m.flags |= EMF_MARKER_EDGE_ARROW;
    if (flag_field(L, t, "minimap", false)) m.flags |= EMF_MARKER_MINIMAP;  // lua 1.27
    lua_getfield(L, t, "bar");  // lua 1.40: a bar under the text, this share filled
    const bool has_bar = !lua_isnil(L, -1);
    lua_pop(L, 1);
    if (has_bar) {
        const EmfApi* a = s->host->api;  // an older core would draw no bar: an error, so a pcall can send it without
        if (!a || a->version < 47 || a->size < offsetof(EmfApi, spawn_facing) + sizeof(a->spawn_facing))
            luaL_error(L, "%s: bar needs EMF 0.6.19 or newer (core API v47)", fn);
        const double bar = number_field(L, t, "bar", fn, 0);
        if (!(bar >= 0 && bar <= 1)) luaL_error(L, "%s: bar must be a number from 0 to 1", fn);
        m.flags |= EMF_MARKER_BAR;
        m.bar = static_cast<float>(bar);
    }
    return sp;
}

// The script's own id for `id`; raises when the full id is not a valid marker id.
}  // namespace

std::string scoped_id(lua_State* L, Script* s, const char* id, const char* fn) {
    std::string f = s->id + ":" + id;
    bool ok = !f.empty() && f.size() <= 48 && id && *id;
    for (char c : f)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
              c == '-' || c == ':'))
            ok = false;
    if (!ok) luaL_error(L, "%s: id '%s' (A-Z a-z 0-9 _ . - :, at most %d with the mod's id)", fn, id ? id : "",
                        static_cast<int>(47 - s->id.size()));
    return f;
}

namespace {

std::string full_id(lua_State* L, Script* s, const char* id, const char* fn) { return scoped_id(L, s, id, fn); }

uint32_t player_arg(lua_State* L, int i) { return lua_isnoneornil(L, i) ? 0u : static_cast<uint32_t>(luaL_checkinteger(L, i)); }

}  // namespace

// emf.marker(id, spec) -> true | false: a marker on this game (not the dedicated server).
int l_marker(lua_State* L) {
    Script* s = script_of(L);
    const char* id = luaL_checkstring(L, 1);
    const std::string full = full_id(L, s, id, "emf.marker");
    Spec sp = read_spec(L, s, id, 2, "emf.marker");
    sp.m.id = sp.id.c_str();
    sp.m.text = sp.text.empty() ? nullptr : sp.text.c_str();
    const EmfApi* a = s->host->api;
    const bool ok = has_v33(a) && a->marker_set && a->marker_set(&sp.m);
    if (ok) s->markers.insert(full);
    lua_pushboolean(L, ok);
    return 1;
}

int l_remove_marker(lua_State* L) {
    Script* s = script_of(L);
    const std::string full = full_id(L, s, luaL_checkstring(L, 1), "emf.remove_marker");
    const EmfApi* a = s->host->api;
    const bool ok = has_v33(a) && a->marker_remove && a->marker_remove(full.c_str());
    s->markers.erase(full);
    lua_pushboolean(L, ok);
    return 1;
}

int l_clear_markers(lua_State* L) {
    Script* s = script_of(L);
    const EmfApi* a = s->host->api;
    lua_Integer n = 0;
    for (const auto& id : s->markers)
        if (has_v33(a) && a->marker_remove && a->marker_remove(id.c_str())) ++n;
    s->markers.clear();
    lua_pushinteger(L, n);
    return 1;
}

int l_marker_types(lua_State* L) {
    Script* s = script_of(L);
    const EmfApi* a = s->host->api;
    lua_newtable(L);
    if (!has_v33(a) || !a->marker_types) return 1;
    const uint32_t n = a->marker_types(nullptr, 0);
    std::vector<EmfMapMarkerType> t(n);
    const uint32_t got = n ? a->marker_types(t.data(), n) : 0;
    for (uint32_t i = 0; i < got && i < n; ++i) {
        lua_pushinteger(L, t[i].id);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i) + 1);
    }
    return 1;
}

int l_world_to_screen(lua_State* L) {
    Script* s = script_of(L);
    const double x = luaL_checknumber(L, 1), y = luaL_checknumber(L, 2), z = luaL_checknumber(L, 3);
    const EmfApi* a = s->host->api;
    float px = 0, py = 0;
    if (!has_v33(a) || !a->world_to_screen || !a->world_to_screen(x, y, z, &px, &py)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, px);
    lua_pushnumber(L, py);
    return 2;
}

// emf.send_marker(player | nil, id, spec) -> the number of players it went to (server side).
int l_send_marker(lua_State* L) {
    Script* s = script_of(L);
    const uint32_t player = player_arg(L, 1);
    const char* id = luaL_checkstring(L, 2);
    const std::string full = full_id(L, s, id, "emf.send_marker");
    Spec sp = read_spec(L, s, id, 3, "emf.send_marker");
    sp.m.id = sp.id.c_str();
    sp.m.text = sp.text.empty() ? nullptr : sp.text.c_str();
    const EmfApi* a = s->host->api;
    const uint32_t n = has_v33(a) && a->marker_send ? a->marker_send(player, &sp.m, 1) : 0;
    s->sent_markers.insert({player, full});
    lua_pushinteger(L, n);
    return 1;
}

int l_unsend_marker(lua_State* L) {
    Script* s = script_of(L);
    const uint32_t player = player_arg(L, 1);
    const std::string full = full_id(L, s, luaL_checkstring(L, 2), "emf.unsend_marker");
    const EmfApi* a = s->host->api;
    const uint32_t n = has_v33(a) && a->marker_unsend ? a->marker_unsend(player, full.c_str()) : 0;
    if (player == 0) {
        for (auto it = s->sent_markers.begin(); it != s->sent_markers.end();)
            it = it->second == full ? s->sent_markers.erase(it) : std::next(it);
    } else {
        s->sent_markers.erase({player, full});
    }
    lua_pushinteger(L, n);
    return 1;
}

void drop_markers(Host::Impl& h, Script& s) {
    const EmfApi* a = h.api;
    if (has_v33(a)) {
        for (const auto& id : s.markers)
            if (a->marker_remove) a->marker_remove(id.c_str());
        for (const auto& [player, id] : s.sent_markers)
            if (a->marker_unsend) a->marker_unsend(player, id.c_str());
    }
    s.markers.clear();
    s.sent_markers.clear();
}

}  // namespace emf::lua
