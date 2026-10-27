#include "mods_ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "emf/keys.hpp"
#include "profiles_ui.h"

namespace emf::core {

namespace {

std::string number(double v) { return setting_number_text(v); }  // 1234567 stays 1234567

std::string range(const SettingDef& d) {
    if (d.has_min && d.has_max) return number(d.min) + " to " + number(d.max);
    if (d.has_min) return "at least " + number(d.min);
    if (d.has_max) return "at most " + number(d.max);
    return "";
}

void tooltip(const SettingDef& d, const std::string& shown_default) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) return;
    std::string t = describe_setting(d) + "\ndefault: " + shown_default + "\n" +
                    (d.live ? "applies at once" : "applies at the next game start");
    ImGui::SetTooltip("%s", t.c_str());
}

// Decimals a slider shows: as many as its step has (0.25 -> 2), 2 without a step, none for whole numbers.
int decimals(const SettingDef& d) {
    if (d.type == SettingType::Int) return 0;
    if (!(d.step > 0)) return 2;
    for (int n = 0; n < 6; ++n) {
        const double scaled = d.step * std::pow(10.0, n);
        if (std::fabs(scaled - std::round(scaled)) < 1e-9) return n;
    }
    return 6;
}

void draw_number(SettingsStore& s, const std::string& mod, const SettingDef& d, ModsUiState& ui, int frame,
                 const std::string& full, const std::string& shown) {
    bool changed = false;
    std::string next;
    switch (number_widget(d)) {
        case NumberWidget::SliderInt: {
            ImS64 v = std::strtoll(shown.c_str(), nullptr, 10);
            const ImS64 lo = std::llround(d.min), hi = std::llround(d.max);
            changed = ImGui::SliderScalar(d.label.c_str(), ImGuiDataType_S64, &v, &lo, &hi, "%lld");
            next = std::to_string(v);
            break;
        }
        case NumberWidget::SliderDouble: {
            double v = std::atof(shown.c_str());
            char fmt[16];
            std::snprintf(fmt, sizeof fmt, "%%.%df", decimals(d));
            changed = ImGui::SliderScalar(d.label.c_str(), ImGuiDataType_Double, &v, &d.min, &d.max, fmt);
            next = number(v);
            break;
        }
        case NumberWidget::InputInt: {
            ImS64 v = std::strtoll(shown.c_str(), nullptr, 10);
            const ImS64 step = d.step > 0 ? std::llround(d.step) : 0, fast = step * 10;
            changed = ImGui::InputScalar(d.label.c_str(), ImGuiDataType_S64, &v, step ? &step : nullptr,
                                         step ? &fast : nullptr, "%lld");
            next = std::to_string(v);
            break;
        }
        case NumberWidget::InputDouble: {
            double v = std::atof(shown.c_str());
            const double step = d.step > 0 ? d.step : 0.0;
            changed = ImGui::InputScalar(d.label.c_str(), ImGuiDataType_Double, &v, step > 0 ? &step : nullptr, nullptr, "%.3f");
            next = number(v);
            break;
        }
    }
    if (changed) {  // moved to the nearest step at once, so the slider moves in steps
        auto snapped = setting_value(d, next, true, nullptr);
        set_pending_edit(ui, full, snapped ? *snapped : next, frame);
    }
    // saved on release (Enter or a click elsewhere): one change per edit, not one per frame
    if (!ImGui::IsItemDeactivatedAfterEdit()) return;
    auto p = ui.pending.find(full);
    if (p == ui.pending.end()) return;
    const std::string v = p->second.value;
    ui.pending.erase(p);
    apply_setting_edit(s, mod, d.key, v, ui);
}

