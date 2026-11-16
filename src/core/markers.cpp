// Markers (API v33): the game-free part (markers.h).
#include "core/markers.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/bridge/json_out.h"
#include "core/json.h"

namespace emf::core {

namespace {

bool finite_within(double v, double limit) { return std::isfinite(v) && std::fabs(v) <= limit; }

uint32_t field_u32(const json::Value* v, uint32_t def = 0) {
    if (!v || !v->is(json::Value::Type::Number) || !(v->n >= 0) || v->n > 4294967295.0) return def;
    return static_cast<uint32_t>(v->n);
}
double field_num(const json::Value* v, double def = 0) {
    return v && v->is(json::Value::Type::Number) ? v->n : def;
}
std::string field_str(const json::Value* v) { return v && v->is(json::Value::Type::String) ? v->s : std::string(); }

}  // namespace

bool marker_id_ok(const char* id) {
    if (!id) return false;
    const size_t n = std::strlen(id);
    if (n == 0 || n > kMarkerIdMax) return false;
    for (size_t i = 0; i < n; ++i) {
        const char c = id[i];
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
                        c == '.' || c == '-' || c == ':';
        if (!ok) return false;
    }
    return true;
}

std::string marker_cut_utf8(std::string s, size_t n) {
    if (s.size() <= n) return s;
    size_t end = n;
    // step back over continuation bytes to the start of the character that would be cut
    while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
    s.resize(end);
    return s;
}

bool marker_numbers_ok(const EmfMarker& m) {
    return m.size >= sizeof(EmfMarker) && finite_within(m.x, kMarkerCoordMax) && finite_within(m.y, kMarkerCoordMax) &&
           finite_within(m.z, kMarkerCoordMax) && finite_within(m.height, 1000.0) &&
           finite_within(m.max_distance, kMarkerCoordMax) && m.max_distance >= 0 && m.shape <= EMF_MARKER_NONE;
}

// ---- MarkerStore ----
MarkerStore::SetResult MarkerStore::set(const std::string& owner, const EmfMarker& m, bool received) {
    if (!marker_id_ok(m.id)) return SetResult::BadId;
    if (!marker_numbers_ok(m)) return SetResult::BadNumbers;
    Marker k;
    k.owner = owner;
    k.id = m.id;
    k.received = received;
    k.x = m.x;
    k.y = m.y;
    k.z = m.z;
    k.entity = m.entity;
    k.height = m.height;
    k.text = marker_cut_utf8(m.text ? std::string(m.text) : std::string(), kMarkerTextBytes);
    k.icon = received ? 0 : m.icon;  // a picture handle means nothing on another game
    k.rgba = m.rgba;
    k.shape = m.shape;
    k.flags = m.flags & (EMF_MARKER_WORLD | EMF_MARKER_GAME_MAP | EMF_MARKER_EDGE_ARROW | EMF_MARKER_DISTANCE |
                         EMF_MARKER_MINIMAP);
    k.max_distance = m.max_distance;
    k.game_icon = m.game_icon;
    std::lock_guard<std::mutex> lock(m_);
    Key key{received, owner, k.id};
    auto it = markers_.find(key);
    if (it != markers_.end()) {
        it->second = std::move(k);
        return SetResult::Replaced;
    }
    if (markers_.size() >= kMarkersTotal) return SetResult::Full;
    size_t mine = 0;
    for (const auto& [kk, v] : markers_)
        if (std::get<0>(kk) == received && std::get<1>(kk) == owner) ++mine;
    if (mine >= kMarkersPerOwner) return SetResult::OwnerFull;
    markers_.emplace(std::move(key), std::move(k));
    return SetResult::Ok;
}

bool MarkerStore::remove(const std::string& owner, const std::string& id, bool received) {
    std::lock_guard<std::mutex> lock(m_);
    return markers_.erase(Key{received, owner, id}) > 0;
}

size_t MarkerStore::clear(const std::string& owner) {
    std::lock_guard<std::mutex> lock(m_);
    return std::erase_if(markers_, [&](const auto& kv) { return !std::get<0>(kv.first) && std::get<1>(kv.first) == owner; });
}

size_t MarkerStore::clear_received() {
    std::lock_guard<std::mutex> lock(m_);
    return std::erase_if(markers_, [](const auto& kv) { return std::get<0>(kv.first); });
}

bool MarkerStore::move(const std::string& owner, const std::string& id, double x, double y, double z) {
    if (!finite_within(x, kMarkerCoordMax) || !finite_within(y, kMarkerCoordMax) || !finite_within(z, kMarkerCoordMax))
        return false;
    std::lock_guard<std::mutex> lock(m_);
    auto it = markers_.find(Key{true, owner, id});
    if (it == markers_.end()) return false;
    it->second.x = x;
    it->second.y = y;
    it->second.z = z;
    return true;
}

std::vector<Marker> MarkerStore::snapshot() const {
    std::lock_guard<std::mutex> lock(m_);
    std::vector<Marker> out;
    out.reserve(markers_.size());
    for (const auto& [k, v] : markers_) out.push_back(v);
    return out;
}

size_t MarkerStore::size() const {
    std::lock_guard<std::mutex> lock(m_);
    return markers_.size();
}

void MarkerStore::follow(const std::function<bool(uint32_t, double out[3])>& pos_of) {
    std::lock_guard<std::mutex> lock(m_);
    for (auto& [k, v] : markers_) {
        if (!v.entity) continue;
        double p[3];
        if (pos_of(v.entity, p) && finite_within(p[0], kMarkerCoordMax) && finite_within(p[1], kMarkerCoordMax) &&
            finite_within(p[2], kMarkerCoordMax)) {
            v.x = p[0];
            v.y = p[1];
            v.z = p[2];
        }
    }
}

// ---- projection (worldToUi, research 1.1) ----
namespace {

struct ViewPoint {
    double x = 0, y = 0, depth = 0;
};

ViewPoint to_view(const CameraSnap& c, double x, double y, double z) {
    const float* V = c.v;
    ViewPoint p;
    p.depth = x * V[2] + y * V[6] + z * V[10] + V[14];
    p.x = x * V[0] + y * V[4] + z * V[8] + V[12];
    p.y = x * V[1] + y * V[5] + z * V[9] + V[13];
    return p;
}

bool has_projection(const CameraSnap& c) { return c.valid && c.p[0] != 0 && c.p[5] != 0; }

constexpr double kNearDepth = 1.1920929e-7;

}  // namespace

std::optional<ScreenPoint> project(const CameraSnap& cam, double x, double y, double z, float w, float h) {
    if (!has_projection(cam) || !(w > 0) || !(h > 0)) return std::nullopt;
    const ViewPoint v = to_view(cam, x, y, z);
    if (!(v.depth > kNearDepth)) return std::nullopt;
    const float* P = cam.p;
    const double vx = v.x / v.depth, vy = v.y / v.depth;
    const double nx = vx * P[0] + vy * P[4] + P[8] + P[12];
    const double ny = vx * P[1] + vy * P[5] + P[9] + P[13];
    ScreenPoint s;
    s.x = static_cast<float>((nx * 0.5 + 0.5) * w);
    s.y = static_cast<float>((0.5 - ny * 0.5) * h);
    s.depth = static_cast<float>(v.depth);
    if (!std::isfinite(s.x) || !std::isfinite(s.y)) return std::nullopt;
    return s;
}

EdgePoint edge_point(const CameraSnap& cam, double x, double y, double z, float w, float h, float inset) {
    EdgePoint e;
    if (!has_projection(cam) || !(w > 2 * inset) || !(h > 2 * inset)) return e;
    const double cx = w * 0.5, cy = h * 0.5;
    double dx = 0, dy = 0;
    const ViewPoint v = to_view(cam, x, y, z);
    if (v.depth > kNearDepth) {
        const auto s = project(cam, x, y, z, w, h);
        if (!s) return e;
        dx = s->x - cx;
        dy = s->y - cy;
    } else {
        // behind: the side it is on, from the view-space direction (right = +x; up is the screen's -y). It never
        // points to the top: behind and above goes along the side it is on.
        e.behind = true;
        dx = v.x * cam.p[0];
        dy = -v.y * cam.p[5];
        if (std::fabs(dx) < 1e-9) {
            dx = 0;
            dy = 1;  // straight behind: the bottom
        } else if (dy < 0) {
            dy = 0;
        }
    }
    if (!std::isfinite(dx) || !std::isfinite(dy) || (dx == 0 && dy == 0)) return e;
    const double hx = cx - inset, hy = cy - inset;
    const double t = std::min(dx != 0 ? hx / std::fabs(dx) : 1e30, dy != 0 ? hy / std::fabs(dy) : 1e30);
    e.x = static_cast<float>(cx + dx * t);
    e.y = static_cast<float>(cy + dy * t);
    e.angle = static_cast<float>(std::atan2(dy, dx));
    e.valid = true;
    return e;
}

float fade(double dist, float max_distance) {
    if (!(max_distance > 0)) return 1.0f;
    if (!(dist < max_distance)) return 0.0f;
    const double start = 0.8 * max_distance;
    if (dist <= start) return 1.0f;
    return static_cast<float>((max_distance - dist) / (max_distance - start));
}

std::string distance_text(double metres) {
    if (!std::isfinite(metres) || metres < 0) return {};
    char buf[32];
    if (metres < 1000)
        std::snprintf(buf, sizeof buf, "%d m", static_cast<int>(std::floor(metres + 0.5)));
    else
        std::snprintf(buf, sizeof buf, "%.1f km", metres / 1000.0);
    return buf;
}

// ---- the wire format ----
std::string encode_markers(const std::string& owner, const std::vector<EmfMarker>& markers,
                           const std::vector<std::string>& removes) {
    using bridge::json_list;
    using bridge::json_quote;
    using bridge::JsonObj;
    std::vector<std::string> s;
    s.reserve(markers.size());
    for (const EmfMarker& m : markers) {
        JsonObj o;
        o.str("i", m.id ? m.id : "").num("x", m.x).num("y", m.y).num("z", m.z);
        if (m.entity) o.num("e", m.entity);
        if (m.text && *m.text) o.str("t", m.text);
        if (m.height != 0) o.num("h", m.height);
        if (m.max_distance != 0) o.num("d", m.max_distance);
        if (m.rgba) o.num("c", m.rgba);
        if (m.shape) o.num("k", m.shape);
        o.num("f", m.flags);
        if (m.game_icon) o.num("g", m.game_icon);
        s.push_back(o.text());
    }
    std::vector<std::string> r;
    for (const auto& id : removes) r.push_back(json_quote(id));
    JsonObj top;
    top.str("o", owner);
    if (!s.empty()) top.raw("s", json_list(s));
    if (!r.empty()) top.raw("r", json_list(r));
    return top.text();
}

std::string encode_positions(const std::string& owner, const std::vector<MarkerPosition>& positions) {
    using bridge::json_list;
    using bridge::json_number;
    using bridge::json_quote;
    std::vector<std::string> p;
    for (const auto& q : positions)
        p.push_back(json_list({json_quote(q.id), json_number(q.x), json_number(q.y), json_number(q.z)}));
    bridge::JsonObj top;
    top.str("o", owner).raw("p", json_list(p));
    return top.text();
}

bool decode_markers(const std::string& text, DecodedMarkers* out) {
    if (!out) return false;
    *out = DecodedMarkers{};
    const json::ParseResult r = json::parse(text);
    if (!r.value || !r.value->is(json::Value::Type::Object)) return false;
    const json::Value& top = *r.value;
    out->owner = field_str(top.get("o"));
    if (out->owner.empty() || out->owner.size() > kMarkerIdMax) return false;
    if (const json::Value* s = top.get("s")) {
        if (!s->is(json::Value::Type::Array) || s->arr.size() > kMarkersPerMessage) return false;
        for (const json::Value& v : s->arr) {
            if (!v.is(json::Value::Type::Object)) return false;
            DecodedMarker d;
            d.id = field_str(v.get("i"));
            d.text = field_str(v.get("t"));
            EmfMarker& m = d.marker;
            m.size = sizeof(EmfMarker);
            m.x = field_num(v.get("x"));
            m.y = field_num(v.get("y"));
            m.z = field_num(v.get("z"));
            m.entity = field_u32(v.get("e"));
            m.height = static_cast<float>(field_num(v.get("h")));
            m.max_distance = static_cast<float>(field_num(v.get("d")));
            m.rgba = field_u32(v.get("c"));
            m.shape = static_cast<uint8_t>(std::min<uint32_t>(field_u32(v.get("k")), 255));
            m.flags = field_u32(v.get("f"));
            m.game_icon = field_u32(v.get("g"));
            out->markers.push_back(std::move(d));
        }
    }
    if (const json::Value* rr = top.get("r")) {
        if (!rr->is(json::Value::Type::Array) || rr->arr.size() > kMarkersPerMessage) return false;
        for (const json::Value& v : rr->arr) out->removes.push_back(field_str(&v));
    }
    if (const json::Value* p = top.get("p")) {
        if (!p->is(json::Value::Type::Array) || p->arr.size() > kMarkersPerMessage) return false;
        for (const json::Value& v : p->arr) {
            // [id, x, y, z]: a place that is not three numbers would read as 0 (the marker to the world's origin)
            if (!v.is(json::Value::Type::Array) || v.arr.size() != 4) return false;
            if (!v.arr[0].is(json::Value::Type::String)) return false;
            for (size_t i = 1; i < 4; ++i)
                if (!v.arr[i].is(json::Value::Type::Number)) return false;
            MarkerPosition q;
            q.id = field_str(&v.arr[0]);
            q.x = field_num(&v.arr[1]);
            q.y = field_num(&v.arr[2]);
            q.z = field_num(&v.arr[3]);
            out->positions.push_back(std::move(q));
        }
    }
    return true;
}

}  // namespace emf::core
