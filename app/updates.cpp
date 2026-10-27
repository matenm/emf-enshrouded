#include "updates.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "core/json.h"
#include "json_out.h"
#include "nexus/nxm.h"
#include "text.h"

namespace emf::app {

namespace {

using core::json::Value;
using VT = Value::Type;

constexpr size_t kMaxLinksBytes = 256 * 1024;
constexpr size_t kMaxRowIdBytes = 200;
constexpr int64_t kMaxModId = 999999999999LL;  // 12 digits, as Nexus ids are elsewhere in EMF

// A Nexus id written as 1 to 12 digits; 0 for anything else.
int64_t digits_id(const std::string& s) {
    if (s.empty() || s.size() > 12 || s.find_first_not_of("0123456789") != std::string::npos) return 0;
    return _atoi64(s.c_str());
}

std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    const size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(ws) - a + 1);
}

std::string lower_ascii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string mod_page(int64_t id) { return "https://www.nexusmods.com/enshrouded/mods/" + std::to_string(id); }

// A file for the page; the changelog only for the file to take (a choice's would only make the message big).
std::string file_json(int64_t mod_id, const nexus::NexusFile& f, bool with_changelog) {
    return Obj()
        .num("file_id", static_cast<double>(f.file_id)).str("name", f.name).str("version", f.version).str("category", f.category)
        .num("date", static_cast<double>(f.date)).num("size", static_cast<double>(f.size_bytes))
        .strs("changelog", with_changelog ? f.changelog : std::vector<std::string>{})
        .str("page", nexus::nexus_file_page(mod_id, f.file_id))
        .text();
}

}  // namespace

// ---- the links ----

std::wstring nexus_links_path(const std::wstring& game) { return game + L"\\emf\\config\\nexus_links.json"; }

std::string link_row(const std::string& source, const std::string& id) {
    static const char* const kSources[] = {"emf", "eml", "chain", "other"};
    if (std::none_of(std::begin(kSources), std::end(kSources), [&](const char* s) { return source == s; })) return {};
    if (id.empty() || id.size() > kMaxRowIdBytes) return {};
    for (unsigned char c : id)
        if (c < 0x20 || c == 0x7F) return {};
    return source + ":" + id;
}

namespace {
bool valid_row(const std::string& row) {
    const size_t colon = row.find(':');
    return colon != std::string::npos && link_row(row.substr(0, colon), row.substr(colon + 1)) == row;
}
}  // namespace

std::map<std::string, int64_t> read_nexus_links(const std::wstring& game) {
    std::map<std::string, int64_t> out;
    std::string text;
    if (game.empty() || !read_file(nexus_links_path(game), text, kMaxLinksBytes)) return out;
    const auto parsed = core::json::parse(text);
    const Value* links = parsed.value ? parsed.value->get("links") : nullptr;
    if (!links || !links->is(VT::Object)) return out;
    for (const auto& [row, v] : links->obj) {
        int64_t id = 0;
        if (v.is(VT::Number) && std::isfinite(v.n) && v.n >= 1 && v.n <= static_cast<double>(kMaxModId) && v.n == std::floor(v.n))
            id = static_cast<int64_t>(v.n);
        else if (v.is(VT::String))
            id = digits_id(v.s);
        if (id > 0 && valid_row(row)) out[row] = id;
    }
    return out;
}

Change write_nexus_link(const std::wstring& game, const std::string& row, int64_t mod_id) {
    if (!valid_row(row)) return {false, "EMF does not know that mod."};
    if (mod_id < 0 || mod_id > kMaxModId) return {false, "That is not a Nexus mod."};
    std::map<std::string, int64_t> links = read_nexus_links(game);
    if (mod_id == 0) links.erase(row);
    else links[row] = mod_id;
    std::string body = "{\"links\":{";
    bool first = true;
    for (const auto& [r, id] : links) {
        body += std::string(first ? "\n  " : ",\n  ") + json_quote(r) + ": " + std::to_string(id);
        first = false;
    }
    body += links.empty() ? "}}\n" : "\n}}\n";
    for (const wchar_t* dir : {L"\\emf", L"\\emf\\config"}) {
        const std::wstring d = game + dir;
        if (!is_dir(d) && !CreateDirectoryW(lp(d).c_str(), nullptr))
            return {false, "Could not make " + utf8(d) + ": " + windows_error_text(GetLastError())};
    }
    return write_file_whole(nexus_links_path(game), body, "emf\\config\\nexus_links.json");
}

