#include "profiles.h"

#include <ctime>
#include <map>
#include <set>

#include "text.h"

namespace emf::app {

namespace {

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// What a Switch would do, without doing it (an import's preview): every switch and setting "succeeds".
class DrySwitcher : public core::ProfileSwitcher {
public:
    std::vector<core::ApplyItem> settings;  // id = "mod.key", why = the value it would get
    Result set_enabled(const std::string&, const std::string&, bool) override { return {true, {}}; }
    Result set_setting(const std::string& mod, const std::string& key, const std::string& value) override {
        settings.push_back({"emf", mod + "." + key, mod, {}, true, value});
        return {true, {}};
    }
};

}  // namespace

std::wstring profiles_dir(const std::wstring& game) {
    std::wstring g = game;
    while (g.size() > 3 && (g.back() == L'\\' || g.back() == L'/')) g.pop_back();
    return g + L"\\emf\\profiles";
}

std::vector<core::FolderMod> profile_folder(const FolderState& s) {
    std::vector<core::FolderMod> out;
    std::map<std::string, size_t> at;  // source and id (case ignored) -> index: two copies of one mod are one
    for (const auto& m : s.mods) {
        if (m.is_loader) continue;  // another loader's own DLL next to the game: not a mod anyone switches with a profile
        core::FolderMod f;
        f.source = m.source;
        f.id = m.id;
        f.name = m.name;
        f.on = m.enabled;
        f.forced = m.state_forced;
        f.can_switch = m.can_toggle && !m.state_forced;
        f.read_only = !m.can_toggle && !m.state_forced;  // its own loader switches it
        if (!f.can_switch) f.why_not = m.state_forced && !m.error.empty() ? m.error : m.toggle_note;
        f.nexus_id = m.nexus_id;
        if (f.nexus_id.empty() && !m.install_key.empty())
            for (const auto& r : s.installed)
                if (r.key == m.install_key) f.nexus_id = r.nexus_id;
        for (const auto& st : m.settings) f.settings.push_back({st.def.key, st.value, st.def});
        const std::string key = f.source + '\x1f' + lower(f.id);
        if (auto it = at.find(key); it != at.end()) {
            // mods\foo and mods\.foo: EML loads the one without the dot; EMF cannot rename either while both are there
            core::FolderMod& first = out[it->second];
            first.on = first.on || f.on;
            if (first.source == "eml") {
                first.can_switch = false;
                first.why_not = "mods\\" + first.id + " and mods\\." + first.id +
                                " both exist (two copies of one mod): remove the one you do not want first";
            }
            continue;
        }
        at.emplace(key, out.size());
        out.push_back(std::move(f));
    }
    return out;
}

core::ProfileSwitcher::Result FolderSwitcher::set_enabled(const std::string& source, const std::string& id, bool on) {
    const Change c = set_mod_enabled(game_, source, id, on, facts_ ? &*facts_ : nullptr);
    return {c.ok, c.error, c.unchanged};
}

core::ProfileSwitcher::Result FolderSwitcher::set_setting(const std::string& mod, const std::string& key,
                                                          const std::string& value) {
    const Change c = set_mod_setting(game_, mod, key, value, facts_ ? &*facts_ : nullptr);
    return {c.ok, c.error, c.unchanged};
}

ProfilesOverview profiles_overview(const std::wstring& game, const FolderState& s) {
    ProfilesOverview o;
    if (game.empty() || !s.game_found) return o;
    const core::ProfileStore store(profiles_dir(game));
    o.list = store.list();
    o.current = store.current();
    if (o.current.empty()) return o;
    const core::ProfileRead r = store.read(o.current);
    if (!r.profile) {  // the current one is broken now: no current one
        o.current.clear();
        return o;
    }
    o.current_name = r.profile->name;
    o.changed = s.read_error.empty() && core::profile_differs(*r.profile, profile_folder(s));
    return o;
}

core::ProfileSave save_folder_profile(const std::wstring& game, const FolderState& s, const std::string& name,
                                      bool with_settings, bool replace, const std::string& created,
                                      const std::string& emf_version) {
    core::ProfileSave out;
    if (game.empty() || !s.game_found) {
        out.error = "Choose the game folder first.";
        return out;
    }
    if (!s.read_error.empty()) {  // a profile of a folder EMF could not read whole would be wrong
        out.error = s.read_error + " Nothing was saved.";
        return out;
    }
    core::Profile p = core::capture_profile(name, profile_folder(s), with_settings);
    p.created = created;
    p.emf_version = emf_version;
    const core::ProfileStore store(profiles_dir(game));
    out = store.save(p, replace);
    if (out.ok) store.set_current(out.slug);
    return out;
}

ProfileApply apply_saved_profile(const std::wstring& game, const FolderState& s, const std::string& slug,
                                 core::ProfileSwitcher& switcher) {
    ProfileApply out;
    if (game.empty() || !s.game_found) {
        out.error = "Choose the game folder first.";
        return out;
    }
    if (!s.read_error.empty()) {
        out.error = s.read_error + " Nothing was switched.";
        return out;
    }
    const core::ProfileStore store(profiles_dir(game));
    const core::ProfileRead r = store.read(slug);
    if (!r.profile) {
        out.error = r.error;
        return out;
    }
    out.report = core::apply_profile(*r.profile, profile_folder(s), switcher);
    store.set_current(slug);
    out.ok = true;
    return out;
}

ProfileImport read_profile_file(const std::wstring& path, const std::wstring& game, const FolderState& s) {
    ProfileImport out;
    std::string text;
    if (!read_file(path, text, core::kProfileFileMax + 1)) {
        out.error = "Could not read " + utf8(path) + ".";
        return out;
    }
    core::ProfileRead r = core::profile_from_json(text);
    if (!r.profile) {
        out.error = r.error;
        return out;
    }
    out.slug = core::profile_slug(r.profile->name);
    if (!game.empty() && s.game_found) {
        const core::ProfileStore store(profiles_dir(game));
        if (const core::ProfileRead old = store.read(out.slug); old.profile || exists(store.dir() + L"\\" + wide(out.slug) + L".json")) {
            out.exists = true;
            out.existing_name = old.profile ? old.profile->name : out.slug;
        }
        const auto folder = profile_folder(s);
        std::set<std::string> here;
        for (const auto& f : folder) here.insert(f.source + '\x1f' + lower(f.id));
        for (const auto& m : r.profile->mods)
            if (!here.count(m.source + '\x1f' + lower(m.id))) out.missing.push_back(m);
        DrySwitcher dry;
        out.preview = core::apply_profile(*r.profile, folder, dry);
        out.settings_to_set = std::move(dry.settings);
    }
    out.profile = std::move(r.profile);
    return out;
}

std::string profile_time_now() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", &tm);
    return b;
}

}  // namespace emf::app
