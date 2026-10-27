#include "manifest.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "emf/keys.hpp"
#include "json.h"

namespace emf::core {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// A number as short text: 4.0 -> "4", 2.5 -> "2.5"
std::string number_text(double v) {
    if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 1e15) {
        char b[32];
        std::snprintf(b, sizeof b, "%lld", static_cast<long long>(v));
        return b;
    }
    char b[64];
    std::snprintf(b, sizeof b, "%.15g", v);
    return b;
}

bool parse_double(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end && *end == '\0' && std::isfinite(out);
}

bool whole(double x) { return std::isfinite(x) && x == std::floor(x); }

bool safe_relative_file(const std::string& p) {
    if (p.empty() || p.size() > 200) return false;
    if (p.find("..") != std::string::npos || p.find(':') != std::string::npos) return false;
    if (p.front() == '/' || p.front() == '\\') return false;
    return true;
}

std::wstring wide(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// ---- version ranges ----

constexpr const char* kRangeForms = "use a version range like \"^1.2.0\", \"~1.2\", \">=1.0, <2.0\", \"1.2.3\" or \"*\"";

struct Partial {
    int n[3] = {0, 0, 0};
    int given = 0;          // numbers before the first wildcard
    bool wildcard = false;  // an x or * stood somewhere (or the whole token was one)
};

bool fail_range(std::string* error, const std::string& text) {
    if (error) *error = text;
    return false;
}

// "1", "1.2", "1.2.3", "1.x", "1.2.*", "x"
bool parse_partial(std::string_view t, Partial& p, std::string* error) {
    if (t.empty()) return fail_range(error, std::string("a version is missing: ") + kRangeForms);
    if (t.front() == 'v' || t.front() == 'V')
        return fail_range(error, "versions are numbers (\"1.2.3\"), without a v in front");
    if (t.find_first_of("-+") != std::string_view::npos)
        return fail_range(error, "pre-release versions and build tags (\"1.2.3-beta\") are not supported: EMF versions "
                                 "are numbers only (a range with a hyphen needs spaces: \"1.0 - 2.0\")");
    int part = 0;
    size_t i = 0;
    while (true) {
        if (part == 3) return fail_range(error, "a version has at most three numbers (\"1.2.3\")");
        if (i < t.size() && (t[i] == 'x' || t[i] == 'X' || t[i] == '*')) {
            p.wildcard = true;
            ++i;
        } else if (i < t.size() && t[i] >= '0' && t[i] <= '9') {
            if (p.wildcard) return fail_range(error, "nothing but x or * may follow an x or * (\"1.x\", not \"1.x.3\")");
            long v = 0;
            while (i < t.size() && t[i] >= '0' && t[i] <= '9') {
                v = v * 10 + (t[i] - '0');
                if (v > 1000000) return fail_range(error, "a version number is too large");
                ++i;
            }
            p.n[part] = static_cast<int>(v);
            ++p.given;
        } else {
            return fail_range(error, "\"" + std::string(t) + "\" is not a version: " + kRangeForms);
        }
        ++part;
        if (i == t.size()) break;
        if (t[i] != '.') return fail_range(error, "\"" + std::string(t) + "\" is not a version: " + kRangeForms);
        ++i;
    }
    return true;
}

Version filled(const Partial& p) { return Version{p.n[0], p.n[1], p.n[2]}; }

// The first version after every version starting with the given numbers: 1 -> 2.0.0, 1.2 -> 1.3.0.
Version next_family(const Partial& p) {
    if (p.given <= 1) return Version{p.n[0] + 1, 0, 0};
    return Version{p.n[0], p.n[1] + 1, 0};
}

void set_lo(VersionSpan& s, Version v, bool in) {
    s.has_lo = true;
    s.lo = v;
    s.lo_in = in;
}
void set_hi(VersionSpan& s, Version v, bool in) {
    s.has_hi = true;
    s.hi = v;
    s.hi_in = in;
}

VersionSpan caret(const Partial& p) {
    VersionSpan s;
    if (p.given == 0) return s;
    set_lo(s, filled(p), true);
    const int M = p.n[0], m = p.n[1], pt = p.n[2];
    if (p.given == 1 || M > 0) set_hi(s, Version{M + 1, 0, 0}, false);
    else if (p.given == 2 || m > 0) set_hi(s, Version{0, m + 1, 0}, false);
    else set_hi(s, Version{0, 0, pt + 1}, false);
    return s;
}

VersionSpan tilde(const Partial& p) {
    VersionSpan s;
    if (p.given == 0) return s;
    set_lo(s, filled(p), true);
    set_hi(s, p.given == 1 ? Version{p.n[0] + 1, 0, 0} : Version{p.n[0], p.n[1] + 1, 0}, false);
    return s;
}

// One comparator ("^1.2", ">=1.0", "1.2.x") as a span.
bool comparator_span(const std::string& op, std::string_view token, VersionSpan& s, std::string* error) {
    Partial p;
    if (!parse_partial(token, p, error)) return false;
    s = VersionSpan{};
    if (op.empty()) {
        if (p.given == 0) return true;  // * / x
        if (p.wildcard) {               // 1.x, 1.2.*
            set_lo(s, filled(p), true);
            set_hi(s, next_family(p), false);
            return true;
        }
        s = caret(p);
        return true;
    }
    if (op == "^") {
        s = caret(p);
        return true;
    }
    if (op == "~") {
        s = tilde(p);
        return true;
    }
    if (p.wildcard) {  // say what the modder most likely meant, as npm reads it
        const Version lo = filled(p), next = next_family(p);
        std::string meant;
        if (op == ">=") meant = ">=" + version_text(lo);
        else if (op == "<") meant = "<" + version_text(lo);
        else if (op == ">") meant = ">=" + version_text(next);
        else if (op == "<=") meant = "<" + version_text(next);
        else meant = std::string(token);  // "=1.x": the wildcard alone
        return fail_range(error, "an x or * stands alone or after ^ or ~: write \"" + meant + "\" instead of \"" + op +
                                     std::string(token) + "\"");
    }
    if (op == ">=") set_lo(s, filled(p), true);
    else if (op == "<") set_hi(s, filled(p), false);
    else if (op == ">") {
        if (p.given == 3) set_lo(s, filled(p), false);
        else set_lo(s, next_family(p), true);
    } else if (op == "<=") {
        if (p.given == 3) set_hi(s, filled(p), true);
        else set_hi(s, next_family(p), false);
    } else {  // "="
        set_lo(s, filled(p), true);
        set_hi(s, filled(p), true);
    }
    return true;
}

// The versions both spans take.
VersionSpan intersect(VersionSpan a, const VersionSpan& b) {
    if (b.has_lo) {
        const int c = a.has_lo ? compare(b.lo, a.lo) : 1;
        if (c > 0) set_lo(a, b.lo, b.lo_in);
        else if (c == 0) a.lo_in = a.lo_in && b.lo_in;
    }
    if (b.has_hi) {
        const int c = a.has_hi ? compare(b.hi, a.hi) : -1;
        if (c < 0) set_hi(a, b.hi, b.hi_in);
        else if (c == 0) a.hi_in = a.hi_in && b.hi_in;
    }
    return a;
}

bool empty_span(const VersionSpan& s) {
    if (s.has_hi && !s.hi_in && compare(s.hi, Version{0, 0, 0}) == 0) return true;  // below 0.0.0: nothing
    if (!s.has_lo || !s.has_hi) return false;
    const int c = compare(s.lo, s.hi);
    return c > 0 || (c == 0 && !(s.lo_in && s.hi_in));
}

bool below(Version v, const VersionSpan& s) {
    if (!s.has_lo) return false;
    const int c = compare(v, s.lo);
    return c < 0 || (c == 0 && !s.lo_in);
}
bool above(Version v, const VersionSpan& s) {
    if (!s.has_hi) return false;
    const int c = compare(v, s.hi);
    return c > 0 || (c == 0 && !s.hi_in);
}

std::string describe_span(const VersionSpan& s) {
    if (!s.has_lo && !s.has_hi) return "any version";
    if (s.has_lo && s.has_hi && s.lo_in && s.hi_in && compare(s.lo, s.hi) == 0) return "exactly " + version_text(s.lo);
    std::string out;
    if (s.has_lo) out = s.lo_in ? version_text(s.lo) + " or newer" : "newer than " + version_text(s.lo);
    if (s.has_hi) {
        if (!out.empty()) out += ", ";
        out += s.hi_in ? (s.has_lo ? "up to " + version_text(s.hi) : version_text(s.hi) + " or older")
                       : "below " + version_text(s.hi);
    }
    return out;
}

// One side of "||": comparators joined by commas or spaces, or a hyphen range.
bool parse_side(std::string_view side, VersionSpan& out, std::string* error) {
    struct Item {
        std::string op;
        std::string_view token;
    };
    std::vector<Item> items;
    bool comma_open = false;  // a comma waits for the comparator after it
    size_t i = 0;
    const auto space = [&](size_t k) { return k < side.size() && (side[k] == ' ' || side[k] == '\t'); };
    while (true) {
        while (space(i)) ++i;
        if (i >= side.size()) break;
        if (side[i] == ',') {
            if (items.empty() || comma_open) return fail_range(error, std::string("a comma needs a version on both sides: ") + kRangeForms);
            comma_open = true;
            ++i;
            continue;
        }
        std::string op;
        for (const char* o : {">=", "<=", ">", "<", "=", "^", "~"}) {
            const size_t n = std::char_traits<char>::length(o);
            if (side.substr(i, n) == o) {
                op = o;
                i += n;
                break;
            }
        }
        if (!op.empty() && i < side.size() && (side[i] == '=' || side[i] == '>' || side[i] == '<'))
            return fail_range(error, "\"" + std::string(side) + "\" is not a version range: " + kRangeForms);
        while (space(i)) ++i;
        const size_t start = i;
        while (i < side.size() && side[i] != ' ' && side[i] != '\t' && side[i] != ',') ++i;
        const std::string_view token = side.substr(start, i - start);
        if (token.empty())
            return fail_range(error, op.empty() ? std::string("a version is missing: ") + kRangeForms
                                                : "\"" + op + "\" needs a version after it (\"" + op + "1.2.0\")");
        items.push_back({op, token});
        comma_open = false;
    }
    if (comma_open) return fail_range(error, std::string("a comma needs a version on both sides: ") + kRangeForms);
    if (items.empty()) return fail_range(error, std::string("a version range is empty: ") + kRangeForms);
    // "1.0 - 2.0"
    for (size_t k = 0; k < items.size(); ++k) {
        if (!(items[k].op.empty() && items[k].token == "-")) continue;
        if (items.size() != 3 || k != 1 || !items[0].op.empty() || !items[2].op.empty())
            return fail_range(error, "a hyphen range is two versions with \" - \" between them (\"1.2.3 - 2.0.0\")");
        Partial lo, hi;
        if (!parse_partial(items[0].token, lo, error) || !parse_partial(items[2].token, hi, error)) return false;
        out = VersionSpan{};
        set_lo(out, filled(lo), true);
        if (hi.given == 3) set_hi(out, filled(hi), true);
        else if (hi.given > 0) set_hi(out, next_family(hi), false);
        return true;
    }
    out = VersionSpan{};
    for (const auto& it : items) {
        VersionSpan s;
        if (!comparator_span(it.op, it.token, s, error)) return false;
        out = intersect(out, s);
    }
    return true;
}

}  // namespace

