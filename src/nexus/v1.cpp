#include "nexus/v1.h"

#include <cmath>
#include <ctime>
#include <optional>

#include "core/json.h"

namespace emf::nexus {

using core::json::Value;
using VT = Value::Type;

namespace {
using Clock = std::chrono::steady_clock;

std::string str(const Value* v) { return v && v->is(VT::String) ? v->s : std::string(); }

std::optional<int64_t> to_i64(const Value* v) {
    if (!v) return std::nullopt;
    if (v->is(VT::Number) && std::isfinite(v->n) && std::fabs(v->n) < 9.0e15) return static_cast<int64_t>(v->n);
    if (v->is(VT::String) && !v->s.empty() && v->s.size() <= 18 && v->s.find_first_not_of("0123456789") == std::string::npos)
        return std::stoll(v->s);
    return std::nullopt;
}

bool flag(const Value* v) {
    if (!v) return false;
    if (v->is(VT::Bool)) return v->b;
    if (v->is(VT::Number)) return v->n != 0;
    return false;
}

// At most `max` bytes of Nexus's own message, cut at a UTF-8 boundary, without line breaks.
std::string short_text(std::string s, size_t max = 300) {
    for (char& c : s)
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    if (s.size() <= max) return s;
    size_t cut = max;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut) + "...";
}

// Nexus's message in an error reply ({"message": ...}, {"msg": ...} or {"error": ...}).
std::string nexus_message(const std::optional<Value>& v) {
    if (!v || !v->is(VT::Object)) return {};
    for (const char* k : {"message", "msg", "error"})
        if (const std::string s = str(v->get(k)); !s.empty()) return short_text(s);
    return {};
}

// The error for a reply that is not a 2xx, in words.
std::string status_error(int status, const std::string& message) {
    const std::string said = message.empty() ? std::string() : ": " + message;
    if (status == 401) return "Nexus did not accept the API key" + said;
    if (status == 403) return "Nexus refused the request (403)" + said;
    if (status == 404) return "Nexus has no such file or mod (404)" + said;
    if (status == 410) return "Nexus says the download link has expired or is not for this account (410)" + said;
    if (status == 429) return "Nexus's request limit for your account is used up (429)" + said;
    if (status >= 500) return "Nexus had a problem (" + std::to_string(status) + "). Try again in a while" + said;
    return "Nexus answered " + std::to_string(status) + said;
}

// The reply as JSON, or the error in `out` (status, not JSON).
template <class T>
std::optional<Value> reply_json(std::string_view body, int status, V1Result<T>& out) {
    out.status = status;
    auto doc = core::json::parse(body, core::json::ParseOptions{true});  // Nexus's text may hold a lone surrogate
    if (status < 200 || status >= 300) {
        out.error = status_error(status, nexus_message(doc.value));
        return std::nullopt;
    }
    if (!doc.value) {
        out.error = "Nexus's answer was not JSON";
        return std::nullopt;
    }
    return std::move(doc.value);
}

int64_t header_count(const HttpHeaders& h, const char* name, bool& seen) {
    const std::string v = header_value(h, name);
    if (v.empty() || v.size() > 12 || v.find_first_not_of("0123456789") != std::string::npos) return -1;
    seen = true;
    return std::stoll(v);
}

}  // namespace

RateLimits parse_rate_limits(const HttpHeaders& headers) {
    RateLimits l;
    bool seen = false, ignored = false;
    l.daily_remaining = header_count(headers, "X-RL-Daily-Remaining", seen);
    l.hourly_remaining = header_count(headers, "X-RL-Hourly-Remaining", seen);
    l.daily_limit = header_count(headers, "X-RL-Daily-Limit", ignored);
    l.hourly_limit = header_count(headers, "X-RL-Hourly-Limit", ignored);
    l.daily_reset = short_text(header_value(headers, "X-RL-Daily-Reset"), 64);
    l.hourly_reset = short_text(header_value(headers, "X-RL-Hourly-Reset"), 64);
    l.known = seen;
    return l;
}

std::string local_clock(int64_t unix_seconds) {
    const std::time_t t = static_cast<std::time_t>(unix_seconds), now = std::time(nullptr);
    std::tm when{}, today{};
    if (localtime_s(&when, &t) != 0 || localtime_s(&today, &now) != 0) return {};
    char b[32];
    const bool same_day = when.tm_year == today.tm_year && when.tm_yday == today.tm_yday;
    std::strftime(b, sizeof b, same_day ? "%H:%M" : "%Y-%m-%d %H:%M", &when);
    return b;
}

