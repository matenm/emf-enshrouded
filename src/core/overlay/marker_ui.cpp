// Markers (API v33) on screen (marker_ui.h).
#include "core/overlay/marker_ui.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace emf::core {

namespace {

// The camera's position from the view (rows 0-2 the axes, row 3 the translation): c = -t * R^T.
void camera_position(const CameraSnap& c, double out[3]) {
    const float* V = c.v;
    out[0] = -(V[12] * V[0] + V[13] * V[1] + V[14] * V[2]);
    out[1] = -(V[12] * V[4] + V[13] * V[5] + V[14] * V[6]);
    out[2] = -(V[12] * V[8] + V[13] * V[9] + V[14] * V[10]);
}

uint32_t with_alpha(uint32_t rgba, float alpha) {
    const float a = static_cast<float>(rgba & 0xFFu) * std::clamp(alpha, 0.0f, 1.0f);
    return (rgba & 0xFFFFFF00u) | static_cast<uint32_t>(a + 0.5f);
}

}  // namespace

std::vector<MarkerLayout> layout_markers(const std::vector<Marker>& markers, const CameraSnap& cam, const double* player,
                                         ImVec2 display) {
    std::vector<MarkerLayout> out;
    if (!cam.valid || !(display.x > 0) || !(display.y > 0)) return out;
    const float scale = display.y / 1080.0f;
    const float inset = kMarkerEdgeInset * scale;
    double from[3];
    if (player) {
        from[0] = player[0];
        from[1] = player[1];
        from[2] = player[2];
    } else {
        camera_position(cam, from);
    }
    for (const Marker& m : markers) {
        if (!(m.flags & EMF_MARKER_WORLD)) continue;
        const double dx = m.x - from[0], dy = m.y - from[1], dz = m.z - from[2];
        const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float alpha = fade(dist, m.max_distance);
        if (!(alpha > 0)) continue;
        const double top = m.y + m.height;
        MarkerLayout l;
        const auto s = project(cam, m.x, top, m.z, display.x, display.y);
        if (s && s->x >= 0 && s->x <= display.x && s->y >= 0 && s->y <= display.y) {
            l.x = s->x;
            l.y = s->y;
        } else if (m.flags & EMF_MARKER_EDGE_ARROW) {
            const EdgePoint e = edge_point(cam, m.x, top, m.z, display.x, display.y, inset);
            if (!e.valid) continue;
            l.x = e.x;
            l.y = std::min(e.y, display.y - kMarkerEdgeBottom * scale);  // never on the game's hotbar
            l.edge = true;
            l.angle = e.angle;
        } else {
            continue;
        }
        l.size = kMarkerSize1080 * scale;
        l.alpha = alpha;
        l.distance = dist;
        l.text = m.text;
        if (m.flags & EMF_MARKER_DISTANCE) l.dist = distance_text(dist);
        l.rgba = m.rgba;
        l.shape = m.shape;
        l.icon = m.icon;
        if (m.flags & EMF_MARKER_BAR) l.bar = std::clamp(m.bar, 0.0f, 1.0f);
        out.push_back(std::move(l));
    }
    // the nearest get their text; then far ones first, so near ones are drawn on top
    std::sort(out.begin(), out.end(), [](const MarkerLayout& a, const MarkerLayout& b) { return a.distance < b.distance; });
    for (size_t i = 0; i < out.size() && i < kMarkerLabels; ++i) out[i].label = true;
    std::reverse(out.begin(), out.end());
    return out;
}

bool show_markers(bool console_open, const MarkerSight& sight, const CameraSnap& cam, uint64_t now_ms) {
    return !console_open && sight.world_ui && !sight.map_open && cam.valid && now_ms >= cam.frame_ms &&
           now_ms - cam.frame_ms <= kMarkerFreshMs;
}

std::vector<MarkerLayout> layout_map_markers(const std::vector<Marker>& markers, const MapView& v, ImVec2 display) {
    std::vector<MarkerLayout> out;
    if (!(v.rect[2] > 0) || !(v.rect[3] > 0) || !(v.ui_w > 0) || !(v.ui_h > 0) || !(display.x > 0) || !(display.y > 0))
        return out;
    const float sx = display.x / v.ui_w, sy = display.y / v.ui_h, scale = display.y / 1080.0f;
    const bool clip = v.clip[2] > 0 && v.clip[3] > 0;
    const float cx0 = clip ? v.clip[0] * sx : 0, cy0 = clip ? v.clip[1] * sy : 0;
    const float cx1 = clip ? (v.clip[0] + v.clip[2]) * sx : display.x, cy1 = clip ? (v.clip[1] + v.clip[3]) * sy : display.y;
    for (const Marker& m : markers) {
        if (!(m.flags & EMF_MARKER_GAME_MAP)) continue;
        const double u = v.rect[0] + m.x / kMapWorldSize * v.rect[2];
        const double w = v.rect[1] + (1.0 - m.z / kMapWorldSize) * v.rect[3];
        MarkerLayout l;
        l.x = static_cast<float>(u * sx);
        l.y = static_cast<float>(w * sy);
        if (!(l.x >= cx0 && l.x <= cx1 && l.y >= cy0 && l.y <= cy1)) continue;
        l.size = kMapMarkerSize1080 * scale;
        l.label = true;
        l.text = m.text;
        l.rgba = m.rgba;
        l.shape = m.game_icon ? static_cast<uint8_t>(EMF_MARKER_NONE) : m.shape;
        l.icon = m.game_icon ? 0 : m.icon;
        out.push_back(std::move(l));
    }
    return out;
}