std::optional<Version> parse_version(std::string_view text) {
    text = trim(text);
    if (text.empty()) return std::nullopt;
    int parts[3] = {0, 0, 0};
    int n = 0;
    size_t i = 0;
    while (true) {
        if (n == 3) return std::nullopt;
        if (i >= text.size() || text[i] < '0' || text[i] > '9') return std::nullopt;
        long v = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            v = v * 10 + (text[i] - '0');
            if (v > 1000000) return std::nullopt;
            ++i;
        }
        parts[n++] = static_cast<int>(v);
        if (i == text.size()) break;
        if (text[i] != '.') return std::nullopt;
        ++i;
    }
    return Version{parts[0], parts[1], parts[2]};
}

int compare(Version a, Version b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

std::string version_text(Version v) {
    return std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
}

std::string Constraint::describe() const {
    if (spans.empty()) return "any version";
    std::string out;
    for (size_t i = 0; i < spans.size(); ++i) out += (i ? "; or " : "") + describe_span(spans[i]);
    return out;
}

std::optional<Constraint> parse_constraint(std::string_view text, std::string* error) {
    text = trim(text);
    if (text.empty()) return fail_range(error, std::string("the version range is empty: ") + kRangeForms), std::nullopt;
    Constraint c;
    c.written = std::string(text);
    bool any = false;
    size_t from = 0;
    while (true) {
        const size_t bar = text.find("||", from);
        const std::string_view side = trim(text.substr(from, bar == std::string_view::npos ? std::string_view::npos : bar - from));
        if (side.empty()) return fail_range(error, "\"||\" needs a version range on both sides (\"^1.0 || ^2.0\")"), std::nullopt;
        VersionSpan s;
        if (!parse_side(side, s, error)) return std::nullopt;
        if (empty_span(s))
            return fail_range(error, "\"" + std::string(side) + "\" matches no version (" + describe_span(s) + "?)"), std::nullopt;
        if (!s.has_lo && !s.has_hi) any = true;
        c.spans.push_back(s);
        if (bar == std::string_view::npos) break;
        from = bar + 2;
    }
    if (any) c.spans.clear();
    return c;
}

bool satisfies(Version v, const Constraint& c) {
    if (c.spans.empty()) return true;
    for (const auto& s : c.spans)
        if (!below(v, s) && !above(v, s)) return true;
    return false;
}

Fit fit(Version v, const Constraint& c) {
    if (satisfies(v, c)) return Fit::Ok;
    bool all_below = true, all_above = true;
    for (const auto& s : c.spans) {
        all_below = all_below && below(v, s);
        all_above = all_above && above(v, s);
    }
    return all_below ? Fit::TooOld : all_above ? Fit::TooNew : Fit::Between;
}

const char* setting_type_name(SettingType t) {
    switch (t) {
        case SettingType::Bool: return "bool";
        case SettingType::Int: return "int";
        case SettingType::Float: return "float";
        case SettingType::Choice: return "choice";
        case SettingType::Key: return "key";
        case SettingType::Select: return "select";
        case SettingType::Color: return "color";
        default: return "text";
    }
}

bool valid_mod_id(std::string_view id) {
    if (id.empty() || id.size() > 48) return false;
    for (char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')) return false;
    return true;
}

bool valid_setting_key(std::string_view key) {
    if (key.empty() || key.size() > 48) return false;
    for (char c : key)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
              c == '-'))
            return false;
    return true;
}

