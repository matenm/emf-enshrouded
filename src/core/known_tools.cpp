#include "core/known_tools.h"

#include <algorithm>
#include <cctype>

#include "loader/chain_list.h"
#include "loader/loader_id.h"

namespace emf::core {

namespace {
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// A text as a DLL built with wide strings holds it (UTF-16LE; markers are plain ASCII). Lower-casing the DLL's bytes
// lowers these letters too, since every second byte is 0.
std::string utf16(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 2);
    for (const char c : s) {
        out += c;
        out += '\0';
    }
    return out;
}

const KnownTool* find_tool(const std::vector<KnownTool>& tools, const std::string& word) {
    for (const auto& t : tools)
        if (t.word == word) return &t;
    return nullptr;
}

std::string display(const std::vector<KnownTool>& tools, const std::string& word) {
    if (word == "emf") return "EMF";
    if (word.empty()) return "unknown tool";
    const KnownTool* t = find_tool(tools, word);
    return t ? t->name : word;
}

std::vector<std::pair<std::string, std::string>> status_lines(const std::string& chain_status) {
    std::vector<std::pair<std::string, std::string>> out;  // name, how
    size_t from = 0;
    while (from < chain_status.size()) {
        size_t to = chain_status.find('\n', from);
        if (to == std::string::npos) to = chain_status.size();
        const std::string line = trim(chain_status.substr(from, to - from));
        from = to + 1;
        const size_t tab = line.find('\t');
        if (tab != std::string::npos) out.emplace_back(line.substr(0, tab), line.substr(tab + 1));
    }
    return out;
}
}  // namespace

std::vector<KnownTool> parse_known_tools(const std::string& text) {
    std::vector<KnownTool> out;
    size_t from = 0;
    while (from < text.size()) {
        size_t to = text.find('\n', from);
        if (to == std::string::npos) to = text.size();
        const std::string line = trim(text.substr(from, to - from));
        from = to + 1;
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> parts;
        size_t a = 0;
        while (true) {
            const size_t bar = line.find('|', a);
            parts.push_back(trim(line.substr(a, bar == std::string::npos ? std::string::npos : bar - a)));
            if (bar == std::string::npos) break;
            a = bar + 1;
        }
        if (parts.size() < 3 || parts[0].empty() || parts[1].empty()) continue;
        KnownTool t{lower(parts[0]), parts[1], {}, {}, {}, {}, {}};
        for (size_t i = 2; i < parts.size(); ++i) {
            const std::string& p = parts[i];
            if (p.rfind("text:", 0) == 0 && p.size() > 5) t.texts.push_back(lower(p.substr(5)));
            else if (p.rfind("beside:", 0) == 0 && p.size() > 7) t.beside.push_back(p.substr(7));
            else if (p.rfind("same-loader:", 0) == 0 && p.size() > 12) t.same_loader.push_back(lower(p.substr(12)));
            else if (p.rfind("helper:", 0) == 0 && p.size() > 7) t.helpers.push_back(lower(p.substr(7)));
            else if (p.rfind("chain-as:", 0) == 0 && p.size() > 9) t.chain_as = lower(p.substr(9));
        }
        if (t.texts.empty() && t.beside.empty()) continue;
        out.push_back(std::move(t));
    }
    return out;
}

std::string tool_for_dll_text(const std::vector<KnownTool>& tools, const std::string& dll_text) {
    if (dll_text.find(EMF_LOADER_MARKER) != std::string::npos) return "emf";
    if (tools.empty()) return {};
    const std::string low = lower(dll_text);  // once per file: markers are stored in lower case
    for (const auto& t : tools)
        for (const auto& s : t.texts)
            if (low.find(s) != std::string::npos || low.find(utf16(s)) != std::string::npos) return t.word;
    return {};
}

std::vector<std::string> started_chain_names(const std::string& chain_status) {
    std::vector<std::string> out;
    for (const auto& [name, how] : status_lines(chain_status))
        if (how == "started") out.push_back(name);
    return out;
}

