#include "nexus/updates.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <set>

#include "core/json.h"

namespace emf::nexus {

using core::json::Value;

namespace {

constexpr const char* kFileFields = "fileId uid name version category date size sizeInBytes";

std::string str(const Value* v) { return v && v->is(Value::Type::String) ? v->s : std::string(); }

std::optional<int64_t> to_i64(const Value* v) {
    if (!v) return std::nullopt;
    if (v->is(Value::Type::Number)) {
        if (!std::isfinite(v->n) || std::fabs(v->n) >= 9.0e18) return std::nullopt;
        return static_cast<int64_t>(v->n);
    }
    if (v->is(Value::Type::String) && !v->s.empty() && v->s.size() <= 18 &&
        v->s.find_first_not_of("0123456789") == std::string::npos)
        return std::stoll(v->s);
    return std::nullopt;
}

std::optional<uint64_t> to_u64(const Value* v) {
    const auto n = to_i64(v);
    return n && *n >= 0 ? std::optional<uint64_t>(static_cast<uint64_t>(*n)) : std::nullopt;
}

// A uid as Nexus sends it (a string of digits, sometimes a number); "" for anything else.
std::string uid_of(const Value* v) {
    if (!v) return {};
    if (v->is(Value::Type::String))
        return !v->s.empty() && v->s.size() <= 20 && v->s.find_first_not_of("0123456789") == std::string::npos ? v->s : "";
    if (const auto n = to_u64(v)) return std::to_string(*n);
    return {};
}

// At most `max` bytes, not ending inside a UTF-8 sequence.
std::string utf8_prefix(const std::string& s, size_t max) {
    if (s.size() <= max) return s;
    size_t cut = max;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut);
}

std::string http_problem(int status) {
    if (status == 403) return "Nexus refused the request (HTTP 403)";
    if (status == 429) return "Nexus says too many requests (HTTP 429); try again in a minute";
    if (status >= 500) return "Nexus is not available right now (HTTP " + std::to_string(status) + ")";
    return "Nexus answered HTTP " + std::to_string(status);
}

std::vector<std::string> changelog_lines(const Value* log) {
    std::vector<std::string> out;
    if (!log || !log->is(Value::Type::Array)) return out;
    for (const Value& line : log->arr) {
        if (out.size() == kChangelogLines) break;
        if (line.is(Value::Type::String) && !line.s.empty()) out.push_back(utf8_prefix(line.s, kChangelogLineBytes));
    }
    return out;
}

std::optional<NexusFile> file_from(const Value& n) {
    if (!n.is(Value::Type::Object)) return std::nullopt;
    const auto id = to_i64(n.get("fileId"));
    if (!id || *id <= 0) return std::nullopt;
    NexusFile f;
    f.file_id = *id;
    f.uid = uid_of(n.get("uid"));
    f.name = str(n.get("name"));
    f.version = str(n.get("version"));
    f.category = str(n.get("category"));
    if (const auto bytes = to_u64(n.get("sizeInBytes"))) f.size_bytes = *bytes;
    else if (const auto kib = to_u64(n.get("size")); kib && *kib < (uint64_t{1} << 53)) f.size_bytes = *kib * 1024;
    const auto date = to_i64(n.get("date"));
    f.date = date && *date > 0 ? *date : 0;
    f.changelog = changelog_lines(n.get("changelogText"));
    return f;
}

// The reply's document and its data, or the error: HTTP status, not JSON, errors without data.
template <class T>
const Value* reply_data(std::string_view body, int status, std::optional<Value>& doc, Result<T>& r) {
    const bool http_ok = status >= 200 && status < 300;
    auto parsed = core::json::parse(body, core::json::ParseOptions{true});  // Nexus's text may hold a lone surrogate
    if (!parsed.value) {
        r.error = http_ok ? "the reply from Nexus is not JSON (" + parsed.error + ")" : http_problem(status);
        return nullptr;
    }
    doc = std::move(parsed.value);
    const Value* data = doc->get("data");
    if (data && !data->is(Value::Type::Object)) data = nullptr;
    const Value* errors = doc->get("errors");
    const bool has_errors = errors && errors->is(Value::Type::Array) && !errors->arr.empty();
    if (!http_ok || (has_errors && !data)) {
        std::string text;
        if (has_errors) {
            size_t shown = 0;
            for (const Value& e : errors->arr) {
                const std::string m = str(e.get("message"));
                if (m.empty()) continue;
                if (shown++ == 3) {
                    text += "; ...";
                    break;
                }
                text += (text.empty() ? "" : "; ") + utf8_prefix(m, 300);
            }
        }
        if (!text.empty()) r.error = "Nexus: " + text + (http_ok ? "" : " (HTTP " + std::to_string(status) + ")");
        else r.error = http_ok ? "Nexus: unknown error" : http_problem(status);
        return nullptr;
    }
    if (!data) r.error = "the reply from Nexus has no data";
    return data;
}

// ---- versions ----

struct Version {
    std::vector<uint64_t> parts;
    std::vector<std::string> pre;  // a pre-release: its word's rank, then its numbers ("beta.2" -> "2", "2"); empty for a release
};

// The rank of a known pre-release word, in the order they come before a release; nullopt for any other word.
std::optional<int> pre_rank(std::string word) {
    for (char& c : word) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (word == "dev") return 0;
    if (word == "alpha") return 1;
    if (word == "beta") return 2;
    if (word == "pre" || word == "preview") return 3;
    if (word == "rc") return 4;
    return std::nullopt;
}

bool digits_only(std::string_view s) { return !s.empty() && s.size() <= 9 && s.find_first_not_of("0123456789") == std::string_view::npos; }

std::optional<Version> parse_version(std::string_view s) {
    const char* ws = " \t\r\n";
    const size_t a = s.find_first_not_of(ws);
    if (a == std::string_view::npos) return std::nullopt;
    s = s.substr(a, s.find_last_not_of(ws) - a + 1);
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.remove_prefix(1);
    const size_t plus = s.find('+');
    if (plus != std::string_view::npos) s = s.substr(0, plus);  // build metadata: ignored
    std::string_view pre;
    const size_t dash = s.find('-');
    if (dash != std::string_view::npos) {
        pre = s.substr(dash + 1);
        s = s.substr(0, dash);
        if (pre.empty()) return std::nullopt;
    }
    Version v;
    size_t i = 0;
    while (true) {
        size_t j = i;
        while (j < s.size() && s[j] >= '0' && s[j] <= '9') ++j;
        if (j == i || j - i > 9) return std::nullopt;  // no digits, or too many for a version part
        v.parts.push_back(std::stoull(std::string(s.substr(i, j - i))));
        if (j == s.size()) break;
        if (s[j] != '.' || v.parts.size() == 8) return std::nullopt;
        i = j + 1;
    }
    if (pre.empty()) return v;
    // the first identifier is a known word, maybe with its number ("beta", "beta2", "RC1"); then only numbers
    size_t k = pre.find('.');
    std::string_view first = pre.substr(0, k);
    size_t letters = 0;
    while (letters < first.size() && std::isalpha(static_cast<unsigned char>(first[letters]))) ++letters;
    const auto rank = pre_rank(std::string(first.substr(0, letters)));
    if (!rank) return std::nullopt;  // "1.0-hotfix", "1.2-3": not a pre-release EMF can order
    v.pre.push_back(std::to_string(*rank));
    if (letters < first.size()) {
        if (!digits_only(first.substr(letters))) return std::nullopt;
        v.pre.emplace_back(first.substr(letters));
    }
    while (k != std::string_view::npos) {
        const size_t e = pre.find('.', k + 1);
        const std::string_view id = pre.substr(k + 1, e == std::string_view::npos ? std::string_view::npos : e - k - 1);
        if (!digits_only(id)) return std::nullopt;
        v.pre.emplace_back(id);
        k = e;
    }
    return v;
}

// Numbers compared as numbers; a release (no identifiers) after its pre-releases.
int compare_pre(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    if (a.empty() || b.empty()) return a.empty() == b.empty() ? 0 : (a.empty() ? 1 : -1);
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
        const uint64_t x = std::stoull(a[i]), y = std::stoull(b[i]);
        if (x != y) return x < y ? -1 : 1;
    }
    return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
}