namespace {
// Length of the UTF-8 sequence at s[i], or 0 when the bytes there are not well-formed.
size_t utf8_length(const std::string& s, size_t i) {
    const auto b = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = b(i);
    const size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    if (n == 0 || i + n > s.size()) return 0;
    for (size_t k = 1; k < n; ++k)
        if ((b(i + k) & 0xC0) != 0x80) return 0;
    if (n == 2 && c < 0xC2) return 0;                                                           // overlong
    if (n == 3 && c == 0xE0 && b(i + 1) < 0xA0) return 0;                                       // overlong
    if (n == 3 && c == 0xED && b(i + 1) >= 0xA0) return 0;                                      // UTF-16 surrogates
    if (n == 4 && (c > 0xF4 || (c == 0xF0 && b(i + 1) < 0x90) || (c == 0xF4 && b(i + 1) >= 0x90))) return 0;
    return n;
}

// Text that is only shown (labels, help, headings): a bad byte becomes U+FFFD.
void sanitize_utf8(std::string& s) {
    if (valid_utf8(s)) return;
    std::string out;
    for (size_t i = 0; i < s.size();) {
        const size_t n = utf8_length(s, i);
        if (n) {
            out.append(s, i, n);
            i += n;
        } else {
            out += "\xEF\xBF\xBD";
            ++i;
        }
    }
    s = std::move(out);
}
}  // namespace

std::string setting_number_text(double v) { return number_text(v); }

bool valid_utf8(const std::string& text) {
    for (size_t i = 0; i < text.size();) {
        const size_t n = utf8_length(text, i);
        if (!n) return false;
        i += n;
    }
    return true;
}