NexusLinkParse parse_nexus_mod_link(const std::string& input) {
    NexusLinkParse r;
    const std::string text = trim(input);
    const std::string say = "Paste the mod's address on nexusmods.com (https://www.nexusmods.com/enshrouded/mods/...) or its number.";
    if (text.empty()) {
        r.error = say;
        return r;
    }
    if (text.find_first_not_of("0123456789") == std::string::npos) {
        r.mod_id = digits_id(text);
        if (r.mod_id <= 0) r.error = "That is not a Nexus mod number. " + say;
        return r;
    }
    std::string rest = text;
    const std::string low = lower_ascii(text);
    if (low.rfind("https://", 0) == 0) rest = text.substr(8);
    else if (low.rfind("http://", 0) == 0) rest = text.substr(7);
    const size_t slash = rest.find('/');
    const std::string host = lower_ascii(rest.substr(0, slash));
    if (slash == std::string::npos || (host != "www.nexusmods.com" && host != "nexusmods.com")) {
        r.error = "EMF can't read that as a Nexus mod. " + say;
        return r;
    }
    std::string path = rest.substr(slash + 1);
    path = path.substr(0, path.find_first_of("?#"));
    const size_t a = path.find('/');
    const std::string game = lower_ascii(path.substr(0, a));
    const std::string after = a == std::string::npos ? std::string() : path.substr(a + 1);
    if (game != nexus::kGameDomain && !game.empty() && after.rfind("mods/", 0) == 0) {
        r.error = "That is a mod for another game (" + game.substr(0, 60) + "). EMF manages Enshrouded mods.";
        return r;
    }
    std::string id = after.rfind("mods/", 0) == 0 ? after.substr(5) : std::string();
    if (!id.empty() && id.back() == '/') id.pop_back();
    r.mod_id = game == nexus::kGameDomain ? digits_id(id) : 0;
    if (r.mod_id <= 0) r.error = "EMF can't read that as a Nexus mod. " + say;
    return r;
}

// ---- what is checked ----

std::vector<UpdateTarget> update_targets(const FolderState& s, const std::vector<InstallRecord>& records,
                                         const std::map<std::string, int64_t>& links) {
    std::vector<UpdateTarget> out;
    std::set<std::string> covered;  // install keys a record with a Nexus id checks
    // The version the record's mods say now (the first that says one), and whether none of them says the version EMF
    // installed any more: the files changed by hand since.
    const auto changed_by_hand = [&](const InstallRecord& r, std::string& now) {
        bool still = false;
        now.clear();
        for (const auto& m : s.mods) {
            if (m.install_key != r.key || m.version.empty()) continue;
            if (now.empty()) now = m.version;
            const auto c = nexus::compare_versions(m.version, r.version);
            if (c ? *c == 0 : m.version == r.version) still = true;
        }
        return !r.version.empty() && !now.empty() && !still;
    };
    for (const auto& r : records) {
        const int64_t id = digits_id(r.nexus_id);
        if (id <= 0) continue;
        UpdateTarget t;
        t.nexus_id = id;
        t.via = "record";
        t.install_key = r.key;
        t.name = r.name;
        t.installed.version = r.version;
        t.installed.file_id = digits_id(r.nexus_file_id);
        t.installed.archive = r.archive;
        for (const auto& m : s.mods) {
            if (m.install_key != r.key) continue;
            if (const std::string row = link_row(m.source, m.id); !row.empty()) t.rows.push_back(row);
            if (t.name.empty() || t.name == r.key) t.name = m.name;
        }
        std::string now;  // what the mod's own files say now
        if (changed_by_hand(r, now)) {
            if (t.installed.file_id > 0 || nexus::archive_upload_time(r.archive) > 0)
                t.note = "Your files say " + now + ", not " + r.version + " as EMF installed them, so EMF compares versions "
                         "instead of the file it installed.";
            t.installed.file_id = 0;
            t.installed.archive_names_file = false;
            t.installed.version = now;
        } else if (r.version.empty() && !now.empty()) {
            t.installed.version = now;
        }
        t.installed.name = t.name;
        covered.insert(r.key);
        out.push_back(std::move(t));
    }
    for (const auto& m : s.mods) {
        const std::string row = link_row(m.source, m.id);
        if (row.empty() || (!m.install_key.empty() && covered.count(m.install_key))) continue;
        int64_t id = 0;
        std::string via;
        if (const auto it = links.find(row); it != links.end()) {
            id = it->second;
            via = "link";
        } else if ((id = digits_id(m.nexus_id)) > 0) {
            via = "mod.json";
        }
        if (id <= 0) continue;
        // two rows of one install (an EML folder and its DLL) linked to the same mod: one check
        const auto same = std::find_if(out.begin(), out.end(), [&](const UpdateTarget& t) {
            return !m.install_key.empty() && t.install_key == m.install_key && t.nexus_id == id && t.via == via;
        });
        if (same != out.end()) {
            same->rows.push_back(row);
            continue;
        }
        UpdateTarget t;
        t.nexus_id = id;
        t.via = via;
        t.install_key = m.install_key;
        t.rows.push_back(row);
        t.name = m.name;
        t.installed.version = m.version;
        t.installed.name = m.name;
        const auto rec = std::find_if(records.begin(), records.end(), [&](const InstallRecord& r) { return r.key == m.install_key; });
        if (rec != records.end()) {  // EMF installed it without knowing its Nexus mod: its archive's name still helps
            t.installed.archive = rec->archive;
            if (t.installed.version.empty()) t.installed.version = rec->version;
            std::string now;
            if (changed_by_hand(*rec, now)) t.installed.archive_names_file = false;
        }
        out.push_back(std::move(t));
    }
    for (auto& t : out) {  // a link on some rows of an install only
        if (t.via == "record" || t.install_key.empty()) continue;
        const auto rows = std::count_if(s.mods.begin(), s.mods.end(), [&](const ModEntry& m) {
            return m.install_key == t.install_key && !link_row(m.source, m.id).empty();
        });
        t.whole_install = static_cast<size_t>(rows) == t.rows.size();
    }
    return out;
}

