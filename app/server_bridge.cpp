#include "server_bridge.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>

#include "core/json.h"
#include "json_out.h"
#include "locate.h"
#include "server_process.h"
#include "text.h"

namespace emf::app {

namespace {

using core::json::Value;
using VT = Value::Type;

std::string text_of(const Value& v, const char* key) {
    const Value* x = v.get(key);
    return x && x->is(VT::String) ? x->s : std::string();
}

std::optional<bool> bool_of(const Value& v, const char* key) {
    const Value* x = v.get(key);
    if (x && x->is(VT::Bool)) return x->b;
    return std::nullopt;
}

std::wstring leaf(const std::wstring& dir) {
    std::wstring d = dir;
    while (d.size() > 3 && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    return d.substr(d.find_last_of(L"\\/") + 1);
}

// The same folder by its text alone (full paths, case ignored): no file is opened, so the window never waits for a
// slow drive here (same_place also follows junctions, on the worker).
bool text_same(const std::wstring& a, const std::wstring& b) { return !a.empty() && !b.empty() && path_inside(a, b) && path_inside(b, a); }

// The end of a log as raw bytes (its last 128 KB, from a line start), read while the server writes it.
std::string raw_tail(const std::wstring& path) {
    const size_t keep = 128 * 1024;
    std::string text;
    if (!read_file_tail(path, text, keep + 1)) return {};
    if (text.size() > keep) {
        const size_t nl = text.find('\n');
        text.erase(0, nl == std::string::npos ? text.size() - keep : nl + 1);
    }
    return text;
}

// A file's size and last write time, to tell whether it changed ("" when it is not there).
std::string file_key(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(lp(path).c_str(), GetFileExInfoStandard, &d)) return {};
    return std::to_string(d.nFileSizeHigh) + ":" + std::to_string(d.nFileSizeLow) + ":" + std::to_string(d.ftLastWriteTime.dwHighDateTime) +
           ":" + std::to_string(d.ftLastWriteTime.dwLowDateTime);
}

uint64_t account_of(std::string e) {
    if (e.size() > 2 && e[0] == '0' && (e[1] == 'x' || e[1] == 'X')) e = e.substr(2);
    return std::strtoull(e.c_str(), nullptr, 16);
}

std::string stop_msg(const char* state, const std::string& text) {
    return Obj().str("type", "server_stop").str("state", state).str("text", text).text();
}

std::string too_long(size_t accounts) {
    return "the admins line would be longer than the 2047 characters EMF's core reads (" + std::to_string(accounts) +
           " accounts). Take some out first.";
}

}  // namespace

// ---- the host's defaults ----

ServerStart BridgeHost::start_server(const std::wstring&, bool) {
    ServerStart s;
    s.error = "This EMF.exe cannot start servers.";
    return s;
}
bool BridgeHost::set_server_stop_on_close(const std::wstring&, bool) { return false; }
bool BridgeHost::server_started_here(unsigned long) { return false; }
StopOutcome BridgeHost::stop_server(unsigned long pid, const StopOptions& o) { return emf::app::stop_server(pid, o); }
bool BridgeHost::kill_server(unsigned long pid) { return emf::app::kill_server(pid); }
EmfPackage BridgeHost::emf_package() {
    EmfPackage p;
    p.error = "This EMF.exe has no EMF package with it. Install EMF into the server with install.cmd from the EMF zip.";
    return p;
}
ScriptRun BridgeHost::install_emf(const EmfPackage& package, const std::wstring& server) { return install_emf_into(package, server); }

// ---- the desk ----

ServerDesk::~ServerDesk() { *alive_ = false; }

bool ServerDesk::handles(const std::string& cmd) { return cmd.rfind("folder_", 0) == 0 || cmd.rfind("server_", 0) == 0; }

std::wstring ServerDesk::list_file() const {
    const std::wstring d = host_.app_data();
    return d.empty() ? std::wstring() : d + L"\\folders.json";
}

FolderList& ServerDesk::list() {
    if (!list_) {
        const std::wstring f = list_file();
        list_ = f.empty() ? FolderList{} : read_folder_list(f);
    }
    return *list_;
}

void ServerDesk::save_list() {
    const std::wstring f = list_file();
    if (!f.empty()) write_folder_list(f, list());
}

std::string ServerDesk::result(const std::string& cmd, bool ok, const std::string& error, const std::string& text) const {
    return Obj().str("type", "result").str("cmd", cmd).boolean("ok", ok).str("error", error).str("text", text).str("ref", ref_).text();
}

std::string ServerDesk::folder_label(const FolderEntry& f) const {
    if (f.kind == "game") return "Game";
    if (snap_)
        for (const auto& [path, label] : snap_->labels)
            if (text_same(path, f.path)) return label;
    return "Server: " + utf8(leaf(f.path));  // read with the next snapshot
}

void ServerDesk::remember_secrets(const std::vector<std::string>& secrets) {
    for (const auto& s : secrets)
        if (std::find(secrets_.begin(), secrets_.end(), s) == secrets_.end() && secrets_.size() < 256) secrets_.push_back(s);
}

void ServerDesk::age_snapshot(unsigned ms) {
    if (snap_) snap_->at = GetTickCount64() - ms;
}

ServerDesk::Snapshot ServerDesk::read_snapshot(const std::wstring& folder, bool server, const std::vector<FolderEntry>& entries,
                                               const std::vector<ProcessInfo>& processes, const Snapshot* before,
                                               std::vector<std::string>* secrets) {
    Snapshot s;
    s.folder = folder;
    s.server = server;
    for (const auto& f : entries) {
        if (folder_kind(f.path) != f.kind) s.missing.push_back(f.path);
        if (f.kind != "server") continue;
        const ServerConfig c = read_server_config(f.path);
        s.labels.emplace_back(f.path, "Server: " + (!c.name.empty() ? c.name : utf8(leaf(f.path))));
    }
    if (server) {
        s.cfg = read_server_config(folder, secrets);
        s.chat = read_chat_settings(folder);
        const std::wstring core_log = folder + L"\\emf\\emf_core.log";
        s.log_key = file_key(core_log);
        if (before && !before->log_key.empty() && before->log_key == s.log_key && text_same(before->folder, folder))
            s.refused = before->refused;  // the log did not change: no need to read 128 KB again
        else
            s.refused = refused_accounts(raw_tail(core_log));
        s.exe = exists(folder + L"\\enshrouded_server.exe");
        s.pid = server_pid(folder, processes);
    }
    s.at = GetTickCount64();
    return s;
}

void ServerDesk::refresh_snapshot(const std::wstring& folder, bool server) {
    if (snap_busy_) return;
    snap_busy_ = true;
    auto alive = alive_;
    const std::vector<FolderEntry> entries = list().folders;
    const std::vector<ProcessInfo> processes = host_.processes();
    std::optional<Snapshot> before = snap_;
    host_.run_async([this, alive, folder, server, entries, processes, before]() -> BridgeHost::Done {
        std::vector<std::string> secrets;
        Snapshot s = read_snapshot(folder, server, entries, processes, before ? &*before : nullptr, &secrets);
        return [this, alive, s = std::move(s), secrets]() mutable -> std::vector<std::string> {
            if (!*alive) return {};
            snap_busy_ = false;
            remember_secrets(secrets);
            // only for the folder still shown, and not over a snapshot made since (after a change here)
            if (snap_ && text_same(snap_->folder, s.folder) && snap_->server == s.server && snap_->at <= s.at) snap_ = std::move(s);
            return {};  // the next refresh sends the state when this changed it
        };
    });
}

std::string ServerDesk::server_log_text(const std::wstring& folder, const std::wstring& log) {
    std::vector<std::string> fresh;
    read_server_config(folder, &fresh);
    remember_secrets(fresh);
    return redact_log(raw_tail(log), secrets_);
}

std::string ServerDesk::state_fields(const FolderState& folder) {
    FolderList& l = list();
    const std::wstring shown = host_.game();
    if (folder.game_found && !shown.empty()) {  // the folder shown joins the list (shown at start, or picked with Change)
        bool changed = false;
        if (!folder.server) {
            if (l.folders.empty() || l.folders[0].kind != "game" || !text_same(l.folders[0].path, shown))
                changed = remember_folder(l, shown, "game");
        } else if (!find_folder(l, shown)) {
            changed = remember_folder(l, shown, "server");
        }
        if (changed) {
            save_list();
            snap_.reset();
        }
    }
    // the snapshot: read here only for a folder shown for the first time (or after a change made here)
    const bool first = !snap_ || !text_same(snap_->folder, shown) || snap_->server != (folder.game_found && folder.server);
    if (first) {
        std::vector<std::string> secrets;
        snap_ = read_snapshot(shown, folder.game_found && folder.server, l.folders, host_.processes(), nullptr, &secrets);
        remember_secrets(secrets);
    }
    const Snapshot& sn = *snap_;
    std::vector<std::string> folders;
    for (const auto& f : l.folders) {
        bool missing = false;
        for (const auto& m : sn.missing) missing = missing || text_same(m, f.path);
        folders.push_back(Obj().str("kind", f.kind).str("path", utf8(f.path)).str("label", folder_label(f))
                              .boolean("current", text_same(f.path, shown)).boolean("missing", missing).text());
    }
    std::string out = "," + json_quote("folders") + ":" + json_list(folders) + "," + json_quote("server_tab") + ":";
    if (!folder.game_found || !folder.server) {
        out += "null";
    } else {
        std::vector<std::string> refused;
        for (const auto& a : sn.refused) {
            bool admin = false;
            for (const auto& x : sn.chat.admins) admin = admin || (valid_account_hash(x) && account_of(x) == account_of(a.account));
            if (!admin) refused.push_back(Obj().str("account", a.account).str("time", a.time).str("command", a.command).text());
        }
        const unsigned long pid = sn.pid;
        const bool stop_here = !stop_folder_.empty() && text_same(stop_folder_, shown);
        if (stop_here && !stopping_ && pid != stop_pid_) stop_failed_ = false;  // it ended meanwhile (closed by hand)
        const FolderEntry* entry = find_folder(l, shown);
        const EmfPackage pkg = host_.emf_package();
        std::vector<std::string> mods;
        for (const auto& m : pkg.mods)
            mods.push_back(Obj().str("id", m.id).str("name", m.name).str("side", m.side).boolean("on", m.on).text());
        Obj o;
        o.raw("config", server_config_json(sn.cfg));
        o.raw("chat", Obj().boolean("players", sn.chat.players).boolean("everyone", sn.chat.everyone).strs("admins", sn.chat.admins).text());
        o.raw("refused", json_list(refused));
        o.raw("process", Obj().boolean("running", pid != 0).num("pid", pid).boolean("started_here", pid && host_.server_started_here(pid)).text());
        o.boolean("stopping", stopping_ && stop_here).boolean("stop_failed", stop_failed_ && stop_here && pid != 0 && pid == stop_pid_);
        o.str("stop_text", stop_here ? stop_text_ : std::string());
        o.boolean("stop_on_close", entry && entry->stop_on_close);
        o.boolean("can_start", !pid && !(stopping_ && stop_here) && !installing_ && sn.exe);
        o.raw("package", Obj().boolean("ok", pkg.ok).str("version", pkg.version).str("error", pkg.error).raw("mods", json_list(mods)).text());
        o.boolean("installing", installing_).boolean("emf_here", path_inside(shown, host_.exe_path()));
        o.str("log_path", utf8(server_log_path(shown, sn.cfg))).str("core_log_path", utf8(shown + L"\\emf\\emf_core.log"));
        out += o.text();
    }
    if (!first && GetTickCount64() - sn.at > 2000) refresh_snapshot(shown, sn.server);  // last: it may answer at once
    return out;
}

ServerDesk::Reply ServerDesk::handle(const std::string& cmd, const core::json::Value& m, const FolderState& folder,
                                     const std::string& ref) {
    ref_ = ref;
    Reply r;
    const auto say = [&](bool ok, const std::string& error, const std::string& text = {}) {
        r.messages.push_back(result(cmd, ok, error, text));
        return r;
    };
    FolderList& l = list();
    const std::wstring shown = host_.game();

    if (cmd == "folder_switch") {
        const std::wstring path = wide(text_of(m, "path"));
        const FolderEntry* e = find_folder(l, path);
        if (!e) return say(false, "That folder is not in EMF's list.");
        if (folder_kind(e->path) != e->kind)
            return say(false, utf8(e->path) + " has no " + (e->kind == "game" ? "enshrouded.exe" : "enshrouded_server.exe") +
                                  " any more.");
        r.switch_to = e->path;
        r.state = true;
        snap_.reset();
        return say(true, "", "Showing " + folder_label(*e) + ".");
    }
    if (cmd == "server_detect") {
        std::vector<std::string> found;
        for (const auto& dir : host_.steam_servers()) {
            const ServerConfig c = read_server_config(dir);
            found.push_back(Obj().str("path", utf8(dir)).str("name", !c.name.empty() ? c.name : utf8(leaf(dir)))
                                .boolean("added", find_folder(l, dir) != nullptr).text());
        }
        r.messages.push_back(Obj().str("type", "server_detect").raw("found", json_list(found)).text());
        return r;
    }
    if (cmd == "folder_add" || cmd == "folder_pick_server") {
        std::wstring path;
        if (cmd == "folder_add") {
            path = wide(text_of(m, "path"));
            bool steam = false;
            for (const auto& dir : host_.steam_servers()) steam = steam || same_folder(dir, path);
            if (!steam && !host_.test_mode())
                return say(false, "EMF adds a server folder Steam has, or one you choose: click Choose a folder.");
        } else {
            const auto picked = host_.pick_folder();
            if (!picked) return say(true, "");
            path = *picked;
        }
        const std::string kind = folder_kind(path);
        if (kind == "game")
            return say(false, utf8(path) + " is the game's folder (enshrouded.exe), not a dedicated server: a server's folder "
                                           "has enshrouded_server.exe. The game is the first entry of the list.");
        if (kind != "server")
            return say(false, "There is no enshrouded_server.exe in " + utf8(path) +
                                  ". Choose the dedicated server's folder (Steam: Enshrouded Dedicated Server; or your SteamCMD folder).");
        const bool added = remember_folder(l, path, "server");
        if (added) save_list();
        const FolderEntry* e = find_folder(l, path);
        r.switch_to = e ? e->path : path;
        r.state = true;
        snap_.reset();
        const ServerConfig c = read_server_config(path);
        return say(true, "", std::string(added ? "Added " : "Showing ") + "Server: " + (!c.name.empty() ? c.name : utf8(leaf(path))) + ".");
    }
    if (cmd == "folder_forget") {
        const std::wstring path = wide(text_of(m, "path"));
        const FolderEntry* e = find_folder(l, path);
        if (!e || e->kind != "server") return say(false, "Only a server folder can be taken off the list.");
        const std::string label = folder_label(*e);
        const bool current = same_folder(e->path, shown);
        if (current && (l.folders.empty() || l.folders[0].kind != "game"))  // it would join the list again at once
            return say(false, "This server is the folder shown and the list has no game to show instead: choose another "
                              "folder first.");
        const std::wstring forgotten = e->path;
        forget_folder(l, path);
        save_list();
        host_.set_server_stop_on_close(forgotten, false);  // off the list: EMF.exe no longer stops it at close
        if (current && !l.folders.empty() && l.folders[0].kind == "game") r.switch_to = l.folders[0].path;
        r.state = true;
        snap_.reset();
        return say(true, "", label + " is off EMF's list. Its folder and files are as they were.");
    }

    // ---- the Server tab: the folder shown must be a server's ----
    if (!folder.game_found || !folder.server)
        return say(false, "This is not a dedicated server's folder: switch to a server at the top first.");
    const std::wstring dir = folder.game;
    const unsigned long pid = server_pid(dir, host_.processes());

    if (cmd == "server_chat_save" || cmd == "server_admin_add") {
        ChatSettings s = read_chat_settings(dir);
        if (cmd == "server_chat_save") {
            const AdminList admins = parse_admin_lines(text_of(m, "admins"));
            if (!admins.bad.empty()) {
                std::string bad;
                for (size_t i = 0; i < admins.bad.size() && i < 5; ++i) bad += (i ? ", " : "") + admins.bad[i];
                return say(false, "Not saved: " + bad + (admins.bad.size() == 1 ? " is" : " are") +
                                      " not an account hash. An account hash is up to 16 hex digits, like 0x00ab00cd00ef0011 (a "
                                      "Steam64 id is not one): the server log's line \"chat command refused from account 0x...\" "
                                      "shows a player's.");
            }
            if (admins_value(admins.ok).size() > kAdminsMaxChars) return say(false, "Not saved: " + too_long(admins.ok.size()));
            s.admins = admins.ok;
            if (const auto e = bool_of(m, "everyone")) s.everyone = *e;
            if (const auto p = bool_of(m, "players")) s.players = *p;
        } else {
            const std::string account = text_of(m, "account");
            if (!valid_account_hash(account)) return say(false, "\"" + account + "\" is not an account hash.");
            std::vector<std::string> kept;  // entries the core skips (a Steam64 id) go: they never let anyone in
            for (const auto& a : s.admins)
                if (valid_account_hash(a)) {
                    if (account_of(a) == account_of(account)) return say(true, "", account + " is an admin already.");
                    kept.push_back(a);
                }
            kept.push_back(account);
            if (admins_value(kept).size() > kAdminsMaxChars) return say(false, "Not added: " + too_long(kept.size()));
            s.admins = kept;
        }
        const Change c = save_chat_settings(dir, s);
        r.state = true;
        snap_.reset();
        if (!c.ok) return say(false, c.error);
        return say(true, "", std::string(cmd == "server_admin_add" ? "Added. " : "") +
                                 "Saved in emf\\emf_core.ini. The server reads it at its next start" +
                                 (pid ? ": restart it to use the change." : "."));
    }
    if (cmd == "server_start") {
        if (pid) return say(false, "The server is running already (process " + std::to_string(pid) + ").");
        if (const std::string busy = host_.running_reason(); !busy.empty())  // a server whose folder Windows hides
            return say(false, "Not started: " + busy);
        if (stopping_ && same_folder(stop_folder_, dir)) return say(false, "EMF is still stopping the server. Wait a moment.");
        if (installing_) return say(false, "EMF is being installed into this server. Start it when that is done.");
        const FolderEntry* e = find_folder(l, dir);
        const ServerStart st = host_.start_server(dir, e && e->stop_on_close);
        r.state = true;
        snap_.reset();
        if (!st.ok) return say(false, st.error);
        stop_failed_ = false;
        stop_text_.clear();
        return say(true, "", "Started enshrouded_server.exe (process " + std::to_string(st.pid) +
                                 "). Its window is minimized in the taskbar; its log is below.");
    }
    if (cmd == "server_stop") {
        if (stopping_) return say(false, "EMF is stopping a server already. Wait for it, then try again.");
        if (!pid) return say(false, "The server is not running.");
        stopping_ = true;
        stop_failed_ = false;
        stop_text_.clear();
        stop_pid_ = pid;
        stop_folder_ = dir;
        StopOptions o;
        o.helper = host_.exe_path();
        o.core_log = dir + L"\\emf\\emf_core.log";
        o.server_log = server_log_path(dir, read_server_config(dir));
        o.shared_ok = bool_of(m, "shared_ok").value_or(false);
        auto alive = alive_;
        const auto poster = host_.ui_poster();
        o.progress = [poster, alive](const std::string& s) {
            poster([alive, s]() -> std::vector<std::string> {
                if (!*alive) return {};
                return {stop_msg("progress", s)};
            });
        };
        BridgeHost* host = &host_;
        host_.run_async([this, alive, host, pid, o]() -> BridgeHost::Done {
            const StopOutcome out = host->stop_server(pid, o);
            return [this, alive, out]() -> std::vector<std::string> {
                if (!*alive) return {};
                stopping_ = false;
                snap_.reset();
                std::vector<std::string> msgs;
                if (out.how == "shared_console") {  // nothing was sent: the page asks the user
                    stop_failed_ = false;
                    stop_text_.clear();
                    msgs.push_back(stop_msg("ask", out.text));
                } else {
                    stop_failed_ = !out.stopped;
                    stop_text_ = out.text;
                    msgs.push_back(stop_msg(out.stopped ? "done" : "failed", out.text));
                }
                if (fresh_state_)
                    if (auto s = fresh_state_()) msgs.push_back(*s);
                return msgs;
            };
        });
        r.state = true;
        return r;
    }
    if (cmd == "server_kill") {
        if (!pid) return say(false, "The server is not running.");
        if (!stop_failed_ || stopping_ || pid != stop_pid_ || !same_folder(stop_folder_, dir))
            return say(false, "Stop now is for a server that did not stop: click Stop first, which lets it save.");
        const bool ok = host_.kill_server(pid);
        if (ok) {
            stop_failed_ = false;
            stop_text_.clear();
        }
        r.state = true;
        snap_.reset();
        return say(ok, ok ? "" : "Windows did not end the server (process " + std::to_string(pid) + ").",
                   ok ? "The server was ended at once; what changed since its last autosave is lost." : "");
    }
    if (cmd == "server_on_close") {
        FolderEntry* e = find_folder(l, dir);
        if (!e) {
            remember_folder(l, dir, "server");
            e = find_folder(l, dir);
        }
        const bool stop = bool_of(m, "stop").value_or(false);
        if (e) e->stop_on_close = stop;
        save_list();
        host_.set_server_stop_on_close(dir, stop);
        r.state = true;
        const bool outside = pid && !host_.server_started_here(pid);
        return say(true, "", stop ? std::string("When EMF.exe closes, it stops this server first (cleanly, as Stop does)") +
                                        (outside ? ", from its next start here: this run was not started by this EMF.exe." : ".")
                                  : "When EMF.exe closes, this server keeps running.");
    }
    if (cmd == "server_logs") {
        if (logs_busy_) return r;  // the last read is still under way
        logs_busy_ = true;
        auto alive = alive_;
        const std::vector<std::string> known = secrets_;
        host_.run_async([this, alive, dir, known]() -> BridgeHost::Done {
            std::vector<std::string> fresh;  // only to take them out of the logs; never sent or written anywhere
            const ServerConfig cfg = read_server_config(dir, &fresh);
            std::vector<std::string> all = known;
            for (const auto& s : fresh)
                if (std::find(all.begin(), all.end(), s) == all.end()) all.push_back(s);
            const std::wstring server_log = server_log_path(dir, cfg), core_log = dir + L"\\emf\\emf_core.log";
            const std::string msg = Obj().str("type", "server_logs").str("server", redact_log(raw_tail(server_log), all))
                                        .str("core", redact_log(raw_tail(core_log), all)).str("server_path", utf8(server_log))
                                        .str("core_path", utf8(core_log)).text();
            return [this, alive, msg, fresh]() -> std::vector<std::string> {
                if (!*alive) return {};
                logs_busy_ = false;
                remember_secrets(fresh);
                return {msg};
            };
        });
        return r;
    }
    if (cmd == "server_install_emf") {
        const EmfPackage pkg = host_.emf_package();
        if (!pkg.ok) return say(false, pkg.error);
        if (installing_) return say(false, "EMF is installing already. Wait a moment.");
        if (bridge_busy_ && bridge_busy_())
            return say(false, "EMF is installing, uninstalling or unpacking a mod for this folder. Wait a moment, then try again.");
        const std::string running = host_.running_reason();
        if (pid || !running.empty())
            return say(false, "Stop the server first: EMF is installed while the server is closed" +
                                  (running.empty() ? std::string(".") : " (" + running + ")."));
        if (path_inside(dir, host_.exe_path()))
            return say(false, "This EMF.exe runs from the server's folder itself, so its installer cannot replace it. Use "
                              "install.cmd from the EMF zip, or EMF.exe from the game's folder.");
        installing_ = true;
        auto alive = alive_;
        BridgeHost* host = &host_;
        host_.run_async([this, alive, host, pkg, dir]() -> BridgeHost::Done {
            ScriptRun run;
            // the mods it brings that say they do nothing on a server start switched off there, unless they were set
            std::vector<std::string> fresh_client;
            for (const auto& mod : pkg.mods)
                if (mod.on && mod.side == "client" && core::valid_mod_id(mod.id) &&
                    !exists(dir + L"\\emf\\config\\" + wide(mod.id) + L".ini"))
                    fresh_client.push_back(mod.id);
            try {
                run = host->install_emf(pkg, dir);
            } catch (const std::exception& e) {
                run.error = std::string("EMF stopped with an error: ") + e.what();
            }
            if (run.ok)
                for (const auto& id : fresh_client) {
                    std::string name = id;
                    for (const auto& mod : pkg.mods)
                        if (mod.id == id) name = mod.name;
                    if (set_mod_enabled(dir, "emf", id, false).ok)
                        run.lines.push_back("Switched " + name + " off on this server: its mod.json says it does nothing on a "
                                            "server (switch it on in the Mods tab if you want it).");
                }
            return [this, alive, run]() -> std::vector<std::string> {
                if (!*alive) return {};
                installing_ = false;
                snap_.reset();
                std::vector<std::string> msgs{Obj().str("type", "server_install").boolean("ok", run.ok).str("error", run.error)
                                                  .strs("lines", run.lines).text()};
                if (fresh_state_)
                    if (auto s = fresh_state_()) msgs.push_back(*s);
                return msgs;
            };
        });
        r.state = true;
        return r;
    }
    return say(false, "The app does not know the command \"" + cmd + "\".");
}

}  // namespace emf::app
