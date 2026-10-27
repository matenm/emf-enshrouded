#include "wheel_ui.h"

#include <algorithm>
#include <cmath>

namespace emf::core {
namespace {
constexpr float kPi = 3.14159265358979323846f;

// The text cut to fit `width` pixels (with "..." when it was cut).
std::string fit(const std::string& text, float width) {
    if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
    std::string s = text;
    while (!s.empty() && ImGui::CalcTextSize((s + "...").c_str()).x > width) s.pop_back();
    return s + "...";
}

void centered_text(ImDrawList* dl, ImVec2 at, ImU32 color, const std::string& text) {
    const ImVec2 size = ImGui::CalcTextSize(text.c_str());
    dl->AddText(ImVec2(at.x - size.x * 0.5f, at.y - size.y * 0.5f), color, text.c_str());
}
}  // namespace

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

void draw_wheel(const WheelModel& wheel, ImVec2 display) {
    if (!wheel.is_open()) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 c(display.x * 0.5f, display.y * 0.5f);
    const float outer = (std::min)(display.x, display.y) * 0.30f;
    const float inner = outer * 0.34f;
    const float line_h = ImGui::GetTextLineHeight();
    const size_t n = wheel.page_count();
    const size_t first = wheel.page_first();
    const int lit = wheel.highlighted();

    dl->AddCircleFilled(c, outer + 6.0f, IM_COL32(0, 0, 0, 90), 96);  // soft backdrop
    for (size_t i = 0; i < n; ++i) {
        const float mid = -kPi / 2.0f + static_cast<float>(i) * 2.0f * kPi / static_cast<float>(n);
        const float half = kPi / static_cast<float>(n);
        const float gap = n > 1 ? 0.012f : 0.0f;
        const bool on = static_cast<int>(first + i) == lit;
        dl->PathClear();
        dl->PathArcTo(c, outer, mid - half + gap, mid + half - gap, 32);
        dl->PathArcTo(c, inner, mid + half - gap, mid - half + gap, 16);
        dl->PathFillConcave(on ? IM_COL32(62, 80, 108, 235) : IM_COL32(18, 20, 26, 205));
        dl->PathClear();
        dl->PathArcTo(c, outer, mid - half + gap, mid + half - gap, 32);
        dl->PathStroke(on ? IM_COL32(118, 140, 172, 255) : IM_COL32(90, 95, 110, 200), 0, 2.0f);

        const float r = (outer + inner) * 0.5f;
        const ImVec2 at(c.x + std::cos(mid) * r, c.y + std::sin(mid) * r);
        // One or two slices are half discs or more: their labels may use most of the chord.
        const float width = n > 2 ? (std::min)((outer - inner) * 1.7f, 2.0f * r * std::sin(half) * 0.92f) : outer * 1.35f;
        const auto [name, detail] = wheel_label_lines(wheel.items()[first + i]);
        const ImU32 text = on ? IM_COL32(255, 255, 255, 255) : IM_COL32(220, 222, 230, 255);
        if (detail.empty()) {
            centered_text(dl, at, text, fit(name, width));
        } else {
            centered_text(dl, ImVec2(at.x, at.y - line_h * 0.55f), text, fit(name, width));
            centered_text(dl, ImVec2(at.x, at.y + line_h * 0.55f), IM_COL32(170, 175, 190, 255), fit(detail, width));
        }
    }
    // Centre: title, page, and a dot showing where the mouse points.
    dl->AddCircleFilled(c, inner * 0.92f, IM_COL32(10, 11, 14, 225), 64);
    const float title_w = inner * 1.6f;
    const auto [t1, t2] = wheel_title_lines(wheel.title(), title_w,
                                            [](const std::string& s) { return ImGui::CalcTextSize(s.c_str()).x; });
    const ImU32 title_color = IM_COL32(235, 210, 150, 255);
    const float title_y = c.y - line_h * (t2.empty() ? 0.6f : 1.1f);
    if (!t1.empty()) centered_text(dl, ImVec2(c.x, title_y), title_color, fit(t1, title_w));
    if (!t2.empty()) centered_text(dl, ImVec2(c.x, title_y + line_h), title_color, fit(t2, title_w));
    if (wheel.pages() > 1) {
        char page[32];
        snprintf(page, sizeof(page), "page %zu/%zu", wheel.page() + 1, wheel.pages());
        centered_text(dl, ImVec2(c.x, c.y + line_h * (t2.empty() ? 0.6f : 1.1f)), IM_COL32(170, 175, 190, 255), page);
    }
    const auto [dx, dy] = wheel.direction();
    const float k = inner * 0.8f / WheelModel::kMaxRadius;
    dl->AddCircleFilled(ImVec2(c.x + dx * k, c.y + dy * k), 5.0f, IM_COL32(255, 255, 255, lit >= 0 ? 230 : 120), 16);
    // Hint under the ring.
    std::string hint = wheel.hold_mode() ? "release the key to choose" : "click to choose";
    hint += ", Esc or right click to cancel";
    if (wheel.pages() > 1) hint += ", mouse wheel: more";
    centered_text(dl, ImVec2(c.x, c.y + outer + line_h * 1.4f), IM_COL32(200, 200, 210, 230), hint);
}

}  // namespace emf::core
