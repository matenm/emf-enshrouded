#include "nexus/api.h"

#include <cmath>
#include <limits>

#include "core/json.h"

namespace emf::nexus {

using core::json::Value;

namespace {

// The fields of a mod in lists and on its page (the page adds the description).
constexpr const char* kModFields =
    "modId name summary version downloads endorsements updatedAt createdAt category pictureUrl thumbnailUrl "
    "adultContent author uploader { name }";

constexpr size_t kMaxSearchBytes = 200;

std::string_view trim(std::string_view s) {
    const char* ws = " \t\r\n\f\v";
    const size_t a = s.find_first_not_of(ws);
    if (a == std::string_view::npos) return {};
    const size_t b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}

// At most `max` bytes, not ending inside a UTF-8 sequence.
std::string_view utf8_prefix(std::string_view s, size_t max) {
    if (s.size() <= max) return s;
    size_t cut = max;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut);
}

int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

std::string request(const char* query, const std::string& variables) {
    return std::string("{\"query\":") + json_quote(query) + ",\"variables\":" + variables + "}";
}

// [{"value": <value>, "op": "<op>"}]: one condition of a Nexus filter.
std::string condition(const std::string& value_json, const char* op) {
    return std::string("[{\"value\":") + value_json + ",\"op\":\"" + op + "\"}]";
}

const char* sort_key(SortBy s) {
    switch (s) {
        case SortBy::Endorsements: return "endorsements";
        case SortBy::Updated: return "updatedAt";
        case SortBy::Created: return "createdAt";
        case SortBy::Name: return "name";
        case SortBy::Downloads: break;
    }
    return "downloads";
}

// --- reading replies ---

std::string str(const Value* v) { return v && v->is(Value::Type::String) ? v->s : std::string(); }

// Digits only (Nexus sends big numbers such as sizeInBytes as strings); nullopt on anything else or overflow.
std::optional<uint64_t> digits(const std::string& s) {
    if (s.empty() || s.size() > 20) return std::nullopt;
    uint64_t n = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return std::nullopt;
        const uint64_t d = static_cast<uint64_t>(c - '0');
        if (n > (std::numeric_limits<uint64_t>::max() - d) / 10) return std::nullopt;
        n = n * 10 + d;
    }
    return n;
}

