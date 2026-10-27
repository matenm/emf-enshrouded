// The bridge's part of "Check for updates" (docs/app.md): the check runs on a worker thread and only when the
// player clicks (no check at start-up, no timer: Nexus's rules); a newer click cancels one still running, and its
// answer is dropped, and so is a check under way when the game folder changes or a mod is installed or uninstalled.
// After a failed check EMF waits a minute before it asks Nexus again. Also the Nexus links the player sets on a mod's
// details. Kept apart from bridge.cpp so the two change independently.
#include <algorithm>
#include <chrono>
#include <cmath>

#include "bridge.h"
#include "core/json.h"
#include "json_out.h"
#include "text.h"
#include "updates.h"

#ifndef EMF_VERSION
#define EMF_VERSION "dev"
#endif

namespace emf::app {

namespace {

using core::json::Value;
using VT = Value::Type;

constexpr std::chrono::seconds kCheckDeadline{60};  // the whole check: at most kUpdateRequestsMax requests and the changelogs

std::string field(const Value& v, const char* key) {
    const Value* x = v.get(key);
    return x && x->is(VT::String) ? x->s : std::string();
}

int seq_of(const Value& v) {
    const Value* x = v.get("seq");
    return x && x->is(VT::Number) && std::isfinite(x->n) && std::fabs(x->n) < 1e9 ? static_cast<int>(x->n) : 0;
}

bool blank(const std::string& s) { return s.find_first_not_of(" \t\r\n") == std::string::npos; }

}  // namespace

std::map<std::string, UpdateKey> Bridge::plan_update_keys() const {
    const std::wstring game = host_.game();
    if (game.empty() || !folder_.game_found) return {};
    const auto records = read_install_records(game);
    return update_keys(update_targets(folder_, records, read_nexus_links(game)), records);
}

std::vector<std::string> Bridge::updates_stop(const std::string& why) {
    if (!updates_running_) return {};
    updates_running_ = false;
    updates_.renew();  // the worker's answer is dropped (its id is old now), and its requests are cancelled
    if (why.empty()) return {};
    const std::string none = Obj().num("update", 0).num("maybe", 0).num("current", 0).num("unavailable", 0).num("gone", 0)
                                 .num("failed", 0).text();
    return {Obj().str("type", "updates").num("seq", updates_seq_).boolean("ok", false).str("error", why).num("retry_after", 0)
                .num("asked", 0).num("requests", 0).num("skipped", 0).raw("summary", none).raw("items", "[]").raw("emf", "null")
                .str("note", "").text()};
}

std::optional<std::vector<std::string>> Bridge::handle_updates(const std::string& cmd, const Value& m) {
    using Out = std::vector<std::string>;
    const std::wstring game = host_.game();

    if (cmd == "nexus_links")
        return Out{nexus_links_json(game, folder_.game_found ? read_nexus_links(game) : std::map<std::string, int64_t>{})};

    if (cmd == "nexus_link") {
        if (game.empty() || !folder_.game_found) return Out{result(cmd, false, "Choose the game folder first.")};
        const std::string source = field(m, "source"), id = field(m, "id"), link = field(m, "link");
        const std::string row = link_row(source, id);
        const bool here = std::any_of(folder_.mods.begin(), folder_.mods.end(),
                                      [&](const ModEntry& e) { return e.source == source && e.id == id; });
        if (row.empty() || !here) return Out{result(cmd, false, "EMF does not see that mod in this folder any more.")};
        int64_t mod_id = 0;
        if (!blank(link)) {
            const NexusLinkParse p = parse_nexus_mod_link(link);
            if (p.mod_id <= 0) return Out{result(cmd, false, p.error)};
            mod_id = p.mod_id;
        }
        const Change c = write_nexus_link(game, row, mod_id);
        const std::string done = mod_id ? "Linked to Nexus mod " + std::to_string(mod_id) + ". Check for updates now asks Nexus about it."
                                        : "The link to Nexus is removed.";
        return Out{result(cmd, c.ok, c.error, c.ok ? done : ""), nexus_links_json(game, read_nexus_links(game))};
    }

    if (cmd == "updates_check") {
        const int seq = seq_of(m);
        if (game.empty() || !folder_.game_found) return Out{result(cmd, false, "Choose the game folder first.")};
        if (const int64_t wait = updates_retry_at_ - host_.now_unix(); wait > 0)  // Nexus failed a moment ago
            return Out{result(cmd, false, "Nexus did not answer a moment ago. EMF asks it again in " + std::to_string(wait) + " s.")};
        const auto records = read_install_records(game);
        const std::vector<UpdateTarget> targets = update_targets(folder_, records, read_nexus_links(game));
        const auto keys = update_keys(targets, records);
        EmfSelf emf;
        emf.nexus_id = host_.emf_nexus_id();
        emf.installed = folder_.emf_installed && !folder_.emf_version.empty() ? folder_.emf_version : std::string(EMF_VERSION);
        std::vector<int64_t> ids;
        if (emf.nexus_id > 0) ids.push_back(emf.nexus_id);  // first: never past the cap
        for (const auto& t : targets) ids.push_back(t.nexus_id);
        updates_.renew();  // a check still running is cancelled, and its answer dropped
        updates_running_ = false;
        if (ids.empty()) return Out{updates_reply(seq, targets, {}, emf, keys)};
        updates_running_ = true;
        updates_seq_ = seq;
        const int id = updates_.id;
        auto alive = alive_;
        auto cancel = updates_.cancel;
        const nexus::Client client = client_;
        host_.run_async([this, alive, cancel, id, seq, targets, emf, ids, keys, client]() -> BridgeHost::Done {
            std::string reply;
            bool failed = false;
            if (!*cancel) {
                const auto started = std::chrono::steady_clock::now();
                auto r = client.updates(ids, cancel.get(), kCheckDeadline);
                // the changelogs of the files to take: one more request, only when there are some
                const auto uids = changelog_uids(targets, r.value);
                const auto left = kCheckDeadline - std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
                if (!uids.empty() && !*cancel && left > std::chrono::milliseconds(0)) {
                    const auto logs = client.changelogs(uids, cancel.get(), std::chrono::duration_cast<std::chrono::milliseconds>(left));
                    ++r.value.requests;
                    if (logs.ok()) attach_changelogs(r.value, logs.value);  // without them the answer still stands
                }
                failed = !r.ok() && r.error != "cancelled";
                if (!*cancel) reply = updates_reply(seq, targets, r, emf, keys);
            }
            return [this, alive, id, reply, failed]() -> std::vector<std::string> {
                if (!*alive || id != updates_.id || reply.empty()) return {};  // a newer check, or a change, replaced it
                updates_running_ = false;
                if (failed) updates_retry_at_ = host_.now_unix() + kUpdatesRetrySeconds;
                return {reply};
            };
        });
        return Out{};
    }
    return std::nullopt;
}

}  // namespace emf::app