std::vector<std::string> installed_report(const std::vector<KnownTool>& tools, const GameFolderView& view) {
    std::vector<std::string> lines;
    std::vector<std::string> active = view.chained_tools;  // tools with a DLL that runs
    const auto helper = [&](const KnownTool& t, const std::string& name) {
        return std::find(t.helpers.begin(), t.helpers.end(), lower(name)) != t.helpers.end();
    };
    std::string files;
    for (const auto& [name, word] : view.proxies) {
        const KnownTool* t = find_tool(tools, word);
        if (!t || !helper(*t, name)) active.push_back(word);  // a helper file does not start the tool alone
        files += (files.empty() ? "" : ", ") + name + " = " + display(tools, word);
    }
    if (!files.empty()) lines.push_back("next to the game: " + files);

    std::string started, skipped;
    for (const auto& [name, how] : status_lines(view.chain_status)) {
        if (how == "started") started += (started.empty() ? "" : ", ") + name;
        else skipped += (skipped.empty() ? "" : ", ") + name + " (" + how + ")";
    }
    if (!started.empty() || !skipped.empty()) {
        std::string s = "started by EMF (dinput8-chain.ini): " + (started.empty() ? std::string("none") : started);
        if (!skipped.empty()) s += "; listed but not started: " + skipped;
        lines.push_back(s);
    }

    // what ModLoader Proxy starts (chainloader.ini) runs too: EMF's loader itself, ShroudForge, ...
    std::string by_proxy, missing;
    for (const auto& d : view.loader_started) {
        if (!d.found) {
            missing += (missing.empty() ? "" : ", ") + d.name;
            continue;
        }
        by_proxy += by_proxy.empty() ? "" : ", ";
        if (d.outside) {
            by_proxy += d.name + " (outside the game folder, not read)";
            continue;
        }
        active.push_back(d.word);
        by_proxy += d.name + " = " + display(tools, d.word);
    }
    if (!view.loader_started.empty()) {
        std::string s = "started by " + display(tools, kModLoaderProxy) +
                        " (chainloader.ini): " + (by_proxy.empty() ? std::string("none") : by_proxy);
        if (!missing.empty()) s += "; listed but not found: " + missing;
        lines.push_back(s);
    }

    for (const auto& t : tools) {
        if (t.texts.empty()) continue;  // its DLLs cannot be told apart: nothing to compare
        std::string here;
        for (const auto& b : t.beside)
            if (std::find(view.beside.begin(), view.beside.end(), b) != view.beside.end()) here = b;
        if (here.empty() || std::find(active.begin(), active.end(), t.word) != active.end()) continue;
        // which known tool holds a name its loader wants ("EML and ShroudForge both want dbghelp.dll")
        std::string why = "another tool has its file name";
        for (const auto& want : t.same_loader)
            for (const auto& [name, word] : view.proxies)
                if (lower(name) == want && !word.empty() && word != t.word)
                    why = display(tools, word) + " has " + name + ", which both want";
        std::string helper_note;
        for (const auto& [name, word] : view.proxies)
            if (word == t.word && helper(t, name)) helper_note += "; its " + name + " alone does not start it";
        lines.push_back(t.name + " is installed (" + here + ") but none of its DLLs is active: " + why + helper_note);
    }
    return lines;
}

// ModLoader Proxy's own reading of the file (src/main.cpp LoadConfig): UTF-8 with or without BOM, trimmed lines,
// ';' or '#' comment lines, "[section]" in any case, " ;" before a comment after a value.
ChainloaderIni parse_chainloader_ini(const std::string& text) {
    ChainloaderIni out;
    size_t from = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    std::string section;
    const auto backslashes = [](std::string s) {
        std::replace(s.begin(), s.end(), '/', '\\');
        return s;
    };
    while (from < text.size()) {
        size_t to = text.find('\n', from);
        if (to == std::string::npos) to = text.size();
        std::string line = trim(text.substr(from, to - from));
        from = to + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = lower(trim(line.substr(1, line.size() - 2)));
            continue;
        }
        if (const size_t sc = line.find(" ;"); sc != std::string::npos) line = trim(line.substr(0, sc));
        if (line.empty()) continue;
        if (section == "settings") {
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = lower(trim(line.substr(0, eq))), value = trim(line.substr(eq + 1));
            const std::string v = lower(value);
            if (key == "loadfromfolder") out.load_from_folder = v == "1" || v == "true" || v == "yes" || v == "on";
            else if (key == "modsfolder") out.mods_folder = value;
            else if (key == "forwardto") out.forward_to = backslashes(value);
        } else if (section == "load") {
            out.load.push_back(backslashes(line));
        }
    }
    return out;
}