std::optional<int64_t> to_i64(const Value* v) {
    if (!v) return std::nullopt;
    if (v->is(Value::Type::Number)) {
        if (!std::isfinite(v->n) || std::fabs(v->n) >= 9.0e18) return std::nullopt;
        return static_cast<int64_t>(v->n);
    }
    if (v->is(Value::Type::String)) {
        const bool neg = !v->s.empty() && v->s[0] == '-';
        const auto n = digits(neg ? v->s.substr(1) : v->s);
        if (!n || *n > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return std::nullopt;
        return neg ? -static_cast<int64_t>(*n) : static_cast<int64_t>(*n);
    }
    return std::nullopt;
}

std::optional<uint64_t> to_u64(const Value* v) {
    if (!v) return std::nullopt;
    if (v->is(Value::Type::Number)) {
        if (!std::isfinite(v->n) || v->n < 0 || v->n >= 1.8e19) return std::nullopt;
        return static_cast<uint64_t>(v->n);
    }
    if (v->is(Value::Type::String)) return digits(v->s);
    return std::nullopt;
}

int64_t count(const Value* v) {
    const auto n = to_i64(v);
    return n && *n > 0 ? *n : 0;
}

bool flag(const Value* v) {
    if (!v) return false;
    if (v->is(Value::Type::Bool)) return v->b;
    if (v->is(Value::Type::Number)) return v->n != 0;
    return false;
}

std::string http_problem(int status) {
    if (status == 403) return "Nexus refused the request (HTTP 403)";
    if (status == 429) return "Nexus says too many requests (HTTP 429); try again in a minute";
    if (status >= 500) return "Nexus is not available right now (HTTP " + std::to_string(status) + ")";
    return "Nexus answered HTTP " + std::to_string(status);
}

// The member `field` of the reply's "data", or an error: HTTP status, not JSON, GraphQL errors, field missing.
// `keep_partial` (lists): GraphQL errors next to a non-null field keep what arrived.
template <class T>
const Value* data_field(std::string_view body, int status, const char* field, std::optional<Value>& doc,
                        Result<T>& out, bool keep_partial = false) {
    auto parsed = core::json::parse(body, core::json::ParseOptions{true});  // Nexus's text may hold a lone surrogate
    const bool http_ok = status >= 200 && status < 300;
    if (!parsed.value) {
        out.error = http_ok ? "the reply from Nexus is not JSON (" + parsed.error + ")" : http_problem(status);
        return nullptr;
    }
    doc = std::move(parsed.value);
    auto field_of = [&]() -> const Value* {
        const Value* data = doc->get("data");
        const Value* v = data ? data->get(field) : nullptr;
        return v && !v->is(Value::Type::Null) ? v : nullptr;
    };
    const Value* errors = doc->get("errors");
    const bool has_errors = errors && errors->is(Value::Type::Array) && !errors->arr.empty();
    if (has_errors && keep_partial && http_ok) {
        if (const Value* v = field_of()) return v;
    }
    if (has_errors) {
        std::string text;
        size_t shown = 0;
        for (const Value& e : errors->arr) {
            std::string m = str(e.get("message"));
            if (m.empty()) continue;
            if (shown++ == 3) {
                text += "; ...";
                break;
            }
            text += (text.empty() ? "" : "; ") + std::string(utf8_prefix(m, 300));
        }
        out.error = "Nexus: " + (text.empty() ? std::string("unknown error") : text);
        if (!http_ok) out.error += " (HTTP " + std::to_string(status) + ")";
        return nullptr;
    }
    if (!http_ok) {
        out.error = http_problem(status);
        return nullptr;
    }
    const Value* v = field_of();
    if (!v) out.error = std::string("the reply from Nexus has no ") + field;
    return v;
}

// A mod node; nullopt without a usable modId.
std::optional<NexusMod> mod_from(const Value& n) {
    if (!n.is(Value::Type::Object)) return std::nullopt;
    const auto id = to_i64(n.get("modId"));
    if (!id || *id <= 0) return std::nullopt;
    NexusMod m;
    m.id = *id;
    m.name = str(n.get("name"));
    m.summary = str(n.get("summary"));
    m.version = str(n.get("version"));
    m.author = str(n.get("author"));
    if (const Value* u = n.get("uploader")) m.uploader = str(u->get("name"));
    m.downloads = count(n.get("downloads"));
    m.endorsements = count(n.get("endorsements"));
    m.updated_at = str(n.get("updatedAt"));
    m.created_at = str(n.get("createdAt"));
    m.category = str(n.get("category"));
    m.picture_url = str(n.get("pictureUrl"));
    m.thumbnail_url = str(n.get("thumbnailUrl"));
    m.adult = flag(n.get("adultContent"));
    m.description = str(n.get("description"));
    return m;
}

}  // namespace

std::optional<SortBy> sort_from_name(std::string_view name) {
    if (name == "downloads") return SortBy::Downloads;
    if (name == "endorsements") return SortBy::Endorsements;
    if (name == "updated") return SortBy::Updated;
    if (name == "created") return SortBy::Created;
    if (name == "name") return SortBy::Name;
    return std::nullopt;
}

