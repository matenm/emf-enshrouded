#include "profiles_ui.h"

#include <atomic>
#include <cstring>
#include <exception>

#include "core/profiles.h"
#include "imgui.h"

namespace emf::core {

namespace {
std::atomic<ProfileBar*> g_bar{nullptr};
ProfileBarUi g_bar_ui;  // render thread

const ImVec4 kRed(1.0f, 0.48f, 0.45f, 1.0f);
const ImVec4 kGreen(0.49f, 0.91f, 0.53f, 1.0f);
const ImVec4 kAmber(1.0f, 0.78f, 0.34f, 1.0f);
const ImVec4 kQuiet(0.61f, 0.64f, 0.78f, 1.0f);
}  // namespace

ProfileBar::View ProfileBar::view() const {
    std::lock_guard<std::mutex> l(m_);
    return v_;
}

bool ProfileBar::begin() {
    std::lock_guard<std::mutex> l(m_);
    if (v_.busy) return false;
    v_.busy = true;
    return true;
}

void ProfileBar::finish(View next, bool with_answer) {
    std::lock_guard<std::mutex> l(m_);
    v_.picker = std::move(next.picker);
    v_.read = true;
    if (with_answer) {
        v_.answer = std::move(next.answer);
        v_.failed = next.failed;
        v_.replace = std::move(next.replace);
        v_.replace_settings = next.replace_settings;
        v_.restart_pending = v_.restart_pending || next.restart_pending;
    }
    v_.busy = false;
}

void ProfileBar::start(std::function<void(View&)> work, bool with_answer) {
    run_([this, work = std::move(work), with_answer] {
        View next;
        bool answer = with_answer;
        try {
            work(next);
        } catch (...) {  // whatever happens, the bar is usable again and says so
            next = View{};
            next.answer = {"profile: stopped with an error"};
            next.failed = true;
            answer = true;
            try {
                next.picker = read_profile_picker(ctx_);
            } catch (...) {
            }
        }
        finish(std::move(next), answer);
    });
}

void ProfileBar::refresh(uint64_t now_ms, uint64_t max_age_ms) {
    {
        std::lock_guard<std::mutex> l(m_);
        if (v_.busy || (v_.read && now_ms - read_ms_ < max_age_ms)) return;
        read_ms_ = now_ms;
    }
    if (!begin()) return;
    start([this](View& next) { next.picker = read_profile_picker(ctx_); }, false);
}

void ProfileBar::load(const std::string& name) {
    if (!begin()) return;
    start([this, name](View& next) {
        const ProfileAnswer a = load_profile_now(ctx_, name);
        next.answer = a.lines;
        next.failed = !a.ok;
        next.restart_pending = a.switched > 0;
        next.picker = read_profile_picker(ctx_);
    }, true);
}

void ProfileBar::save(const std::string& name, bool settings, bool replace) {
    if (!begin()) return;
    start([this, name, settings, replace](View& next) {
        const ProfileAnswer a = save_profile_now(ctx_, name, settings, replace);
        if (a.exists) {
            next.replace = trim_profile_name(name);
            next.replace_settings = settings;
        } else {
            next.answer = a.lines;
            next.failed = !a.ok;
        }
        next.picker = read_profile_picker(ctx_);
    }, true);
}

void ProfileBar::dismiss() {
    std::lock_guard<std::mutex> l(m_);
    v_.answer.clear();
    v_.failed = false;
    v_.replace.clear();
}

void draw_profile_bar(ProfileBar& bar, ProfileBarUi& ui, uint64_t now_ms) {
    bar.refresh(now_ms);
    const ProfileBar::View v = bar.view();
    const auto& entries = v.picker.entries;
    const ProfilePickerEntry* picked = nullptr;
    for (const auto& e : entries)
        if (e.slug == ui.pick) picked = &e;
    if (!picked) {  // the current one until the player picks another
        for (const auto& e : entries)
            if (e.slug == v.picker.current_slug) picked = &e;
        if (!picked && !entries.empty()) picked = &entries.front();
        ui.pick = picked ? picked->slug : std::string();
    }

    ImGui::PushID("profiles");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Profile");
    ImGui::SameLine();
    const auto label = [&](const ProfilePickerEntry& e) {
        if (e.slug != v.picker.current_slug) return e.name;
        return e.name + (v.picker.changed ? " (changed)" : " (current)");
    };
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    ImGui::BeginDisabled(entries.empty());
    const std::string shown = picked ? label(*picked) : std::string(v.read ? "no profiles yet" : "reading...");
    if (ImGui::BeginCombo("##pick", shown.c_str())) {
        for (const auto& e : entries) {
            const bool selected = picked && e.slug == picked->slug;
            const std::string item = label(e) + "##" + e.slug;  // two profiles may share a display name
            if (ImGui::Selectable(item.c_str(), selected)) ui.pick = e.slug;
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(v.busy || ui.pick.empty());
    if (ImGui::Button("Load")) bar.load(ui.pick);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Switches every mod to this profile from the next game start");
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
    ImGui::InputTextWithHint("##name", "new profile name", ui.name, sizeof ui.name);
    ImGui::SameLine();
    ImGui::Checkbox("with settings", &ui.settings);
    ImGui::SameLine();
    ImGui::BeginDisabled(v.busy || !ui.name[0]);
    if (ImGui::Button("Save as")) {
        bar.save(ui.name, ui.settings, false);
        ui.pick = profile_slug(ui.name);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Saves the on/off state of every mod (and, with settings, the EMF mods' settings)");

    if (!v.replace.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::Text("A profile named %s is there already. Replace it with the mods as they are now?", v.replace.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::BeginDisabled(v.busy);
        if (ImGui::Button("Replace")) bar.save(v.replace, v.replace_settings, true);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) bar.dismiss();
    } else if (!v.answer.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        for (size_t i = 0; i < v.answer.size(); ++i) {
            const bool bad = v.answer[i].find("FAILED") != std::string::npos || v.answer[i].rfind("refused", 0) == 0 ||
                             v.answer[i].rfind("no profile", 0) == 0 || v.answer[i].rfind("profile:", 0) == 0;
            ImGui::PushStyleColor(ImGuiCol_Text, bad ? kRed : i == 0 ? (v.failed ? kAmber : kGreen) : kQuiet);
            ImGui::TextUnformatted(v.answer[i].c_str());
            ImGui::PopStyleColor();
        }
        ImGui::PopTextWrapPos();
        if (ImGui::SmallButton("OK")) bar.dismiss();
    } else if (!v.picker.current.empty()) {
        if (v.picker.changed)
            ImGui::TextDisabled("Switches changed since %s. Load sets them back from the next game start.",
                                v.picker.current.c_str());
        else if (v.restart_pending)
            ImGui::TextDisabled("The switches are as %s has them from the next game start (restart the game to apply).",
                                v.picker.current.c_str());
        else
            ImGui::TextDisabled("The mods are as %s has them.", v.picker.current.c_str());
    } else {
        ImGui::TextDisabled("A profile saves the on/off state of every mod under a name (also: the profile command).");
    }
    ImGui::PopID();
}

void set_mods_tab_profile_bar(ProfileBar* bar) { g_bar = bar; }

void draw_mods_tab_profile_bar(uint64_t now_ms) {
    ProfileBar* bar = g_bar.load();
    if (!bar) return;
    draw_profile_bar(*bar, g_bar_ui, now_ms);
    ImGui::Separator();
}

}  // namespace emf::core
