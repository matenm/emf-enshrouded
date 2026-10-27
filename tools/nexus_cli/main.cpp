// emf_nexus: Nexus Mods' public API from the command line (src/nexus/), for checks by hand. Each command is one
// request (a long archive listing takes a few), so use it like a player would, not in loops.
//   emf_nexus search <text> [count] [--sort downloads|endorsements|updated|created|name] [--asc]
//   emf_nexus mod <modId> [--html]          the mod's page; --html prints the whole description as HTML
//   emf_nexus files <modId>
//   emf_nexus contents <modId> <fileId> [--owner <proxy.dll>=<tool word> ...]   paths and what the file is
//   emf_nexus classify <listing.json> [--owner ...]   the same for a saved modFileContents reply (no network)
//   emf_nexus nxm <link>                    what an nxm:// link says (no network; the key is not printed)
//   emf_nexus sso-selftest ws://127.0.0.1:<port>/ [seconds]   "Log in with Nexus" against a local fake of the login
//                                           service (scripts/sso_selftest.ps1), with a time limit; never Nexus itself
// Exit: 0 = done; 1 = Nexus or the network failed; 2 = usage or an unreadable file.
#include <windows.h>

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "nexus/api.h"
#include "nexus/bbcode.h"
#include "nexus/classify.h"
#include "nexus/client.h"
#include "nexus/nxm.h"
#include "nexus/sso.h"

using namespace emf::nexus;

namespace {

std::string utf8(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return "";
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring wide(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

int usage() {
    std::fputs("usage:\n"
               "  emf_nexus search <text> [count] [--sort downloads|endorsements|updated|created|name] [--asc]\n"
               "  emf_nexus mod <modId> [--html]\n"
               "  emf_nexus files <modId>\n"
               "  emf_nexus contents <modId> <fileId> [--owner <proxy.dll>=<tool word> ...]\n"
               "  emf_nexus classify <listing.json> [--owner <proxy.dll>=<tool word> ...]\n"
               "  emf_nexus nxm <nxm://link>\n"
               "  emf_nexus sso-selftest ws://127.0.0.1:<port>/ [seconds]\n",
               stderr);
    return 2;
}

bool to_id(const std::string& s, int64_t& out) {
    if (s.empty() || s.size() > 12) return false;
    out = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        out = out * 10 + (c - '0');
    }
    return out > 0;
}

std::string size_text(uint64_t b) {
    char buf[32];
    if (b < 1024) std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(b));
    else if (b < 1024 * 1024) std::snprintf(buf, sizeof buf, "%.1f KiB", b / 1024.0);
    else std::snprintf(buf, sizeof buf, "%.1f MiB", b / (1024.0 * 1024.0));
    return buf;
}

std::string date_text(int64_t unix_seconds) {
    const std::time_t t = static_cast<std::time_t>(unix_seconds);
    std::tm tm{};
    if (gmtime_s(&tm, &t) != 0) return "?";
    char buf[16];
    std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
    return buf;
}

std::string shorten(const std::string& s, size_t max) {
    if (s.size() <= max) return s;
    size_t cut = max;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut) + "...";
}

int failed(const std::string& error) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return 1;
}

void print_classification(const Classification& c) {
    std::printf("Kind: %s [%s]\n", kind_label(c.kind), kind_id(c.kind));
    if (!c.also.empty()) {
        std::printf("Also:");
        for (auto k : c.also) std::printf(" %s;", kind_label(k));
        std::printf("\n");
    }
    if (!c.loader.empty()) std::printf("Loader it brings: %s\n", c.loader.c_str());
    if (!c.strip_prefix.empty()) std::printf("Wrapper folder: %s\n", c.strip_prefix.c_str());
    for (const auto& m : c.mods)
        std::printf("Mod folder: %s \"%s\" at \"%s\"\n", kind_id(m.kind), m.id.c_str(), m.path.c_str());
    if (!c.proxy_dlls.empty()) {
        std::printf("Proxy DLLs:");
        for (const auto& d : c.proxy_dlls) std::printf(" %s", d.c_str());
        std::printf("\n");
    }
    std::printf("Notes:\n");
    for (const auto& n : c.notes) std::printf("  - %s\n", n.c_str());
}

void print_entries(const ContentsPage& p) {
    std::printf("%zu entries (Nexus counts %lld)\n", p.entries.size(), static_cast<long long>(p.total));
    size_t shown = 0;
    for (const auto& e : p.entries) {
        if (shown++ == 60) {
            std::printf("  ... and %zu more\n", p.entries.size() - 60);
            break;
        }
        std::printf("  %-70s %10s\n", e.path.c_str(), size_text(e.size).c_str());
    }
}

}  // namespace

