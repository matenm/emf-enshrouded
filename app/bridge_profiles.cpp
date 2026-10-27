// The bridge's part of mod profiles (docs/app.md "Profiles"): Save as, Switch, Rename, Delete, Export and
// Import on the Mods tab, and the picker's data in the state. Kept apart from bridge.cpp so the two change
// independently. The rules and the files are app/profiles.h's (shared with the core's "profile" command).
#include "bridge.h"
#include "core/json.h"
#include "json_out.h"
#include "profiles.h"
#include "server_bridge.h"
#include "text.h"

#ifndef EMF_VERSION
#define EMF_VERSION "dev"
#endif

namespace emf::app {

namespace {

using core::json::Value;
using VT = Value::Type;

std::string field(const Value& v, const char* key) {
    const Value* x = v.get(key);
    return x && x->is(VT::String) ? x->s : std::string();
}

bool flag(const Value& v, const char* key) {
    const Value* x = v.get(key);
    return x && x->is(VT::Bool) && x->b;
}

std::string item_json(const core::ApplyItem& i) {
    return Obj().str("source", i.source).str("id", i.id).str("name", i.name).str("nexus_id", i.nexus_id)
        .boolean("on", i.on).str("why", i.why).text();
}

std::string items_json(const std::vector<core::ApplyItem>& items) {
    std::vector<std::string> out;
    for (const auto& i : items) out.push_back(item_json(i));
    return json_list(out);
}

std::string report_json(const core::ApplyReport& r) {
    return Obj().str("type", "profile_report").str("profile", r.profile).boolean("ok", r.ok())
        .raw("switched", items_json(r.switched)).num("unchanged", static_cast<double>(r.unchanged.size()))
        .raw("failed", items_json(r.failed)).raw("missing", items_json(r.missing)).raw("read_only", items_json(r.read_only))
        .raw("recorded_read_only", items_json(r.recorded_read_only))
        .raw("not_in_profile", items_json(r.not_in_profile)).num("settings_set", static_cast<double>(r.settings_set))
        .num("settings_unchanged", static_cast<double>(r.settings_unchanged))
        .raw("settings_failed", items_json(r.settings_failed)).raw("settings_skipped", items_json(r.settings_skipped))
        .text();
}

std::string mods_text(size_t n) { return std::to_string(n) + (n == 1 ? " mod" : " mods"); }

std::string busy_json(bool busy, const std::string& slug) {
    return Obj().str("type", "profile_busy").boolean("busy", busy).str("slug", slug).text();
}

}  // namespace

std::string profiles_json(const FolderState& s) {
    const ProfilesOverview o = profiles_overview(s.game, s);
    std::vector<std::string> list;
    for (const auto& p : o.list)
        list.push_back(Obj().str("slug", p.slug).str("name", p.name).str("created", p.created).str("emf_version", p.emf_version)
                           .num("mods", static_cast<double>(p.mods)).boolean("with_settings", p.with_settings)
                           .str("error", p.error).text());
    return Obj().str("current", o.current).str("current_name", o.current_name).boolean("changed", o.changed)
        .raw("list", json_list(list)).text();
}

std::optional<std::vector<std::string>> Bridge::handle_profiles(const std::string& cmd, const Value& m) {
    using Out = std::vector<std::string>;
    if (cmd.rfind("profile_", 0) != 0) return std::nullopt;
    const std::wstring game = host_.game();
    const auto with_state = [&](std::string r) {
        Out out{std::move(r)};
        if (auto s = state_if_changed(true)) out.push_back(*s);
        return out;
    };
    if (!is_game_folder(game)) return Out{result(cmd, false, "Choose the game folder first.")};
    const core::ProfileStore store(profiles_dir(game));
    // while a Switch (or an install) changes the folder on a worker, nothing reads a half-switched folder or moves the
    // profile it is about
    const bool writes = cmd == "profile_save" || cmd == "profile_rename" || cmd == "profile_delete" || cmd == "profile_import_save";
    if (writes && (busy_ || desk_->installing())) return Out{result(cmd, false, "EMF is busy (switching or installing). Wait a moment.")};

    if (cmd == "profile_save") {
        const std::string name = core::trim_profile_name(field(m, "name"));
        const bool settings = flag(m, "settings");
        const FolderState now = scan_game_folder(game, ScanDepth::Profiles);  // what is there now, not at the last look
        const core::ProfileSave r = save_folder_profile(game, now, name, settings, flag(m, "replace"), profile_time_now(), EMF_VERSION);
        if (r.exists)  // the page asks, then sends the same with replace
            return Out{Obj().str("type", "profile_exists").str("name", name).str("existing_name", r.existing_name)
                           .boolean("settings", settings).text()};
        if (!r.ok) return Out{result(cmd, false, r.error)};
        const auto saved = store.read(r.slug);
        return with_state(result(cmd, true, "", "Saved profile " + name + " (" +
                                                    mods_text(saved.profile ? saved.profile->mods.size() : 0) +
                                                    (settings ? ", with their settings" : "") + ")."));
    }
    if (cmd == "profile_switch") {
        const std::string reason = host_.running_reason();
        if (!reason.empty())
            return with_state(result(cmd, false, "Mods are switched while the game is closed: close the game first (" + reason + ")."));
        if (busy_ || desk_->installing())  // also while EMF is installed into this server folder (server_bridge.h)
            return Out{result(cmd, false, "EMF is busy (switching or installing). Wait a moment.")};
        const std::string slug = field(m, "slug");
        // the folder is read and switched on a worker: a long profile never freezes the window
        busy_ = true;
        changing_ = true;
        auto alive = alive_;
        host_.run_async([this, alive, game, slug, cmd]() -> BridgeHost::Done {
            ProfileApply a;
            try {
                const FolderState s = scan_game_folder(game, ScanDepth::Profiles);
                FolderSwitcher sw(game, folder_facts(s));
                a = apply_saved_profile(game, s, slug, sw);
            } catch (const std::exception& e) {
                a = ProfileApply{};
                a.error = std::string("EMF stopped with an error: ") + e.what();
            }
            return [this, alive, a, slug, cmd]() -> std::vector<std::string> {
                if (!*alive) return {};
                busy_ = false;
                changing_ = false;
                Out out;
                if (!a.ok) {
                    out.push_back(result(cmd, false, a.error));
                } else {
                    const core::ApplyReport& r = a.report;
                    std::string text = "Switched to " + r.profile + ": " + mods_text(r.switched.size()) + " switched";
                    if (!r.missing.empty()) text += ", " + std::to_string(r.missing.size()) + " not in this folder";
                    text += ". Takes effect at the next game start.";
                    std::string error;
                    if (!r.ok())
                        error = r.profile + ": " + std::to_string(r.failed.size() + r.settings_failed.size()) +
                                " change(s) failed; the others were made. See the report.";
                    out.push_back(report_json(r));
                    out.push_back(result(cmd, r.ok(), error, r.ok() ? text : ""));
                }
                out.push_back(busy_json(false, slug));
                if (auto s = state_if_changed(true)) out.push_back(*s);
                return out;
            };
        });
        return Out{busy_json(true, slug)};
    }
    if (cmd == "profile_rename") {
        const core::ProfileSave r = store.rename(field(m, "slug"), field(m, "name"));
        return with_state(result(cmd, r.ok, r.error, r.ok ? "Renamed to " + core::trim_profile_name(field(m, "name")) + "." : ""));
    }
    if (cmd == "profile_delete") {
        const std::string slug = field(m, "slug");
        const core::ProfileRead p = store.read(slug);
        std::string why;
        const bool ok = core::valid_profile_slug(slug) && store.remove(slug, &why);
        if (!ok && why.empty()) why = "There is no profile \"" + slug + "\".";
        return with_state(result(cmd, ok, why, ok ? "Deleted the profile " + (p.profile ? p.profile->name : slug) + " (no mod was touched)." : ""));
    }
    if (cmd == "profile_export") {
        const std::string slug = field(m, "slug");
        const core::ProfileRead p = store.read(slug);
        if (!p.profile) return Out{result(cmd, false, p.error)};
        std::string bytes;
        if (!read_file(store.dir() + L"\\" + wide(slug) + L".json", bytes, core::kProfileFileMax + 1))
            return Out{result(cmd, false, "Could not read the profile's file.")};
        const auto to = host_.pick_profile_save(wide(p.profile->name) + L".json");
        if (!to) return Out{};  // cancelled
        const Change c = write_file_whole(*to, bytes, utf8(*to));
        return Out{result(cmd, c.ok, c.error, c.ok ? "Exported " + p.profile->name + " to " + utf8(*to) + "." : "")};
    }
    if (cmd == "profile_import" || cmd == "profile_import_path") {
        std::optional<std::wstring> path;
        if (cmd == "profile_import_path") {
            if (!host_.test_mode()) return Out{result(cmd, false, "The app does not know the command \"" + cmd + "\".")};
            path = wide(field(m, "path"));
        } else {
            path = host_.pick_profile_file();
            if (!path) return Out{};  // cancelled
        }
        const ProfileImport i = read_profile_file(*path, game, scan_game_folder(game, ScanDepth::Profiles));
        profile_import_ = i.profile;
        std::vector<std::string> missing;
        for (const auto& x : i.missing)
            missing.push_back(Obj().str("source", x.source).str("id", x.id).str("name", x.name).str("nexus_id", x.nexus_id)
                                  .boolean("on", x.on).boolean("read_only", x.read_only).text());
        Obj o;
        o.str("type", "profile_import").boolean("ok", i.profile.has_value()).str("error", i.error);
        o.str("file", utf8(*path)).str("name", i.profile ? i.profile->name : "").str("slug", i.slug);
        o.str("created", i.profile ? i.profile->created : "").str("emf_version", i.profile ? i.profile->emf_version : "");
        o.num("mods", i.profile ? static_cast<double>(i.profile->mods.size()) : 0.0);
        o.boolean("with_settings", i.profile && i.profile->with_settings);
        o.boolean("exists", i.exists).str("existing_name", i.existing_name).raw("missing", json_list(missing));
        // what Switch would do here: the mods it would flip, those it could not, and the settings it would rewrite
        o.raw("would_switch", items_json(i.preview.switched)).raw("would_not", items_json(i.preview.read_only));
        o.raw("recorded_read_only", items_json(i.preview.recorded_read_only));
        std::vector<std::string> would_set;
        for (const auto& x : i.settings_to_set) would_set.push_back(Obj().str("id", x.id).str("value", x.why).text());
        o.raw("would_set", json_list(would_set));
        return Out{o.text()};
    }
    if (cmd == "profile_import_save") {
        if (!profile_import_) return Out{result(cmd, false, "Nothing waits to be imported: choose the file again.")};
        core::Profile p = *profile_import_;
        if (const std::string name = core::trim_profile_name(field(m, "name")); !name.empty()) p.name = name;
        const core::ProfileSave r = store.save(p, flag(m, "replace"));
        if (!r.ok) return Out{result(cmd, false, r.exists ? "A profile named " + r.existing_name + " exists already: replace it, or choose another name." : r.error)};
        profile_import_.reset();
        return with_state(result(cmd, true, "", "Imported " + p.name + ". Switch to it to use it."));
    }
    if (cmd == "profile_import_cancel") {
        profile_import_.reset();
        return Out{};
    }
    return Out{result(cmd, false, "The app does not know the command \"" + cmd + "\".")};
}

}  // namespace emf::app