void draw_setting(SettingsStore& s, const std::string& mod, const SettingDef& d, const std::vector<SettingDef>& defs,
                  ModsUiState& ui, int frame) {
    const std::string value = s.get(mod, d.key);
    const std::string full = mod + "." + d.key;
    const std::string* pending = pending_edit(ui, full, frame);
    const std::string shown = pending ? *pending : value;
    const bool active = setting_active(d, [&](const std::string& k) { return s.get(mod, k); });
    ImGui::PushID(full.c_str());
    if (!active) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    switch (d.type) {
        case SettingType::Bool: {
            bool v = value == "1";
            if (ImGui::Checkbox(d.label.c_str(), &v)) apply_setting_edit(s, mod, d.key, v ? "1" : "0", ui);
            break;
        }
        case SettingType::Int:
        case SettingType::Float:
            draw_number(s, mod, d, ui, frame, full, shown);
            break;
        case SettingType::Choice: {
            const size_t index = static_cast<size_t>(std::strtoul(value.c_str(), nullptr, 10));
            const char* preview = index < d.choices.size() ? d.choices[index].c_str() : "?";
            if (ImGui::BeginCombo(d.label.c_str(), preview)) {
                for (size_t i = 0; i < d.choices.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i));  // two choices may read the same
                    if (ImGui::Selectable(d.choices[i].c_str(), i == index))
                        apply_setting_edit(s, mod, d.key, std::to_string(i), ui);
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            break;
        }
        case SettingType::Select: {
            const std::string preview = setting_shown_value(d, value);
            if (ImGui::BeginCombo(d.label.c_str(), preview.c_str())) {
                for (size_t i = 0; i < d.options.size(); ++i) {
                    const auto& o = d.options[i];
                    ImGui::PushID(static_cast<int>(i));  // two options may have the same label
                    if (ImGui::Selectable(o.label.c_str(), o.value == value)) apply_setting_edit(s, mod, d.key, o.value, ui);
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            break;
        }
        case SettingType::Color: {
            float rgb[3] = {1, 1, 1};
            color_to_floats(shown, rgb);
            if (ImGui::ColorEdit3(d.label.c_str(), rgb,
                                  ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_PickerHueBar | ImGuiColorEditFlags_NoAlpha))
                set_pending_edit(ui, full, floats_to_color(rgb), frame);
            // saved once nothing is dragged or typed any more (the picker is a popup of its own)
            if (pending_edit(ui, full, frame) && !ImGui::IsAnyItemActive()) {
                const std::string v = ui.pending[full].value;
                ui.pending.erase(full);
                apply_setting_edit(s, mod, d.key, v, ui);
            }
            break;
        }
        case SettingType::Key: {
            const bool waiting = ui.capturing == full;
            if (waiting) {
                ui.capture_frame = frame;
                if (!ui.captured.empty()) {
                    const std::string key = ui.captured;
                    ui.capturing.clear();
                    ui.captured.clear();
                    apply_setting_edit(s, mod, d.key, key, ui);
                }
            }
            const std::string label = (ui.capturing == full ? std::string("press a key (Esc: cancel)")
                                                             : value.empty() ? std::string("no key") : value) +
                                      "###bind";
            if (ImGui::Button(label.c_str(), ImVec2(ImGui::GetFontSize() * 14.0f, 0))) {
                ui.capturing = full;
                ui.captured.clear();
                ui.capture_modifier.clear();
                ui.capture_frame = frame;
            }
            tooltip(d, d.default_value.empty() ? "no key" : d.default_value);
            ImGui::SameLine();
            if (!value.empty() && ImGui::SmallButton("clear")) apply_setting_edit(s, mod, d.key, "", ui);
            if (!value.empty()) ImGui::SameLine();
            ImGui::TextUnformatted(d.label.c_str());
            break;
        }
        case SettingType::Text: {
            // room for the whole value, whatever it is (ImGui must never get text longer than its buffer)
            const size_t cap = (std::max)(d.max_length ? (std::min<size_t>)(d.max_length * 4u + 1u, 260) : size_t{260},
                                          value.size() + 1);
            std::vector<char> text(cap, '\0');
            std::memcpy(text.data(), value.data(), value.size());
            char* buf = text.data();
            // maxLength counts characters while typing (the buffer holds bytes: up to 4 per character)
            const auto limit = [](ImGuiInputTextCallbackData* cb) -> int {
                const auto max = static_cast<int>(reinterpret_cast<uintptr_t>(cb->UserData));
                int chars = 0;
                for (int i = 0; i < cb->BufTextLen; ++i) {
                    if ((static_cast<unsigned char>(cb->Buf[i]) & 0xC0) == 0x80) continue;
                    if (++chars > max) {
                        cb->DeleteChars(i, cb->BufTextLen - i);
                        break;
                    }
                }
                return 0;
            };
            const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | (d.max_length ? ImGuiInputTextFlags_CallbackEdit : 0);
            if (ImGui::InputText(d.label.c_str(), buf, cap, flags, d.max_length ? +limit : nullptr,
                                 reinterpret_cast<void*>(static_cast<uintptr_t>(d.max_length))))
                apply_setting_edit(s, mod, d.key, buf, ui);
            break;
        }
    }
    if (d.type != SettingType::Key) tooltip(d, setting_shown_value(d, d.default_value));
    if (!d.live) {
        ImGui::SameLine();
        ImGui::TextDisabled("(next start)");
    }
    if (!active) ImGui::EndDisabled();
    const std::string condition = active ? "" : setting_condition_text(d, defs);
    for (const std::string* line : {&d.help, &condition}) {
        if (line->empty()) continue;
        ImGui::Indent();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", line->c_str());
        ImGui::PopTextWrapPos();
        ImGui::Unindent();
    }
    ImGui::PopID();
}

}  // namespace

std::string mod_row_label(const ModRecord& r) {
    return r.name + (r.version.empty() ? "" : "  " + r.version) + "##" + r.source;
}

NumberWidget number_widget(const SettingDef& d) {
    const bool whole = d.type == SettingType::Int;
    const bool slider = d.has_min && d.has_max && (d.slider || !whole || d.max - d.min <= 1000000.0);
    if (slider) return whole ? NumberWidget::SliderInt : NumberWidget::SliderDouble;
    return whole ? NumberWidget::InputInt : NumberWidget::InputDouble;
}

std::string describe_setting(const SettingDef& d) {
    const std::string steps = d.step > 0 ? ", in steps of " + number(d.step) : "";
    switch (d.type) {
        case SettingType::Bool: return "on or off";
        case SettingType::Int: {
            const std::string r = range(d);
            return (r.empty() ? "a whole number" : "a whole number, " + r) + steps;
        }
        case SettingType::Float: {
            const std::string r = range(d);
            return (r.empty() ? "a number" : "a number, " + r) + steps;
        }
        case SettingType::Choice: {
            std::string t = "one of: ";
            for (size_t i = 0; i < d.choices.size(); ++i) t += (i ? ", " : "") + d.choices[i];
            return t;
        }
        case SettingType::Select: {
            std::string t = "one of: ";
            for (size_t i = 0; i < d.options.size(); ++i) t += (i ? ", " : "") + d.options[i].label;
            return t;
        }
        case SettingType::Color: return "a colour (#rrggbb)";
        case SettingType::Key: return "a key: a letter, a digit, F1-F24, Space, PageUp, Numpad0-9...";
        default: return d.max_length ? "text, at most " + std::to_string(d.max_length) + " characters" : "text";
    }
}

ImVec4 status_colour(ModStatus s) {
    switch (s) {
        case ModStatus::Loaded: return ImVec4(0.49f, 0.91f, 0.53f, 1.0f);
        case ModStatus::Script: return ImVec4(0.55f, 0.75f, 1.00f, 1.0f);
        case ModStatus::Disabled: return ImVec4(0.55f, 0.57f, 0.60f, 1.0f);
        default: return ImVec4(1.00f, 0.48f, 0.45f, 1.0f);
    }
}

bool apply_setting_edit(SettingsStore& store, const std::string& mod, const std::string& key, const std::string& value,
                        ModsUiState& ui) {
    std::string why;
    const auto r = store.set(mod, key, value, &why);
    if (r != SettingsStore::SetResult::Ok && r != SettingsStore::SetResult::Unchanged) {
        ui.message = key + ": " + why;
        ui.message_error = true;
        return false;
    }
    bool live = false;
    for (const auto& d : store.defs(mod))
        if (d.key == key) live = d.live;
    ui.message = key + " = " + store.get(mod, key) + (live ? "" : ": applies at the next game start");
    ui.message_error = false;
    return true;
}

const std::string* pending_edit(ModsUiState& ui, const std::string& full, int frame) {
    auto it = ui.pending.find(full);
    if (it == ui.pending.end()) return nullptr;
    it->second.frame = frame;
    return &it->second.value;
}

void set_pending_edit(ModsUiState& ui, const std::string& full, std::string value, int frame) {
    ui.pending[full] = PendingEdit{std::move(value), frame};
}

void forget_stale_edits(ModsUiState& ui, int frame) {
    for (auto it = ui.pending.begin(); it != ui.pending.end();)
        it = it->second.frame < frame - 1 ? ui.pending.erase(it) : std::next(it);
    if (!ui.capturing.empty() && ui.capture_frame < frame - 1) {
        ui.capturing.clear();
        ui.captured.clear();
        ui.capture_modifier.clear();
    }
}

bool mod_switch(SettingsStore& store, const ModRecord& mod, ModsUiState& ui, uint64_t now_ms) {
    if (ui.switch_mod != mod.id || ui.switch_version != store.version() || now_ms - ui.switch_read_ms >= 1000) {
        ui.switch_on = store.enabled(mod.id, mod.enabled_default);
        ui.switch_mod = mod.id;
        ui.switch_version = store.version();
        ui.switch_read_ms = now_ms;
    }
    return ui.switch_on;
}

std::string key_event_name(const KeyEvent& e) {
    UINT vk = e.vkey;
    if (!vk) vk = MapVirtualKeyW(e.scan | (e.extended ? 0xE000 : 0), MAPVK_VSC_TO_VK_EX);
    switch (vk) {  // left and right keys bind as the generic key (as EMF's hotkeys read them)
        case VK_LSHIFT: case VK_RSHIFT: vk = VK_SHIFT; break;
        case VK_LCONTROL: case VK_RCONTROL: vk = VK_CONTROL; break;
        case VK_LMENU: case VK_RMENU: vk = VK_MENU; break;
        default: break;
    }
    if (vk == 0 || vk <= 0x06 || vk > 0xFE || vk == VK_ESCAPE) return "";  // nothing, mouse buttons, Esc
    if (vk == VK_PROCESSKEY || vk == VK_PACKET) return "";                 // the IME composing, injected text
    const std::string name = emf::keys::detail::key_name(vk);
    if (name.rfind("key ", 0) != 0) return name;
    char b[8];
    std::snprintf(b, sizeof b, "0x%02x", vk);
    return b;
}

bool capture_key_event(ModsUiState& ui, const KeyEvent& e, int frame) {
    if (!ui.capturing.empty() && ui.capture_frame < frame - 1) {  // its widget is gone: the keys belong to the console
        ui.capturing.clear();
        ui.captured.clear();
        ui.capture_modifier.clear();
    }
    if (ui.capturing.empty()) return false;
    const std::string name = key_event_name(e);
    const bool modifier = name == "Shift" || name == "Ctrl" || name == "Alt";
    if (!e.down) {  // releases always reach ImGui (no key stays held there); a modifier let go alone is the key
        if (modifier && name == ui.capture_modifier && ui.captured.empty()) ui.captured = name;
        return false;
    }
    if (!ui.captured.empty()) return true;  // keys after the one taken stay out of ImGui
    UINT vk = e.vkey;
    if (!vk) vk = MapVirtualKeyW(e.scan | (e.extended ? 0xE000 : 0), MAPVK_VSC_TO_VK_EX);
    if (vk == VK_ESCAPE) {
        ui.capturing.clear();
        ui.capture_modifier.clear();
        return true;
    }
    if (modifier) ui.capture_modifier = name;  // wait: Shift+G binds G, Shift alone binds Shift
    else if (!name.empty()) ui.captured = name;
    return true;
}

bool color_to_floats(const std::string& hex, float rgb[3]) {
    if (hex.size() != 7 || hex[0] != '#') return false;
    for (int i = 0; i < 3; ++i) {
        char* end = nullptr;
        const std::string part = hex.substr(1 + 2 * i, 2);
        const long v = std::strtol(part.c_str(), &end, 16);
        if (!end || *end) return false;
        rgb[i] = static_cast<float>(v) / 255.0f;
    }
    return true;
}

std::string floats_to_color(const float rgb[3]) {
    char b[8];
    const auto c = [](float f) { return static_cast<int>(std::lround((std::clamp)(f, 0.0f, 1.0f) * 255.0f)); };
    std::snprintf(b, sizeof b, "#%02x%02x%02x", c(rgb[0]), c(rgb[1]), c(rgb[2]));
    return b;
}

void draw_mods_tab(const std::vector<ModRecord>& mods, SettingsStore* settings, ModsUiState& ui) {
    draw_mods_tab_profile_bar(static_cast<uint64_t>(ImGui::GetTime() * 1000.0));  // mod profiles (profiles_ui.h)
    if (mods.empty()) {
        ImGui::TextDisabled("No mods in emf\\mods.");
        return;
    }
    const int frame = ImGui::GetFrameCount();
    forget_stale_edits(ui, frame);
    const ModRecord* sel = nullptr;
    for (const auto& m : mods)
        if (m.source == ui.selected) sel = &m;
    if (!sel) {
        sel = &mods.front();
        ui.selected = sel->source;
    }

    const float list_w = (std::max)(ImGui::GetFontSize() * 16.0f, ImGui::GetContentRegionAvail().x * 0.28f);
    ImGui::BeginChild("##mods_list", ImVec2(list_w, 0), ImGuiChildFlags_Borders);
    // The selected and hovered rows get a dark slate instead of ImGui's bright blue, so the status colours stay readable;
    // low values also stay dark when the game's HDR swapchain reads the overlay's colours as PQ (see ENGINE_MAP.md).
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.22f, 0.26f, 0.32f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.17f, 0.20f, 0.25f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.27f, 0.32f, 0.39f, 1.0f));
    for (const auto& m : mods) {
        ImGui::PushStyleColor(ImGuiCol_Text, status_colour(m.status));
        if (ImGui::Selectable(mod_row_label(m).c_str(), m.source == ui.selected)) {
            ui.selected = m.source;
            ui.message.clear();
        }
        ImGui::PopStyleColor();
    }
    ImGui::PopStyleColor(3);
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##mod_details", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::Text("%s", sel->name.c_str());
    if (!sel->version.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", sel->version.c_str());
    }
    if (!sel->authors.empty()) ImGui::TextDisabled("by %s", sel->authors.c_str());
    if (!sel->description.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(sel->description.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, status_colour(sel->status));
    ImGui::Text("%s%s%s", mod_status_name(sel->status), sel->reason.empty() ? "" : ": ", sel->reason.c_str());
    ImGui::PopStyleColor();
    ImGui::TextDisabled("emf\\mods\\%s", sel->source.c_str());
    if (settings && sel->status != ModStatus::Rejected) {
        bool on = mod_switch(*settings, *sel, ui, static_cast<uint64_t>(ImGui::GetTime() * 1000.0));
        if (ImGui::Checkbox("Switched on (applies at the next game start)", &on)) {
            if (settings->set_enabled(sel->id, on)) {
                ui.message = sel->name + std::string(on ? " is switched on" : " is switched off") +
                             " from the next game start";
                ui.message_error = false;
            } else {
                ui.message = "could not write emf\\config\\" + sel->id + ".ini";
                ui.message_error = true;
            }
        }
    }
    ImGui::Separator();
    const auto defs = settings ? settings->defs(sel->id) : std::vector<SettingDef>{};
    if (defs.empty()) {
        ImGui::TextDisabled("No settings.");
    } else {
        std::string group;
        for (size_t i : setting_display_order(defs)) {
            if (defs[i].group != group) {  // a heading where a group starts
                group = defs[i].group;
                ImGui::Spacing();
                ImGui::SeparatorText(group.c_str());
            }
            draw_setting(*settings, sel->id, defs[i], defs, ui, frame);
        }
    }
    if (!ui.message.empty()) {
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, ui.message_error ? ImVec4(1.0f, 0.48f, 0.45f, 1.0f)
                                                              : ImVec4(0.49f, 0.91f, 0.53f, 1.0f));
        ImGui::TextUnformatted(ui.message.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
}

}  // namespace emf::core