bool show_map_markers(bool console_open, const MarkerSight& sight, const MapView& v, uint64_t now_ms) {
    return !console_open && sight.map_open && v.rect[2] > 0 && v.rect[3] > 0 && v.ui_w > 0 && v.ui_h > 0 && now_ms >= v.ms &&
           now_ms - v.ms <= kMarkerFreshMs;
}

void draw_markers(ImDrawList* dl, const std::vector<MarkerLayout>& layouts, const TextLook& look, const ImageLookup* pictures) {
    if (!dl) return;
    const float display_h = ImGui::GetIO().DisplaySize.y;
    for (const MarkerLayout& l : layouts) {
        const float s = l.size, r = s * 0.5f, k = s / kMarkerSize1080;
        const ImVec2 c(l.x, l.y);
        const ImU32 fill = look_colour(with_alpha(l.rgba ? l.rgba : look.accent, l.alpha));
        const ImU32 edge = IM_COL32(0, 0, 0, static_cast<int>(170 * l.alpha));
        bool drew_picture = false;
        if (l.icon && pictures && *pictures) {
            if (const auto pic = (*pictures)(l.icon, ImGui::GetTime(), 0)) {
                const float w = pic->aspect >= 1 ? s : s * pic->aspect, h = pic->aspect >= 1 ? s / pic->aspect : s;
                dl->AddImage(pic->tex, ImVec2(c.x - w * 0.5f, c.y - h * 0.5f), ImVec2(c.x + w * 0.5f, c.y + h * 0.5f),
                             pic->uv0, pic->uv1, IM_COL32(255, 255, 255, static_cast<int>(255 * l.alpha)));
                drew_picture = true;
            }
        }
        if (!drew_picture) {
            switch (l.shape) {
                case EMF_MARKER_CIRCLE:
                    dl->AddCircleFilled(c, r * 0.8f, fill);
                    dl->AddCircle(c, r * 0.8f, edge, 0, 1.5f * k);
                    break;
                case EMF_MARKER_SQUARE:
                    dl->AddRectFilled(ImVec2(c.x - r * 0.7f, c.y - r * 0.7f), ImVec2(c.x + r * 0.7f, c.y + r * 0.7f), fill);
                    dl->AddRect(ImVec2(c.x - r * 0.7f, c.y - r * 0.7f), ImVec2(c.x + r * 0.7f, c.y + r * 0.7f), edge, 0, 0, 1.5f * k);
                    break;
                case EMF_MARKER_NONE:
                    break;
                default: {  // the diamond
                    const ImVec2 t(c.x, c.y - r), rt(c.x + r * 0.75f, c.y), b(c.x, c.y + r), lt(c.x - r * 0.75f, c.y);
                    dl->AddQuadFilled(t, rt, b, lt, fill);
                    dl->AddQuad(t, rt, b, lt, edge, 1.5f * k);
                }
            }
        }
        if (l.edge) {  // an arrow beyond the shape, pointing out
            const float ca = std::cos(l.angle), sa = std::sin(l.angle);
            const ImVec2 tip(c.x + ca * s * 1.05f, c.y + sa * s * 1.05f);
            const ImVec2 b1(c.x + ca * s * 0.65f - sa * s * 0.3f, c.y + sa * s * 0.65f + ca * s * 0.3f);
            const ImVec2 b2(c.x + ca * s * 0.65f + sa * s * 0.3f, c.y + sa * s * 0.65f - ca * s * 0.3f);
            dl->AddTriangleFilled(tip, b1, b2, fill);
            dl->AddTriangle(tip, b1, b2, edge, 1.0f * k);
        }
        if (!l.label || (l.text.empty() && l.dist.empty())) continue;
        ImFont* font = look.font ? look.font : ImGui::GetFont();
        const float big = 22.0f * k, small = 18.0f * k;
        // the text goes under the shape; near the bottom of the screen above it
        const bool above = l.y > display_h * 0.85f;
        const float bar_w = 56.0f * k, bar_h = 6.0f * k, bar_gap = 3.0f * k;
        const float bar_room = l.bar >= 0 ? bar_h + bar_gap : 0;
        float y = above ? l.y - r - (l.text.empty() ? 0 : big) - (l.dist.empty() ? 0 : small) - bar_room - 2 * k
                        : l.y + r + 2 * k;
        const uint32_t text_rgba = with_alpha(look.colour, l.alpha);
        if (!l.text.empty()) {
            const ImVec2 sz = font->CalcTextSizeA(big, FLT_MAX, 0, l.text.c_str());
            draw_look_text(dl, look, font, big, ImVec2(l.x - sz.x * 0.5f, y), text_rgba, l.text.c_str(), nullptr, 0);
            y += big;
        }
        if (!l.dist.empty()) {
            const ImVec2 sz = font->CalcTextSizeA(small, FLT_MAX, 0, l.dist.c_str());
            draw_look_text(dl, look, font, small, ImVec2(l.x - sz.x * 0.5f, y), with_alpha(text_rgba, 0.85f), l.dist.c_str(),
                           nullptr, 0);
            y += small;
        }
        if (l.bar >= 0) {  // the bar under the text (a health bar): its share filled in the marker's colour
            const ImVec2 a(l.x - bar_w * 0.5f, y + bar_gap), b(l.x + bar_w * 0.5f, y + bar_gap + bar_h);
            dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, static_cast<int>(150 * l.alpha)));
            if (l.bar > 0) dl->AddRectFilled(a, ImVec2(a.x + bar_w * l.bar, b.y), fill);
            dl->AddRect(a, b, edge, 0, 0, 1.0f * k);
        }
    }
}

}  // namespace emf::core
