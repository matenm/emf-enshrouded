#include "wheel_ui.h"

#include <algorithm>
#include <cmath>

namespace emf::core {
namespace {
constexpr float kPi = 3.14159265358979323846f;

// The text cut to fit `width` pixels in the current font (with "..." when it was cut).
std::string fit(const std::string& text, float width) {
    return wheel_fit(text, width, [](const std::string& s) { return ImGui::CalcTextSize(s.c_str()).x; });
}

// `text` centred on `at` in the current font, with the look's shadow; rgba is 0xRRGGBBAA.
void centered_text(ImDrawList* dl, const TextLook& look, ImVec2 at, uint32_t rgba, const std::string& text) {
    const ImVec2 size = ImGui::CalcTextSize(text.c_str());
    draw_look_text(dl, look, ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(at.x - size.x * 0.5f, at.y - size.y * 0.5f),
                   rgba, text.c_str());
}

// a and b mixed (t = 0: a), 0xRRGGBBAA with alpha `alpha`.
uint32_t blend(uint32_t a, uint32_t b, float t, uint32_t alpha) {
    uint32_t out = 0;
    for (int shift = 24; shift >= 8; shift -= 8) {
        const float x = static_cast<float>((a >> shift) & 0xFF), y = static_cast<float>((b >> shift) & 0xFF);
        out |= static_cast<uint32_t>(x + (y - x) * t + 0.5f) << shift;
    }
    return out | (alpha & 0xFF);
}
}  // namespace

std::string wheel_fit(const std::string& text, float width, const std::function<float(const std::string&)>& measure) {
    if (measure(text) <= width) return text;
    std::string s = text;
    while (!s.empty() && measure(s + "...") > width) utf8_pop_back(s);  // whole characters: never half of one
    return s + "...";
}

std::pair<std::string, std::string> wheel_label_lines(const std::string& label) {
    const size_t cut = label.find(" (");
    if (cut == std::string::npos) return {label, ""};
    return {label.substr(0, cut), label.substr(cut + 1)};
}

std::pair<std::string, std::string> wheel_title_lines(const std::string& title, float width,
                                                      const std::function<float(const std::string&)>& measure) {
    if (measure(title) <= width) return {title, ""};
    for (size_t cut = title.rfind(' '); cut != std::string::npos && cut > 0; cut = title.rfind(' ', cut - 1)) {
        const std::string first = title.substr(0, cut);
        if (measure(first) <= width) return {first, title.substr(cut + 1)};
    }
    return {title, ""};
}

std::vector<WheelLabelLine> wheel_label_layout(const std::string& label, float width,
                                               const std::function<float(const std::string&)>& measure) {
    const auto [name, detail] = wheel_label_lines(label);
    std::vector<WheelLabelLine> lines;
    std::string rest = name;
    if (measure(name) > width) {
        const size_t colon = name.find(": ");  // a shared wheel's "Section: label"
        if (colon != std::string::npos && colon > 0 && colon + 2 < name.size()) {
            lines.push_back({name.substr(0, colon + 1), true});
            rest = name.substr(colon + 2);
        }
        const auto [first, second] = wheel_title_lines(rest, width, measure);
        lines.push_back({first, false});
        if (!second.empty()) lines.push_back({second, false});
    } else {
        lines.push_back({rest, false});
    }
    if (!detail.empty()) lines.push_back({detail, true});
    return lines;
}

float wheel_label_width(ImVec2 display, size_t slices) {
    const float outer = (std::min)(display.x, display.y) * 0.30f;
    const float inner = outer * 0.34f;
    const float r = (outer + inner) * 0.5f;
    const float half = kPi / static_cast<float>(slices ? slices : 1);
    // One or two slices are half discs or more: their labels may use most of the chord.
    return slices > 2 ? (std::min)((outer - inner) * 1.7f, 2.0f * r * std::sin(half) * 0.92f) : outer * 1.35f;
}

void draw_wheel(const WheelModel& wheel, ImVec2 display, const TextLook& look) {
    if (!wheel.is_open()) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (look.font) ImGui::PushFont(look.font);  // the [hud] font (measuring and drawing below use it)
    const ImVec2 c(display.x * 0.5f, display.y * 0.5f);
    const float outer = (std::min)(display.x, display.y) * 0.30f;
    const float inner = outer * 0.34f;
    const float line_h = ImGui::GetTextLineHeight();
    const size_t n = wheel.page_count();
    const size_t first = wheel.page_first();
    const int lit = wheel.highlighted();
    // navy slices; the lit one warmed by the accent with a gold ring, the others with a faint gold edge
    const uint32_t slice = 0x12141ACDu, accent = look.accent | 0xFFu;
    const ImU32 slice_lit = look_colour(blend(slice, accent, 0.28f, 235));
    const ImU32 ring_lit = look_colour(accent), ring = look_colour(blend(0x5A5A5AFFu, accent, 0.45f, 170));
    const uint32_t text_on = 0xFFFFFFFFu, text_off = look.colour | 0xFFu, text_dim = blend(look.colour, 0x000000FFu, 0.32f, 255);
    const uint32_t title_colour = blend(accent, 0xFFFFFFFFu, 0.35f, 255);

    dl->AddCircleFilled(c, outer + 6.0f, IM_COL32(0, 0, 0, 90), 96);  // soft backdrop
    for (size_t i = 0; i < n; ++i) {
        const float mid = -kPi / 2.0f + static_cast<float>(i) * 2.0f * kPi / static_cast<float>(n);
        const float half = kPi / static_cast<float>(n);
        const float gap = n > 1 ? 0.012f : 0.0f;
        const bool on = static_cast<int>(first + i) == lit;
        dl->PathClear();
        dl->PathArcTo(c, outer, mid - half + gap, mid + half - gap, 32);
        dl->PathArcTo(c, inner, mid + half - gap, mid - half + gap, 16);
        dl->PathFillConcave(on ? slice_lit : look_colour(slice));
        dl->PathClear();
        dl->PathArcTo(c, outer, mid - half + gap, mid + half - gap, 32);
        dl->PathStroke(on ? ring_lit : ring, 0, on ? 3.0f : 2.0f);

        const float r = (outer + inner) * 0.5f;
        const ImVec2 at(c.x + std::cos(mid) * r, c.y + std::sin(mid) * r);
        const float width = wheel_label_width(display, n);
        // The label in up to 4 lines (a name too wide wraps; a shared wheel's section and the detail are dimmed),
        // centred on the slice, each line cut with "..." if it is still too wide.
        const auto lines = wheel_label_layout(wheel.items()[first + i], width,
                                              [](const std::string& s) { return ImGui::CalcTextSize(s.c_str()).x; });
        const float step = line_h * 1.1f;
        float y = at.y - step * 0.5f * static_cast<float>(lines.size() - 1);
        for (const auto& l : lines) {
            centered_text(dl, look, ImVec2(at.x, y), l.dim ? text_dim : on ? text_on : text_off, fit(l.text, width));
            y += step;
        }
        // its number (1-8 choose it), small, near the outer edge
        const char digit[2] = {static_cast<char>('1' + i), '\0'};
        const float rn = outer - line_h * 0.7f;
        centered_text(dl, look, ImVec2(c.x + std::cos(mid) * rn, c.y + std::sin(mid) * rn - line_h * 0.5f), text_dim, digit);
    }
    // Centre: title (bold), page, and a dot showing where the mouse points.
    dl->AddCircleFilled(c, inner * 0.92f, IM_COL32(10, 11, 14, 225), 64);
    const float title_w = inner * 1.6f;
    if (look.bold) ImGui::PushFont(look.bold);
    const auto [t1, t2] = wheel_title_lines(wheel.title(), title_w,
                                            [](const std::string& s) { return ImGui::CalcTextSize(s.c_str()).x; });
    const float title_y = c.y - line_h * (t2.empty() ? 0.6f : 1.1f);
    if (!t1.empty()) centered_text(dl, look, ImVec2(c.x, title_y), title_colour, fit(t1, title_w));
    if (!t2.empty()) centered_text(dl, look, ImVec2(c.x, title_y + line_h), title_colour, fit(t2, title_w));
    if (look.bold) ImGui::PopFont();
    if (wheel.pages() > 1) {
        char page[32];
        snprintf(page, sizeof(page), "page %zu/%zu", wheel.page() + 1, wheel.pages());
        centered_text(dl, look, ImVec2(c.x, c.y + line_h * (t2.empty() ? 0.6f : 1.1f)), text_dim, page);
    }
    const auto [dx, dy] = wheel.direction();
    const float k = inner * 0.8f / WheelModel::kMaxRadius;
    dl->AddCircleFilled(ImVec2(c.x + dx * k, c.y + dy * k), 5.0f, IM_COL32(255, 255, 255, lit >= 0 ? 230 : 120), 16);
    // Hint under the ring.
    std::string hint = wheel.hold_mode() ? "release the key to choose" : "click to choose";
    hint += ", 1-8: choose, Esc or right click to cancel";
    if (wheel.pages() > 1) hint += ", mouse wheel or Left/Right: more";
    centered_text(dl, look, ImVec2(c.x, c.y + outer + line_h * 1.4f), (look.colour & 0xFFFFFF00u) | 0xE6u, hint);
    if (look.font) ImGui::PopFont();
}

}  // namespace emf::core