// ---- names ----

std::string strip_archive_extension(std::string s) {
    for (const char* ext : {".zip", ".7z", ".rar"}) {
        const size_t n = std::char_traits<char>::length(ext);
        if (s.size() > n) {
            std::string tail = s.substr(s.size() - n);
            for (char& c : tail) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (tail == ext) {
                s.resize(s.size() - n);
                return s;
            }
        }
    }
    return s;
}

std::vector<std::string> name_words(std::string_view name) {
    const std::string s = strip_archive_extension(std::string(name));
    const auto lower = [](char c) { return c >= 'a' && c <= 'z'; };
    const auto upper = [](char c) { return c >= 'A' && c <= 'Z'; };
    const auto alnum = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (static_cast<unsigned char>(c) >= 0x80); };
    std::vector<std::string> words;
    std::string cur;
    const auto flush = [&] {
        if (cur.empty()) return;
        bool number = cur.find_first_not_of("0123456789") == std::string::npos;
        if (!number && cur.size() > 1 && cur[0] == 'v' && cur.find_first_not_of("0123456789", 1) == std::string::npos) number = true;
        if (!number) words.push_back(cur);
        cur.clear();
    };
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (!alnum(c)) {
            flush();
            continue;
        }
        // camelCase and ACRONYMWord boundaries: "AutoLootPlus" -> auto loot plus, "DLLInstaller" -> dll installer
        if (i > 0 && upper(c) && (lower(s[i - 1]) || (upper(s[i - 1]) && i + 1 < s.size() && lower(s[i + 1])))) flush();
        cur += upper(c) ? static_cast<char>(c - 'A' + 'a') : c;
    }
    flush();
    return words;
}