std::string rate_limit_message(const RateLimits& l) {
    // Nexus's times are UTC; the player reads them in their own time zone (Nexus's text when it is not a time)
    const auto when = [](const std::string& iso) {
        const int64_t t = iso8601_to_unix(iso);
        return t > 0 ? local_clock(t) : iso;
    };
    std::string m = "Nexus's request limit for your account is used up for now";
    if (!l.hourly_reset.empty()) m += ": it opens again at " + when(l.hourly_reset);
    if (!l.daily_reset.empty())
        m += (l.hourly_reset.empty() ? ": the daily allowance resets at " : ", and the daily one resets at ") + when(l.daily_reset);
    return m + ". EMF asks Nexus nothing more until then.";
}

int64_t iso8601_to_unix(std::string_view t) {
    const auto num = [&](size_t at, size_t n, int& out) {
        if (at + n > t.size()) return false;
        out = 0;
        for (size_t i = at; i < at + n; ++i) {
            if (t[i] < '0' || t[i] > '9') return false;
            out = out * 10 + (t[i] - '0');
        }
        return true;
    };
    int y, mo, d, h, mi, s;
    if (t.size() < 19 || !num(0, 4, y) || t[4] != '-' || !num(5, 2, mo) || t[7] != '-' || !num(8, 2, d) ||
        (t[10] != 'T' && t[10] != ' ') || !num(11, 2, h) || t[13] != ':' || !num(14, 2, mi) || t[16] != ':' || !num(17, 2, s))
        return 0;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60) return 0;
    size_t at = 19;
    if (at < t.size() && t[at] == '.')
        for (++at; at < t.size() && t[at] >= '0' && t[at] <= '9';) ++at;
    int64_t offset = 0;
    if (at < t.size()) {
        if (t[at] == 'Z' && at + 1 == t.size()) {
        } else if ((t[at] == '+' || t[at] == '-') && at + 6 == t.size() && t[at + 3] == ':') {
            int oh, om;
            if (!num(at + 1, 2, oh) || !num(at + 4, 2, om)) return 0;
            offset = (t[at] == '+' ? 1 : -1) * (oh * 3600 + om * 60);
        } else {
            return 0;
        }
    }
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    const int64_t utc = static_cast<int64_t>(_mkgmtime(&tm));
    return utc <= 0 ? 0 : utc - offset;
}

V1Result<NexusUser> parse_validate(std::string_view body, int http_status) {
    V1Result<NexusUser> out;
    const auto v = reply_json(body, http_status, out);
    if (!v) return out;
    if (!v->is(VT::Object)) {
        out.error = "Nexus's answer has no user in it";
        return out;
    }
    NexusUser& u = out.value;
    u.user_id = to_i64(v->get("user_id")).value_or(0);
    u.name = short_text(str(v->get("name")), 120);
    // both spellings: "is_premium" and the older "is_premium?"
    u.premium = v->get("is_premium") ? flag(v->get("is_premium")) : flag(v->get("is_premium?"));
    u.supporter = v->get("is_supporter") ? flag(v->get("is_supporter")) : flag(v->get("is_supporter?"));
    u.profile_url = short_text(str(v->get("profile_url")), 1024);
    // "email" and "key" are never read
    if (u.user_id <= 0 || u.name.empty()) {
        out.value = {};
        out.error = "Nexus's answer has no user in it";
    }
    return out;
}

V1Result<V1File> parse_file_info(std::string_view body, int http_status) {
    V1Result<V1File> out;
    const auto v = reply_json(body, http_status, out);
    if (!v) return out;
    if (!v->is(VT::Object) || !to_i64(v->get("file_id"))) {
        out.error = "Nexus's answer has no file in it";
        return out;
    }
    V1File& f = out.value;
    f.file_id = *to_i64(v->get("file_id"));
    f.name = short_text(str(v->get("name")), 300);
    f.version = short_text(str(v->get("version")), 100);
    f.category = short_text(str(v->get("category_name")), 40);
    f.file_name = str(v->get("file_name"));
    if (const auto size = to_i64(v->get("size_in_bytes")); size && *size >= 0) {
        f.size_bytes = static_cast<uint64_t>(*size);
        f.size_known = true;
    }
    return out;
}

