#include "nexus/classify.h"

#include <algorithm>
#include <set>

namespace emf::nexus {

namespace {

// File names DLL mods use to get into the game (core::proxy_dll_names, kept the same).
const char* const kProxyNames[] = {"dinput8.dll", "dbghelp.dll", "dbgcore.dll",   "winmm.dll", "powrprof.dll",
                                   "version.dll", "xinput1_4.dll", "dxgi.dll",    "umpdc.dll", "cfgmgr32.dll"};
// Folders of the game folder itself: never a wrapper.
const char* const kGameFolders[] = {"emf", "mods", "greeneye", "shroudforge", "reshade-shaders", "aldrich-modmenu"};

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool ends_with(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

bool is_proxy(const std::string& low_name) {
    for (const char* p : kProxyNames)
        if (low_name == p) return true;
    return false;
}

std::string tool_name(const std::string& word) {
    if (word == "emf") return "EMF";
    if (word == "eml") return "EML";
    if (word == "shroudforge") return "ShroudForge";
    if (word == "shroudtopia") return "Shroudtopia";
    if (word == "greeneye") return "Greeneye Loader";
    if (word == "aldrich-modmenu") return "Aldrich Mod Menu";
    if (word == "modloader-proxy") return "ModLoader Proxy";
    return word;
}

bool is_sep(char c) { return c == '/' || c == '\\'; }

// A name as Windows stores it: trailing dots and spaces dropped.
std::string_view trimmed_name(std::string_view seg) {
    while (!seg.empty() && (seg.back() == '.' || seg.back() == ' ')) seg.remove_suffix(1);
    return seg;
}

// A path with '/' between folders and Windows' names; "" for a folder entry. Sets `unsafe` (unsafe_archive_path).
std::string normalise(const std::string& raw, bool& unsafe) {
    unsafe = unsafe_archive_path(raw);
    if (unsafe || is_sep(raw.back())) return "";
    std::string p;
    size_t from = 0;
    while (from < raw.size()) {
        size_t to = from;
        while (to < raw.size() && !is_sep(raw[to])) ++to;
        if (to > from) {
            if (!p.empty()) p += '/';
            p += trimmed_name(std::string_view(raw).substr(from, to - from));
        }
        from = to + 1;
    }
    return p;
}

bool junk(const std::string& low) {
    if (low.rfind("__macosx/", 0) == 0) return true;
    const size_t slash = low.rfind('/');
    const std::string name = slash == std::string::npos ? low : low.substr(slash + 1);
    return name == ".ds_store" || name == "thumbs.db" || name == "desktop.ini";
}

std::string file_name(const std::string& p) {
    const size_t slash = p.rfind('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

size_t depth(const std::string& p) { return static_cast<size_t>(std::count(p.begin(), p.end(), '/')); }

bool is_hex8(const std::string& s, size_t at) {
    if (s.size() < at + 8) return false;
    for (size_t i = at; i < at + 8; ++i)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return true;
}

// A save file name: "characters" or 8 hex digits (a world), optionally followed by _info and/or -<backup number>.
// Returns the length of the stem (10 or 8) and whether it is an _info file; 0 when it is no save name.
size_t save_name(const std::string& low, bool& info) {
    size_t at;
    if (low.rfind("characters", 0) == 0) at = 10;
    else if (is_hex8(low, 0)) at = 8;
    else return 0;
    const size_t stem = at;
    info = low.compare(at, 5, "_info") == 0;
    if (info) at += 5;
    if (at == low.size()) return stem;
    if (low[at] != '-' || at + 1 == low.size()) return 0;
    for (size_t i = at + 1; i < low.size(); ++i)
        if (low[i] < '0' || low[i] > '9') return 0;
    return stem;
}

struct Entry {
    std::string path;  // below the wrapper, original case
    std::string low;
};

class Archive {
public:
    Archive(const std::vector<std::string>& raw, Classification& out) : out_(out) {
        for (const auto& r : raw) {
            if (r.empty()) continue;
            bool unsafe = false;
            std::string p = normalise(r, unsafe);
            if (unsafe) {
                unsafe_.push_back(r);
                continue;
            }
            if (p.empty() || junk(lower(p))) continue;
            entries_.push_back({p, lower(p)});
        }
        strip_wrappers();
        for (const auto& e : entries_) set_.insert(e.low);
    }

    bool empty() const { return entries_.empty(); }
    const std::vector<std::string>& unsafe() const { return unsafe_; }
    const std::vector<Entry>& entries() const { return entries_; }
    const std::string& wrapper_name() const { return wrapper_name_; }
    bool has(const std::string& low) const { return set_.count(low) != 0; }
    bool has_top_dir(const std::string& low) const {
        for (const auto& e : entries_)
            if (e.low.size() > low.size() && e.low.compare(0, low.size(), low) == 0 && e.low[low.size()] == '/')
                return true;
        return false;
    }
    // The original spelling of a lower-case path prefix, from the entry `low_file` that starts with it.
    std::string original(const std::string& low_file, size_t prefix_len) const {
        for (const auto& e : entries_)
            if (e.low == low_file) return e.path.substr(0, prefix_len);
        return low_file.substr(0, prefix_len);
    }

private:
    void strip_wrappers() {
        for (int level = 0; level < 2 && !entries_.empty(); ++level) {
            const size_t slash = entries_[0].low.find('/');
            if (slash == std::string::npos) return;
            const std::string top = entries_[0].low.substr(0, slash + 1);
            for (const auto& e : entries_)
                if (e.low.compare(0, top.size(), top) != 0) return;
            const std::string name = top.substr(0, top.size() - 1);
            for (const char* g : kGameFolders)
                if (name == g) return;
            wrapper_name_ = entries_[0].path.substr(0, slash);
            out_.strip_prefix += entries_[0].path.substr(0, slash + 1);
            for (auto& e : entries_) {
                e.path.erase(0, top.size());
                e.low.erase(0, top.size());
            }
        }
    }

    Classification& out_;
    std::vector<Entry> entries_;
    std::set<std::string> set_;
    std::vector<std::string> unsafe_;
    std::string wrapper_name_;
};

void add_unique(std::vector<ArchiveKind>& v, ArchiveKind k) {
    if (std::find(v.begin(), v.end(), k) == v.end()) v.push_back(k);
}

// Who owns proxy file `name` next to the game: nullptr when nobody (or not known).
const std::string* owner(const GameState& g, const std::string& name) {
    for (const auto& [file, word] : g.proxies)
        if (lower(file) == name) return &word;
    return nullptr;
}

bool tool_installed(const GameState& g, const std::string& word) {
    for (const auto& [file, w] : g.proxies)
        if (w == word) return true;
    return std::find(g.chained.begin(), g.chained.end(), word) != g.chained.end();
}

// The app looked, and EMF's loader is not the dinput8.dll there.
bool emf_missing(const GameState& g) {
    if (!g.known) return false;
    const std::string* o = owner(g, "dinput8.dll");
    return !o || *o != "emf";
}

std::string windows_path(std::string p) {
    std::replace(p.begin(), p.end(), '/', '\\');
    return p;
}

std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) s += (i == 0 ? "" : (i + 1 == v.size() ? " and " : ", ")) + v[i];
    return s;
}

std::string first_folder(const std::vector<ModFolder>& mods, ArchiveKind k, const char* place) {
    for (const auto& m : mods)
        if (m.kind == k) return std::string(place) + (m.id.empty() ? "<id>" : m.id) + "\\";
    return std::string(place) + "<id>\\";
}

std::string eml_state(const GameState& g) {
    if (!g.known) return "";
    if (tool_installed(g, "eml")) return " EML is installed.";
    if (tool_installed(g, "shroudforge"))
        return " ShroudForge is installed. ShroudForge loads EML mods once you switch them on in its F9 window (they "
               "start off).";
    return " Neither EML nor ShroudForge is installed, so it does nothing yet.";
}

std::string installed_state(const GameState& g, const char* word, const char* name) {
    if (!g.known) return "";
    return std::string(" ") + name + (tool_installed(g, word) ? " is installed." : " is not installed.");
}

void write_notes(Classification& c, const Archive& a, bool emf_by_layout, bool reshade_dll, bool has_exe,
                 const std::string& dll_manifest, const GameState& g) {
    auto& n = c.notes;
    const bool emf_folder = std::any_of(c.mods.begin(), c.mods.end(),
                                        [](const ModFolder& m) { return m.kind == ArchiveKind::EmfMod; });
    const std::string emf_place =
        emf_folder ? first_folder(c.mods, ArchiveKind::EmfMod, "emf\\mods\\") : std::string("emf\\mods\\");
    switch (c.kind) {
        case ArchiveKind::EmfFramework:
            n.push_back("This is EMF itself. Install or update it with EMF's own installer (install.cmd), not as a "
                        "mod.");
            break;
        case ArchiveKind::EmfMod:
            n.push_back(emf_by_layout
                            ? "It looks like an EMF Lua mod (mod.json next to main.lua); it goes to " +
                                  emf_place + " and EMF checks its mod.json when installing."
                            : "An EMF mod: it goes to " + emf_place + " and EMF loads it at the next game start.");
            break;
        case ArchiveKind::EmlMod:
            n.push_back("An EML mod: it goes to " + first_folder(c.mods, ArchiveKind::EmlMod, "mods\\") +
                        " and is loaded by EML or ShroudForge, not by EMF." + eml_state(g));
            break;
        case ArchiveKind::ShroudForgePackage:
            n.push_back("A ShroudForge mod (an EML mod with ShroudForge's extended.mod.json): it goes to " +
                        first_folder(c.mods, ArchiveKind::ShroudForgePackage, "mods\\") + " and needs ShroudForge." +
                        installed_state(g, "shroudforge", "ShroudForge"));
            break;
        case ArchiveKind::DllModDinput8:
            n.push_back(std::string("A DLL mod named dinput8.dll. ") +
                        (emf_missing(g) ? "With EMF installed, EMF starts it" : "EMF will start it") +
                        " through dinput8-chain.ini, so it runs next to EMF and other dinput8.dll mods.");
            if (emf_missing(g))
                n.push_back("EMF is not installed in this game folder: install EMF first, so this file does not "
                            "replace another dinput8.dll.");
            break;
        case ArchiveKind::ProxyDll:
            if (!c.loader.empty())
                n.push_back("This is " + tool_name(c.loader) + ", a mod loader: it puts " + join(c.proxy_dlls) +
                            " next to the game.");
            else
                n.push_back("A DLL mod or loader that gets into the game as " + join(c.proxy_dlls) +
                            " (a file name only one tool can have).");
            break;
        case ArchiveKind::GreeneyeMod:
            n.push_back("A Greeneye mod: it goes to " + first_folder(c.mods, ArchiveKind::GreeneyeMod, "greeneye\\") +
                        " and is loaded by the Greeneye Loader (dbgcore.dll), not by EMF.");
            if (a.has("dbgcore.dll")) n.push_back("The archive brings the Greeneye Loader's dbgcore.dll too.");
            else if (g.known && !tool_installed(g, "greeneye"))
                n.push_back("The Greeneye Loader (dbgcore.dll) is not installed; get it from one of its mods.");
            break;
        case ArchiveKind::ShroudtopiaMod:
            n.push_back("A Shroudtopia mod: it goes to mods\\ and is loaded by Shroudtopia or its Shroudweave fork "
                        "(winmm.dll), not by EMF." +
                        installed_state(g, "shroudtopia", "Shroudtopia"));
            break;
        case ArchiveKind::ReshadePreset:
            n.push_back("A ReShade preset: it needs ReShade (installed separately) and goes next to the game.");
            if (reshade_dll) n.push_back("It brings ReShade's dxgi.dll too.");
            break;
        case ArchiveKind::SaveFiles:
            n.push_back("Save files, not a mod: they replace a character or world. Back up your saves first "
                        "(Saved Games\\Enshrouded, or Steam's userdata folder for cloud saves); EMF does not install "
                        "save files.");
            break;
        case ArchiveKind::CheatTable:
            n.push_back("A Cheat Engine table, not a mod: open it in Cheat Engine while the game runs. EMF does not "
                        "install it.");
            break;
        case ArchiveKind::Unknown:
            if (a.empty() && a.unsafe().empty())
                n.push_back("Nexus has no list of this file's contents (new uploads can take a while). EMF checks "
                            "the archive itself after the download.");
            else if (!dll_manifest.empty())
                n.push_back("It has a mod.json next to a DLL (" + dll_manifest +
                            "): it could be an EMF, Greeneye or Shroudtopia mod packed without its parent folder. EMF "
                            "checks its mod.json after the download.");
            else
                n.push_back("EMF cannot tell what this is from its file list. Read the mod's description before "
                            "installing it by hand.");
            break;
    }

    for (ArchiveKind k : c.also) {
        if (k == ArchiveKind::EmlMod)
            n.push_back("It also contains an EML mod (" + first_folder(c.mods, k, "mods\\") +
                        "), which needs EML or ShroudForge." + eml_state(g));
        else if (k == ArchiveKind::ShroudForgePackage && c.loader == "shroudforge")
            n.push_back("It also brings ShroudForge mods (in mods\\).");
        else if (k == ArchiveKind::ShroudForgePackage)
            n.push_back("It also contains a ShroudForge mod (" + first_folder(c.mods, k, "mods\\") +
                        "), which needs ShroudForge." + installed_state(g, "shroudforge", "ShroudForge"));
        else if (k == ArchiveKind::EmfMod)
            n.push_back("It also contains an EMF mod (" + emf_place + ").");
        else if (k == ArchiveKind::GreeneyeMod)
            n.push_back("It also contains a Greeneye mod (" + first_folder(c.mods, k, "greeneye\\") + ").");
    }

    for (const auto& dll : c.proxy_dlls) {
        if (dll == "dinput8.dll") {
            if (c.kind == ArchiveKind::DllModDinput8 || c.kind == ArchiveKind::EmfFramework) continue;
            if (c.loader == "shroudforge")
                n.push_back("Its dinput8.dll is a second copy of the ShroudForge loader: EMF keeps it aside and does "
                            "not start it (ShroudForge runs through dbghelp.dll).");
            else if (c.loader == "eml")
                n.push_back("EML ships its loader twice, as dbghelp.dll and as dinput8.dll: install only dbghelp.dll, "
                            "or EML starts twice.");
            else if (emf_missing(g))
                n.push_back("With EMF installed, EMF starts its dinput8.dll through dinput8-chain.ini.");
            else
                n.push_back("Its dinput8.dll: EMF will start it through dinput8-chain.ini.");
            continue;
        }
        if (!g.known) continue;
        const std::string* o = owner(g, dll);
        if (!o) continue;
        if (!c.loader.empty() && *o == c.loader)
            n.push_back(dll + ": " + tool_name(*o) + " is already installed; this copy replaces it (keep the newer "
                                                    "one).");
        else if (o->empty())
            n.push_back(dll + " is already there (a tool EMF does not know): installing this replaces it, and that "
                              "tool stops loading.");
        else
            n.push_back(dll + " is " + tool_name(*o) + "'s now: installing this replaces it, and " + tool_name(*o) +
                        " stops loading.");
    }

    std::vector<std::string> unnamed;  // proxy DLLs besides dinput8.dll that no note above names
    for (const auto& dll : c.proxy_dlls) {
        if (dll == "dinput8.dll" || c.kind == ArchiveKind::ProxyDll) continue;
        if (c.kind == ArchiveKind::GreeneyeMod && dll == "dbgcore.dll") continue;
        if (c.kind == ArchiveKind::ReshadePreset && reshade_dll && dll == "dxgi.dll") continue;
        unnamed.push_back(dll);
    }
    if (!unnamed.empty())
        n.push_back("It also puts " + join(unnamed) + " next to the game (a file name only one tool can have); the "
                    "game loads it directly, not through EMF.");

    if (has_exe && c.kind != ArchiveKind::EmfFramework)
        n.push_back("It contains programs (.exe): run them only if the mod's page says so; EMF does not run them.");
    if (!a.unsafe().empty())
        n.push_back("Some paths are unsafe to unpack (" + a.unsafe()[0] + (a.unsafe().size() > 1 ? ", ..." : "") +
                    "): EMF will not unpack those.");
}

}  // namespace

const char* kind_id(ArchiveKind k) {
    switch (k) {
        case ArchiveKind::EmfFramework: return "emf";
        case ArchiveKind::EmfMod: return "emf-mod";
        case ArchiveKind::EmlMod: return "eml-mod";
        case ArchiveKind::ShroudForgePackage: return "shroudforge-mod";
        case ArchiveKind::DllModDinput8: return "dll-dinput8";
        case ArchiveKind::ProxyDll: return "proxy-dll";
        case ArchiveKind::GreeneyeMod: return "greeneye-mod";
        case ArchiveKind::ShroudtopiaMod: return "shroudtopia-mod";
        case ArchiveKind::ReshadePreset: return "reshade-preset";
        case ArchiveKind::SaveFiles: return "save-files";
        case ArchiveKind::CheatTable: return "cheat-table";
        case ArchiveKind::Unknown: break;
    }
    return "unknown";
}

const char* kind_label(ArchiveKind k) {
    switch (k) {
        case ArchiveKind::EmfFramework: return "EMF itself";
        case ArchiveKind::EmfMod: return "EMF mod";
        case ArchiveKind::EmlMod: return "EML mod";
        case ArchiveKind::ShroudForgePackage: return "ShroudForge mod";
        case ArchiveKind::DllModDinput8: return "DLL mod (dinput8.dll)";
        case ArchiveKind::ProxyDll: return "Loader or DLL mod";
        case ArchiveKind::GreeneyeMod: return "Greeneye mod";
        case ArchiveKind::ShroudtopiaMod: return "Shroudtopia mod";
        case ArchiveKind::ReshadePreset: return "ReShade preset";
        case ArchiveKind::SaveFiles: return "Save files";
        case ArchiveKind::CheatTable: return "Cheat Engine table";
        case ArchiveKind::Unknown: break;
    }
    return "Unknown";
}

Classification classify_archive(const std::vector<std::string>& paths, const GameState& game) {
    Classification c;
    const Archive a(paths, c);
    const std::string& wrap = c.strip_prefix;  // set by Archive

    // --- what is in it ---
    std::vector<ModFolder> found;
    bool has_exe = false, reshade = false, saves = false, table = false, shroudtopia_dll = false, emf_loose = false;
    std::set<std::string> proxies;
    std::set<std::string> worlds, world_infos;  // folder + 8 hex digits of world files and of their _info files
    for (const auto& e : a.entries()) {
        const std::string name = file_name(e.low);
        if (depth(e.low) == 0 && is_proxy(name)) proxies.insert(name);
        if (ends_with(name, ".exe")) has_exe = true;
        if ((ends_with(name, ".ini") && name.find("reshade") != std::string::npos) ||
            e.low.rfind("reshade-shaders/", 0) == 0)
            reshade = true;
        bool info = false;
        if (ends_with(name, ".ksd")) saves = true;
        if (const size_t stem = save_name(name, info)) {
            const std::string key = e.low.substr(0, e.low.size() - name.size() + stem);
            if (stem == 10 && !info) saves = true;  // the characters file
            else if (stem == 8) (info ? world_infos : worlds).insert(key);
        }
        if (ends_with(name, ".ct") || ends_with(name, ".cetrainer")) table = true;
        if (ends_with(name, ".dll") && e.low.rfind("mods/", 0) == 0 && (depth(e.low) == 1 || depth(e.low) == 2))
            shroudtopia_dll = true;

        // emf/mods/<id>/..., or a loose DLL in emf/mods/
        if (e.low.rfind("emf/mods/", 0) == 0 && depth(e.low) == 2 && ends_with(name, ".dll")) emf_loose = true;
        if (e.low.rfind("emf/mods/", 0) == 0 && depth(e.low) >= 3) {
            const size_t end = e.low.find('/', 9);
            found.push_back({ArchiveKind::EmfMod, e.path.substr(9, end - 9), wrap + e.path.substr(0, end + 1)});
        }
        // greeneye/<id>/...
        if (e.low.rfind("greeneye/", 0) == 0 && depth(e.low) >= 2) {
            const size_t end = e.low.find('/', 9);
            found.push_back({ArchiveKind::GreeneyeMod, e.path.substr(9, end - 9), wrap + e.path.substr(0, end + 1)});
        }
        // mods/<id>/<x>.dll: a Shroudtopia mod folder
        if (ends_with(name, ".dll") && e.low.rfind("mods/", 0) == 0 && depth(e.low) == 2) {
            const size_t end = e.low.find('/', 5);
            found.push_back({ArchiveKind::ShroudtopiaMod, e.path.substr(5, end - 5), wrap + e.path.substr(0, end + 1)});
        }
        // EML: <folder>mod.json with <folder>src/mod.lua, the folder being the root, <id>/ or mods/<id>/
        if (name == "mod.json") {
            const std::string folder = e.low.substr(0, e.low.size() - 8);
            const size_t d = depth(folder);
            const bool place = d == 0 || d == 1 || (d == 2 && folder.rfind("mods/", 0) == 0);
            if (place && a.has(folder + "src/mod.lua")) {
                const std::string orig = e.path.substr(0, folder.size());
                std::string id = d == 0 ? a.wrapper_name() : file_name(orig.substr(0, orig.size() - 1));
                const ArchiveKind k =
                    a.has(folder + "extended.mod.json") ? ArchiveKind::ShroudForgePackage : ArchiveKind::EmlMod;
                // a wrapper that is the mod's own folder: the folder is the wrapper
                const std::string path = d == 0 ? wrap : wrap + orig;
                found.push_back({k, id, path});
            }
        }
    }
    c.proxy_dlls.assign(proxies.begin(), proxies.end());
    for (const auto& w : worlds)
        if (world_infos.count(w)) saves = true;  // a world file with its _info file

    // EMF mod by layout: <id>/mod.json with <id>/main.lua (the wrapper counts as <id>/). mod.json next to a DLL is
    // remembered for the Unknown note: an EMF, Greeneye or Shroudtopia mod without its parent folder.
    bool emf_by_layout = false;
    std::string dll_manifest;
    auto emf_layout_at = [&](const std::string& folder_low) {
        return a.has(folder_low + "mod.json") && !a.has(folder_low + "src/mod.lua") && a.has(folder_low + "main.lua");
    };
    std::vector<ModFolder> emf_layout;
    if (emf_layout_at("")) emf_layout.push_back({ArchiveKind::EmfMod, a.wrapper_name(), wrap});
    for (const auto& e : a.entries()) {
        if (depth(e.low) != 1 || file_name(e.low) != "mod.json") continue;
        const std::string folder = e.low.substr(0, e.low.size() - 8);
        const std::string id = e.path.substr(0, folder.size() - 1);
        if (emf_layout_at(folder)) emf_layout.push_back({ArchiveKind::EmfMod, id, wrap + e.path.substr(0, folder.size())});
    }
    for (const auto& e : a.entries()) {
        const size_t d = depth(e.low);
        if (d > 1 || !ends_with(e.low, ".dll") || !dll_manifest.empty()) continue;
        const std::string folder = d == 0 ? std::string() : e.low.substr(0, e.low.find('/') + 1);
        if (a.has(folder + "mod.json")) dll_manifest = wrap + e.path;
    }

    auto any = [&](ArchiveKind k) {
        return std::any_of(found.begin(), found.end(), [k](const ModFolder& m) { return m.kind == k; });
    };
    const bool other_proxy = std::any_of(proxies.begin(), proxies.end(), [](const std::string& p) {
        return p != "dinput8.dll";
    });
    const bool reshade_dll = proxies.size() == 1 && proxies.count("dxgi.dll");

    // --- the kind: the first rule that matches (classify.h) ---
    if (a.has("emf/emf_core.dll") || a.has("game/emf/emf_core.dll")) {  // EMF's release zip has game/
        c.kind = ArchiveKind::EmfFramework;
        c.loader = "emf";
    } else if (a.has_top_dir("shroudforge") && !proxies.empty()) {
        c.kind = ArchiveKind::ProxyDll;
        c.loader = "shroudforge";
    } else if (a.has("shroudtopia.json") && !proxies.empty()) {
        c.kind = ArchiveKind::ProxyDll;
        c.loader = "shroudtopia";
    } else if (a.has("chainloader.ini") && !proxies.empty()) {
        c.kind = ArchiveKind::ProxyDll;
        c.loader = "modloader-proxy";
    } else if (a.has_top_dir("aldrich-modmenu") && !proxies.empty()) {
        c.kind = ArchiveKind::ProxyDll;
        c.loader = "aldrich-modmenu";
    } else if ((a.has("emm.exe") || a.has("kfc-parser.exe")) && !proxies.empty()) {
        c.kind = ArchiveKind::ProxyDll;  // EML's release
        c.loader = "eml";
    } else if (any(ArchiveKind::EmfMod) || emf_loose) {
        c.kind = ArchiveKind::EmfMod;
    } else if (any(ArchiveKind::GreeneyeMod)) {
        c.kind = ArchiveKind::GreeneyeMod;
        if (proxies.count("dbgcore.dll")) c.loader = "greeneye";
    } else if (proxies.count("dinput8.dll")) {
        c.kind = ArchiveKind::DllModDinput8;
    } else if (reshade && (proxies.empty() || reshade_dll)) {
        c.kind = ArchiveKind::ReshadePreset;
    } else if (other_proxy) {
        c.kind = ArchiveKind::ProxyDll;
        if (proxies.count("dbgcore.dll") && a.has_top_dir("greeneye")) c.loader = "greeneye";  // the loader alone
    } else if (a.has("shroudtopia.json") || shroudtopia_dll) {
        c.kind = ArchiveKind::ShroudtopiaMod;
    } else if (any(ArchiveKind::ShroudForgePackage)) {
        c.kind = ArchiveKind::ShroudForgePackage;
    } else if (any(ArchiveKind::EmlMod)) {
        c.kind = ArchiveKind::EmlMod;
    } else if (!emf_layout.empty()) {
        c.kind = ArchiveKind::EmfMod;
        emf_by_layout = true;
        found = emf_layout;
    } else if (saves) {
        c.kind = ArchiveKind::SaveFiles;
    } else if (table) {
        c.kind = ArchiveKind::CheatTable;
    }

    // --- the unambiguous layouts besides it, and the mod folders of both ---
    for (ArchiveKind k : {ArchiveKind::EmlMod, ArchiveKind::ShroudForgePackage, ArchiveKind::EmfMod,
                          ArchiveKind::GreeneyeMod})
        if (k != c.kind && !emf_by_layout && c.kind != ArchiveKind::EmfFramework && any(k)) add_unique(c.also, k);
    std::sort(found.begin(), found.end(), [](const ModFolder& x, const ModFolder& y) { return x.path < y.path; });
    for (const auto& m : found) {
        const bool wanted = m.kind == c.kind || std::find(c.also.begin(), c.also.end(), m.kind) != c.also.end();
        const bool seen = std::any_of(c.mods.begin(), c.mods.end(), [&](const ModFolder& x) {
            return x.kind == m.kind && lower(x.path) == lower(m.path);
        });
        if (wanted && !seen) c.mods.push_back(m);
    }

    write_notes(c, a, emf_by_layout, reshade_dll, has_exe, dll_manifest, game);
    return c;
}

bool unsafe_archive_path(std::string_view path) {
    if (path.empty() || is_sep(path[0])) return true;
    for (char ch : path) {
        const auto u = static_cast<unsigned char>(ch);
        if (u < 0x20 || ch == ':' || ch == '<' || ch == '>' || ch == '"' || ch == '|' || ch == '?' || ch == '*')
            return true;
    }
    static const char* const kDevices[] = {"con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4",
                                           "com5", "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3",
                                           "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    size_t from = 0;
    while (from < path.size()) {
        size_t to = from;
        while (to < path.size() && !is_sep(path[to])) ++to;
        const std::string_view seg = path.substr(from, to - from);
        from = to + 1;
        if (seg.empty()) continue;  // "a//b", or the '/' of a folder entry
        if (seg == "." || seg == "..") return true;
        const std::string_view name = trimmed_name(seg);
        if (name.empty()) return true;  // "...", " "
        std::string base = lower(std::string(name.substr(0, name.find('.'))));
        while (!base.empty() && base.back() == ' ') base.pop_back();
        for (const char* dev : kDevices)
            if (base == dev) return true;  // CON, nul.txt, COM1.log ...
    }
    return false;
}

Classification classify_archive(const std::vector<NexusArchiveEntry>& entries, const GameState& game) {
    std::vector<std::string> paths;
    paths.reserve(entries.size());
    for (const auto& e : entries) paths.push_back(e.path);
    return classify_archive(paths, game);
}

}  // namespace emf::nexus
