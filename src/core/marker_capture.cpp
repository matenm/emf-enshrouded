// Markers (API v33): the capture (marker_capture.h).
#include "core/marker_capture.h"

#include "common/seh.h"

namespace emf::core {

namespace {

struct PlayerPos {
    double p[3] = {};
};
struct UiSize {
    float w = 0, h = 0;
};
struct MapRects {
    float rect[4] = {};
    float clip[4] = {};
};

std::atomic<uint32_t> g_view_off{0};
std::atomic<uint8_t*> g_uipd{nullptr};
std::atomic<uint64_t> g_uipd_ms{0};
std::atomic<uint64_t> g_world_ui_ms{0}, g_map_ms{0};
std::atomic<uint32_t> g_faults{0};
SeqBox<CameraSnap> g_camera;
SeqBox<PlayerPos> g_player;
struct PlayerRot {
    float q[4] = {0, 0, 0, 1};
};
SeqBox<PlayerRot> g_player_rot;
SeqBox<UiSize> g_ui;
SeqBox<MapRects> g_map;
SeqBox<UiSize> g_display;
// the UI thread's own copy of the map's two rects (they open in different scopes of one frame)
thread_local MapRects t_map;

bool fresh(uint64_t at, uint64_t now) { return at && now >= at && now - at <= kMarkerFreshMs; }

bool read_rect(const uint8_t* node, float out[4]) { return node && seh_read(node + kMarkerNodeRect, out, 16); }

void capture_camera(uint64_t now_ms) {
    const uint32_t off = g_view_off.load(std::memory_order_relaxed);
    uint8_t* u = g_uipd.load(std::memory_order_acquire);
    const uint64_t at = g_uipd_ms.load(std::memory_order_relaxed);
    if (!off || !u || now_ms < at || now_ms - at > kMarkerWorldStaleMs) return;
    float m[32];
    if (!seh_read(u + off, m, sizeof m)) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
        g_uipd.store(nullptr, std::memory_order_release);  // not read again until the client world sets it anew
        return;
    }
    CameraSnap c;
    std::memcpy(c.v, m, sizeof c.v);
    std::memcpy(c.p, m + 16, sizeof c.p);
    c.frame_ms = now_ms;
    c.valid = true;
    g_camera.store(c);
}

}  // namespace

void marker_capture_set_layout(uint32_t view_off) { g_view_off.store(view_off, std::memory_order_relaxed); }

bool marker_capture_ready() { return g_view_off.load(std::memory_order_relaxed) != 0; }

void marker_capture_set_uipd(uint8_t* uipd, uint64_t now_ms) {
    g_uipd_ms.store(now_ms, std::memory_order_relaxed);
    g_uipd.store(uipd, std::memory_order_release);
}

void marker_capture_set_player(bool known, double x, double y, double z) {
    if (!known) {
        g_player.reset();
        g_player_rot.reset();
        return;
    }
    PlayerPos p;
    p.p[0] = x;
    p.p[1] = y;
    p.p[2] = z;
    g_player.store(p);
}

void markers_ui_frame(uint64_t hash, const uint8_t* node, uint64_t now_ms) {
    if (hash == kMarkerUiSpaceHash) {
        capture_camera(now_ms);
        float r[4];
        if (read_rect(node, r) && r[2] > 0 && r[3] > 0) g_ui.store(UiSize{r[2], r[3]});
        return;
    }
    if (hash == kMarkerWorldUiHash) {
        g_world_ui_ms.store(now_ms, std::memory_order_relaxed);
        return;
    }
    if (hash == kMarkerMapScaledHash) {
        if (read_rect(node, t_map.rect)) {
            g_map.store(t_map);
            g_map_ms.store(now_ms, std::memory_order_relaxed);
        }
        return;
    }
    if (hash == kMarkerMapFrameHash) {
        if (read_rect(node, t_map.clip)) g_map.store(t_map);
    }
}

void marker_capture_set_display(float w, float h) {
    if (w > 0 && h > 0) g_display.store(UiSize{w, h});
}

bool marker_display(float* w, float* h) {
    UiSize s;
    if (!g_display.load(&s) && !g_ui.load(&s)) return false;
    if (w) *w = s.w;
    if (h) *h = s.h;
    return true;
}

bool marker_camera(CameraSnap* out) { return out && g_camera.load(out) && out->valid; }

void marker_capture_set_player_rotation(float x, float y, float z, float w) {
    PlayerRot r;
    r.q[0] = x;
    r.q[1] = y;
    r.q[2] = z;
    r.q[3] = w;
    g_player_rot.store(r);
}

bool marker_player_rotation(float out[4]) {
    PlayerRot r;
    if (!out || !g_player_rot.load(&r)) return false;
    std::memcpy(out, r.q, sizeof r.q);
    return true;
}

bool marker_ui_size(float* w, float* h) {
    UiSize u;
    if (!w || !h || !g_ui.load(&u) || !(u.w > 0) || !(u.h > 0)) return false;
    *w = u.w;
    *h = u.h;
    return true;
}

bool marker_player_pos(double out[3]) {
    PlayerPos p;
    if (!out || !g_player.load(&p)) return false;
    std::memcpy(out, p.p, sizeof p.p);
    return true;
}

MarkerSight marker_sight(uint64_t now_ms) {
    MarkerSight s;
    s.world_ui = fresh(g_world_ui_ms.load(std::memory_order_relaxed), now_ms);
    s.map_open = fresh(g_map_ms.load(std::memory_order_relaxed), now_ms);
    return s;
}

MarkerSeen marker_seen() {
    MarkerSeen s;
    s.world_ui_ms = g_world_ui_ms.load(std::memory_order_relaxed);
    s.map_ms = g_map_ms.load(std::memory_order_relaxed);
    return s;
}

bool marker_map_view(MapView* out) {
    MapRects r;
    UiSize u;
    if (!out || !g_map.load(&r) || !g_ui.load(&u)) return false;
    std::memcpy(out->rect, r.rect, sizeof r.rect);
    std::memcpy(out->clip, r.clip, sizeof r.clip);
    out->ui_w = u.w;
    out->ui_h = u.h;
    out->ms = g_map_ms.load(std::memory_order_relaxed);
    return true;
}

uint32_t marker_capture_faults() { return g_faults.load(std::memory_order_relaxed); }

void marker_capture_reset_for_tests() {
    g_view_off = 0;
    g_uipd = nullptr;
    g_uipd_ms = 0;
    g_world_ui_ms = 0;
    g_map_ms = 0;
    g_faults = 0;
    g_camera.reset();
    g_player.reset();
    g_player_rot.reset();
    g_ui.reset();
    g_map.reset();
    g_display.reset();
    t_map = MapRects{};
}

}  // namespace emf::core