V1Result<std::vector<CdnLink>> parse_download_links(std::string_view body, int http_status) {
    V1Result<std::vector<CdnLink>> out;
    const auto v = reply_json(body, http_status, out);
    if (!v) return out;
    if (!v->is(VT::Array)) {
        out.error = "Nexus's answer has no download links in it";
        return out;
    }
    for (const Value& e : v->arr) {
        CdnLink l;
        l.name = short_text(str(e.get("name")), 120);
        l.short_name = short_text(str(e.get("short_name")), 60);
        l.uri = str(e.get("URI"));
        if (l.uri.rfind("https://", 0) == 0 && l.uri.size() < 4096) out.value.push_back(std::move(l));
    }
    if (out.value.empty()) out.error = "Nexus gave no https download link for this file";
    return out;
}

bool api_key_text_ok(std::string_view key) {
    if (key.size() < 8 || key.size() > 1024) return false;
    for (unsigned char c : key)
        if (c <= 0x20 || c >= 0x7F) return false;
    return true;
}

std::string url_query_escape(std::string_view s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

V1Client::V1Client(std::string app_version, Transport transport, int timeout_ms)
    : version_(app_version.empty() ? std::string("unknown") : std::move(app_version)),
      transport_(std::move(transport)),
      timeout_ms_(timeout_ms) {}

HttpRequest V1Client::request_for(const std::string& path, const std::string& api_key) const {
    HttpRequest r;
    r.method = "GET";
    r.url = std::string(kV1Base) + path;
    r.user_agent = "EMF/" + version_ + " (Enshrouded Modding Framework; Windows)";
    r.headers = {{"apikey", api_key},
                 {"Accept", "application/json"},
                 {"Application-Name", "EMF"},
                 {"Application-Version", version_}};
    r.timeout_ms = timeout_ms_;
    return r;
}

template <class T, class Parse>
V1Result<T> V1Client::get(const std::string& path, const std::string& api_key, const std::atomic<bool>* cancel,
                          std::chrono::milliseconds deadline, Parse parse) const {
    V1Result<T> out;
    if (!api_key_text_ok(api_key)) {
        out.error = "The API key is not one EMF can send (8 to 1024 characters, no spaces).";
        return out;
    }
    if (cancel && cancel->load()) {
        out.error = "cancelled";
        return out;
    }
    HttpRequest r = request_for(path, api_key);
    r.cancel = cancel;
    r.deadline = Clock::now() + deadline;
    const HttpResponse resp = transport_ ? transport_(r) : http_request(r);
    if (resp.status == 0) {
        switch (resp.failure) {
            case HttpFailure::Cancelled: out.error = "cancelled"; break;
            case HttpFailure::TimedOut: out.error = "timed out"; break;
            case HttpFailure::TooLarge: out.error = "the reply was too large"; break;
            default: out.error = "could not reach Nexus: " + resp.error; break;
        }
        return out;
    }
    out = parse(resp.body, resp.status);
    out.limits = parse_rate_limits(resp.headers);
    if (resp.status == 429) out.error = rate_limit_message(out.limits);
    return out;
}

V1Result<NexusUser> V1Client::validate(const std::string& api_key, const std::atomic<bool>* cancel,
                                       std::chrono::milliseconds deadline) const {
    return get<NexusUser>("users/validate.json", api_key, cancel, deadline, parse_validate);
}

V1Result<V1File> V1Client::file(const std::string& api_key, int64_t mod_id, int64_t file_id,
                                const std::atomic<bool>* cancel, std::chrono::milliseconds deadline) const {
    const std::string path = "games/" + std::string(kGameDomain) + "/mods/" + std::to_string(mod_id) + "/files/" +
                             std::to_string(file_id) + ".json";
    return get<V1File>(path, api_key, cancel, deadline, parse_file_info);
}

V1Result<std::vector<CdnLink>> V1Client::download_links(const std::string& api_key, int64_t mod_id, int64_t file_id,
                                                        const std::string& nxm_key, int64_t expires,
                                                        const std::atomic<bool>* cancel,
                                                        std::chrono::milliseconds deadline) const {
    std::string path = "games/" + std::string(kGameDomain) + "/mods/" + std::to_string(mod_id) + "/files/" +
                       std::to_string(file_id) + "/download_link.json";
    if (!nxm_key.empty()) path += "?key=" + url_query_escape(nxm_key) + "&expires=" + std::to_string(expires);
    return get<std::vector<CdnLink>>(path, api_key, cancel, deadline, parse_download_links);
}

}  // namespace emf::nexus