std::set<std::string> word_set(std::string_view name) {
    const auto w = name_words(name);
    return {w.begin(), w.end()};
}

// ---- the rules ----

bool retired(const std::string& c) { return c == "OLD_VERSION" || c == "ARCHIVED" || c == "REMOVED"; }
bool newer(const NexusFile& a, const NexusFile& b) { return a.date != b.date ? a.date > b.date : a.file_id > b.file_id; }

std::vector<NexusFile> newest_first(std::vector<NexusFile> v) {
    std::stable_sort(v.begin(), v.end(), [](const NexusFile& a, const NexusFile& b) { return newer(a, b); });
    return v;
}

std::string file_text(const NexusFile& f) {
    return f.name.empty() ? "file " + std::to_string(f.file_id) : f.name + (f.version.empty() || f.name.find(f.version) != std::string::npos ? "" : " (" + f.version + ")");
}

std::string names_text(const std::vector<NexusFile>& files) {
    std::string s;
    for (size_t i = 0; i < files.size() && i < 4; ++i) s += (i ? ", " : "") + file_text(files[i]);
    if (files.size() > 4) s += ", ...";
    return s;
}

constexpr size_t kMaxChoices = 6;

// The main files to take (the optional ones when a mod has no main file), newest first.
std::vector<NexusFile> main_pool(const ModStatus& m) {
    std::vector<NexusFile> mains, optional;
    for (const auto& f : m.files) {
        if (f.category == "MAIN") mains.push_back(f);
        else if (f.category == "OPTIONAL") optional.push_back(f);
    }
    return newest_first(mains.empty() ? optional : mains);
}

// The uploads of one file line (the same words), newest first: the newest, unless an older upload has a higher version
// (a legacy build uploaded last); then all of them, for the player to choose.
std::vector<NexusFile> line_pick(const std::vector<NexusFile>& uploads) {
    if (uploads.size() <= 1) return uploads;
    for (size_t i = 1; i < uploads.size(); ++i) {
        const auto c = compare_versions(uploads[0].version, uploads[i].version);
        if (c && *c < 0) return std::vector<NexusFile>(uploads.begin(), uploads.begin() + static_cast<std::ptrdiff_t>(std::min(uploads.size(), kMaxChoices)));
    }
    return {uploads[0]};
}

