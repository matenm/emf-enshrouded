// API v33 markers (api.h): a mod's markers on a game, a server's marker_send for players' games, world_to_screen, the
// client world's tick (the UiPlayData pointer, the local player's position, followed entities), the server world's
// tick (followed positions for players' games) and the hooks on the game's clear_map_markers_for_ui (EMF's entries in
// the game's own map list). Private spec 2026-11-11-markers-design, plan 2026-11-11-markers-step1.
#include <windows.h>

#include <intrin.h>

#include <cstdio>
#include <cstring>

#include "common/offsets.h"
#include "common/seh.h"
#include "common/world_view.h"
#include "core/api.h"
#include "core/fault_report.h"
#include "core/game_screen.h"
#include "core/overlay/input_hook.h"
#include "core/marker_capture.h"
#include "core/system_programs.h"

#include <cmath>
#include <map>

namespace emf::core {

namespace {

constexpr uint32_t kCurrentTransformHash = 0x027ac564u;  // CurrentTransform: the position (3 x s64, 32.32) at +0
constexpr double kFixed = 4294967296.0;

uint8_t* raw(EmfWorld* w) { return reinterpret_cast<uint8_t*>(w); }

uint64_t now_ms(const CoreState* s) { return s && s->now_ms ? s->now_ms() : GetTickCount64(); }

void say_limited(CoreState* s, const std::string& line) {
    std::string out;
    {
        std::lock_guard<std::mutex> l(s->markers.log_lock);
        out = s->markers.log.pass(line, now_ms(s));
    }
    if (!out.empty() && s->log_line) s->log_line(out);
}

const char* why_of(MarkerStore::SetResult r) {
    switch (r) {
        case MarkerStore::SetResult::BadId: return "a bad id (1-48 of A-Z a-z 0-9 _ . - :)";
        case MarkerStore::SetResult::BadNumbers:
            return "bad numbers (a size smaller than v33's EmfMarker, coordinates not finite or beyond 100 km, a bad "
                   "height, max_distance or shape)";
        case MarkerStore::SetResult::OwnerFull: return "the mod has 256 markers already";
        case MarkerStore::SetResult::Full: return "this game has 1024 markers already";
        default: return "";
    }
}

// "never seen", "seen 0.12 s ago", "seen 45 s ago", "seen 10 min ago" (a time a moment ahead of now: 0).
std::string seen_ago(uint64_t at, uint64_t now) {
    if (!at) return "never seen";
    const uint64_t d = now > at ? now - at : 0;
    char b[48];
    if (d < 10000) std::snprintf(b, sizeof b, "seen %.2f s ago", static_cast<double>(d) / 1000.0);
    else if (d < 120000) std::snprintf(b, sizeof b, "seen %llu s ago", static_cast<unsigned long long>(d / 1000));
    else std::snprintf(b, sizeof b, "seen %llu min ago", static_cast<unsigned long long>(d / 60000));
    return b;
}

bool pos_in(EmfWorld* w, uint32_t entity, double out[3]) {
    CoreState* s = core_state();
    if (!s || !s->get_component || !w || !entity) return false;
    const ComponentRef c = get_component(s->get_component, raw(w), kCurrentTransformHash, entity);
    int64_t p[3];
    if (!c.ptr || c.size < sizeof p || !seh_read(c.ptr, p, sizeof p)) return false;
    for (int i = 0; i < 3; ++i) out[i] = static_cast<double>(p[i]) / kFixed;
    return true;
}

// The game's map list in UiPlayData: data, count, capacity (the count's offset from the MapMarkerSliceClear guard).
bool read_list(uint8_t* uipd, uint32_t count_off, MapList* l) {
    return uipd && count_off >= 8 && seh_read(uipd + count_off - 8, &l->data, 8) && seh_read(uipd + count_off, &l->count, 8) &&
           seh_read(uipd + count_off + 8, &l->capacity, 8);
}

bool write_count(uint8_t* uipd, uint32_t count_off, uint64_t count) {
    __try {
        std::memcpy(uipd + count_off, &count, sizeof count);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint8_t* list_owner(void* ctx, uint32_t* count_off) {
    CoreState* s = core_state();
    if (!s || !s->markers.list_count_off) return nullptr;
    uint8_t* w = program_context_world(ctx);
    if (!w) return nullptr;
    *count_off = s->markers.list_count_off;
    return static_cast<uint8_t*>(WorldView(w).singleton("G38UiPlayData"));
}

}  // namespace

bool api_marker_set(const EmfMarker* m) {
    CoreState* s = core_state();
    if (!s || !m) return false;
    const std::string owner = module_name_of(_ReturnAddress());
    if (s->exe.server) {
        say_limited(s, "marker_set refused (" + owner + "): the dedicated server draws nothing; marker_send gives players' "
                       "games markers");
        return false;
    }
    const MarkerStore::SetResult r = s->markers.store.set(owner, *m, false);
    if (r == MarkerStore::SetResult::Ok || r == MarkerStore::SetResult::Replaced) return true;
    say_limited(s, "marker_set refused (" + owner + "): " + why_of(r));
    return false;
}

bool api_marker_remove(const char* id) {
    CoreState* s = core_state();
    if (!s || !id) return false;
    return s->markers.store.remove(module_name_of(_ReturnAddress()), id, false);
}

uint32_t api_marker_clear() {
    CoreState* s = core_state();
    if (!s) return 0;
    return static_cast<uint32_t>(s->markers.store.clear(module_name_of(_ReturnAddress())));
}

uint32_t api_marker_types(EmfMapMarkerType* out, uint32_t cap) {
    CoreState* s = core_state();
    if (!s) return 0;
    const std::vector<uint32_t> types = s->markers.map.seen_types();
    for (uint32_t i = 0; out && i < cap && i < types.size(); ++i) {
        out[i] = EmfMapMarkerType{};
        out[i].size = sizeof(EmfMapMarkerType);
        out[i].id = types[i];
    }
    return static_cast<uint32_t>(types.size());
}

bool api_world_to_screen(double x, double y, double z, float* px, float* py) {
    CameraSnap c;
    float w = 0, h = 0;
    if (!marker_camera(&c) || !marker_display(&w, &h)) return false;
    const uint64_t now = now_ms(core_state());
    if (now < c.frame_ms || now - c.frame_ms > kMarkerFreshMs) return false;  // a menu, a loading screen: no answer
    const auto p = project(c, x, y, z, w, h);
    if (!p) return false;
    if (px) *px = p->x;
    if (py) *py = p->y;
    return true;
}

uint32_t api_marker_send(uint32_t player, const EmfMarker* m, uint32_t count) {
    CoreState* s = core_state();
    if (!s || !m || !count) return 0;
    const std::string owner = module_name_of(_ReturnAddress());
    if (!s->markers.sender) {
        say_limited(s, "marker_send refused (" + owner + "): mod messages are not available on this game build");
        return 0;
    }
    return s->markers.sender->send(owner, player, m, count);
}

uint32_t api_marker_unsend(uint32_t player, const char* id) {
    CoreState* s = core_state();
    if (!s || !id || !s->markers.sender) return 0;
    return s->markers.sender->unsend(module_name_of(_ReturnAddress()), player, id);
}

void markers_client_tick(EmfWorld* client) {
    CoreState* s = core_state();
    if (!s || !client) return;
    auto& mk = s->markers;
    if (mk.client != client) {
        if (mk.client) mk.receiver.reset(mk.store);  // another world: the old server's markers go
        mk.map.reset();
        mk.client = client;
    }
    const WorldView view(raw(client));
    auto* uipd = static_cast<uint8_t*>(view.singleton("G38UiPlayData"));
    marker_capture_set_uipd(uipd, now_ms(s));
    game_screen().read(uipd, GetTickCount64());  // the chat key gate (game_screen.h): the game's clock, as the hooks read it
    uint32_t me = 0;
    const auto* lpd = static_cast<const uint8_t*>(view.singleton("LocalPlayerData"));
    double p[3];
    if (lpd && seh_read(lpd + off::kLocalPlayerId, &me, sizeof me) && me && pos_in(client, me, p)) {
        marker_capture_set_player(true, p[0], p[1], p[2]);
        // v35 player_view: the rotation (a quaternion after the 24-byte position)
        float q[4];
        const ComponentRef c = get_component(s->get_component, raw(client), kCurrentTransformHash, me);
        if (c.ptr && c.size >= 0x28 && seh_read(static_cast<const uint8_t*>(c.ptr) + 0x18, q, sizeof q) &&
            std::isfinite(q[0]) && std::isfinite(q[1]) && std::isfinite(q[2]) && std::isfinite(q[3]))
            marker_capture_set_player_rotation(q[0], q[1], q[2], q[3]);
    } else {
        marker_capture_set_player(false, 0, 0, 0);
    }
    mk.store.follow([client](uint32_t e, double out[3]) { return pos_in(client, e, out); });
}

void markers_server_tick(EmfWorld* server) {
    CoreState* s = core_state();
    if (!s || !server || !s->markers.sender) return;
    if (s->markers.server != server) {
        if (s->markers.server) s->markers.sender->reset();  // another world: nothing of the old one is followed
        s->markers.server = server;
    }
    const uint64_t now = now_ms(s);
    if (now - s->markers.refresh_ms < kMarkerRefreshMs) return;
    s->markers.refresh_ms = now;
    s->markers.sender->refresh([server](uint32_t e, double out[3]) { return pos_in(server, e, out); });
}

void markers_world_setup(uint8_t kind) {
    CoreState* s = core_state();
    if (!s) return;
    if (kind == EMF_WORLD_CLIENT) {
        s->markers.receiver.reset(s->markers.store);  // a server's markers belong to the session that sent them
        s->markers.map.reset();
        marker_capture_set_uipd(nullptr, 0);
        marker_capture_set_player(false, 0, 0, 0);
        s->markers.client = nullptr;
        s->minimap.fog.clear();  // v35: the last world's fog of war goes with it
    } else if (kind == EMF_WORLD_SERVER) {
        if (s->markers.sender) s->markers.sender->reset();
        s->markers.server = nullptr;
    }
}

void markers_map_pre(void* ctx) {
    uint32_t off = 0;
    uint8_t* u = list_owner(ctx, &off);
    MapList l;
    if (!u || !read_list(u, off, &l)) return;
    CoreState* s = core_state();
    const auto canvases = s->minimap.canvases.view();
    s->markers.map.pre(&l, minimap_wanted(canvases && !canvases->empty(),
                                          s->minimap.markers_asked_ms.load(std::memory_order_relaxed), now_ms(s)));
}

void markers_map_post(void* ctx) {
    uint32_t off = 0;
    uint8_t* u = list_owner(ctx, &off);
    MapList l;
    if (!u || !read_list(u, off, &l)) return;
    CoreState* s = core_state();
    double player[3];
    const bool has_player = marker_player_pos(player);
    const uint64_t before = l.count;
    if (s->markers.map.post(&l, s->markers.store.snapshot(), has_player ? player : nullptr) && l.count != before)
        write_count(u, off, l.count);
}

std::vector<std::string> markers_status() {
    std::vector<std::string> out;
    CoreState* s = core_state();
    if (!s) return out;
    const auto all = s->markers.store.snapshot();
    std::map<std::string, size_t> owners;
    size_t received = 0;
    for (const auto& m : all) {
        ++owners[(m.received ? "from the server: " : "") + m.owner];
        received += m.received ? 1 : 0;
    }
    char line[256];
    std::snprintf(line, sizeof line, "markers: %zu marker%s (%zu from the server)", all.size(), all.size() == 1 ? "" : "s",
                  received);
    out.push_back(line);
    for (const auto& [o, n] : owners) out.push_back("  " + o + ": " + std::to_string(n));
    CameraSnap c;
    const uint64_t now = now_ms(s);
    const bool cam = marker_camera(&c);
    out.push_back(std::string("  world: ") + (s->markers.world_why.empty() ? "on" : "off (" + s->markers.world_why + ")") +
                  "; camera " + (cam ? (now - c.frame_ms <= kMarkerFreshMs ? "fresh" : "not fresh (a menu, a pause or "
                                                                                    "a loading screen)")
                                     : "not seen yet"));
    // where canvases and world markers draw (the overlay and the minimap check marker_sight): a player's report of
    // panels that never show, or flash, says which of the two scopes the game did not open
    const MarkerSeen seen = marker_seen();
    out.push_back("  the game's HUD scope (doCachedEntityHealthWidgets): " + seen_ago(seen.world_ui_ms, now));
    out.push_back("  the game's map screen (mapScaled): " + seen_ago(seen.map_ms, now));
    char rule[160];
    std::snprintf(rule, sizeof rule,
                  "  canvases and world markers draw while the HUD scope was seen in the last %.2f s and the map screen "
                  "was not (and the console is closed)",
                  static_cast<double>(kMarkerFreshMs) / 1000.0);
    out.push_back(rule);
    out.push_back(std::string("  the game's map list: ") +
                  (s->markers.list_why.empty() ? "on (EMF's entries for markers with a game icon)"
                                               : "off (" + s->markers.list_why + ")"));
    const auto types = s->markers.map.seen_types();
    if (!types.empty()) {
        std::string t = "  map icon types the game used here:";
        for (uint32_t id : types) {
            char b[16];
            std::snprintf(b, sizeof b, " %08x", id);
            t += b;
        }
        out.push_back(t);
    }
    return out;
}

bool markers_test(const std::string& text, uint32_t game_icon, std::string* why) {
    CoreState* s = core_state();
    double p[3];
    CameraSnap c;
    if (!s || !marker_player_pos(p)) {
        if (why) *why = "no local player (load a world first)";
        return false;
    }
    if (!marker_camera(&c)) {
        if (why) *why = "no camera yet (the game has not drawn its world UI)";
        return false;
    }
    // the camera's forward (row 2 of the view's columns: x V[2], z V[10]), flat
    double fx = c.v[2], fz = c.v[10];
    const double len = std::sqrt(fx * fx + fz * fz);
    if (len < 1e-6) {
        fx = 0;
        fz = 1;
    } else {
        fx /= len;
        fz /= len;
    }
    EmfMarker m{};
    m.size = sizeof(EmfMarker);
    m.id = "test";
    m.x = p[0] + fx * 10;
    m.y = p[1];
    m.z = p[2] + fz * 10;
    m.height = 1.5f;
    m.text = text.empty() ? "EMF test" : text.c_str();
    m.flags = EMF_MARKER_WORLD | EMF_MARKER_GAME_MAP | EMF_MARKER_EDGE_ARROW | EMF_MARKER_DISTANCE;
    m.game_icon = game_icon;
    const auto r = s->markers.store.set("emf", m, false);
    if (r != MarkerStore::SetResult::Ok && r != MarkerStore::SetResult::Replaced) {
        if (why) *why = why_of(r);
        return false;
    }
    return true;
}

namespace {

void cmd_markers(EmfWorld*, int, const char* const*, void*) {
    for (const auto& l : markers_status()) command_reply_line(l);
}

void cmd_markers_test(EmfWorld*, int argc, const char* const* argv, void*) {
    CoreState* s = core_state();
    if (argc > 1 && std::string(argv[1]) == "clear") {
        command_reply_line(s && s->markers.store.remove("emf", "test", false) ? "markers.test: removed"
                                                                              : "markers.test: there is none");
        return;
    }
    const std::string text = argc > 1 ? argv[1] : "";
    const uint32_t icon = argc > 2 ? static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 16)) : 0;
    std::string why;
    if (markers_test(text, icon, &why))
        command_reply_line("markers.test: a marker 10 m ahead of you (markers.test clear removes it)");
    else
        command_reply_line("markers.test: " + why);
}

void cmd_screen(EmfWorld*, int, const char* const*, void*) {
    const CoreState* s = core_state();
    command_reply_line(game_screen_line(game_screen().screen(GetTickCount64()), game_screen().offset(),
                                        s && s->exe.server, key_state_hooked()));
}

}  // namespace

bool add_marker_commands(CommandCenter& commands) {
    // anywhere: on a game the player joined too (markers are drawn on every game)
    bool ok = commands.add("markers",
                           ": the markers this game draws, whether drawing and the game's map list are on, and when the "
                           "game last showed its HUD and map screen",
                           cmd_markers, nullptr, nullptr, true);
    ok &= commands.add("markers.test", " [text] [game icon type, hex] | clear: a marker 10 m ahead of you", cmd_markers_test,
                       nullptr, nullptr, true);
    ok &= commands.add("screen",
                       ": which game screen is open (the game's chat, a menu, play) and whether mods' keys read as up "
                       "(they do while the game's chat or rename box is open)",
                       cmd_screen, nullptr, nullptr, true);
    return ok;
}

}  // namespace emf::core