std::map<std::string, UpdateKey> update_keys(const std::vector<UpdateTarget>& targets, const std::vector<InstallRecord>& records) {
    std::set<std::string> keys;
    for (const auto& r : records) keys.insert(r.key);
    std::map<std::string, UpdateKey> out;
    std::set<std::string> twice;
    for (const auto& t : targets) {
        if (t.install_key.empty() || !valid_install_key(t.install_key) || !t.whole_install) continue;
        const std::string id = std::to_string(t.nexus_id);
        if (t.install_key == "nexus-" + id || keys.count("nexus-" + id)) continue;  // the usual key does it
        const std::string why = t.via == "link" ? "you linked " + t.name + " to this Nexus mod"
                                : t.via == "record" ? "its install record names this Nexus mod"
                                                    : "the mod.json of " + t.name + " names this Nexus mod";
        const auto it = out.find(id);
        if (it != out.end() && it->second.key != t.install_key) twice.insert(id);
        else out[id] = UpdateKey{t.install_key, why};
    }
    for (const auto& id : twice) out.erase(id);  // tied to two installs: EMF does not choose
    return out;
}

std::vector<std::string> changelog_uids(const std::vector<UpdateTarget>& targets, const nexus::UpdatesCheck& check) {
    std::vector<std::string> out;
    for (const auto& t : targets) {
        const auto it = check.mods.find(t.nexus_id);
        if (it == check.mods.end()) continue;
        const auto d = nexus::decide_update(t.installed, it->second);
        if (!d.file || d.file->uid.empty() || (d.state != nexus::UpdateState::Update && d.state != nexus::UpdateState::Maybe)) continue;
        if (std::find(out.begin(), out.end(), d.file->uid) == out.end()) out.push_back(d.file->uid);
    }
    return out;
}

void attach_changelogs(nexus::UpdatesCheck& check, const std::map<std::string, std::vector<std::string>>& logs) {
    for (auto& [id, m] : check.mods)
        for (auto& f : m.files)
            if (const auto it = logs.find(f.uid); !f.uid.empty() && it != logs.end()) f.changelog = it->second;
}

// ---- the page's message ----

