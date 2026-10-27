// EMF.exe's Saves tab (docs/app.md "Saves"): the save backups (src/saves/backup.h) through the page bridge.
// The list, Back up now, Restore (all or single files; refused while the game runs; a before-restore backup first),
// Delete (the page asks first), Open folder, and the automatic backup settings ([backup] in emf\emf_core.ini). Copies run
// on a worker; one at a time. At EMF.exe's first start (the page asks for the state the first time) it makes the starter
// backup when there is none.
#include <windows.h>

#include <filesystem>

#include "bridge.h"
#include "core/json.h"
#include "json_out.h"
#include "locate.h"
#include "saves/save_info.h"
#include "text.h"

#ifndef EMF_VERSION
#define EMF_VERSION "dev"
#endif

namespace emf::app {

namespace {

using core::json::Value;
using VT = Value::Type;

std::string text_of(const Value& v, const char* key) {
    const Value* x = v.get(key);
    return x && x->is(VT::String) ? x->s : std::string();
}

std::string file_json(const saves::Backup& b, const saves::BackupFile& f) {
    int64_t play = 0;  // a world's play time, when the core saw it in the game's menu
    for (const auto& i : b.info)
        if (i.storage == f.storage && i.type == f.type && i.slot == f.slot &&
            (f.storage != "steam" || i.steam_id.empty() || i.steam_id == f.steam_id))
            play = i.play_seconds;
    return Obj()
        .str("path", f.path).str("storage", f.storage).str("steam_id", f.steam_id).str("type", f.type)
        .num("slot", f.slot).str("name", f.name).str("label", saves::file_label(b, f))
        .num("size", static_cast<double>(f.size)).num("modified", static_cast<double>(f.modified))
        .num("play_seconds", static_cast<double>(play))
        .text();
}

std::string backup_json(const saves::Backup& b) {
    std::vector<std::string> files;
    for (const auto& f : b.files) files.push_back(file_json(b, f));
    return Obj()
        .str("id", b.id).str("kind", b.kind).str("label", b.label).num("time", static_cast<double>(b.time))
        .str("time_local", b.time_local).str("made_by", b.made_by).str("emf_version", b.emf_version)
        .num("bytes", static_cast<double>(b.bytes())).str("size_text", saves::size_text(b.bytes()))
        .str("summary", saves::backup_summary(b)).boolean("damaged", b.damaged).str("error", b.error)
        .strs("problems", b.problems).raw("files", json_list(files))
        .text();
}

// A label typed by the player: one line, at most 80 characters (whole UTF-8 sequences).
std::string clean_label(std::string s) {
    for (char& c : s)
        if (static_cast<unsigned char>(c) < 0x20) c = ' ';
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    if (s.size() > 80) {
        size_t cut = 80;
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
        s.resize(cut);
    }
    return s;
}

std::wstring core_ini(const std::wstring& game) { return game + L"\\emf\\emf_core.ini"; }

}  // namespace

std::string saves_busy_reason(const std::vector<ProcessInfo>& processes, const std::wstring& game) {
    for (const auto& p : processes) {
        if (iequals(p.name, L"enshrouded.exe"))
            return "Enshrouded is running (process " + std::to_string(p.pid) + "). Close the game first: it writes the saves "
                   "while it runs.";
        if (iequals(p.name, L"enshrouded_server.exe") && !game.empty() && (p.path.empty() || path_inside(game, p.path)))
            return "The dedicated server in this folder is running (process " + std::to_string(p.pid) + "). Stop it first.";
    }
    return {};
}

std::string BridgeHost::saves_busy_for(const std::wstring& game) { return saves_busy_reason(processes(), game); }

std::string Bridge::saves_json() {
    const saves::SaveRoots roots = host_.save_roots();
    const auto list = saves::list_backups(roots.store);
    std::vector<std::string> items;
    uint64_t total = 0;
    for (const auto& b : list) {
        items.push_back(backup_json(b));
        total += b.bytes();
    }
    const auto now = saves::find_saves(roots);
    uint64_t now_bytes = 0;
    for (const auto& f : now) now_bytes += f.size;
    const std::wstring game = host_.game();
    const bool emf_here = folder_.emf_installed && !folder_.server && !game.empty();
    const saves::AutoSettings a = saves::read_auto_settings(emf_here ? core_ini(game) : std::wstring());
    return Obj()
        .str("type", "saves").str("store", utf8(roots.store)).boolean("available", !roots.store.empty())
        .str("busy", host_.saves_busy()).boolean("working", saves_working_)
        .num("total_bytes", static_cast<double>(total)).str("total_text", saves::size_text(total))
        .num("count", static_cast<double>(list.size()))
        .num("now_files", static_cast<double>(now.size())).str("now_text", saves::size_text(now_bytes))
        .strs("steam_users", saves::steam_users_with_saves(roots))
        .raw("auto", Obj().boolean("available", emf_here).boolean("on", a.on).num("keep", static_cast<double>(a.keep))
                         .num("min_minutes", static_cast<double>(a.min_minutes)).text())
        .raw("list", json_list(items))
        .text();
}

void Bridge::saves_first_start() {
    if (starter_tried_) return;
    starter_tried_ = true;
    const saves::SaveRoots roots = host_.save_roots();
    if (roots.store.empty() || saves::has_starter(roots.store)) return;
    if (!host_.saves_busy().empty()) return;  // the game may be saving: the next start (or the game's own start) does it
    saves_working_ = true;
    auto alive = alive_;
    host_.run_async([this, alive, roots]() -> BridgeHost::Done {
        saves::BackupOptions o;
        o.made_by = "EMF.exe";
        o.emf_version = EMF_VERSION;
        o.info = saves::read_save_info(roots.info).slots;
        const saves::BackupResult r = saves::ensure_starter(roots, o);
        return [this, alive, r]() -> std::vector<std::string> {
            if (!*alive) return {};
            saves_working_ = false;
            if (r.ok && r.nothing) return {};
            std::string text = r.ok ? "EMF backed up your saves before anything else: the starter backup (" +
                                          std::to_string(r.backup.files.size()) + " files, " +
                                          saves::size_text(r.backup.bytes()) + "). The Saves tab lists it."
                                    : "EMF could not back up your saves: " + r.error;
            if (r.ok && !r.backup.problems.empty()) text += " Not copied: " + r.backup.problems.front();
            return {Obj().str("type", "saves_note").boolean("ok", r.ok && r.backup.problems.empty()).str("text", text).text(),
                    saves_json()};
        };
    });
}

std::optional<std::vector<std::string>> Bridge::handle_saves(const std::string& cmd, const Value& m) {
    if (cmd.rfind("saves_", 0) != 0) return std::nullopt;
    const saves::SaveRoots roots = host_.save_roots();
    auto with_list = [&](std::string r) { return std::vector<std::string>{std::move(r), saves_json()}; };
    if (cmd == "saves_list") return std::vector<std::string>{saves_json()};
    if (roots.store.empty()) return with_list(result(cmd, false, "EMF has no folder for save backups on this PC."));

    if (cmd == "saves_backup") {
        if (saves_working_) return with_list(result(cmd, false, "EMF is still busy with the saves. Wait a moment."));
        if (const std::string why = host_.saves_busy(); !why.empty())
            return with_list(result(cmd, false, "Not backed up: " + why + " (In the game, the console's saves.backup works "
                                                "at the main menu.)"));
        const std::string label = clean_label(text_of(m, "label"));
        saves_working_ = true;
        auto alive = alive_;
        host_.run_async([this, alive, roots, label]() -> BridgeHost::Done {
            saves::BackupOptions o;
            o.kind = "manual";
            o.label = label;
            o.made_by = "EMF.exe";
            o.emf_version = EMF_VERSION;
            o.info = saves::read_save_info(roots.info).slots;
            const saves::BackupResult r = saves::make_backup(roots, o);
            return [this, alive, r]() -> std::vector<std::string> {
                if (!*alive) return {};
                saves_working_ = false;
                if (!r.ok) return {result("saves_backup", false, r.error), saves_json()};
                std::string text = "Backed up " + std::to_string(r.backup.files.size()) + " save files (" +
                                   saves::size_text(r.backup.bytes()) + ").";
                if (!r.backup.problems.empty()) {
                    std::string not_copied;
                    for (const auto& p : r.backup.problems) not_copied += " " + p + ".";
                    return {result("saves_backup", false, "The backup is incomplete. Not copied:" + not_copied), saves_json()};
                }
                return {result("saves_backup", true, "", text), saves_json()};
            };
        });
        return std::vector<std::string>{};
    }
    if (cmd == "saves_restore") {
        const std::string id = text_of(m, "id");
        std::vector<std::string> paths;
        if (const Value* p = m.get("paths"); p && p->is(VT::Array))
            for (const Value& x : p->arr)
                if (x.is(VT::String)) paths.push_back(x.s);
        auto refuse = [&](const std::string& why) {
            return std::vector<std::string>{
                Obj().str("type", "saves_restored").boolean("ok", false).str("error", why).strs("restored", {})
                    .str("before_id", "").boolean("steam", false).str("id", id).text(),
                saves_json()};
        };
        if (saves_working_) return refuse("EMF is still busy with the saves. Wait a moment.");
        if (const std::string why = host_.saves_busy(); !why.empty()) return refuse("Saves are restored while the game is closed. " + why);
        saves_working_ = true;
        auto alive = alive_;
        BridgeHost* host = &host_;
        const std::wstring game = host_.game();
        host_.run_async([this, alive, roots, id, paths, host, game]() -> BridgeHost::Done {
            saves::RestoreOptions o;
            o.id = id;
            o.paths = paths;
            o.busy = [host, game] { return host->saves_busy_for(game); };  // checked again just before it writes
            o.made_by = "EMF.exe";
            o.emf_version = EMF_VERSION;
            o.info = saves::read_save_info(roots.info).slots;
            const saves::RestoreResult r = saves::restore_backup(roots, o);
            return [this, alive, r, id]() -> std::vector<std::string> {
                if (!*alive) return {};
                saves_working_ = false;
                return {Obj().str("type", "saves_restored").boolean("ok", r.ok).str("error", r.error).strs("restored", r.restored)
                            .str("before_id", r.before_id).boolean("steam", r.steam).str("id", id).text(),
                        saves_json()};
            };
        });
        return std::vector<std::string>{};
    }
    if (cmd == "saves_delete") {
        if (saves_working_) return with_list(result(cmd, false, "EMF is still busy with the saves. Wait a moment."));
        const std::string id = text_of(m, "id");
        if (saves::backup_folder(roots.store, id).empty())
            return with_list(result(cmd, false, "That backup is not there any more."));
        // the starter is the copy from before EMF: the page's dialog says so and sends starter: true
        const Value* starter = m.get("starter");
        const auto b = saves::read_backup(roots.store, id);
        if (b && b->kind == "starter" && !(starter && starter->is(VT::Bool) && starter->b))
            return with_list(result(cmd, false, "That is your backup from before EMF was installed. Delete it from the Saves "
                                                "tab's dialog, which asks first."));
        saves_working_ = true;
        auto alive = alive_;
        const std::string ref = ref_;
        host_.run_async([this, alive, roots, id, ref]() -> BridgeHost::Done {
            const auto o = saves::delete_backup(roots.store, id);
            return [this, alive, o, ref]() -> std::vector<std::string> {
                if (!*alive) return {};
                saves_working_ = false;
                ref_ = ref;
                return {result("saves_delete", o.ok, o.error, o.ok ? "Deleted the backup." : ""), saves_json()};
            };
        });
        return std::vector<std::string>{};
    }
    if (cmd == "saves_open") {
        const std::string id = text_of(m, "id");
        std::wstring target = roots.store;
        if (!id.empty()) {
            target = saves::backup_folder(roots.store, id);
            if (target.empty()) return std::vector<std::string>{result(cmd, false, "That backup is not there any more.")};
        } else {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(roots.store), ec);
        }
        const bool ok = host_.open(target, BridgeHost::Open::Folder);
        return std::vector<std::string>{result(cmd, ok, ok ? "" : "Explorer did not open.")};
    }
    if (cmd == "saves_auto") {
        const std::wstring game = host_.game();
        if (game.empty() || !folder_.emf_installed || folder_.server)
            return with_list(result(cmd, false, "Automatic backups are made by EMF in the game: install EMF into this folder first."));
        const Value* on = m.get("on");
        const Value* keep = m.get("keep");
        const std::wstring ini = core_ini(game);
        bool ok = true;
        if (on && on->is(VT::Bool)) ok &= WritePrivateProfileStringW(L"backup", L"on_start", on->b ? L"1" : L"0", ini.c_str()) != 0;
        if (keep && keep->is(VT::Number) && keep->n >= 1 && keep->n <= 100)
            ok &= WritePrivateProfileStringW(L"backup", L"keep", std::to_wstring(static_cast<int>(keep->n)).c_str(), ini.c_str()) != 0;
        return with_list(result(cmd, ok, ok ? "" : "EMF could not write " + utf8(ini) + ".",
                                ok ? "Saved. EMF uses it from the next game start." : ""));
    }
    return std::nullopt;
}

}  // namespace emf::app