std::string search_request(const SearchQuery& q) {
    std::string filter = "{\"gameDomainName\":" + condition(json_quote(kGameDomain), "EQUALS");
    const std::string_view text = utf8_prefix(trim(q.text), kMaxSearchBytes);
    if (!text.empty()) filter += ",\"name\":" + condition(json_quote(text), "WILDCARD");
    const std::string_view category = trim(q.category);
    if (!category.empty()) filter += ",\"categoryName\":" + condition(json_quote(category), "EQUALS");
    filter += "}";
    const std::string sort =
        std::string("[{\"") + sort_key(q.sort) + "\":{\"direction\":\"" + (q.descending ? "DESC" : "ASC") + "\"}}]";
    const std::string vars = "{\"filter\":" + filter + ",\"sort\":" + sort +
                             ",\"count\":" + std::to_string(clamp(q.count, 1, 100)) +
                             ",\"offset\":" + std::to_string(q.offset < 0 ? 0 : q.offset) + "}";
    const std::string query =
        std::string("query EmfSearch($filter: ModsFilter, $sort: [ModsSort!], $count: Int, $offset: Int) { "
                    "mods(filter: $filter, sort: $sort, count: $count, offset: $offset) { totalCount nodes { ") +
        kModFields + " } } }";
    return request(query.c_str(), vars);
}

std::string mod_request(int64_t mod_id) {
    const std::string query = std::string("query EmfMod($modId: ID!, $gameId: ID!) { "
                                          "mod(modId: $modId, gameId: $gameId) { ") +
                              kModFields + " description } }";
    return request(query.c_str(), "{\"modId\":" + std::to_string(mod_id) +
                                      ",\"gameId\":" + std::to_string(kGameId) + "}");
}

std::string files_request(int64_t mod_id) {
    return request("query EmfFiles($modId: ID!, $gameId: ID!) { modFiles(modId: $modId, gameId: $gameId) { "
                   "fileId name version category size sizeInBytes date primary } }",
                   "{\"modId\":" + std::to_string(mod_id) + ",\"gameId\":" + std::to_string(kGameId) + "}");
}

std::string contents_request(int64_t mod_id, int64_t file_id, int count, int offset) {
    const std::string filter = "{\"gameId\":" + condition(std::to_string(kGameId), "EQUALS") +
                               ",\"modId\":" + condition(std::to_string(mod_id), "EQUALS") +
                               ",\"fileId\":" + condition(std::to_string(file_id), "EQUALS") + "}";
    return request("query EmfContents($filter: ModFileContentSearchFilter, $count: Int, $offset: Int) { "
                   "modFileContents(filter: $filter, count: $count, offset: $offset) { "
                   "totalCount nodes { filePath fileSize } } }",
                   "{\"filter\":" + filter + ",\"count\":" + std::to_string(clamp(count, 1, 1000)) +
                       ",\"offset\":" + std::to_string(offset < 0 ? 0 : offset) + "}");
}

Result<SearchPage> parse_search(std::string_view body, int http_status) {
    Result<SearchPage> r;
    std::optional<Value> doc;
    const Value* mods = data_field(body, http_status, "mods", doc, r, true);
    if (!mods) return r;
    const Value* nodes = mods->get("nodes");
    if (!nodes || !nodes->is(Value::Type::Array)) {
        r.error = "the reply from Nexus has no list of mods";
        return r;
    }
    for (const Value& n : nodes->arr)
        if (auto m = mod_from(n)) r.value.mods.push_back(std::move(*m));
    const auto total = to_i64(mods->get("totalCount"));
    r.value.total = total && *total >= 0 ? *total : static_cast<int64_t>(r.value.mods.size());
    return r;
}

Result<NexusMod> parse_mod(std::string_view body, int http_status) {
    Result<NexusMod> r;
    std::optional<Value> doc;
    const Value* mod = data_field(body, http_status, "mod", doc, r);
    if (!mod) return r;
    if (auto m = mod_from(*mod)) r.value = std::move(*m);
    else r.error = "the reply from Nexus has no mod";
    return r;
}