std::string updates_reply(int seq, const std::vector<UpdateTarget>& targets, const nexus::Result<nexus::UpdatesCheck>& check,
                          const EmfSelf& emf, const std::map<std::string, UpdateKey>& keys) {
    size_t n_update = 0, n_maybe = 0, n_current = 0, n_unavailable = 0, n_gone = 0, n_failed = 0;
    const size_t cap = nexus::kUpdatesPerRequest * nexus::kUpdateRequestsMax;
    std::set<int64_t> asked;
    std::vector<std::string> items;
    for (const auto& t : targets) {
        asked.insert(t.nexus_id);
        Obj o;
        o.num("nexus_id", static_cast<double>(t.nexus_id));
        const auto it = check.value.mods.find(t.nexus_id);
        const bool skipped = std::find(check.value.skipped.begin(), check.value.skipped.end(), t.nexus_id) != check.value.skipped.end();
        std::string installed_file = t.installed.archive;
        if (it == check.value.mods.end()) {
            ++n_failed;
            o.str("state", "failed")
                .str("reason", skipped ? "EMF asks Nexus about at most " + std::to_string(cap) +
                                             " mods per check, and this one was past them, so it was not checked."
                                       : "EMF could not ask Nexus: " + (check.error.empty() ? std::string("no answer") : check.error) + ".");
            o.raw("nexus", Obj().str("name", "").str("version", "").str("updated", "").str("status", "")
                                .str("url", mod_page(t.nexus_id)).str("files_url", mod_page(t.nexus_id) + "?tab=files").text());
            o.raw("file", "null").raw("choices", "[]");
        } else {
            const nexus::ModStatus& m = it->second;
            const nexus::UpdateDecision d = nexus::decide_update(t.installed, m);
            switch (d.state) {
                case nexus::UpdateState::Update: ++n_update; break;
                case nexus::UpdateState::Maybe: ++n_maybe; break;
                case nexus::UpdateState::Current: ++n_current; break;
                case nexus::UpdateState::Unavailable: ++n_unavailable; break;
                case nexus::UpdateState::Gone: ++n_gone; break;
                case nexus::UpdateState::Failed: ++n_failed; break;
            }
            if (d.installed_file) installed_file = d.installed_file->name;
            o.str("state", nexus::update_state_name(d.state)).str("reason", d.reason);
            o.raw("nexus", Obj().str("name", m.name).str("version", d.nexus_version).str("updated", m.updated_at).str("status", m.status)
                               .str("url", mod_page(t.nexus_id)).str("files_url", mod_page(t.nexus_id) + "?tab=files").text());
            o.raw("file", d.file ? file_json(t.nexus_id, *d.file, true) : "null");
            std::vector<std::string> choices;
            for (const auto& f : d.choices) choices.push_back(file_json(t.nexus_id, f, false));
            o.raw("choices", json_list(choices));
        }
        o.str("via", t.via).strs("rows", t.rows).str("install_key", t.install_key).str("name", t.name).str("note", t.note);
        // the install Update with EMF puts it in (update_keys, else nexus-<id>), and why when that is not this one
        const std::string id = std::to_string(t.nexus_id);
        const auto key = keys.find(id);
        const std::string installs_as = key != keys.end() ? key->second.key : "nexus-" + id;
        std::string separate;
        if (!t.install_key.empty() && installs_as != t.install_key)
            separate = !t.whole_install ? "your link covers only part of that install"
                       : key != keys.end() ? "this Nexus mod is tied to another install (" + installs_as + ")"
                                           : "this Nexus mod is tied to more than one install, or EMF already has an install of it";
        o.str("installs_as", installs_as).str("separate", separate);
        o.raw("installed", Obj().str("version", t.installed.version).num("file_id", static_cast<double>(t.installed.file_id))
                               .str("file_name", installed_file).text());
        items.push_back(o.text());
    }
    std::string emf_json = "null";
    if (emf.nexus_id > 0) {
        if (const auto it = check.value.mods.find(emf.nexus_id); it != check.value.mods.end() && it->second.found) {
            const auto cmp = nexus::compare_versions(emf.installed, it->second.version);
            emf_json = Obj()
                           .num("nexus_id", static_cast<double>(emf.nexus_id)).str("name", it->second.name)
                           .str("installed", emf.installed).str("nexus", it->second.version)
                           .boolean("newer", cmp && *cmp < 0).boolean("known", cmp.has_value()).str("url", mod_page(emf.nexus_id))
                           .text();
        }
    }
    std::string note;
    if (targets.empty())
        note = "No installed mod has a Nexus id EMF knows. On a mod's details, Link to a Nexus mod... ties it to its page.";
    else if (const size_t past = static_cast<size_t>(std::count_if(check.value.skipped.begin(), check.value.skipped.end(),
                                                                    [&](int64_t id) { return asked.count(id) > 0; }));
             past > 0)
        note = "EMF asked Nexus about " + std::to_string(asked.size() - past) + " of " + std::to_string(asked.size()) +
               " Nexus mods: a check asks about " + std::to_string(cap) + " at most.";
    const bool failed = !check.ok() && check.error != "cancelled";
    return Obj()
        .str("type", "updates").num("seq", seq).boolean("ok", check.ok()).str("error", check.error)
        .num("retry_after", failed ? kUpdatesRetrySeconds : 0)
        .num("asked", static_cast<double>(asked.size())).num("requests", static_cast<double>(check.value.requests))
        .num("skipped", static_cast<double>(check.value.skipped.size()))
        .raw("summary", Obj().num("update", static_cast<double>(n_update)).num("maybe", static_cast<double>(n_maybe))
                            .num("current", static_cast<double>(n_current)).num("unavailable", static_cast<double>(n_unavailable))
                            .num("gone", static_cast<double>(n_gone))
                            .num("failed", static_cast<double>(n_failed)).text())
        .raw("items", json_list(items)).raw("emf", emf_json).str("note", note)
        .text();
}

std::string nexus_links_json(const std::wstring& game, const std::map<std::string, int64_t>& links) {
    Obj l;
    for (const auto& [row, id] : links) l.num(row.c_str(), static_cast<double>(id));
    return Obj().str("type", "nexus_links").str("game", utf8(game)).raw("links", l.text()).text();
}

}  // namespace emf::app