std::vector<std::string> chainloader_dlls(const std::string& ini_text, const std::string& self,
                                          const std::function<std::vector<std::string>(const std::string&)>& dlls_in) {
    const ChainloaderIni ini = parse_chainloader_ini(ini_text);
    const size_t self_slash = self.find_last_of("\\/");
    const std::string me = lower(self_slash == std::string::npos ? self : self.substr(self_slash + 1));  // its file name
    std::vector<std::string> out;
    const auto add = [&](const std::string& path, bool skip_own_name) {
        const size_t slash = path.find_last_of('\\');
        const std::string file = lower(slash == std::string::npos ? path : path.substr(slash + 1));
        if (path.empty() || (skip_own_name && !me.empty() && file == me)) return;
        for (const auto& o : out)
            if (lower(o) == lower(path)) return;  // the ForwardTo DLL, or a DLL listed and also in the folder
        out.push_back(path);
    };
    // ForwardTo first (before the game can call the proxy's functions): skipped only when it is the proxy itself
    if (!ini.forward_to.empty() && lower(ini.forward_to) != me) add(ini.forward_to, false);
    for (const auto& p : ini.load) add(p, true);  // then [Load] in order: never a DLL with the proxy's own name
    if (ini.load_from_folder) {
        std::string folder = trim(ini.mods_folder);
        std::replace(folder.begin(), folder.end(), '/', '\\');
        while (!folder.empty() && folder.back() == '\\') folder.pop_back();
        if (!folder.empty() && folder != ".") {  // a subfolder only, as ModLoader Proxy demands
            std::vector<std::string> names = dlls_in(folder);
            std::sort(names.begin(), names.end(),
                      [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
            for (const auto& n : names)
                if (n.size() > 4 && lower(n.substr(n.size() - 4)) == ".dll") add(folder + "\\" + n, true);
        }
    }
    return out;
}

bool modloader_proxy_runs(const GameFolderView& view, std::string* self) {
    for (const auto& [name, word] : view.proxies)
        if (word == kModLoaderProxy) {
            if (self) *self = name;
            return true;
        }
    const auto names = started_chain_names(view.chain_status);  // in the order chained_tools was read
    for (size_t i = 0; i < view.chained_tools.size(); ++i)
        if (view.chained_tools[i] == kModLoaderProxy) {
            if (self) *self = names.size() == view.chained_tools.size() ? names[i] : std::string();
            return true;
        }
    for (const auto& [name, word] : view.chain_listed)
        if (word == kModLoaderProxy) {
            if (self) *self = name;
            return true;
        }
    return false;
}

namespace {
// A Windows device name (CON, PRN, AUX, NUL, COM1-9, LPT1-9; also NUL.txt) as any part of a path: opening it reaches
// a device (a console, a serial port), not a file.
bool device_path(const std::string& path) {
    size_t from = 0;
    while (from <= path.size()) {
        size_t to = path.find_first_of("\\/", from);
        if (to == std::string::npos) to = path.size();
        std::string base = lower(path.substr(from, to - from));
        base = trim(base.substr(0, base.find('.')));
        if (base == "con" || base == "prn" || base == "aux" || base == "nul") return true;
        if (base.size() == 4 && (base.compare(0, 3, "com") == 0 || base.compare(0, 3, "lpt") == 0) &&
            base[3] >= '0' && base[3] <= '9')
            return true;
        from = to + 1;
    }
    return false;
}
}  // namespace

std::vector<StartedDll> chainloader_entries(const std::string& self, const FolderLook& look) {
    // its folder: ModLoader Proxy reads the chainloader.ini next to its DLL and starts paths relative to it
    const size_t slash = self.find_last_of("\\/");
    const std::string base = slash == std::string::npos ? std::string() : self.substr(0, slash + 1);
    // a listed path as a path in the game folder; false when it leaves the game folder (a drive, "..", \\server). A
    // path starting with one \ is next to the proxy, as ModLoader Proxy resolves it.
    const auto in_game = [&](std::string rel, std::string* path) {
        if (!rel.empty() && rel[0] == '\\' && (rel.size() < 2 || rel[1] != '\\')) rel.erase(0, 1);
        *path = base + rel;
        return !rel.empty() && loader::inside_game_folder(loader::widen_chain(*path));
    };
    std::string outside_folder;  // a ModsFolder outside the game folder: listed, never looked into
    const auto names = chainloader_dlls(look.read_text(base + "chainloader.ini"), self, [&](const std::string& folder) {
        std::string path;
        if (in_game(folder, &path) && !device_path(path)) return look.dlls_in(path);
        outside_folder = folder;
        return std::vector<std::string>{};
    });
    std::vector<StartedDll> out;
    for (const auto& n : names) {
        StartedDll d{n, {}, true, false};
        std::string path;
        if (!in_game(n, &path)) {
            d.outside = true;
        } else {
            d.name = path;
            d.found = !device_path(path) && look.exists(path);  // never opens CON or COM1
            if (d.found) d.word = look.tool(path);
        }
        out.push_back(std::move(d));
    }
    if (!outside_folder.empty()) out.push_back({outside_folder + "\\*.dll", {}, true, true});
    return out;
}

void add_loader_started(GameFolderView& view, const FolderLook& look) {
    std::string self;
    if (modloader_proxy_runs(view, &self)) view.loader_started = chainloader_entries(self, look);
}

const std::vector<std::string>& proxy_dll_names() {
    // umpdc.dll and cfgmgr32.dll: the Greeneye loader's other names; d3d12.dll to hid.dll: other DLLs the game loads by
    // name that Windows does not pin to System32 (as EMF.exe's installer, src/nexus/classify.cpp)
    static const std::vector<std::string> names = {"dinput8.dll",  "dbghelp.dll",   "dbgcore.dll",  "winmm.dll",
                                                   "powrprof.dll", "version.dll",   "xinput1_4.dll", "dxgi.dll",
                                                   "umpdc.dll",    "cfgmgr32.dll",  "d3d12.dll",    "opengl32.dll",
                                                   "dwmapi.dll",   "uxtheme.dll",   "wtsapi32.dll", "iphlpapi.dll",
                                                   "hid.dll"};
    return names;
}

}  // namespace emf::core