int wmain(int argc, wchar_t** wargv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(utf8(wargv[i]));
    if (args.empty()) return usage();

    // --owner name=word (several), --sort x, --asc, --html
    GameState game;
    SearchQuery q;
    bool html = false;
    std::vector<std::string> pos;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--owner" && i + 1 < args.size()) {
            const std::string& v = args[++i];
            const size_t eq = v.find('=');
            if (eq == std::string::npos) return usage();
            game.proxies.emplace_back(v.substr(0, eq), v.substr(eq + 1));
            game.known = true;
        } else if (a == "--sort" && i + 1 < args.size()) {
            const auto s = sort_from_name(args[++i]);
            if (!s) return usage();
            q.sort = *s;
            if (*s == SortBy::Name) q.descending = false;
        } else if (a == "--asc") {
            q.descending = false;
        } else if (a == "--html") {
            html = true;
        } else {
            pos.push_back(a);
        }
    }
    if (pos.empty()) return usage();
    const std::string cmd = pos[0];
    const Client client(EMF_VERSION);
    int64_t mod_id = 0, file_id = 0;

    if (cmd == "search" && (pos.size() == 2 || pos.size() == 3)) {
        q.text = pos[1];
        int64_t count = 20;
        if (pos.size() == 3 && !to_id(pos[2], count)) return usage();
        q.count = static_cast<int>(count);
        const auto r = client.search(q);
        if (!r.ok()) return failed(r.error);
        std::printf("%lld mods match \"%s\" (showing %zu)\n", static_cast<long long>(r.value.total), q.text.c_str(),
                    r.value.mods.size());
        for (const auto& m : r.value.mods) {
            std::printf("#%-5lld %s %s  by %s%s\n", static_cast<long long>(m.id), m.name.c_str(), m.version.c_str(),
                        m.author.empty() ? m.uploader.c_str() : m.author.c_str(), m.adult ? "  [adult]" : "");
            std::printf("       %lld downloads, %lld endorsements, %s, updated %s\n",
                        static_cast<long long>(m.downloads), static_cast<long long>(m.endorsements),
                        m.category.c_str(), m.updated_at.substr(0, 10).c_str());
            std::printf("       %s\n", shorten(m.summary, 150).c_str());
        }
        return 0;
    }
    if (cmd == "mod" && pos.size() == 2 && to_id(pos[1], mod_id)) {
        const auto r = client.mod(mod_id);
        if (!r.ok()) return failed(r.error);
        const auto& m = r.value;
        std::printf("#%lld %s %s by %s (uploaded by %s)\n%s\n%lld downloads, %lld endorsements, %s\n"
                    "created %s, updated %s\npicture: %s\n\n",
                    static_cast<long long>(m.id), m.name.c_str(), m.version.c_str(), m.author.c_str(),
                    m.uploader.c_str(), m.summary.c_str(), static_cast<long long>(m.downloads),
                    static_cast<long long>(m.endorsements), m.category.c_str(), m.created_at.c_str(),
                    m.updated_at.c_str(), m.picture_url.c_str());
        const std::string h = bbcode_to_html(m.description);
        std::printf("Description: %zu characters of BBCode, %zu of HTML\n%s\n", m.description.size(), h.size(),
                    html ? h.c_str() : shorten(h, 600).c_str());
        return 0;
    }
    if (cmd == "files" && pos.size() == 2 && to_id(pos[1], mod_id)) {
        const auto r = client.files(mod_id);
        if (!r.ok()) return failed(r.error);
        std::printf("%zu files of mod %lld\n", r.value.size(), static_cast<long long>(mod_id));
        for (const auto& f : r.value)
            std::printf("  %-6lld %-13s %-45s %-10s %10s  %s%s\n", static_cast<long long>(f.file_id),
                        f.category.c_str(), shorten(f.name, 45).c_str(), f.version.c_str(),
                        size_text(f.size_bytes).c_str(), date_text(f.date).c_str(), f.primary ? "  primary" : "");
        return 0;
    }
    if (cmd == "contents" && pos.size() == 3 && to_id(pos[1], mod_id) && to_id(pos[2], file_id)) {
        const auto r = client.contents(mod_id, file_id);
        if (!r.ok()) return failed(r.error);
        std::printf("Mod %lld, file %lld: ", static_cast<long long>(mod_id), static_cast<long long>(file_id));
        print_entries(r.value);
        print_classification(classify_archive(r.value.entries, game));
        return 0;
    }
    if (cmd == "classify" && pos.size() == 2) {
        std::ifstream f(std::filesystem::path(wide(pos[1])), std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "could not read %s\n", pos[1].c_str());
            return 2;
        }
        std::stringstream s;
        s << f.rdbuf();
        const auto r = parse_contents(s.str());
        if (!r.ok()) return failed(r.error);
        print_entries(r.value);
        print_classification(classify_archive(r.value.entries, game));
        return 0;
    }
    if (cmd == "nxm" && pos.size() == 2) {
        const auto p = parse_nxm(pos[1]);
        if (!p.link) return failed(p.error);
        std::printf("game %s, mod %lld, file %lld, key %s, expires %lld, user %lld\n", p.link->game.c_str(),
                    static_cast<long long>(p.link->mod_id), static_cast<long long>(p.link->file_id),
                    p.link->key.empty() ? "none" : "(set)", static_cast<long long>(p.link->expires),
                    static_cast<long long>(p.link->user_id));
        return 0;
    }
    if (cmd == "sso-selftest" && (pos.size() == 2 || pos.size() == 3)) {
        // the real WebSocket client against a local fake of Nexus's login service: loopback ws:// only
        if (pos[1].rfind("ws://127.0.0.1:", 0) != 0) {
            std::fputs("sso-selftest talks only to ws://127.0.0.1:<port>/ (a local fake of the login service)\n", stderr);
            return 2;
        }
        SsoOptions o;
        o.id = new_sso_id();
        o.slug = "emf-selftest";
        int64_t seconds = 20;
        if (pos.size() == 3 && !to_id(pos[2], seconds)) return usage();
        o.timeout = std::chrono::seconds(seconds);
        o.reconnect_wait = std::chrono::milliseconds(200);
        o.open_browser = [](const std::string& url) {
            std::printf("browser: %s\n", url.c_str());
            std::fflush(stdout);
        };
        const auto r = run_sso(winhttp_sso_sockets(std::string("EMF/") + EMF_VERSION + " (Enshrouded Modding Framework; Windows)", pos[1]), o);
        if (!r.ok) return failed(r.error);
        std::printf("api key: %zu characters, starts %s\n", r.api_key.size(), r.api_key.substr(0, 6).c_str());
        return 0;
    }
    return usage();
}