// Each file line once (line_pick of its uploads), lines in the order of their newest upload; at most kMaxChoices.
std::vector<NexusFile> distinct_lines(const std::vector<NexusFile>& files) {
    std::vector<std::set<std::string>> keys;
    std::vector<std::vector<NexusFile>> lines;
    for (const auto& f : files) {
        const auto w = word_set(f.name);
        const auto it = std::find(keys.begin(), keys.end(), w);
        if (it == keys.end()) {
            keys.push_back(w);
            lines.push_back({f});
        } else {
            lines[static_cast<size_t>(it - keys.begin())].push_back(f);
        }
    }
    std::vector<NexusFile> out;
    for (const auto& line : lines)
        for (const auto& f : line_pick(line))
            if (out.size() < kMaxChoices) out.push_back(f);
    return out;
}

// Names the file to take from `pool` (newest first): the only one; or the one whose own words (those not every file in
// the pool has) all appear in one of `file_names` and that shares a word with it, the one with the most such words when
// several do; or, from the mod's own name, only a file with at least one own word in it (the plain variant has none, and
// a variant's mod.json often carries the plain name). Otherwise the player chooses. So "AutoLootPlus 1.1.7" takes
// "AutoLootPlus 1.1.8" over "AutoLootPlus Lite 1.1.8", "ShroudHud 1.6 - For ModLoader" takes "ShroudHud 1.7 - ModLoader",
// and the mod's name "Better Map" settles nothing between "Better Map" and "Better Map Compact".
void pick(UpdateDecision& d, const std::vector<NexusFile>& pool, const std::vector<std::string>& file_names,
          const std::string& mod_name) {
    if (pool.empty()) return;
    if (pool.size() == 1) {
        d.file = pool[0];
        return;
    }
    std::vector<std::set<std::string>> words;
    for (const auto& f : pool) words.push_back(word_set(f.name));
    std::set<std::string> shared = words[0];
    for (const auto& w : words) {
        std::set<std::string> keep;
        for (const auto& x : shared)
            if (w.count(x)) keep.insert(x);
        shared = keep;
    }
    std::vector<std::pair<std::string, bool>> names;  // a name, and whether a fit needs an own word (the mod's name)
    for (const auto& n : file_names) names.emplace_back(n, false);
    names.emplace_back(mod_name, true);
    for (const auto& [name, need_own] : names) {
        const std::set<std::string> mine = word_set(name);
        if (mine.empty()) continue;
        long best = -1;
        std::vector<size_t> top;
        for (size_t i = 0; i < pool.size(); ++i) {
            size_t own = 0, common = 0;
            bool fits = true;
            for (const auto& x : words[i]) {
                if (mine.count(x)) ++common;
                if (shared.count(x)) continue;
                if (!mine.count(x)) fits = false;
                ++own;
            }
            if (!fits || common == 0 || (need_own && own == 0)) continue;
            if (static_cast<long>(own) > best) {
                best = static_cast<long>(own);
                top.clear();
            }
            if (static_cast<long>(own) == best) top.push_back(i);
        }
        if (top.empty()) continue;
        if (!std::all_of(top.begin(), top.end(), [&](size_t i) { return words[i] == words[top[0]]; })) continue;
        std::vector<NexusFile> line;  // one file line: its uploads, newest first
        for (size_t i : top) line.push_back(pool[i]);
        const auto taken = line_pick(line);
        if (taken.size() == 1) d.file = taken[0];
        else d.choices = taken;
        return;
    }
    d.choices = distinct_lines(pool);
    if (d.choices.size() == 1) {  // every main file is an upload of one file: the newest
        d.file = d.choices[0];
        d.choices.clear();
    }
}

std::string category_words(const std::string& c) {
    if (c == "OLD_VERSION") return "an old version";
    if (c == "ARCHIVED") return "archived";
    if (c == "REMOVED") return "removed";
    return c;
}

