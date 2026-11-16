#include "hud_page.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/hud_editor/elements.h"
#include "core/hud_editor/keys.h"
#include "core/hud_editor/layout.h"
#include "hud_pick.h"

namespace emf::core {

using namespace hud;

namespace {

constexpr uint64_t kConfirmMs = 3000;
const ImVec4 kErrorColour(1.0f, 0.48f, 0.45f, 1.0f);

uint64_t now_ms() { return static_cast<uint64_t>(ImGui::GetTime() * 1000.0); }

bool waiting(uint64_t since, uint64_t now) { return since != 0 && now - since < kConfirmMs; }

void say(HudPageState& st, const LayoutResult& r) {
    st.message = r.message;
    st.message_error = !r.ok;
}

// The hide rule shared by the checkbox and the H key: a part that asks first needs two requests within 3 s.
void request_hide(HudEditor& editor, HudPageState& st, const HudView& v, size_t i, bool hide) {
    const HudElement& part = hud_elements()[i];
    const uint64_t now = now_ms();
    if (hide && part.confirm_hide && !waiting(st.confirm_hide_ms, now)) {
        st.confirm_hide_ms = now ? now : 1;
        return;
    }
    st.confirm_hide_ms = 0;
    editor.set_hidden(i, hide, Save::Later);
    (void)v;
}

void apply_keys(HudEditor& editor, HudPageState& st, const HudView& v) {
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsAnyItemActive() || io.WantTextInput || io.KeyCtrl) return;
    struct Map {
        ImGuiKey key;
        PageKey page;
        bool repeat;
    };
    static const Map kMap[] = {
        {ImGuiKey_Tab, PageKey::Tab, false},           {ImGuiKey_LeftArrow, PageKey::Left, true},
        {ImGuiKey_RightArrow, PageKey::Right, true},   {ImGuiKey_UpArrow, PageKey::Up, true},
        {ImGuiKey_DownArrow, PageKey::Down, true},     {ImGuiKey_Equal, PageKey::Plus, true},
        {ImGuiKey_KeypadAdd, PageKey::Plus, true},     {ImGuiKey_Minus, PageKey::Minus, true},
        {ImGuiKey_KeypadSubtract, PageKey::Minus, true}, {ImGuiKey_H, PageKey::H, false},
        {ImGuiKey_R, PageKey::R, false},
    };
    for (const Map& m : kMap) {
        if (!ImGui::IsKeyPressed(m.key, m.repeat)) continue;
        const PageCommand c = page_command(m.page, io.KeyShift);
        const size_t i = st.selected;
        const ElementLayout& cur = v.elements[i];
        // a bar's move and hide do nothing while the bars do not move on their own (its card has them off too)
        if ((c.action == PageAction::Move || c.action == PageAction::ToggleHide) && !part_moves(v, i)) break;
        switch (c.action) {
            case PageAction::Next: st.selected = step_element(st.selected, false); break;
            case PageAction::Previous: st.selected = step_element(st.selected, true); break;
            case PageAction::Move: editor.nudge(i, c.dx, c.dy, Save::Later); break;
            case PageAction::ScaleUp: editor.set_scale(i, cur.scale + kScaleStep, Save::Later); break;
            case PageAction::ScaleDown: editor.set_scale(i, cur.scale - kScaleStep, Save::Later); break;
            case PageAction::ToggleHide: request_hide(editor, st, v, i, !cur.hidden); break;
            case PageAction::Reset: editor.reset(i, Save::Later); break;
            case PageAction::None: break;
        }
        break;  // one key per frame
    }
}

// A click on the game's HUD (no ImGui window under the pointer) selects the part under it; dragging moves it.
void apply_mouse(HudEditor& editor, HudPageState& st, const HudView& v, ImVec2 display) {
    ImGuiIO& io = ImGui::GetIO();
    if (st.dragging >= 0) {
        if (!io.MouseDown[0]) {
            st.dragging = -1;
            return;
        }
        const ElementLayout moved =
            dragged(ElementLayout{st.drag_start_x, st.drag_start_y, 1, false},
                    ImVec2(io.MousePos.x - st.drag_start_mouse.x, io.MousePos.y - st.drag_start_mouse.y), display);
        editor.set_offset(static_cast<size_t>(st.dragging), moved.x, moved.y, Save::Later);
        return;
    }
    if (!io.MouseClicked[0] || ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) return;
    const UiSpace ui = hud_ui_space();
    std::array<std::optional<ScreenRect>, kHudElementCount> rects{};
    for (size_t i = 0; i < kHudElementCount; ++i)
        if (v.live[i].seen && !v.elements[i].hidden) rects[i] = to_screen(v.live[i].rect, ui, display);
    const int hit = pick_part(rects, io.MousePos);
    if (hit < 0) return;
    st.selected = static_cast<size_t>(hit);
    if (!part_moves(v, st.selected)) return;  // a bar that cannot move on its own: picked, not dragged
    st.dragging = hit;
    st.drag_start_mouse = io.MousePos;
    st.drag_start_x = v.elements[static_cast<size_t>(hit)].x;
    st.drag_start_y = v.elements[static_cast<size_t>(hit)].y;
}

void draw_layout_bar(HudEditor& editor, HudPageState& st, const HudView& v) {
    const uint64_t now = now_ms();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    if (ImGui::BeginCombo("Layout", v.layout_name.c_str())) {
        for (const std::string& name : v.layouts) {
            if (ImGui::Selectable(name.c_str(), name == v.layout_name)) say(st, editor.load(name));
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s", st.save_name.c_str());
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
    if (ImGui::InputTextWithHint("##hud_save_name", "new name", buf, sizeof buf)) st.save_name = buf;
    ImGui::SameLine();
    if (ImGui::Button("Save as")) say(st, editor.save_as(st.save_name));
    ImGui::SameLine();
    const bool confirming = waiting(st.confirm_delete_ms, now);
    if (ImGui::Button(confirming ? "Click again to delete##hud_delete" : "Delete##hud_delete")) {
        if (confirming) {
            st.confirm_delete_ms = 0;
            say(st, editor.remove(v.layout_name));
        } else {
            st.confirm_delete_ms = now ? now : 1;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset all")) say(st, editor.reset_all());
    if (!st.message.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        if (st.message_error)
            ImGui::TextColored(kErrorColour, "%s", st.message.c_str());
        else
            ImGui::TextDisabled("%s", st.message.c_str());
        ImGui::PopTextWrapPos();
    }
}

void draw_part_list(HudPageState& st, const HudView& v) {
    const auto parts = hud_elements();
    for (size_t i = 0; i < kHudElementCount; ++i) {
        std::string label = parts[i].label;
        if (v.elements[i].hidden)
            label += " (hidden)";
        else if (!v.elements[i].is_default())
            label += " (moved)";
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable(label.c_str(), st.selected == i)) st.selected = i;
        if (!v.live[i].seen) {
            ImGui::SameLine();
            ImGui::TextDisabled("(not seen yet)");
        }
        ImGui::PopID();
    }
}

std::string order_line(const BarOrder& o) {
    std::string s = "Order: ";
    for (size_t k = 0; k < kBarCount; ++k) s += (k ? ", " : "") + std::string(bar_id(o[k]));
    return s;
}

// The vitals' card and the bars' cards: the two swap buttons and the order.
void draw_order(HudEditor& editor, const HudView& v) {
    ImGui::BeginDisabled(!v.bars_work);
    if (ImGui::Button("Swap health and mana")) editor.swap_bars(kBarHealth, kBarMana, Save::Later);
    ImGui::SameLine();
    if (ImGui::Button("Swap health and stamina")) editor.swap_bars(kBarHealth, kBarStamina, Save::Later);
    ImGui::EndDisabled();
    ImGui::TextDisabled("%s", order_line(v.order).c_str());
}

void to_floats(uint32_t rgba, float out[4]) {
    for (int k = 0; k < 4; ++k) out[k] = static_cast<float>(rgba >> (24 - 8 * k) & 0xFFu) / 255.0f;
}

bool colour_button(const char* label, uint32_t rgba, float out[4]) {
    to_floats(rgba, out);
    return ImGui::ColorEdit4(label, out,
                             ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf | ImGuiColorEditFlags_NoInputs);
}

// A bar's card: the filled and the empty part's colours, and the game's colours back.
void draw_bar_colours(HudEditor& editor, const HudView& v, int bar) {
    ImGui::Spacing();
    ImGui::TextUnformatted("Colours");
    ImGui::BeginDisabled(!v.colours_work);
    float c[4];
    if (colour_button("Filled", page_colour(v, bar, BarPart::Fill), c))
        editor.set_colour(bar, BarPart::Fill, picked(c), Save::Later);
    ImGui::SameLine();
    if (colour_button("Empty", page_colour(v, bar, BarPart::Empty), c))
        editor.set_colour(bar, BarPart::Empty, picked(c), Save::Later);
    ImGui::SameLine();
    if (ImGui::Button("Game's colours")) editor.clear_colours(bar, Save::Later);
    ImGui::EndDisabled();
    ImGui::PushTextWrapPos(0.0f);
    const std::string more = console_only_colours(v.colours[static_cast<size_t>(bar)]);
    if (!more.empty()) ImGui::TextDisabled("Also set in the console: %s", more.c_str());
    if (!v.colours_work) ImGui::TextColored(kErrorColour, "%s", v.colours_note.c_str());
    ImGui::PopTextWrapPos();
}

// The vitals' card: the colour and opacity of the dark panel behind the bars.
void draw_panel_colour(HudEditor& editor, const HudView& v) {
    ImGui::Spacing();
    ImGui::BeginDisabled(!panel_switch_enabled(v));
    bool on = v.panel.set;
    float c[4];
    if (ImGui::Checkbox("Panel colour", &on)) {
        if (on) {
            to_floats(page_panel_colour(v), c);
            editor.set_panel(picked(c), Save::Later);
        } else {
            editor.set_panel(std::nullopt, Save::Later);
        }
    }
    ImGui::EndDisabled();
    if (v.panel.set) {
        ImGui::SameLine();
        ImGui::BeginDisabled(!v.panel_works);
        if (colour_button("##panel_colour", page_panel_colour(v), c)) editor.set_panel(picked(c), Save::Later);
        ImGui::EndDisabled();
    }
    ImGui::PushTextWrapPos(0.0f);
    if (!v.panel_works)
        ImGui::TextColored(kErrorColour, "%s", v.panel_note.c_str());
    else
        ImGui::TextDisabled("The dark panel behind the bars, with its opacity.");
    ImGui::PopTextWrapPos();
}

void draw_part_panel(HudEditor& editor, HudPageState& st, const HudView& v, ImVec2 display) {
    const size_t i = st.selected;
    const HudElement& part = hud_elements()[i];
    const bool bar = part.bar >= 0;
    ElementLayout el = v.elements[i];
    ImGui::TextUnformatted(part.label);
    ImGui::TextDisabled("%s", part.scope);
    if (part.note) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", part.note);
        ImGui::PopTextWrapPos();
    }
    ImGui::Spacing();
    if (bar && !v.bars_work) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(kErrorColour, "%s", v.bars_note.c_str());
        ImGui::PopTextWrapPos();
    }
    const bool moves = part_moves(v, i);  // a bar's offset and Hidden need the bars to move on their own
    float x = el.x, y = el.y;
    ImGui::BeginDisabled(!moves);
    bool moved = ImGui::SliderFloat("Offset X", &x, kOffsetMin, kOffsetMax, "%.1f %%");
    ImGui::SameLine();
    ImGui::TextDisabled("%d px", percent_to_pixels(x, display.x));
    moved |= ImGui::SliderFloat("Offset Y", &y, kOffsetMin, kOffsetMax, "%.1f %%");
    ImGui::SameLine();
    ImGui::TextDisabled("%d px", percent_to_pixels(y, display.y));
    ImGui::EndDisabled();
    if (moved && moves) editor.set_offset(i, x, y, Save::Later);

    const bool lines = part.lines >= 0;  // EMF draws them: their scale needs no transform stack
    if (bar) {
        ImGui::TextDisabled("No scale of its own: scale Health, mana and stamina.");
    } else {
        float s = el.scale;
        const bool scale_works = v.scale_works || lines;
        ImGui::BeginDisabled(!scale_works);
        if (ImGui::SliderFloat("Scale", &s, kScaleMin, kScaleMax, "%.2f")) editor.set_scale(i, s, Save::Later);
        ImGui::EndDisabled();
        if (!scale_works) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", v.scale_note.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::TextDisabled(lines ? "Scales the text." : "Scales around the screen edge the part is nearest.");
    }
    if (i == 0 || bar) draw_order(editor, v);
    if (bar) draw_bar_colours(editor, v, part.bar);
    if (i == 0) draw_panel_colour(editor, v);
    if (i == 1) {
        bool up = v.buffs_up;
        ImGui::BeginDisabled(!v.grow_works);
        if (ImGui::Checkbox("Status effects grow upward", &up)) editor.set_buffs_up(up, Save::Later);
        ImGui::EndDisabled();
        ImGui::PushTextWrapPos(0.0f);
        if (!v.grow_works) ImGui::TextColored(kErrorColour, "%s", v.grow_note.c_str());
        ImGui::TextDisabled(
            "The list sits on the food icons' bottom edge and grows up; it stops at the screen's edge. Move this part where "
            "the list has room.");
        ImGui::PopTextWrapPos();
    }

    bool hidden = el.hidden;
    ImGui::BeginDisabled(!moves);
    if (ImGui::Checkbox("Hidden", &hidden) && moves) request_hide(editor, st, v, i, hidden);
    ImGui::EndDisabled();
    if (waiting(st.confirm_hide_ms, now_ms()) && !el.hidden && part.confirm_hide)
        ImGui::TextColored(kErrorColour, "Hide %s? Click again within 3 s.", part.label);
    ImGui::SameLine();
    if (ImGui::Button("Reset")) editor.reset(i, Save::Later);
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", page_keys_line(v, i).c_str());
    ImGui::PopTextWrapPos();
}

}  // namespace

void draw_hud_page(HudEditor& editor, HudPageState& st, ImVec2 display) {
    const HudView v = editor.view();
    if (st.selected >= kHudElementCount) st.selected = 0;
    if (!v.gate.message.empty()) {
        const bool bad = v.gate.state == GateState::Faulted || v.gate.state == GateState::HookFailed ||
                         v.gate.state == GateState::OtherHudMod || v.gate.state == GateState::UnknownBuild;
        ImGui::PushTextWrapPos(0.0f);
        if (bad)
            ImGui::TextColored(kErrorColour, "%s", v.gate.message.c_str());
        else
            ImGui::TextDisabled("%s", v.gate.message.c_str());
        ImGui::PopTextWrapPos();
    }
    if (!v.gate.show_editor) return;
    // Tab steps through the HUD parts here, not through ImGui's widgets (a tab stop on the name box would take the key).
    ImGui::PushItemFlag(ImGuiItemFlags_NoTabStop, true);
    draw_layout_bar(editor, st, v);
    ImGui::Spacing();
    const float list_w = ImGui::GetFontSize() * 16.0f;
    const float avail = ImGui::GetContentRegionAvail().x;
    ImGui::BeginChild("##hud_parts", ImVec2((std::min)(list_w, avail * 0.45f), ImGui::GetFontSize() * 18.0f),
                      ImGuiChildFlags_Borders);
    draw_part_list(st, v);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##hud_part", ImVec2(0, ImGui::GetFontSize() * 18.0f), ImGuiChildFlags_Borders);
    draw_part_panel(editor, st, v, display);
    ImGui::EndChild();
    ImGui::PopItemFlag();
    apply_keys(editor, st, v);
    apply_mouse(editor, st, v, display);
    editor.page_frame(st.selected);
}

uint32_t page_colour(const HudView& v, int bar, BarPart part) {
    static constexpr size_t kField[kBarPartCount] = {kFill, kRange, kFillEnd, kDamage, kHeal};
    const size_t p = static_cast<size_t>(part);
    if (bar < 0 || bar >= static_cast<int>(kBarCount) || p >= kBarPartCount) return 0xFFFFFFFFu;
    const std::optional<StyleBlock>& g = v.game_colours[static_cast<size_t>(bar)];
    const Colour& c = v.colours[static_cast<size_t>(bar)].part[p];
    if (c.set) {
        if (c.has_alpha) return c.rgba;
        const uint32_t alpha = g ? game_to_rgba((*g)[kField[p]].value) & 0xFFu : 0xFFu;
        return (c.rgba & 0xFFFFFF00u) | alpha;
    }
    return g ? game_to_rgba((*g)[kField[p]].value) : 0xFFFFFFFFu;
}

uint32_t page_panel_colour(const HudView& v) {
    const std::optional<StyleBlock>& g = v.game_colours[static_cast<size_t>(kBarHealth)];
    if (v.panel.set) {
        if (v.panel.has_alpha) return v.panel.rgba;
        const uint32_t alpha = g ? game_to_rgba((*g)[kFrame].value) & 0xFFu : 0xFFu;
        return (v.panel.rgba & 0xFFFFFF00u) | alpha;
    }
    return g ? game_to_rgba((*g)[kFrame].value) : 0x000000B0u;
}

bool panel_switch_enabled(const HudView& v) { return v.panel_works || v.panel.set; }

bool part_moves(const HudView& v, size_t part) {
    return part >= kHudElementCount || hud_elements()[part].bar < 0 || v.bars_work;
}

std::string page_keys_line(const HudView& v, size_t part) {
    const bool bar = part < kHudElementCount && hud_elements()[part].bar >= 0;
    std::string s = "Keys: Tab / Shift+Tab next part";
    if (part_moves(v, part)) s += ", arrows move 0.5 % (Shift: 5 %)";
    if (!bar) s += ", + / - scale";
    if (part_moves(v, part)) s += ", H hide";
    return s + ", R reset";
}

Colour picked(const float rgba[4]) {
    uint32_t v = 0;
    for (int k = 0; k < 4; ++k) {
        float f = rgba[k];
        if (!(f > 0.0f)) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        v = v << 8 | static_cast<uint32_t>(std::lround(f * 255.0f));
    }
    Colour c;
    c.rgba = v;
    c.set = true;
    c.has_alpha = true;
    return c;
}

std::string console_only_colours(const BarColours& c) {
    std::string s;
    for (BarPart p : {BarPart::End, BarPart::Damage, BarPart::Heal}) {
        const Colour& col = c.part[static_cast<size_t>(p)];
        if (col.set) s += (s.empty() ? "" : ", ") + std::string(bar_part_id(p)) + " " + colour_text(col);
    }
    return s;
}

PageToggle page_toggle(bool console_open, bool showing_page, bool busy) {
    if (busy) return PageToggle::Refuse;
    if (!console_open) return PageToggle::Open;
    return showing_page ? PageToggle::Close : PageToggle::Show;
}

}  // namespace emf::core