Result<std::vector<NexusFile>> parse_files(std::string_view body, int http_status) {
    Result<std::vector<NexusFile>> r;
    std::optional<Value> doc;
    const Value* files = data_field(body, http_status, "modFiles", doc, r);
    if (!files) return r;
    if (!files->is(Value::Type::Array)) {
        r.error = "the reply from Nexus has no list of files";
        return r;
    }
    for (const Value& n : files->arr) {
        if (!n.is(Value::Type::Object)) continue;
        const auto id = to_i64(n.get("fileId"));
        if (!id || *id <= 0) continue;
        NexusFile f;
        f.file_id = *id;
        f.name = str(n.get("name"));
        f.version = str(n.get("version"));
        f.category = str(n.get("category"));
        if (const auto bytes = to_u64(n.get("sizeInBytes"))) {
            f.size_bytes = *bytes;
        } else if (const auto kib = to_u64(n.get("size")); kib && *kib < (uint64_t{1} << 53)) {
            f.size_bytes = *kib * 1024;  // only the size in KiB
        }
        f.date = count(n.get("date"));
        f.primary = flag(n.get("primary"));
        r.value.push_back(std::move(f));
    }
    return r;
}

Result<ContentsPage> parse_contents(std::string_view body, int http_status) {
    Result<ContentsPage> r;
    std::optional<Value> doc;
    const Value* page = data_field(body, http_status, "modFileContents", doc, r, true);
    if (!page) return r;
    const Value* nodes = page->get("nodes");
    if (!nodes || !nodes->is(Value::Type::Array)) {
        r.error = "the reply from Nexus has no list of archive entries";
        return r;
    }
    for (const Value& n : nodes->arr) {
        const std::string path = str(n.get("filePath"));
        if (path.empty()) continue;
        r.value.entries.push_back({path, to_u64(n.get("fileSize")).value_or(0)});
    }
    const auto total = to_i64(page->get("totalCount"));
    r.value.total = total && *total >= 0 ? *total : static_cast<int64_t>(r.value.entries.size());
    return r;
}

std::string json_quote(std::string_view s) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                default:
                    if (c < 0x20) {
                        out += "\\u00";
                        out += kHex[c >> 4];
                        out += kHex[c & 15];
                    } else {
                        out += static_cast<char>(c);
                    }
            }
            ++i;
            continue;
        }
        // The length of a valid UTF-8 sequence starting here, or 0 (RFC 3629: no overlongs, no surrogates).
        auto cont = [&](size_t k, unsigned lo, unsigned hi) {
            if (i + k >= s.size()) return false;
            const auto b = static_cast<unsigned char>(s[i + k]);
            return b >= lo && b <= hi;
        };
        size_t len = 0;
        if (c >= 0xC2 && c <= 0xDF) len = cont(1, 0x80, 0xBF) ? 2 : 0;
        else if (c == 0xE0) len = cont(1, 0xA0, 0xBF) && cont(2, 0x80, 0xBF) ? 3 : 0;
        else if ((c >= 0xE1 && c <= 0xEC) || c == 0xEE || c == 0xEF) len = cont(1, 0x80, 0xBF) && cont(2, 0x80, 0xBF) ? 3 : 0;
        else if (c == 0xED) len = cont(1, 0x80, 0x9F) && cont(2, 0x80, 0xBF) ? 3 : 0;
        else if (c == 0xF0) len = cont(1, 0x90, 0xBF) && cont(2, 0x80, 0xBF) && cont(3, 0x80, 0xBF) ? 4 : 0;
        else if (c >= 0xF1 && c <= 0xF3) len = cont(1, 0x80, 0xBF) && cont(2, 0x80, 0xBF) && cont(3, 0x80, 0xBF) ? 4 : 0;
        else if (c == 0xF4) len = cont(1, 0x80, 0x8F) && cont(2, 0x80, 0xBF) && cont(3, 0x80, 0xBF) ? 4 : 0;
        if (len == 0) {
            out += "\xEF\xBF\xBD";
            ++i;
        } else {
            out.append(s.substr(i, len));
            i += len;
        }
    }
    out += '"';
    return out;
}

}  // namespace emf::nexus