// The highest version that parses among the page's and its main files'; "" when none does.
std::string best_version(const ModStatus& m, const std::vector<NexusFile>& pool) {
    std::string best;
    const auto consider = [&](const std::string& v) {
        if (!compare_versions(v, v)) return;  // does not parse
        if (best.empty() || *compare_versions(v, best) > 0) best = v;
    };
    consider(m.version);
    for (const auto& f : pool) consider(f.version);
    return best;
}

}  // namespace

std::vector<int64_t> updates_request_ids(const std::vector<int64_t>& mod_ids) {
    std::vector<int64_t> out;
    for (int64_t id : mod_ids) {
        if (out.size() == kUpdatesPerRequest) break;
        if (id > 0 && std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    }
    return out;
}

std::string updates_request(const std::vector<int64_t>& mod_ids) {
    const std::vector<int64_t> ids = updates_request_ids(mod_ids);
    std::string query = "query EmfUpdates($ids: [CompositeDomainWithIdInput!]!, $count: Int, $gameId: ID!";
    for (int64_t id : ids) query += ", $f" + std::to_string(id) + ": ID!";
    query += ") { mods: legacyModsByDomain(ids: $ids, count: $count) { nodes { modId name version updatedAt status } }";
    for (int64_t id : ids) {
        const std::string f = "f" + std::to_string(id);
        query += " " + f + ": modFiles(modId: $" + f + ", gameId: $gameId) { " + kFileFields + " }";
    }
    query += " }";
    std::string vars = "{\"ids\":[";
    for (size_t i = 0; i < ids.size(); ++i)
        vars += std::string(i ? "," : "") + "{\"gameDomain\":" + json_quote(kGameDomain) + ",\"modId\":" + std::to_string(ids[i]) + "}";
    vars += "],\"count\":" + std::to_string(ids.size()) + ",\"gameId\":" + std::to_string(kGameId);
    for (int64_t id : ids) vars += ",\"f" + std::to_string(id) + "\":" + std::to_string(id);
    vars += "}";
    return "{\"query\":" + json_quote(query) + ",\"variables\":" + vars + "}";
}

Result<std::map<int64_t, ModStatus>> parse_updates(std::string_view body, const std::vector<int64_t>& asked, int http_status) {
    Result<std::map<int64_t, ModStatus>> r;
    std::optional<Value> doc;
    const Value* data = reply_data(body, http_status, doc, r);
    if (!data) return r;
    const Value* mods = data->get("mods");
    const Value* nodes = mods ? mods->get("nodes") : nullptr;
    if (!nodes || !nodes->is(Value::Type::Array)) {  // files alone would make every mod look gone from Nexus
        r.error = "the reply from Nexus has no mods";
        return r;
    }
    for (int64_t id : asked) {
        ModStatus m;
        m.mod_id = id;
        r.value[id] = m;
    }
    for (const Value& n : nodes->arr) {
        const auto id = to_i64(n.get("modId"));
        if (!id) continue;
        const auto it = r.value.find(*id);
        if (it == r.value.end()) continue;
        ModStatus& m = it->second;
        m.found = true;
        m.name = str(n.get("name"));
        m.version = str(n.get("version"));
        m.updated_at = str(n.get("updatedAt"));
        m.status = str(n.get("status"));
    }
    // errors next to the data: the alias each one names ("f170") marks that mod's files as failed
    if (const Value* errors = doc->get("errors"); errors && errors->is(Value::Type::Array))
        for (const Value& e : errors->arr) {
            const Value* path = e.get("path");
            const std::string alias = path && path->is(Value::Type::Array) && !path->arr.empty() ? str(&path->arr[0]) : "";
            if (alias.size() < 2 || alias[0] != 'f') continue;
            const std::string digits = alias.substr(1);
            if (digits.size() > 12 || digits.find_first_not_of("0123456789") != std::string::npos) continue;
            if (const auto it = r.value.find(std::stoll(digits)); it != r.value.end()) it->second.files_failed = true;
        }
    for (auto& [id, m] : r.value) {
        const Value* files = data->get("f" + std::to_string(id));
        if (!files || !files->is(Value::Type::Array)) continue;
        for (const Value& n : files->arr)
            if (auto f = file_from(n)) m.files.push_back(std::move(*f));
    }
    return r;
}

std::vector<std::string> changelogs_request_uids(const std::vector<std::string>& uids) {
    std::vector<std::string> out;
    for (const auto& u : uids) {
        if (out.size() == kChangelogsPerRequest) break;
        if (u.empty() || u.size() > 20 || u.find_first_not_of("0123456789") != std::string::npos) continue;
        if (std::find(out.begin(), out.end(), u) == out.end()) out.push_back(u);
    }
    return out;
}

std::string changelogs_request(const std::vector<std::string>& uids) {
    const auto asked = changelogs_request_uids(uids);
    std::string list;
    for (size_t i = 0; i < asked.size(); ++i) list += std::string(i ? "," : "") + json_quote(asked[i]);
    return "{\"query\":" +
           json_quote("query EmfChangelogs($uids: [ID!]!, $count: Int) { files: modFilesByUid(uids: $uids, count: $count) { "
                      "nodes { uid fileId modId changelogText } } }") +
           ",\"variables\":{\"uids\":[" + list + "],\"count\":" + std::to_string(asked.size()) + "}}";
}

Result<std::map<std::string, std::vector<std::string>>> parse_changelogs(std::string_view body, int http_status) {
    Result<std::map<std::string, std::vector<std::string>>> r;
    std::optional<Value> doc;
    const Value* data = reply_data(body, http_status, doc, r);
    if (!data) return r;
    const Value* files = data->get("files");
    const Value* nodes = files ? files->get("nodes") : nullptr;
    if (!nodes || !nodes->is(Value::Type::Array)) {
        r.error = "the reply from Nexus has no files";
        return r;
    }
    for (const Value& n : nodes->arr) {
        const std::string uid = uid_of(n.get("uid"));
        if (!uid.empty()) r.value[uid] = changelog_lines(n.get("changelogText"));
    }
    return r;
}

std::optional<int> compare_versions(std::string_view a, std::string_view b) {
    const auto va = parse_version(a), vb = parse_version(b);
    if (!va || !vb) return std::nullopt;
    const size_t n = std::max(va->parts.size(), vb->parts.size());
    for (size_t i = 0; i < n; ++i) {
        const uint64_t x = i < va->parts.size() ? va->parts[i] : 0, y = i < vb->parts.size() ? vb->parts[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return compare_pre(va->pre, vb->pre);
}

int64_t archive_upload_time(std::string_view archive_name) {
    const std::string full(archive_name);
    const std::string s = strip_archive_extension(full);
    if (s.size() == full.size()) return 0;  // not a .zip, .7z or .rar
    const size_t last = s.rfind('-');
    if (last == std::string::npos) return 0;
    const std::string time = s.substr(last + 1);
    if (time.size() < 9 || time.size() > 11 || time.find_first_not_of("0123456789") != std::string::npos) return 0;
    const size_t before = s.rfind('-', last - 1);  // the number before it: a version part or the mod id
    if (before == std::string::npos || last == 0) return 0;
    const std::string prev = s.substr(before + 1, last - before - 1);
    if (prev.empty() || prev.find_first_not_of("0123456789") != std::string::npos) return 0;
    return std::stoll(time);
}

std::string file_name_key(std::string_view name) {
    std::string out;
    for (const auto& w : name_words(name)) out += (out.empty() ? "" : " ") + w;
    return out;
}

const char* update_state_name(UpdateState s) {
    switch (s) {
        case UpdateState::Update: return "update";
        case UpdateState::Current: return "current";
        case UpdateState::Unavailable: return "unavailable";
        case UpdateState::Gone: return "gone";
        case UpdateState::Failed: return "failed";
        case UpdateState::Maybe: break;
    }
    return "maybe";
}

UpdateDecision decide_update(const Installed& in, const ModStatus& m) {
    UpdateDecision d;
    d.nexus_version = m.version;
    // ---- what Nexus could not say, and mods not on Nexus ----
    if (m.files_failed || (!m.found && !m.files.empty())) {
        d.state = UpdateState::Failed;
        d.reason = "Nexus's answer about this mod was incomplete, so EMF decided nothing. Check again later.";
        return d;
    }
    if (!m.found || m.status == "removed" || m.status == "wastebinned") {
        d.state = UpdateState::Gone;
        d.reason = m.found ? "This mod was removed from Nexus." : "Nexus has no mod with this id: it was deleted, or the id is wrong.";
        return d;
    }
    if (m.status == "hidden" || m.status == "under_moderation" || m.status == "not_published") {
        d.state = UpdateState::Unavailable;
        d.reason = m.status == "hidden" ? "This mod is hidden on Nexus (by its author or the moderators), so it can't be downloaded now."
                   : m.status == "under_moderation" ? "This mod is under moderation on Nexus, so it can't be downloaded now."
                                                    : "This mod is not published on Nexus, so it can't be downloaded now.";
        return d;
    }
    const std::vector<NexusFile> pool = main_pool(m);
    const std::string best = best_version(m, pool);
    if (!best.empty()) d.nexus_version = best;

    // ---- the installed file: the file id EMF downloaded, or the upload time in a Nexus archive's name ----
    const NexusFile* mine = nullptr;
    if (in.file_id > 0) {
        for (const auto& f : m.files)
            if (f.file_id == in.file_id) mine = &f;
    } else if (in.archive_names_file) {
        if (const int64_t t = archive_upload_time(in.archive); t > 0) {
            size_t n = 0;
            for (const auto& f : m.files)
                if (f.date == t) mine = &f, ++n;
            if (n != 1) mine = nullptr;  // only a single file of that time names it
        }
    }

    if (mine) {
        d.installed_file = *mine;
        const std::string key = file_name_key(mine->name);
        std::vector<NexusFile> same_up, same_eq, others;
        for (const auto& f : m.files) {
            if (f.file_id == mine->file_id || retired(f.category) || !newer(f, *mine)) continue;
            const auto c = compare_versions(f.version, mine->version);  // the newer file against the installed one
            if (!key.empty() && file_name_key(f.name) == key) {
                if (c ? *c > 0 : !f.version.empty() && f.version != mine->version) same_up.push_back(f);
                else if (!(c && *c < 0)) same_eq.push_back(f);  // an older build uploaded later is no update
                continue;
            }
            if (c && *c < 0) continue;  // an older version under another name
            const bool relevant = f.category == "MAIN" || f.category == "UPDATE" || (f.category == "OPTIONAL" && mine->category == "OPTIONAL");
            if (!relevant) continue;
            if (f.date <= mine->date + kSiblingSeconds && c && *c == 0) continue;  // a variant of the same release
            others.push_back(f);
        }
        same_up = newest_first(same_up);
        same_eq = newest_first(same_eq);
        others = newest_first(others);
        if (!retired(mine->category)) {
            if (!same_up.empty()) {
                d.state = UpdateState::Update;
                d.file = same_up[0];
                d.reason = "Nexus has a newer upload of your file: " + file_text(same_up[0]) + ".";
            } else if (!same_eq.empty()) {
                same_eq.resize(std::min(same_eq.size(), kMaxChoices));
                d.choices = same_eq;
                d.reason = "Nexus has a newer file named like yours but without a newer version (" + names_text(same_eq) +
                           "). EMF can't tell whether it replaces yours.";
            } else if (!others.empty()) {
                d.choices = distinct_lines(others);
                d.reason = "You have a current file (" + file_text(*mine) + "), but Nexus has newer files with other names (" +
                           names_text(d.choices) + "). EMF can't tell whether one replaces yours.";
            } else {
                d.state = UpdateState::Current;
                d.reason = "You have Nexus's current file (" + file_text(*mine) + ").";
            }
            return d;
        }
        d.state = UpdateState::Update;
        d.reason = "Your file " + file_text(*mine) + " is " + category_words(mine->category) + " on Nexus.";
        if (!same_up.empty()) d.file = same_up[0];
        else if (!same_eq.empty()) d.file = same_eq[0];  // a re-upload of the same version
        else pick(d, pool, {mine->name, in.archive}, in.name);
    } else if (in.file_id > 0) {
        d.state = UpdateState::Update;
        d.reason = "The file you have (" + std::to_string(in.file_id) + ") is no longer on Nexus.";
        pick(d, pool, {in.archive}, in.name);
    }

    if (mine || in.file_id > 0) {
        // the file to take must not be older than the one retired: a rollback, or an archived beta, is no update
        const std::string ref_version = mine ? mine->version : in.version;
        const int64_t ref_date = mine ? mine->date : archive_upload_time(in.archive);
        const auto older = [&](const NexusFile& f) {
            const auto c = compare_versions(f.version, ref_version);
            return (c && *c < 0) || (ref_date > 0 && f.date > 0 && f.date < ref_date);
        };
        if (d.file && older(*d.file)) {
            d.state = UpdateState::Maybe;
            d.reason = "Nexus retired your file; its main file now is older (" + (d.file->version.empty() ? file_text(*d.file) : d.file->version) +
                       "). EMF can't tell whether to go back to it.";
            return d;
        }
        if (!d.choices.empty()) {
            std::vector<NexusFile> keep;
            for (const auto& f : d.choices)
                if (!older(f)) keep.push_back(f);
            if (keep.empty()) {
                d.state = UpdateState::Maybe;
                d.reason = "Nexus retired your file; its main files now are older (" + names_text(d.choices) + "). EMF can't tell whether to go back to one.";
                return d;
            }
            d.choices = keep;
        }
        if (d.file) d.reason += " Take " + file_text(*d.file) + ".";
        else if (!d.choices.empty()) d.reason += " Nexus has several files that could be yours: choose the one you use.";
        else {
            d.state = UpdateState::Maybe;
            d.reason += " The mod has no main file to take now: see its Files tab on Nexus.";
        }
        return d;
    }

    // ---- only the version is known ----
    if (pool.empty()) {
        d.reason = "Nexus lists no files for this mod now.";
        return d;
    }
    const std::string nexus = !best.empty() ? best : (m.version.empty() ? pool[0].version : m.version);
    d.nexus_version = nexus;
    const std::string nexus_text = nexus.empty() ? std::string("no version") : nexus;
    if (in.version.empty()) {
        d.reason = "EMF doesn't know which version you have. Nexus has " + nexus_text + ".";
        pick(d, pool, {in.archive}, in.name);
        return d;
    }
    const auto cmp = best.empty() ? std::nullopt : compare_versions(in.version, best);
    if (!cmp) {
        d.reason = "You have " + in.version + "; Nexus has " + nexus_text + ". EMF can't compare these versions.";
        pick(d, pool, {in.archive}, in.name);
        return d;
    }
    if (*cmp >= 0) {
        d.state = UpdateState::Current;
        d.reason = "You have " + in.version + "; Nexus has " + nexus + (*cmp > 0 ? " (yours is newer)." : ".");
        return d;
    }
    pick(d, pool, {in.archive}, in.name);
    // the newest version can be on the page or another variant: a main file that fits yours at your version is no update
    const std::vector<NexusFile> offered = d.file ? std::vector<NexusFile>{*d.file} : d.choices;
    const bool none_newer = !offered.empty() && std::all_of(offered.begin(), offered.end(), [&](const NexusFile& f) {
        const auto c = compare_versions(in.version, f.version);
        return c && *c >= 0;
    });
    if (none_newer) {
        d.reason = "Nexus has " + nexus + ", but its main file" +
                   (offered.size() == 1 ? " for yours is " + file_text(offered[0]) + ", the version" : "s are the version") +
                   " you have (" + in.version + "). EMF can't tell whether there is an update.";
        return d;
    }
    d.state = UpdateState::Update;
    d.reason = "You have " + in.version + "; Nexus has " + nexus + ".";
    if (d.file) d.reason += " Take " + file_text(*d.file) + ".";
    else d.reason += " Nexus has several files that could be yours: choose the one you use.";
    return d;
}

}  // namespace emf::nexus
