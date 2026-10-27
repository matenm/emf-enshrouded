#include "nexus/nxm.h"

#include <cctype>
#include <vector>

namespace emf::nexus {

namespace {

constexpr size_t kMaxUrl = 2048;
constexpr size_t kMaxKey = 256;

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

bool all_digits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

// A positive id of at most 12 digits, or 0.
int64_t id_of(std::string_view s) {
    if (!all_digits(s) || s.size() > 12) return 0;
    int64_t v = 0;
    for (char c : s) v = v * 10 + (c - '0');
    return v;
}

int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// %XX decoded ('+' stays '+': keys are base64 and a '+' in them is literal). False on a broken escape.
bool percent_decode(std::string_view in, std::string& out) {
    out.clear();
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '%') {
            out += in[i];
            continue;
        }
        if (i + 2 >= in.size()) return false;
        const int a = hex(in[i + 1]), b = hex(in[i + 2]);
        if (a < 0 || b < 0) return false;
        out += static_cast<char>(a * 16 + b);
        i += 2;
    }
    return true;
}

// The characters an nxm key has (base64 and URL-safe base64, plus '.' and '~').
bool key_ok(const std::string& key) {
    if (key.empty() || key.size() > kMaxKey) return false;
    for (unsigned char c : key)
        if (!(std::isalnum(c) || c == '-' || c == '_' || c == '+' || c == '/' || c == '=' || c == '.' || c == '~')) return false;
    return true;
}

std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> parts;
    size_t from = 0;
    for (;;) {
        const size_t at = s.find(sep, from);
        parts.push_back(s.substr(from, at == std::string_view::npos ? std::string_view::npos : at - from));
        if (at == std::string_view::npos) break;
        from = at + 1;
    }
    return parts;
}

NxmParse fail(std::string why) {
    NxmParse r;
    r.error = std::move(why);
    return r;
}

}  // namespace

NxmParse parse_nxm(std::string_view url, std::string_view game) {
    while (!url.empty() && (url.front() == ' ' || url.front() == '\t')) url.remove_prefix(1);
    while (!url.empty() && (url.back() == ' ' || url.back() == '\t' || url.back() == '\r' || url.back() == '\n'))
        url.remove_suffix(1);
    const std::string_view scheme = "nxm://";
    if (url.size() < scheme.size()) return fail("That is not an nxm:// link.");
    for (size_t i = 0; i < scheme.size(); ++i)
        if (lower(url[i]) != scheme[i]) return fail("That is not an nxm:// link.");
    if (url.size() > kMaxUrl) return fail("That nxm:// link is too long.");
    for (unsigned char c : url)
        if (c < 0x21 || c >= 0x7F) return fail("That nxm:// link has characters a link never has.");
    std::string_view rest = url.substr(scheme.size());
    if (const size_t hash = rest.find('#'); hash != std::string_view::npos) rest = rest.substr(0, hash);
    std::string_view query;
    if (const size_t q = rest.find('?'); q != std::string_view::npos) {
        query = rest.substr(q + 1);
        rest = rest.substr(0, q);
    }
    if (!rest.empty() && rest.back() == '/') rest.remove_suffix(1);
    const auto parts = split(rest, '/');
    // only the path is ever repeated in a message, never the query (it holds the key)
    const std::string shown = "nxm://" + std::string(rest);

    std::string domain;
    for (char c : parts[0]) domain += lower(c);
    if (domain.empty()) return fail("This nxm:// link names no game: " + shown);
    for (char c : domain)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return fail("EMF does not understand this nxm:// link: " + shown);
    if (domain != game) {
        NxmParse r = fail("This link is for another game (" + domain + "). EMF installs Enshrouded mods only: open it " +
                          "with the mod manager for that game.");
        r.other_game = true;
        return r;
    }
    if (parts.size() >= 2 && parts[1] == "collections")
        return fail("EMF does not install Nexus collections: download their mods one by one.");
    if (parts.size() != 5 || parts[1] != "mods" || parts[3] != "files")
        return fail("EMF does not understand this nxm:// link: " + shown);
    NxmLink link;
    link.game = domain;
    link.mod_id = id_of(parts[2]);
    link.file_id = id_of(parts[4]);
    if (link.mod_id <= 0 || link.file_id <= 0) return fail("This nxm:// link has no valid mod and file numbers: " + shown);

    if (!query.empty())
        for (std::string_view pair : split(query, '&')) {
            if (pair.empty()) continue;
            const size_t eq = pair.find('=');
            const std::string_view name = pair.substr(0, eq);
            const std::string_view raw = eq == std::string_view::npos ? std::string_view() : pair.substr(eq + 1);
            std::string value;
            if (!percent_decode(raw, value)) return fail("This nxm:// link has a broken parameter: " + shown);
            if (name == "key") {
                if (!key_ok(value)) return fail("This nxm:// link's key is not valid: click Mod Manager Download on Nexus again.");
                link.key = value;
            } else if (name == "expires") {
                if (!all_digits(value) || value.size() > 12) return fail("This nxm:// link's expiry is not valid: " + shown);
                link.expires = id_of(value);
            } else if (name == "user_id") {
                if (!all_digits(value) || value.size() > 12) return fail("This nxm:// link's user is not valid: " + shown);
                link.user_id = id_of(value);
            }
        }
    NxmParse r;
    r.link = std::move(link);
    return r;
}

bool nxm_expired(const NxmLink& link, int64_t now_unix) { return link.expires > 0 && now_unix >= link.expires; }

std::string nexus_file_page(int64_t mod_id, int64_t file_id) {
    return "https://www.nexusmods.com/" + std::string(kGameDomain) + "/mods/" + std::to_string(mod_id) +
           "?tab=files&file_id=" + std::to_string(file_id);
}

}  // namespace emf::nexus