size_t utf8_chars(const std::string& text) {
    size_t n = 0;
    for (unsigned char c : text)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

namespace {

bool fail(std::string* why, const std::string& text) {
    if (why) *why = text;
    return false;
}

std::string range_text(const SettingDef& d) {
    if (d.has_min && d.has_max) return "between " + number_text(d.min) + " and " + number_text(d.max);
    if (d.has_min) return "at least " + number_text(d.min);
    return "at most " + number_text(d.max);
}

std::optional<bool> parse_bool(const std::string& s) {
    const std::string l = lower(s);
    if (l == "1" || l == "true" || l == "on" || l == "yes") return true;
    if (l == "0" || l == "false" || l == "off" || l == "no") return false;
    return std::nullopt;
}

// What the ini cannot keep in a text value ("" when it can).
std::string text_problem(const std::string& raw) {
    if (raw.size() > 256) return "longer than 256 bytes";
    if (!valid_utf8(raw)) return "not valid UTF-8 text (save the file as UTF-8)";
    if (raw.find_first_of("\r\n") != std::string::npos) return "no line breaks";
    // GetPrivateProfileString drops spaces and a pair of quotes around a value
    if (!raw.empty() && (raw.front() == ' ' || raw.front() == '\t' || raw.back() == ' ' || raw.back() == '\t'))
        return "no spaces at the start or end";
    if (raw.size() >= 2 && (raw.front() == '"' || raw.front() == '\'') && raw.back() == raw.front())
        return "no quote at both ends (the ini file drops them)";
    return "";
}

// "#rgb" or "#rrggbb" (the # may be left out, any case) as "#rrggbb"
std::optional<std::string> parse_color(const std::string& text) {
    std::string h = text;
    if (!h.empty() && h[0] == '#') h.erase(0, 1);
    for (char c : h)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return std::nullopt;
    if (h.size() == 3) h = std::string{h[0], h[0], h[1], h[1], h[2], h[2]};
    if (h.size() != 6) return std::nullopt;
    return "#" + lower(h);
}

// x moved to the nearest of min (or 0) + k * step, kept within min/max
double snap(const SettingDef& d, double x) {
    if (!(d.step > 0)) return x;
    const double base = d.has_min ? d.min : 0.0;
    double k = std::round((x - base) / d.step);
    double y = base + k * d.step;
    if (d.has_max && y > d.max + 1e-9 * (std::fabs(d.max) + 1)) y = base + (k - 1) * d.step;
    if (d.has_min && y < d.min - 1e-9 * (std::fabs(d.min) + 1)) y = base + (k + 1) * d.step;
    if (d.type == SettingType::Int) return std::round(y);  // whole numbers stay exact up to 999999999999999
    char b[64];
    std::snprintf(b, sizeof b, "%.15g", y);  // 0.1 * 3 is 0.30000000000000004: keep what the player expects
    return std::strtod(b, nullptr);
}

bool on_step(const SettingDef& d, double x) {
    if (!(d.step > 0)) return true;
    return std::fabs(snap(d, x) - x) <= 1e-9 * (std::fabs(x) + 1);
}

const SettingOption* find_option(const SettingDef& d, const std::string& text) {
    const std::string l = lower(text);
    for (const auto& o : d.options)
        if (lower(o.value) == l) return &o;
    for (const auto& o : d.options)
        if (lower(o.label) == l) return &o;
    return nullptr;
}

}  // namespace

std::optional<std::string> setting_value(const SettingDef& d, const std::string& raw, bool clamp, std::string* why) {
    const std::string text(trim(raw));
    switch (d.type) {
        case SettingType::Bool: {
            auto b = parse_bool(text);
            if (!b) return fail(why, "use 1/0, true/false or on/off"), std::nullopt;
            return std::string(*b ? "1" : "0");
        }
        case SettingType::Int:
        case SettingType::Float: {
            double x = 0.0;
            if (!parse_double(text, x)) return fail(why, "not a number"), std::nullopt;
            if (d.type == SettingType::Int && x != std::floor(x)) {
                if (!clamp) return fail(why, "must be a whole number"), std::nullopt;
                if (!(d.step > 0)) x = std::floor(x + 0.5);  // with a step: moved to the nearest step below
            }
            if (d.type == SettingType::Int && std::fabs(x) > kMaxWholeSetting) {  // kept exactly up to here
                if (!clamp) return fail(why, "must be between -999999999999999 and 999999999999999"), std::nullopt;
                x = x < 0 ? -kMaxWholeSetting : kMaxWholeSetting;
            }
            const bool low = d.has_min && x < d.min, high = d.has_max && x > d.max;
            if (low || high) {
                if (!clamp) return fail(why, "must be " + range_text(d)), std::nullopt;
                x = low ? d.min : d.max;
            }
            return number_text(snap(d, x));
        }
        case SettingType::Choice: {
            double x = 0.0;
            if (parse_double(text, x) && x == std::floor(x) && x >= 0 && x < static_cast<double>(d.choices.size()))
                return number_text(x);
            for (size_t i = 0; i < d.choices.size(); ++i)
                if (lower(d.choices[i]) == lower(text)) return std::to_string(i);
            std::string list;
            for (size_t i = 0; i < d.choices.size(); ++i) list += (i ? ", " : "") + std::to_string(i) + " " + d.choices[i];
            return fail(why, "choose one of: " + list), std::nullopt;
        }
        case SettingType::Select: {
            if (const SettingOption* o = find_option(d, text)) return o->value;
            std::string list;
            for (size_t i = 0; i < d.options.size(); ++i)
                list += (i ? ", " : "") + d.options[i].value +
                        (d.options[i].label != d.options[i].value ? " (" + d.options[i].label + ")" : "");
            return fail(why, "choose one of: " + list), std::nullopt;
        }
        case SettingType::Color: {
            auto c = parse_color(text);
            if (!c) return fail(why, "a colour like #ff8800 (red, green, blue as two hex digits each)"), std::nullopt;
            return *c;
        }
        case SettingType::Key: {
            if (text.empty()) return std::string();  // no key
            std::string name;
            const uint32_t vk = emf::keys::parse_key(wide(text), &name);
            if (!vk)
                return fail(why, "not a key (a letter, a digit, F1-F24, Space, Tab, PageUp, Numpad0-9, Shift...)"),
                       std::nullopt;
            if (name.rfind("key ", 0) == 0) {  // no name: keep the number, which reads back
                char b[8];
                std::snprintf(b, sizeof b, "0x%02x", vk);
                return std::string(b);
            }
            return name;
        }
        case SettingType::Text: {
            if (const std::string p = text_problem(raw); !p.empty()) return fail(why, p), std::nullopt;
            if (d.max_length && utf8_chars(raw) > d.max_length)
                return fail(why, "at most " + std::to_string(d.max_length) + " characters"), std::nullopt;
            return raw;
        }
    }
    return std::nullopt;
}

namespace {
// What is wrong with the fields that only change how a setting shows or when it can be changed (step, slider,
// options on another kind, maxLength, group, enabledWhen's key), or "".
std::string display_problem(const SettingDef& d) {
    const bool number = d.type == SettingType::Int || d.type == SettingType::Float;
    if (d.slider && !number) return "only numbers can be sliders";
    if (d.slider && !(d.has_min && d.has_max)) return "a slider needs \"min\" and \"max\"";
    if (d.step != 0.0) {
        if (!number) return "\"step\" is only for numbers";
        if (!std::isfinite(d.step) || d.step < 0) return "\"step\" must be a positive number";
        if (d.type == SettingType::Int && !whole(d.step)) return "\"step\" of a whole-number setting must be whole";
        if (d.has_min && d.has_max && d.step > d.max - d.min) return "\"step\" is larger than max - min";
    }
    if (!d.options.empty() && d.type != SettingType::Select) return "\"options\" are for select settings";
    if (!d.choices.empty() && d.type == SettingType::Select) return "a select setting takes \"options\", not \"choices\"";
    if (d.max_length && d.type != SettingType::Text) return "\"maxLength\" is for text settings";
    if (d.max_length > 256) return "\"maxLength\" must be 1 to 256";
    if (d.group.size() > 64 || d.group.find_first_of("\r\n") != std::string::npos)
        return "\"group\" must be one line of at most 64 bytes";
    if (!d.when_key.empty() && !valid_setting_key(d.when_key)) return "\"enabledWhen\" names no valid setting key";
    if (!d.when_key.empty() && d.when_key == d.key) return "\"enabledWhen\" cannot name the setting itself";
    return "";
}

// The display fields set back to nothing (a setting refused for one of them still normalises).
void drop_display_fields(SettingDef& d) {
    d.slider = false;
    d.step = 0.0;
    if (d.type != SettingType::Select) d.options.clear();
    d.max_length = 0;
    d.group.clear();
    d.when_key.clear();
    d.when_value.clear();
}
}  // namespace

std::string normalise_setting(SettingDef& d) {
    if (!valid_setting_key(d.key)) return "setting key \"" + d.key + "\" is not valid (1-48 of A-Z a-z 0-9 _ . -)";
    const std::string s = "setting \"" + d.key + "\": ";
    const std::string whole_range = " is out of range (whole numbers go up to 999999999999999 either way)";
    if (d.type == SettingType::Int && d.has_min && std::fabs(d.min) > kMaxWholeSetting) return s + "min" + whole_range;
    if (d.type == SettingType::Int && d.has_max && std::fabs(d.max) > kMaxWholeSetting) return s + "max" + whole_range;
    if (d.key == "enabled") return "setting key \"enabled\" is reserved (it is the mod's on/off switch)";
    if (d.label.empty()) d.label = d.key;
    for (std::string* shown : {&d.label, &d.help, &d.group}) sanitize_utf8(*shown);
    for (auto& o : d.options) sanitize_utf8(o.label);
    if (d.has_min && d.has_max && d.min > d.max) return s + "min is above max";
    // A mistake in a display field refuses the mod for the newer kinds (EMF 0.3.0); for the old kinds only this setting
    // (SettingsStore::declare refuses it with d.problem): those fields were not read before, and the mod loaded.
    const auto soft = [&](const std::string& e) -> bool {
        if (d.strict) return false;
        if (d.problem.empty()) d.problem = s + e;
        return true;
    };
    if (const std::string e = display_problem(d); !e.empty()) {
        if (!soft(e)) return s + e;
        drop_display_fields(d);
    }
    if (d.type == SettingType::Select) {
        if (d.options.empty()) return s + "a select setting needs \"options\"";
        for (size_t i = 0; i < d.options.size(); ++i) {
            SettingOption& o = d.options[i];
            if (o.value.empty()) return s + "an option's value is empty";
            if (o.value.size() > 64) return s + "option \"" + o.value + "\" is longer than 64 bytes";
            if (const std::string p = text_problem(o.value); !p.empty()) return s + "option \"" + o.value + "\": " + p;
            if (o.label.empty()) o.label = o.value;
            if (o.label.size() > 128 || o.label.find_first_of("\r\n") != std::string::npos)
                return s + "the label of option \"" + o.value + "\" must be one line of at most 128 bytes";
            for (size_t k = 0; k < i; ++k)
                if (lower(d.options[k].value) == lower(o.value)) return s + "option \"" + o.value + "\" is listed twice";
        }
    }
    std::string& v = d.default_value;
    switch (d.type) {
        case SettingType::Bool:
            if (v.empty() || v == "0" || v == "false") v = "0";
            else if (v == "1" || v == "true") v = "1";
            else return s + "default must be true or false";
            break;
        case SettingType::Int:
        case SettingType::Float: {
            double x = 0.0;
            if (v.empty()) x = d.has_min ? d.min : 0.0;
            else if (!parse_double(v, x)) return s + "default is not a number";
            if (d.type == SettingType::Int && x != std::floor(x)) return s + "default is not a whole number";
            if (d.type == SettingType::Int && std::fabs(x) > kMaxWholeSetting) return s + "default" + whole_range;
            if ((d.has_min && x < d.min) || (d.has_max && x > d.max)) return s + "default is outside min/max";
            if (!on_step(d, x)) {
                const std::string e = "default " + number_text(x) + " is not " + (d.has_min ? number_text(d.min) : "0") +
                                      " plus whole steps of " + number_text(d.step);
                if (!soft(e)) return s + e;
                drop_display_fields(d);
            }
            v = number_text(x);
            break;
        }
        case SettingType::Choice: {
            if (d.choices.empty()) return s + "a choice needs \"choices\"";
            if (v.empty()) {
                v = "0";
                break;
            }
            double x = 0.0;
            if (parse_double(v, x) && x == std::floor(x)) {
                if (x < 0 || x >= static_cast<double>(d.choices.size())) return s + "default is not one of the choices";
                v = number_text(x);
                break;
            }
            bool found = false;
            for (size_t i = 0; i < d.choices.size(); ++i)
                if (d.choices[i] == v) {
                    v = std::to_string(i);
                    found = true;
                    break;
                }
            if (!found) return s + "default is not one of the choices";
            break;
        }
        case SettingType::Select: {
            if (v.empty()) {
                v = d.options.front().value;
                break;
            }
            const SettingOption* o = find_option(d, v);
            if (!o) return s + "default is not one of the options";
            v = o->value;
            break;
        }
        case SettingType::Color: {
            if (v.empty()) {
                v = "#ffffff";
                break;
            }
            auto c = parse_color(v);
            if (!c) return s + "default must be a colour like \"#ff8800\"";
            v = *c;
            break;
        }
        case SettingType::Key:
            // a default that is no key refuses only this setting, when it is declared (SettingsStore::declare): a
            // manifest with one loaded before EMF 0.3.0 and still does
            if (v.size() > 256) return s + "default is longer than 256 bytes";
            break;
        case SettingType::Text:
            if (v.size() > 256) return s + "default is longer than 256 bytes";
            if (!valid_utf8(v)) {  // a mod.json saved in another encoding: before EMF 0.3.0 it loaded
                if (!soft("default is not valid UTF-8 text (save mod.json as UTF-8)"))
                    return s + "default is not valid UTF-8 text (save mod.json as UTF-8)";
                v.clear();
            }
            if (d.max_length && utf8_chars(v) > d.max_length) {
                const std::string e = "default is longer than maxLength (" + std::to_string(d.max_length) + " characters)";
                if (!soft(e)) return s + e;
                drop_display_fields(d);
            }
            break;
    }
    return "";
}

std::vector<size_t> setting_display_order(const std::vector<SettingDef>& defs) {
    std::vector<size_t> out;
    std::vector<std::string> groups;  // in order of first appearance
    for (size_t i = 0; i < defs.size(); ++i) {
        if (defs[i].group.empty()) {
            out.push_back(i);
            continue;
        }
        bool seen = false;
        for (const auto& g : groups) seen = seen || g == defs[i].group;
        if (!seen) groups.push_back(defs[i].group);
    }
    for (const auto& g : groups)
        for (size_t i = 0; i < defs.size(); ++i)
            if (defs[i].group == g) out.push_back(i);
    return out;
}

bool setting_active(const SettingDef& def, const std::function<std::string(const std::string&)>& value_of) {
    return def.when_key.empty() || value_of(def.when_key) == def.when_value;
}

std::string setting_shown_value(const SettingDef& d, const std::string& v) {
    switch (d.type) {
        case SettingType::Bool: return v == "1" ? "on" : "off";
        case SettingType::Choice: {
            const size_t i = static_cast<size_t>(std::strtoul(v.c_str(), nullptr, 10));
            return i < d.choices.size() ? d.choices[i] : v;
        }
        case SettingType::Select:
            for (const auto& o : d.options)
                if (o.value == v) return o.label;
            return v;
        case SettingType::Key: return v.empty() ? "no key" : v;
        default: return v;
    }
}

std::string setting_condition_text(const SettingDef& def, const std::vector<SettingDef>& defs) {
    if (def.when_key.empty()) return "";
    for (const auto& d : defs)
        if (d.key == def.when_key) return "only while " + d.label + " is " + setting_shown_value(d, def.when_value);
    return "only while " + def.when_key + " is " + def.when_value;
}

namespace {

using json::Value;
using VT = Value::Type;

// A JSON value as setting text: true/false as 1/0, numbers short, text as it is; nullopt for anything else.
std::optional<std::string> value_text(const Value& v) {
    if (v.is(VT::Bool)) return std::string(v.b ? "1" : "0");
    if (v.is(VT::Number)) return number_text(v.n);
    if (v.is(VT::String)) return v.s;
    return std::nullopt;
}

// A field that is there and not null (null counts as left out, as in most JSON tools).
const Value* field(const Value& obj, const char* key) {
    const Value* v = obj.get(key);
    return v && !v->is(VT::Null) ? v : nullptr;
}

// Reads one entry of "settings". *when_kind tells what JSON gave "equals" (0 text, 1 number, 2 true/false).
std::string read_setting(const Value& s, SettingDef& d, int* when_kind) {
    if (!s.is(VT::Object)) return "each entry of \"settings\" must be an object";
    const Value* key = field(s, "key");
    if (!key || !key->is(VT::String)) return "a setting has no \"key\"";
    d.key = key->s;
    const std::string where = "setting \"" + d.key + "\": ";
    const Value* type = field(s, "type");
    const std::string t = type && type->is(VT::String) ? type->s : "";
    if (t == "bool") d.type = SettingType::Bool;
    else if (t == "int") d.type = SettingType::Int;
    else if (t == "float") d.type = SettingType::Float;
    else if (t == "slider") d.slider = true;  // whole numbers or not: decided below from step, min, max and default
    else if (t == "choice") d.type = SettingType::Choice;
    else if (t == "select") d.type = SettingType::Select;
    else if (t == "key" || t == "keybind") d.type = SettingType::Key;
    else if (t == "color") d.type = SettingType::Color;
    else if (t == "text") d.type = SettingType::Text;
    else return where + "\"type\" must be bool, int, float, slider, choice, select, keybind, color or text (or key)";
    d.strict = t == "slider" || t == "select" || t == "keybind" || t == "color";
    // a field of the wrong shape: refuses the mod for a new kind, only this setting for an old one (see normalise)
    std::string hard;
    const auto soft = [&](const std::string& e) {
        if (d.strict) {
            if (hard.empty()) hard = where + e;
        } else if (d.problem.empty()) {
            d.problem = where + e;
        }
    };
    if (const Value* v = field(s, "label"); v && v->is(VT::String)) d.label = v->s;
    if (const Value* v = field(s, "help"); v && v->is(VT::String)) d.help = v->s;
    if (const Value* v = field(s, "live"); v && v->is(VT::Bool)) d.live = v->b;
    if (const Value* v = field(s, "min"); v && v->is(VT::Number)) {
        d.min = v->n;
        d.has_min = true;
    }
    if (const Value* v = field(s, "max"); v && v->is(VT::Number)) {
        d.max = v->n;
        d.has_max = true;
    }
    if (const Value* v = field(s, "step")) {
        if (v->is(VT::Number)) d.step = v->n;
        else soft("\"step\" must be a number");
    }
    if (const Value* v = field(s, "control")) {
        const bool number = d.type == SettingType::Int || d.type == SettingType::Float || d.slider;
        if (!v->is(VT::String) || v->s != "slider") soft("\"control\" is \"slider\" or left out");
        else if (!number) soft("\"control\": \"slider\" is for int and float settings");
        else if (!d.slider) d.slider = true;  // int or float as written, shown as a slider
    }
    if (const Value* v = field(s, "choices")) {
        if (!v->is(VT::Array)) return where + "\"choices\" must be a list of texts";
        for (const Value& c : v->arr) {
            if (!c.is(VT::String)) return where + "\"choices\" must be a list of texts";
            d.choices.push_back(c.s);
        }
    }
    if (const Value* v = field(s, "options")) {
        const char* shape = "\"options\" must be a list: [\"a\", \"b\"] or [{\"value\": \"a\", \"label\": \"A\"}]";
        if (d.type != SettingType::Select) {
            soft("\"options\" are for select settings");
        } else if (!v->is(VT::Array)) {
            return where + shape;
        } else {
            for (const Value& o : v->arr) {
                SettingOption opt;
                if (o.is(VT::String)) {
                    opt.value = o.s;
                } else if (o.is(VT::Object)) {
                    const Value* val = field(o, "value");
                    if (!val || !value_text(*val)) return where + "each option needs a \"value\"";
                    opt.value = *value_text(*val);
                    if (const Value* l = field(o, "label"); l && l->is(VT::String)) opt.label = l->s;
                } else {
                    return where + "each option is a text or {\"value\": ..., \"label\": ...}";
                }
                d.options.push_back(std::move(opt));
            }
        }
    }
    if (const Value* v = field(s, "maxLength")) {
        if (!v->is(VT::Number) || !whole(v->n) || v->n < 1 || v->n > 256) soft("\"maxLength\" must be a whole number from 1 to 256");
        else d.max_length = static_cast<uint32_t>(v->n);
    }
    if (const Value* v = field(s, "group")) {
        if (v->is(VT::String)) d.group = v->s;
        else soft("\"group\" must be a text (the heading)");
    }
    if (const Value* v = field(s, "enabledWhen")) {
        const Value* other = v->is(VT::Object) ? field(*v, "setting") : nullptr;
        const Value* equals = v->is(VT::Object) ? field(*v, "equals") : nullptr;
        if (!other || !other->is(VT::String) || !equals || !value_text(*equals)) {
            soft("\"enabledWhen\" must be {\"setting\": \"<another key>\", \"equals\": <its value>}");
        } else {
            d.when_key = other->s;
            d.when_value = *value_text(*equals);
            if (when_kind) *when_kind = equals->is(VT::Bool) ? 2 : equals->is(VT::Number) ? 1 : 0;
        }
    }
    if (const Value* v = field(s, "default")) {
        if (!value_text(*v)) return where + "\"default\" must be a value";
        d.default_value = *value_text(*v);
    }
    if (!hard.empty()) return hard;
    if (t == "slider") {  // whole numbers only with a whole step (and whole ends and default); else decimals
        double x = 0.0;
        const bool whole_default = d.default_value.empty() || (parse_double(d.default_value, x) && whole(x));
        const bool ints = d.step > 0 && whole(d.step) && (!d.has_min || whole(d.min)) && (!d.has_max || whole(d.max)) &&
                          whole_default;
        d.type = ints ? SettingType::Int : SettingType::Float;
    }
    return normalise_setting(d);
}

std::string read_dependency(const std::string& id, const Value* version, const Value* optional, Dependency& d) {
    d.id = id;
    if (!valid_mod_id(id)) return "\"depends\": \"" + id + "\" is not a valid mod id (1-48 of a-z 0-9 _ . -)";
    if (version) {
        if (!version->is(VT::String)) return "\"depends\": \"" + id + "\": the version must be a text such as \"^1.2.0\"";
        std::string why;
        auto c = parse_constraint(version->s, &why);
        if (!c) return "\"depends\": \"" + id + "\": " + why;
        d.version = *c;
    }
    if (optional) {
        if (!optional->is(VT::Bool)) return "\"depends\": \"" + id + "\": \"optional\" must be true or false";
        d.optional = optional->b;
    }
    return "";
}

std::string read_depends(const Value& deps, Manifest& m) {
    if (deps.is(VT::Object)) {
        for (const auto& [dep_id, v] : deps.obj) {
            Dependency d;
            std::string e;
            if (v.is(VT::String)) e = read_dependency(dep_id, &v, nullptr, d);
            else if (v.is(VT::Object)) e = read_dependency(dep_id, field(v, "version"), field(v, "optional"), d);
            else e = "\"depends\": \"" + dep_id + "\" needs a version range (\"^1.2.0\", \"*\") or {\"version\": ..., \"optional\": true}";
            if (!e.empty()) return e;
            m.depends.push_back(std::move(d));
        }
    } else if (deps.is(VT::Array)) {
        for (const Value& v : deps.arr) {
            Dependency d;
            std::string e;
            if (v.is(VT::String)) e = read_dependency(v.s, nullptr, nullptr, d);
            else if (v.is(VT::Object) && field(v, "id") && field(v, "id")->is(VT::String))
                e = read_dependency(field(v, "id")->s, field(v, "version"), field(v, "optional"), d);
            else e = "\"depends\": each entry is a mod id or {\"id\": ..., \"version\": ..., \"optional\": true}";
            if (!e.empty()) return e;
            m.depends.push_back(std::move(d));
        }
    } else {
        return "\"depends\" must be an object ({\"mod_id\": \"^1.0\"}) or a list ([{\"id\": \"mod_id\", \"version\": \"^1.0\"}])";
    }
    for (size_t i = 0; i < m.depends.size(); ++i) {
        if (m.depends[i].id == m.id) return "\"depends\": a mod cannot depend on itself (\"" + m.id + "\")";
        for (size_t k = 0; k < i; ++k)
            if (m.depends[k].id == m.depends[i].id) return "\"depends\": \"" + m.depends[i].id + "\" is listed twice";
    }
    return "";
}

// SettingsStore::declare would refuse this setting (its problem, or a key default that is no key): its reason, or "".
std::string refusal(const SettingDef& d) {
    if (!d.problem.empty()) return d.problem;
    if (d.type == SettingType::Key && !d.default_value.empty() && !setting_value(d, d.default_value, false, nullptr))
        return "setting \"" + d.key + "\": the default is not a key";
    return "";
}

// "enabledWhen" names another setting of the same mod, and asks for one of its values, as written (true only for a
// switch, a number the setting can hold without being moved); it is stored normalised. A condition on a setting that
// will be refused is dropped with a warning, so the setting is not greyed out for good.
std::string check_conditions(std::vector<SettingDef>& settings, const std::vector<int>& when_kinds) {
    for (size_t i = 0; i < settings.size(); ++i) {
        SettingDef& d = settings[i];
        if (d.when_key.empty()) continue;
        const std::string where = "setting \"" + d.key + "\": \"enabledWhen\"";
        const auto soft = [&](const std::string& e) -> std::string {
            if (d.strict) return e;
            if (d.problem.empty()) d.problem = e;
            d.when_key.clear();
            d.when_value.clear();
            return "";
        };
        const SettingDef* other = nullptr;
        for (const auto& o : settings)
            if (o.key == d.when_key) other = &o;
        if (!other) {
            if (auto e = soft(where + " names \"" + d.when_key + "\", which is not one of this mod's settings"); !e.empty()) return e;
            continue;
        }
        if (const std::string why = refusal(*other); !why.empty()) {
            d.warning = where + " names \"" + other->key + "\", which is refused (" + why +
                        "); the setting shows without the condition";
            d.when_key.clear();
            d.when_value.clear();
            continue;
        }
        const int kind = i < when_kinds.size() ? when_kinds[i] : 0;
        const std::string raw = kind == 2 ? (d.when_value == "1" ? "true" : "false")
                                : kind == 1 ? d.when_value
                                            : "\"" + d.when_value + "\"";
        const std::string head = where + ": " + raw + " is not a value of \"" + d.when_key + "\"";
        if (kind == 2 && other->type != SettingType::Bool) {
            if (auto e = soft(head + " (true and false are for switches)"); !e.empty()) return e;
            continue;
        }
        std::string why;
        auto v = setting_value(*other, d.when_value, false, &why);
        if (!v) {
            if (auto e = soft(head + " (" + why + ")"); !e.empty()) return e;
            continue;
        }
        double asked = 0.0, got = 0.0;
        const bool number = other->type == SettingType::Int || other->type == SettingType::Float;
        if (number && parse_double(d.when_value, asked) && parse_double(*v, got) && asked != got) {
            if (auto e = soft(head + " (it would be " + *v + ")"); !e.empty()) return e;
            continue;
        }
        d.when_value = *v;
    }
    return "";
}

}  // namespace

ManifestResult parse_manifest(std::string_view json_text) {
    ManifestResult r;
    if (json_text.size() > kMaxManifestBytes) {
        r.error = "mod.json is larger than 256 KB";
        return r;
    }
    const auto byte = [&](size_t i) { return i < json_text.size() ? static_cast<unsigned char>(json_text[i]) : 0x100u; };
    const bool bom16 = (byte(0) == 0xFF && byte(1) == 0xFE) || (byte(0) == 0xFE && byte(1) == 0xFF);
    if (bom16 || json_text.substr(0, 4).find('\0') != std::string_view::npos) {  // no NUL in UTF-8 JSON
        r.error = "mod.json is saved as UTF-16 (\"Unicode\"): save mod.json as UTF-8 (Notepad: Save as, Encoding UTF-8)";
        return r;
    }
    auto parsed = json::parse(json_text);
    if (!parsed.value) {
        r.error = "mod.json is not valid JSON: " + parsed.error;
        return r;
    }
    const Value& root = *parsed.value;
    if (!root.is(VT::Object)) {
        r.error = "mod.json must hold an object ({...})";
        return r;
    }
    Manifest m;
    const Value* id = root.get("id");
    if (!id || !id->is(VT::String) || !valid_mod_id(id->s)) {
        r.error = "\"id\" is missing or not valid (1-48 of a-z 0-9 _ . -)";
        return r;
    }
    m.id = id->s;
    const Value* ver = root.get("version");
    if (!ver || !ver->is(VT::String)) {
        r.error = "\"version\" is missing (e.g. \"1.0.0\")";
        return r;
    }
    auto v = parse_version(ver->s);
    if (!v) {
        r.error = "\"version\" must be numbers like \"1.2.3\"";
        return r;
    }
    m.version = *v;
    m.version_text = ver->s;
    m.name = m.id;
    if (const Value* x = root.get("name"); x && x->is(VT::String) && !x->s.empty()) m.name = x->s;
    if (const Value* x = root.get("description"); x && x->is(VT::String)) m.description = x->s;
    if (const Value* x = root.get("authors"); x && x->is(VT::Array))
        for (const Value& a : x->arr)
            if (a.is(VT::String)) m.authors.push_back(a.s);
    if (const Value* x = root.get("emf")) {
        if (!x->is(VT::Number) || x->n < 0 || x->n > 1000000 || x->n != std::floor(x->n)) {
            r.error = "\"emf\" must be a whole number: the lowest EMF API version the mod needs (e.g. 16)";
            return r;
        }
        m.emf = static_cast<uint32_t>(x->n);
    }
    if (const Value* x = root.get("enabled"); x && x->is(VT::Bool)) m.enabled_default = x->b;
    const Value* dll = root.get("dll");
    const Value* main = root.get("main");
    if (dll && main) {
        r.error = "give either \"dll\" or \"main\", not both";
        return r;
    }
    if (main) {
        if (!main->is(VT::String) || !safe_relative_file(main->s)) {
            r.error = "\"main\" must be a file inside the mod's folder";
            return r;
        }
        m.main = main->s;
    } else if (dll) {
        if (!dll->is(VT::String) || !safe_relative_file(dll->s)) {
            r.error = "\"dll\" must be a file inside the mod's folder";
            return r;
        }
        m.dll = dll->s;
    } else {
        m.dll = m.id + ".dll";
    }
    {
        const Value* deps = field(root, "depends");
        const Value* sf = field(root, "dependencies");  // ShroudForge's (and EML's) name for the same list
        if (deps && sf) {
            r.error = "give either \"depends\" or \"dependencies\" (ShroudForge's name for it), not both";
            return r;
        }
        if (deps || sf) {
            std::string e = read_depends(deps ? *deps : *sf, m);
            if (!e.empty() && sf && e.rfind("\"depends\"", 0) == 0) e = "\"dependencies\"" + e.substr(9);
            if (!e.empty()) {
                r.error = e;
                return r;
            }
        }
    }
    if (const Value* settings = root.get("settings")) {
        if (!settings->is(VT::Array)) {
            r.error = "\"settings\" must be a list";
            return r;
        }
        std::vector<int> when_kinds;
        for (const Value& s : settings->arr) {
            SettingDef d;
            int when_kind = 0;
            std::string e = read_setting(s, d, &when_kind);
            if (e.empty())
                for (const auto& other : m.settings)
                    if (other.key == d.key) e = "setting \"" + d.key + "\" is declared twice";
            if (!e.empty()) {
                r.error = "\"settings\": " + e;
                return r;
            }
            m.settings.push_back(std::move(d));
            when_kinds.push_back(when_kind);
        }
        if (std::string e = check_conditions(m.settings, when_kinds); !e.empty()) {
            r.error = "\"settings\": " + e;
            return r;
        }
    }
    r.manifest = std::move(m);
    return r;
}

}  // namespace emf::core
