#include "game_folder.h"
#include "common/move_retry.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>

#include "core/json.h"
#include "core/known_tools.h"
#include "core/mod_order.h"
#include "core/key_list.h"
#include "core/settings.h"
#include "emf/emf_api.h"
#include "loader/chain_list.h"
#include "content_panel.h"
#include "loader/loader_id.h"
#include "loader/play_once.h"
#include "text.h"

namespace emf::app {

namespace {

using core::json::Value;
using VT = Value::Type;

constexpr const wchar_t* kChainFile = L"dinput8-chain.ini";
constexpr size_t kReadmeMax = 64 * 1024;

std::atomic<size_t> g_folder_reads{0};  // scans and EMF manifest lookups (folder_reads)

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ends_with_nocase(const std::wstring& s, const wchar_t* tail) {
    const size_t n = wcslen(tail);
    return s.size() > n && _wcsicmp(s.c_str() + s.size() - n, tail) == 0;
}

// Lower case without surrounding spaces ("side" values).
std::string lower_trimmed(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) ++a;
    return lower(s.substr(a));
}

// A file or folder name as an EMF mod id, as EMF's core makes it (mods.cpp): lower case, other characters '_'.
std::string make_id(const std::string& name) {
    std::string id;
    for (char c : lower(name)) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
        id += ok ? c : '_';
        if (id.size() == 48) break;
    }
    return id.empty() ? "mod" : id;
}

// The script host mod that runs a script mod's entry file, as EMF's core decides it (mods.cpp).
std::string script_host_of(const std::string& main) {
    if (main.size() > 4 && lower(main.substr(main.size() - 4)) == ".lua") return "lua";
    return "";
}

struct DirEntry {
    std::wstring name;
    bool dir = false;
};

// The folder's entries, sorted by name (case ignored).
std::vector<DirEntry> list_dir(const std::wstring& dir) {
    std::vector<DirEntry> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        out.push_back({name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const DirEntry& a, const DirEntry& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    return out;
}

bool has_dll(const std::wstring& dir) {
    for (const auto& e : list_dir(dir))
        if (!e.dir && ends_with_nocase(e.name, L".dll")) return true;
    return false;
}

std::string read_text(const std::wstring& path, size_t max = 1u << 20) {
    std::string s;
    read_file(path, s, max);
    return s;
}

std::string strip_bom(std::string s) {
    if (s.compare(0, 3, "\xEF\xBB\xBF") == 0) s.erase(0, 3);
    return s;
}

// Drops a UTF-8 character cut off at the end (a file read only up to a limit).
void trim_partial_utf8(std::string& s) {
    size_t cont = 0;
    while (cont < 3 && cont < s.size() && (static_cast<unsigned char>(s[s.size() - 1 - cont]) & 0xC0) == 0x80) ++cont;
    if (cont == s.size()) return;
    const unsigned char lead = static_cast<unsigned char>(s[s.size() - 1 - cont]);
    const size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (need > cont + 1) s.resize(s.size() - 1 - cont);
}

// The README next to a mod, as UTF-8 text ("" when there is none).
std::string read_readme(const std::wstring& folder) {
    for (const wchar_t* name : {L"README.md", L"README.txt", L"README"}) {
        std::string text;
        if (!read_file(folder + L"\\" + name, text, kReadmeMax)) continue;
        if (text.size() == kReadmeMax) trim_partial_utf8(text);  // cut at the limit, maybe inside a character
        return to_utf8_text(strip_bom(text));
    }
    return {};
}

std::optional<Value> parse_json(const std::string& text, std::string* error) {
    auto r = core::json::parse(text);
    if (!r.value) {
        if (error) *error = "not valid JSON: " + r.error;
        return std::nullopt;
    }
    if (!r.value->is(VT::Object)) {
        if (error) *error = "must hold an object ({...})";
        return std::nullopt;
    }
    return std::move(r.value);
}

std::string json_text(const Value& v, const char* key) {
    const Value* x = v.get(key);
    return x && x->is(VT::String) ? x->s : std::string();
}

// A number or text field as text (Nexus ids are numbers in some mods and text in others).
std::string json_id_text(const Value& v, const char* key) {
    const Value* x = v.get(key);
    if (!x) return {};
    if (x->is(VT::String)) return x->s;
    if (x->is(VT::Number) && x->n >= 0 && x->n < 1e15) return std::to_string(static_cast<long long>(x->n));
    return {};
}

std::vector<std::string> json_authors(const Value& v) {
    std::vector<std::string> out;
    for (const char* key : {"authors", "author"}) {
        const Value* x = v.get(key);
        if (!x) continue;
        if (x->is(VT::String) && !x->s.empty()) out.push_back(x->s);
        if (x->is(VT::Array))
            for (const Value& a : x->arr)
                if (a.is(VT::String)) out.push_back(a.s);
    }
    return out;
}

// EML's "dependencies": a list of ids, a list of {id, version} or an object {id: version}.
std::vector<std::string> json_dependencies(const Value& v) {
    std::vector<std::string> out;
    for (const char* key : {"dependencies", "depends"}) {
        const Value* x = v.get(key);
        if (!x) continue;
        if (x->is(VT::Array))
            for (const Value& d : x->arr) {
                if (d.is(VT::String)) out.push_back(d.s);
                else if (d.is(VT::Object) && !json_text(d, "id").empty()) {
                    const Value* opt = d.get("optional");
                    out.push_back(json_text(d, "id") + (json_text(d, "version").empty() ? "" : " " + json_text(d, "version")) +
                                  (opt && opt->is(VT::Bool) && opt->b ? " (optional)" : ""));
                }
            }
        if (x->is(VT::Object))
            for (const auto& [id, ver] : x->obj) out.push_back(id + (ver.is(VT::String) && ver.s != "*" ? " " + ver.s : ""));
    }
    return out;
}

// mod.json's "side" (where the mod has to run).
void read_side(ModEntry& e, const Value& v) {
    const ModSide side = read_mod_side(v);
    e.side = side.side;
    e.side_declared = side.declared;
    e.side_error = side.error;
}

// Something read out of a big file (a DLL), worked out again only when the file changes: the app looks every few
// seconds and proxy DLLs can be megabytes.
std::string cached_fact(const std::wstring& path, const std::string& tag, const std::function<std::string(const std::string&)>& work) {
    static std::map<std::wstring, std::pair<std::string, std::string>> cache;  // path|tag -> (file key, fact)
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return {};
    const std::string key = std::to_string(fa.nFileSizeHigh) + ":" + std::to_string(fa.nFileSizeLow) + ":" +
                            std::to_string(fa.ftLastWriteTime.dwHighDateTime) + ":" +
                            std::to_string(fa.ftLastWriteTime.dwLowDateTime) + ":" + tag;
    auto& slot = cache[path + L"|" + wide(tag)];
    if (slot.first == key) return slot.second;
    std::string bytes;
    read_file(path, bytes);
    slot = {key, work(bytes)};
    return slot.second;
}

bool contains_nocase(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return false;
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           }) != hay.end();
}

// The parts of a DLL that tell tools apart: each marker it contains (as plain text or UTF-16), one per line.
// tool_for_dll_text() gives the same answer for this short text as for the whole DLL, so only this is kept.
std::string dll_signature(const std::wstring& path, const std::vector<core::KnownTool>& tools, const std::string& table) {
    return cached_fact(path, "sig2:" + std::to_string(std::hash<std::string>{}(table)), [&](const std::string& bytes) {
        std::string sig;
        if (bytes.find(EMF_LOADER_MARKER) != std::string::npos) sig += std::string(EMF_LOADER_MARKER) + "\n";
        for (const auto& t : tools)
            for (const auto& text : t.texts) {
                std::string wide_text;  // UTF-16LE, as a DLL built with wide strings holds it
                for (const char ch : text) wide_text += std::string(1, ch) + '\0';
                if (contains_nocase(bytes, text) || contains_nocase(bytes, wide_text)) sig += text + "\n";
            }
        return sig;
    });
}

// EMF's loader takes the one-shot flag of EMF.exe's Play menu ("Play without EMF mods", loader/play_once.h): its
// marker is in the DLL. EMF 0.3.2's loader does not have it.
bool loader_plays_once(const std::wstring& path) {
    return cached_fact(path, "play_once", [](const std::string& bytes) {
               return bytes.find(EMF_PLAY_ONCE_MARKER) != std::string::npos ? std::string("1") : std::string();
           }) == "1";
}

// "emf_core <version>" that the build writes into emf_core.dll.
std::string core_version(const std::wstring& path) {
    return cached_fact(path, "version", [](const std::string& bytes) {
        const std::string mark = "emf_core ";
        for (size_t at = bytes.find(mark); at != std::string::npos; at = bytes.find(mark, at + 1)) {
            size_t end = at + mark.size();
            while (end < bytes.size() && (std::isdigit(static_cast<unsigned char>(bytes[end])) || bytes[end] == '.')) ++end;
            if (end > at + mark.size() && end < bytes.size() && bytes[end] == '\0') return bytes.substr(at + mark.size(), end - at - mark.size());
        }
        return std::string();
    });
}

std::string tool_display(const std::vector<core::KnownTool>& tools, const std::string& word) {
    if (word == "emf") return "EMF";
    for (const auto& t : tools)
        if (t.word == word) return t.name;
    return word;
}

// The settings ini as the app reads it while only looking: nothing is written back (EMF's core does that at the
// next start when it clamps a value).
class ReadOnlyIni : public core::IniBackend {
public:
    explicit ReadOnlyIni(std::wstring dir) : file_(std::move(dir)) {}
    std::optional<std::string> read(const std::string& mod, const std::string& key) override { return file_.read(mod, key); }
    bool write(const std::string&, const std::string&, const std::string&) override { return true; }

private:
    core::FileIniBackend file_;
};

// The settings ini while one setting is saved: the declarations read it but write nothing back (EMF's core clamps
// stored values at its next start), and only the setting being saved is written.
class SaveOneIni : public core::IniBackend {
public:
    explicit SaveOneIni(std::wstring dir) : file_(std::move(dir)) {}
    std::optional<std::string> read(const std::string& mod, const std::string& key) override { return file_.read(mod, key); }
    bool write(const std::string& mod, const std::string& key, const std::string& value) override {
        return !writing || file_.write(mod, key, value);
    }
    bool erase(const std::string& mod, const std::string& key) override { return !writing || file_.erase(mod, key); }
    bool writing = false;

private:
    core::FileIniBackend file_;
};

// dinput8-chain.ini as text: UTF-8 (or ANSI) as it is, UTF-16LE (BOM FF FE) turned into UTF-8 and back when written.
struct ChainText {
    std::string text;
    bool utf16 = false;
};

ChainText decode_chain(const std::string& bytes) {
    const bool utf16 = bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE;
    return {loader::chain_text_utf8(bytes), utf16};
}

std::string encode_chain(const std::string& text, bool utf16) {
    if (!utf16) return text;
    const std::wstring w = wide(text);
    std::string out = "\xFF\xFE";
    out.append(reinterpret_cast<const char*>(w.data()), w.size() * sizeof(wchar_t));
    return out;
}

std::string g_fallback_tools;  // known_tools.txt next to EMF.exe, for a folder without EMF

// What the scan learns about the folder's DLLs before it lists mods.
struct Context {
    std::wstring game;
    std::string table;
    std::vector<core::KnownTool> tools;
    std::vector<std::pair<std::wstring, std::string>> proxies;  // proxy file, its tool word ("" unknown)
    std::string chain_text;                                     // dinput8-chain.ini as UTF-8
    std::vector<std::wstring> chain_active;
    bool eml_loader = false;   // EML or ShroudForge runs (as a proxy or chain-loaded)
    std::wstring shroudtopia_name = L"Shroudtopia";
    std::vector<std::pair<std::wstring, std::wstring>> shroudtopia_dlls;  // name shown, path: DLL mods in mods\ for Shroudtopia
    bool light = false;  // ScanDepth::Profiles: only what the mods' switches and settings need
    bool modloader_proxy = false;    // ModLoader Proxy runs (a proxy DLL next to the game, or started by EMF)
    std::string modloader_proxy_as;  // the proxy name it runs as ("" when EMF starts it)
    bool chainloader_ini = false;    // its chainloader.ini is there (next to its DLL, else next to the game)
    std::vector<core::StartedDll> chainloader;  // what ModLoader Proxy starts from it (chainloader_entries)
    // EMF's own EML content (emf_content.dll, docs/engine_parts/content_loader.md): the EMF tab's card for the next
    // start (content_panel.h, the loader's own rules), when emf_content.dll is there and nothing else applies it
    std::optional<ContentPanel> content;
    bool emf_content = false;     // the next start builds the content or uses the copy (Ready, Builds at the next start)
    bool content_failed = false;  // a build with these files failed: the game starts without it until something changes
    bool content_off = false;     // the loader builds nothing (mode=off, no base, no EML mod it can build): content->detail
};

void warn(FolderState& s, const std::string& level, const std::string& text) { s.warnings.push_back({level, text}); }

// What EMF's core needs to decide whether an EMF mod loads (mods.cpp, mod_order.cpp).
struct EmfLoadInfo {
    size_t entry = 0;                      // index in FolderState::mods
    std::optional<core::Manifest> manifest;
    std::string sort_name;                 // folder or file name
    bool candidate = false;                // the core would consider it (no broken mod.json, no missing DLL)
};

// The core's checks at start-up, run here so the list says "Will not load" where the core would not load a mod:
// order_mods (a dependency missing or too old, a newer EMF API, a cycle, an id taken twice), then the switches in load
// order (a dependency or a script mod's host that is switched off or does not load). A mod that is switched off itself
// gets no error: it does not load anyway.
void apply_load_rules(FolderState& s, const std::vector<EmfLoadInfo>& infos) {
    std::vector<core::ModCandidate> cands;
    std::vector<size_t> info_of;  // candidate -> infos index
    std::vector<std::string> host(infos.size()), why(infos.size());
    for (size_t i = 0; i < infos.size(); ++i) {
        const EmfLoadInfo& in = infos[i];
        if (!in.candidate) continue;
        core::ModCandidate c;
        c.id = s.mods[in.entry].id;
        c.sort_name = in.sort_name;
        if (in.manifest) {
            c.version = in.manifest->version;
            c.depends = in.manifest->depends;
            c.emf = in.manifest->emf;
            if (!in.manifest->main.empty()) host[i] = script_host_of(in.manifest->main);
            if (host[i] == c.id) host[i].clear();
            c.depends = core::with_script_host(c.depends, host[i]);
        }
        info_of.push_back(i);
        cands.push_back(std::move(c));
    }
    const core::OrderResult order = core::order_mods(cands, EMF_API_VERSION);
    enum State { Runs, Off, NotLoaded };
    std::map<std::string, State> state;
    for (const auto& [ci, reason] : order.rejected) why[info_of[ci]] = reason;
    const auto is_script = [&](size_t i) { return infos[i].manifest && !infos[i].manifest->main.empty(); };
    for (size_t ci : order.order)  // script mods' switches are settled first, as in the core
        if (is_script(info_of[ci])) state[cands[ci].id] = s.mods[infos[info_of[ci]].entry].enabled ? Runs : Off;
    std::map<std::string, core::Version> versions;  // id -> the version of the mod that owns it, as the core sees it
    for (size_t ci : order.order)
        if (infos[info_of[ci]].manifest) versions[cands[ci].id] = cands[ci].version;
    const auto unmet = [&](size_t i, const core::ModCandidate& c) -> std::string {
        return core::unmet_dependency(
            c.depends, host[i],
            [&](const std::string& id) -> std::optional<core::ModFate> {
                auto it = state.find(id);
                if (it == state.end()) return std::nullopt;
                return it->second == Runs ? core::ModFate::Runs : it->second == Off ? core::ModFate::Off : core::ModFate::NotLoaded;
            },
            [&](const std::string& id) -> std::optional<core::Version> {
                auto it = versions.find(id);
                return it == versions.end() ? std::nullopt : std::optional<core::Version>(it->second);
            });
    };
    for (size_t ci : order.order) {
        const size_t i = info_of[ci];
        const std::string& id = cands[ci].id;
        if (is_script(i)) {
            if (state[id] != Runs) continue;
            if (const std::string w = unmet(i, cands[ci]); !w.empty()) {
                why[i] = w;
                state[id] = NotLoaded;
            }
            continue;
        }
        if (const std::string w = unmet(i, cands[ci]); !w.empty()) {
            why[i] = w;
            state[id] = NotLoaded;
            continue;
        }
        state[id] = s.mods[infos[i].entry].enabled ? Runs : Off;
    }
    for (size_t i = 0; i < infos.size(); ++i) {
        ModEntry& e = s.mods[infos[i].entry];
        if (!why[i].empty() && e.enabled && e.error.empty()) e.error = "Will not load: " + why[i];
    }
}

// ---- keys two EMF mods use (the console's `keys` lists them too) ----
void note_shared_keys(FolderState& s) {
    std::vector<core::KeyUse> uses;
    for (size_t i = 0; i < s.mods.size(); ++i) {
        const ModEntry& m = s.mods[i];
        if (m.source != "emf") continue;
        for (size_t j = 0; j < m.settings.size(); ++j) {
            const ModSetting& ms = m.settings[j];
            if (ms.def.type != core::SettingType::Key) continue;
            core::KeyUse k;
            k.owner = m.id;
            k.owner_name = m.name.empty() ? m.id : m.name;
            k.label = ms.def.label.empty() ? ms.def.key : ms.def.label;
            k.key = ms.value;
            k.on = m.enabled && m.error.empty();
            k.shared = ms.def.shared;
            k.ref = i << 16 | j;
            uses.push_back(std::move(k));
        }
    }
    const auto said = [](const core::KeyUse& k) { return k.owner_name + " (" + k.label + ")"; };
    for (const auto& g : core::group_keys(uses, [](unsigned scan) { return MapVirtualKeyW(scan, MAPVK_VSC_TO_VK); })) {
        if (g.on_uses() < 2) continue;
        std::vector<const core::KeyUse*> on;
        for (const auto& u : g.uses)
            if (u.on) on.push_back(&u);
        std::sort(on.begin(), on.end(), [](const core::KeyUse* a, const core::KeyUse* b) { return a->owner_name < b->owner_name; });
        for (const auto* u : on) {
            std::string others;
            for (const auto* o : on)
                if (o != u) others += (others.empty() ? "" : ", ") + said(*o);
            ModSetting& ms = s.mods[u->ref >> 16].settings[u->ref & 0xFFFF];
            ms.also = others;
            ms.also_clash = g.clash();
        }
        if (!g.clash()) continue;
        std::string list;
        for (size_t i = 0; i < on.size(); ++i)
            list += (i == 0 ? "" : i + 1 == on.size() ? " and " : ", ") + said(*on[i]);
        warn(s, "info", g.name + " is the key of " + (on.size() == 2 ? std::string("two") : std::to_string(on.size())) +
                            " mods that are switched on: " + list +
                            ". One press does both; change one of them in the mod's settings.");
    }
}

// ---- EMF mods: emf\mods\<folder>\mod.json and loose emf\mods\*.dll ----
void scan_emf_mods(FolderState& s, const Context& c) {
    const std::wstring dir = c.game + L"\\emf\\mods";
    ReadOnlyIni ini(c.game + L"\\emf\\config");
    core::SettingsStore store(&ini);
    std::map<std::string, std::wstring> seen;  // id -> folder, for two mods with one id
    std::vector<EmfLoadInfo> infos;
    for (const auto& d : list_dir(dir)) {
        ModEntry e;
        e.source = "emf";
        e.loaded_by = "EMF";
        e.path = dir + L"\\" + d.name;
        EmfLoadInfo info;
        info.sort_name = utf8(d.name);
        if (!d.dir) {
            if (!ends_with_nocase(d.name, L".dll")) continue;
            const std::string base = utf8(d.name.substr(0, d.name.size() - 4));
            e.id = make_id(base);
            e.name = base;
            e.enabled = store.enabled(e.id, true);
            e.can_toggle = true;
            e.note = "A single DLL in emf\\mods (no mod.json).";
            info.entry = s.mods.size();
            info.candidate = true;
            infos.push_back(std::move(info));
            s.mods.push_back(std::move(e));
            continue;
        }
        std::string text;
        if (!read_file(e.path + L"\\mod.json", text, core::kMaxManifestBytes + 1)) continue;  // not a mod folder
        if (!c.light) e.readme = read_readme(e.path);
        const auto r = core::parse_manifest(text);
        if (!r.manifest) {
            e.id = make_id(utf8(d.name));
            e.name = utf8(d.name);
            e.error = r.error;
            e.enabled = false;
            e.toggle_note = "EMF does not load it until mod.json is fixed.";
            e.state_forced = true;
            s.mods.push_back(std::move(e));
            continue;
        }
        const core::Manifest& m = *r.manifest;
        e.id = m.id;
        e.name = m.name;
        e.version = m.version_text;
        e.description = m.description;
        e.authors = m.authors;
        e.enabled = store.enabled(m.id, m.enabled_default);
        e.can_toggle = true;
        for (const auto& dep : m.depends) e.depends.push_back(core::dependency_text(dep));
        if (!m.main.empty()) {
            e.note = "A script mod (" + m.main + "), run by EMF's script host.";
            if (!exists(e.path + L"\\" + wide(m.main))) e.error = m.main + " not found in the mod's folder";
        } else if (!exists(e.path + L"\\" + wide(m.dll))) {
            e.error = m.dll + " not found in the mod's folder";
        }
        if (auto v = parse_json(strip_bom(text), nullptr)) {
            e.nexus_id = json_id_text(*v, "nexus_id");
            e.game_build = json_id_text(*v, "game_build");
            read_side(e, *v);
        }
        for (const auto& def : m.settings) store.declare(m.id, def);
        const auto defs = store.defs(m.id);
        const auto value_of = [&](const std::string& k) { return store.get(m.id, k); };
        for (size_t i : core::setting_display_order(defs)) {  // as the in-game Mods tab lists them
            ModSetting ms{defs[i], store.get(m.id, defs[i].key)};
            ms.active = core::setting_active(defs[i], value_of);
            ms.stored = store.stored(m.id, defs[i].key);
            ms.when = core::setting_condition_text(defs[i], defs);
            for (const auto& other : defs) ms.controls = ms.controls || other.when_key == defs[i].key;
            e.settings.push_back(std::move(ms));
        }
        if (auto it = seen.find(m.id); it != seen.end())
            warn(s, "warn", "Two EMF mods have the id \"" + m.id + "\" (emf\\mods\\" + utf8(it->second) + " and emf\\mods\\" +
                                utf8(d.name) + "); EMF loads only one of them.");
        seen[m.id] = d.name;
        info.entry = s.mods.size();
        info.candidate = e.error.empty();
        info.manifest = m;
        infos.push_back(std::move(info));
        s.mods.push_back(std::move(e));
    }
    if (!c.light) apply_load_rules(s, infos);  // the Will-not-load texts: the light scan only needs the switches
}

constexpr const char* kEmlStops = "EML stops the game at start until it is fixed or switched off";

// ---- EML mods (or ShroudForge packages): mods\<folder>\mod.json and mods\*.zip; a leading '.' is off ----
void scan_eml_mods(FolderState& s, Context& c) {
    const std::wstring dir = c.game + L"\\mods";
    const auto entries = list_dir(dir);
    for (const auto& d : entries) {
        const bool off = d.name[0] == L'.';
        const std::wstring base = off ? d.name.substr(1) : d.name;
        if (base.empty()) continue;
        ModEntry e;
        e.source = "eml";
        e.id = utf8(base);
        e.path = dir + L"\\" + d.name;
        if (!d.dir) {
            if (ends_with_nocase(d.name, L".dll")) {  // Shroudtopia loads mods\*.dll
                c.shroudtopia_dlls.emplace_back(base.substr(0, base.size() - 4), e.path);
                continue;
            }
            if (!ends_with_nocase(d.name, L".zip")) continue;
            e.name = utf8(base.substr(0, base.size() - 4));
            e.note = "A zip mod: EML reads it without unpacking. Its details are inside the zip.";
        } else {
            std::string text;
            if (!read_file(e.path + L"\\mod.json", text, 1u << 20)) {
                if (off) continue;      // .git, an editor's folder: not a switched-off mod
                if (has_dll(e.path)) {  // a DLL mod folder (Shroudtopia's way), not an EML mod
                    c.shroudtopia_dlls.emplace_back(base, e.path);
                    continue;
                }
                e.name = e.id;
                e.error = "no mod.json in this folder";
                e.state_forced = true;  // EML does not take it as a mod, and its '.' form is not listed
                for (const auto& sub : list_dir(e.path))
                    if (sub.dir && exists(e.path + L"\\" + sub.name + L"\\mod.json"))
                        e.error += " (the mod is one folder deeper: move mods\\" + utf8(d.name) + "\\" + utf8(sub.name) +
                                   " up into mods\\)";
                if (c.eml_loader) e.error += std::string(". ") + kEmlStops;
            } else {
                std::string why;
                if (auto v = parse_json(to_utf8_text(strip_bom(text)), &why)) {
                    e.name = json_text(*v, "name");
                    if (e.name.empty()) e.name = json_text(*v, "id");
                    if (e.name.empty()) e.name = e.id;
                    e.version = json_text(*v, "version");
                    e.description = json_text(*v, "description");
                    e.authors = json_authors(*v);
                    e.depends = json_dependencies(*v);
                    e.nexus_id = json_id_text(*v, "nexus_id");
                    e.game_build = json_id_text(*v, "game_build");
                    read_side(e, *v);
                } else {
                    e.name = e.id;
                    e.error = "mod.json is " + why;
                }
            }
            if (!c.light) e.readme = read_readme(e.path);
        }
        const bool extended = d.dir && exists(e.path + L"\\extended.mod.json");
        if (s.shroudforge_active && d.dir) {  // ShroudForge ignores zips: EML's rule applies to them
            // ShroudForge's own switch: "enabled" in extended.mod.json, off when missing (plain EML mods start off)
            bool sf_on = false;
            if (extended)
                if (auto v = parse_json(read_text(e.path + L"\\extended.mod.json"), nullptr))
                    if (const Value* en = v->get("enabled"); en && en->is(VT::Bool)) sf_on = en->b;
            e.loaded_by = "ShroudForge";
            e.enabled = !off && sf_on;
            e.can_toggle = false;
            e.toggle_note = "ShroudForge is the active loader here: switch it in ShroudForge (F9).";
            if (off) e.note = "The name starts with '.', so it is skipped.";
        } else if (c.emf_content || c.content_failed) {
            // EMF's loader builds it at the start (or a build with these files failed), or leaves it out for the
            // loader's reason (eml_mods.h), as the EMF tab's card says
            e.loaded_by = "EMF";
            e.enabled = !off;
            e.can_toggle = true;
            e.toggle_note = "Off renames it to ." + utf8(base) + " (EMF's content build skips names that start with '.', as EML "
                            "does); on renames it back.";
            if (!off) {
                bool built = false;
                for (const auto& m : c.content->mods) built = built || m.folder == utf8(d.name);
                if (built && c.emf_content) {
                    e.note = "EML content: built by EMF at the start.";
                } else if (built) {
                    e.loaded_by = "not built";
                    e.note = "EML content: the last build with these files failed, so the game starts without it. EMF tries "
                             "again when a mod, the game or emf_content.dll changes (the EMF tab has the details).";
                } else {
                    e.loaded_by = "left out";
                    for (const auto& l : c.content->left_out)
                        if (l.folder == utf8(d.name) && e.error.empty()) e.note = "Not built by EMF: " + l.why + ".";
                }
            }
        } else {
            e.loaded_by = "EML";
            e.enabled = !off;
            e.can_toggle = true;
            e.toggle_note = "Off renames it to ." + utf8(base) + " (EML skips names that start with '.'); on renames it back.";
            if (extended) e.note = "A ShroudForge package. ShroudForge is not the active loader here, so EML loads it.";
        }
        if (off && exists(dir + L"\\" + base))
            warn(s, "warn", "mods\\" + utf8(base) + " and mods\\." + utf8(base) +
                                " both exist (two copies of one mod). Remove the one you do not want; EMF cannot switch it until then.");
        s.mods.push_back(std::move(e));
    }
}

// The tool of a DLL in the game folder ("" unknown or missing).
std::string dll_tool(const Context& c, const std::wstring& file) {
    const std::wstring path = c.game + L"\\" + file;
    return exists(path) ? core::tool_for_dll_text(c.tools, dll_signature(path, c.tools, c.table)) : "";
}

// The same for names shown and statuses: the light scan reads no DLL for those (the names stay the file names).
std::string tool_of(const Context& c, const std::wstring& file) { return c.light ? std::string() : dll_tool(c, file); }

// The DLLs emf\chain_status.txt says the last start skipped ("<name>\tskipped": a start without any mods).
std::vector<std::string> skipped_chain_names(const std::string& chain_status) {
    std::vector<std::string> out;
    size_t from = 0;
    while (from < chain_status.size()) {
        size_t to = chain_status.find('\n', from);
        if (to == std::string::npos) to = chain_status.size();
        const std::string line = loader::trim_chain(chain_status.substr(from, to - from));
        from = to + 1;
        const size_t tab = line.find('\t');
        if (tab != std::string::npos && line.substr(tab + 1) == "skipped") out.push_back(line.substr(0, tab));
    }
    return out;
}

// ---- DLL mods EMF starts: dinput8-chain.ini, with emf\chain_status.txt from the last game start ----
void scan_chain(FolderState& s, const Context& c) {
    std::map<std::wstring, std::string> status;  // lower-case name -> started / missing / failed N
    const std::string st = c.light ? std::string() : read_text(c.game + L"\\emf\\chain_status.txt");
    size_t from = 0;
    while (from < st.size()) {
        size_t to = st.find('\n', from);
        if (to == std::string::npos) to = st.size();
        std::string line = loader::trim_chain(st.substr(from, to - from));
        from = to + 1;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        std::wstring name = loader::widen_chain(line.substr(0, tab));
        CharLowerBuffW(name.data(), static_cast<DWORD>(name.size()));
        status[name] = line.substr(tab + 1);
    }
    const auto status_of = [&](std::wstring name) {
        CharLowerBuffW(name.data(), static_cast<DWORD>(name.size()));
        auto it = status.find(name);
        return it == status.end() ? std::string() : it->second;
    };

    auto add = [&](const std::wstring& file, bool on) {
        ModEntry e;
        e.source = "chain";
        e.id = utf8(file);
        e.path = c.game + L"\\" + file;
        e.loaded_by = "EMF";
        e.enabled = on;
        e.can_toggle = true;
        e.toggle_note = "Off puts \"; \" in front of its line in dinput8-chain.ini; on takes it away.";
        const bool here = exists(e.path);
        const std::string word = tool_of(c, file);
        e.name = word.empty() || word == "emf" ? e.id : tool_display(c.tools, word);
        e.status = status_of(file);
        e.note = "Listed in dinput8-chain.ini: EMF's loader starts it before the game runs.";
        if (on && !here) e.error = "listed in dinput8-chain.ini, but " + e.id + " is not in the game folder";
        else if (on && e.status.rfind("failed", 0) == 0) {
            // "failed 193": Windows' own words for the error ("%1 is not a valid Win32 application."), with the name
            std::string why = windows_error_text(static_cast<DWORD>(std::strtoul(e.status.c_str() + 6, nullptr, 10)));
            if (const size_t at = why.find("%1"); at != std::string::npos) why.replace(at, 2, e.id);
            e.error = "did not load at the last game start: " + why;
        }
        else if (on && e.status.empty()) e.note += " Not started yet: it was listed after the last game start.";
        else if (on && e.status == "skipped")  // the loader's start without mods (loader/play_once.h)
            e.note += " Not started at the last game start: the game was started without any mods (Play's menu).";
        s.mods.push_back(std::move(e));
    };
    for (const auto& n : c.chain_active) add(n, true);
    for (const auto& n : switched_off_chain_entries(c.chain_text)) {
        bool active = false;
        for (const auto& a : c.chain_active) active = active || loader::same_name(a, n);
        if (!active && exists(c.game + L"\\" + n)) add(n, false);  // a "; name.dll" for a file that is not there is just a comment
    }

    // DLLs EMF's installer renamed (emf\moved.txt) that dinput8-chain.ini does not list
    const std::string moved = read_text(c.game + L"\\emf\\moved.txt");
    from = 0;
    while (from < moved.size()) {
        size_t to = moved.find('\n', from);
        if (to == std::string::npos) to = moved.size();
        const std::string line = loader::trim_chain(moved.substr(from, to - from));
        from = to + 1;
        if (line.rfind("moved\t", 0) != 0) continue;
        const std::wstring file = loader::widen_chain(line.substr(6));
        if (file.empty() || !loader::inside_game_folder(file) || !exists(c.game + L"\\" + file)) continue;
        bool listed = false;
        for (const auto& m : s.mods) listed = listed || (m.source == "chain" && loader::same_name(wide(m.id), file));
        if (listed) continue;
        ModEntry e;
        e.source = "chain";
        e.id = utf8(file);
        e.path = c.game + L"\\" + file;
        const std::string word = tool_of(c, file);
        e.name = word.empty() || word == "emf" ? e.id : tool_display(c.tools, word);
        e.loaded_by = "EMF";
        e.enabled = false;
        // kept aside on purpose only when its tool also runs through another proxy name it uses (same-loader), as the
        // installer decides it
        std::string runs_as;
        if (!word.empty() && word != "emf")
            for (const auto& t : c.tools)
                if (t.word == word)
                    for (const auto& other : t.same_loader)
                        if (tool_of(c, wide(other)) == word) runs_as = other;
        if (!runs_as.empty()) {
            e.status = "kept aside";
            e.toggle_note = "Not started on purpose: the same tool already runs as " + runs_as + ".";
            e.note = "EMF's installer renamed this DLL because the same tool already runs as " + runs_as +
                     ", and starting it too would run the tool twice. It is kept so that uninstalling EMF can put it back.";
        } else {
            e.status = "not listed";
            e.toggle_note = "Not listed in dinput8-chain.ini, so EMF does not start it.";
            e.note = "renamed by EMF's installer; not listed in dinput8-chain.ini. It is kept so that uninstalling EMF can "
                     "put it back.";
        }
        s.mods.push_back(std::move(e));
    }
}

// ---- other loaders: their proxy DLLs, Greeneye mods, Shroudtopia mods, ModLoader Proxy's list ----
void scan_other(FolderState& s, const Context& c) {
    for (const auto& [file, word] : c.proxies) {
        if (word == "emf") continue;
        ModEntry e;
        e.source = "other";
        e.is_loader = true;
        e.id = "loader:" + utf8(file);
        e.name = word.empty() ? "Unknown DLL mod (" + utf8(file) + ")" : tool_display(c.tools, word);
        e.loaded_by = "the game (as " + utf8(file) + ")";
        e.path = c.game + L"\\" + file;
        e.can_toggle = false;
        e.toggle_note = "EMF does not switch other loaders.";
        e.note = utf8(file) + " sits next to the game, which loads it by that name. It manages its own mods.";
        s.mods.push_back(std::move(e));
    }

    // Greeneye Loader: greeneye\<mod>\mod.json (id, name, version, dll, side, api, description, author)
    for (const auto& d : list_dir(c.game + L"\\greeneye")) {
        if (!d.dir) continue;
        ModEntry e;
        e.source = "other";
        e.id = "greeneye:" + utf8(d.name);
        e.path = c.game + L"\\greeneye\\" + d.name;
        e.name = utf8(d.name);
        e.loaded_by = "Greeneye Loader";
        e.toggle_note = "Greeneye Loader manages it.";
        std::string text, why;
        if (read_file(e.path + L"\\mod.json", text, 1u << 20)) {
            if (auto v = parse_json(to_utf8_text(strip_bom(text)), &why)) {
                if (!json_text(*v, "name").empty()) e.name = json_text(*v, "name");
                e.version = json_text(*v, "version");
                e.description = json_text(*v, "description");
                e.authors = json_authors(*v);
                if (!json_text(*v, "side").empty()) e.note = "Runs on the " + json_text(*v, "side") + " side.";
            } else {
                e.error = "mod.json is " + why;
            }
        }
        if (!c.light) e.readme = read_readme(e.path);
        s.mods.push_back(std::move(e));
    }

    // Shroudtopia / Shroudweave: shroudtopia.json "mods": {"<name>": {"active": bool}}; its DLLs are in mods (folders, loose)
    const bool shroudtopia = exists(c.game + L"\\shroudtopia.json");
    // a DLL folder in mods\ is also read by EML, which stops the game at start for a folder without mod.json
    const auto eml_trouble = [&](const std::wstring& path) {
        return c.eml_loader && is_dir(path) ? std::string("EML also runs here and stops the game at start for a folder in "
                                                          "mods\\ without mod.json: rename this folder with a leading '.' "
                                                          "or move it out of mods\\")
                                            : std::string();
    };
    std::vector<bool> matched(c.shroudtopia_dlls.size(), false);
    if (shroudtopia)
        if (auto v = parse_json(to_utf8_text(strip_bom(read_text(c.game + L"\\shroudtopia.json"))), nullptr))
            if (const Value* mods = v->get("mods"); mods && mods->is(VT::Object))
                for (const auto& [name, cfg] : mods->obj) {
                    ModEntry e;
                    e.source = "other";
                    e.id = "shroudtopia:" + name;
                    e.name = name;
                    e.loaded_by = utf8(c.shroudtopia_name);
                    e.path = c.game + L"\\shroudtopia.json";
                    if (const Value* a = cfg.get("active"); a && a->is(VT::Bool)) e.enabled = a->b;
                    e.toggle_note = "Shroudtopia manages it (\"active\" in shroudtopia.json).";
                    for (size_t i = 0; i < c.shroudtopia_dlls.size(); ++i)
                        if (!matched[i] && iequals(c.shroudtopia_dlls[i].first, wide(name))) {
                            matched[i] = true;
                            e.path = c.shroudtopia_dlls[i].second;
                            e.error = eml_trouble(e.path);
                        }
                    s.mods.push_back(std::move(e));
                }
    const auto modloader_proxy_starts = [&](const std::wstring& path) {
        for (const auto& d : c.chainloader)
            if (!d.outside && d.found && iequals(c.game + L"\\" + wide(d.name), path)) return true;
        return false;
    };
    for (size_t i = 0; i < c.shroudtopia_dlls.size(); ++i) {
        // (one ModLoader Proxy starts is listed under ModLoader Proxy)
        if (matched[i] || modloader_proxy_starts(c.shroudtopia_dlls[i].second)) continue;
        ModEntry e;
        e.source = "other";
        e.id = "shroudtopia:" + utf8(c.shroudtopia_dlls[i].first);
        e.name = utf8(c.shroudtopia_dlls[i].first);
        e.path = c.shroudtopia_dlls[i].second;
        e.loaded_by = shroudtopia ? utf8(c.shroudtopia_name) : "no loader found";
        e.toggle_note = "Shroudtopia manages it.";
        if (shroudtopia) e.note = "A DLL mod in mods\\ that shroudtopia.json does not list yet.";
        else e.error = "a DLL mod in mods\\, but Shroudtopia (shroudtopia.json) is not installed, so nothing loads it";
        if (const std::string t = eml_trouble(e.path); !t.empty()) e.error = e.error.empty() ? t : e.error + ". " + t;
        s.mods.push_back(std::move(e));
    }

    // ModLoader Proxy's list: what it starts from its chainloader.ini (chainloader_entries, as the status reads it); it
    // reads the file itself, with its own settings
    if (c.chainloader_ini) {
        for (const auto& d : c.chainloader) {
            ModEntry e;
            e.source = "other";
            e.id = "modloader-proxy:" + d.name;
            e.name = d.name;
            e.path = d.outside ? wide(d.name) : c.game + L"\\" + wide(d.name);
            e.loaded_by = "ModLoader Proxy";
            e.toggle_note = "ModLoader Proxy reads chainloader.ini itself.";
            e.note = d.outside ? "Started by ModLoader Proxy from outside the game folder: EMF does not look at it."
                               : "Started by ModLoader Proxy (chainloader.ini, its own list).";
            if (!d.outside && !d.found)
                e.error = exists(e.path) ? "listed in chainloader.ini, but it is not a DLL file"
                                         : "listed in chainloader.ini, but not in the game folder";
            s.mods.push_back(std::move(e));
        }
        if (!c.modloader_proxy_as.empty())
            warn(s, "info", "ModLoader Proxy (" + c.modloader_proxy_as +
                                ") starts the DLLs listed in chainloader.ini. It reads that file itself, with its own "
                                "settings.");
        else if (c.modloader_proxy)
            warn(s, "info", "EMF starts ModLoader Proxy (dinput8-chain.ini), and ModLoader Proxy starts the DLLs "
                            "listed in chainloader.ini. It reads that file itself, with its own settings.");
        else
            warn(s, "info", "chainloader.ini is ModLoader Proxy's list: ModLoader Proxy reads it itself, with its own "
                            "settings. EMF starts ModLoader Proxy when its DLL is listed in dinput8-chain.ini.");
    }
}

// ShroudForge can hand mods\ to EML instead of running them itself: "modloader": "eml" in its config.
bool shroudforge_hands_to_eml(const std::wstring& game) {
    const auto v = parse_json(to_utf8_text(strip_bom(read_text(game + L"\\shroudforge\\config\\modloader-config.json"))), nullptr);
    return v && lower(json_text(*v, "modloader")) == "eml";
}

// A key for each entry, unique in the list: the page selects rows by it (two entries can share an id, or a path).
void assign_keys(FolderState& s) {
    std::set<std::string> used;
    for (auto& m : s.mods) {
        std::string key = m.source + ":" + utf8(m.path);
        if (used.count(key)) key += "|" + m.id;
        for (int n = 2; used.count(key); ++n) key = m.source + ":" + utf8(m.path) + "|" + m.id + "#" + std::to_string(n);
        used.insert(key);
        m.key = key;
    }
}

std::wstring trim_slashes(std::wstring dir) {
    while (dir.size() > 3 && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    return dir;
}

}  // namespace

void set_fallback_known_tools(const std::string& table) { g_fallback_tools = table; }

ModSide read_mod_side(const core::json::Value& mod_json) {
    ModSide m;
    const Value* x = mod_json.get("side");
    if (!x) return m;
    const std::string v = x->is(VT::String) ? lower_trimmed(x->s) : std::string();
    if (v == "server" || v == "client" || v == "both") {
        m.side = v;
        m.declared = true;
    } else {
        m.error = "\"side\" must be \"server\", \"client\" or \"both\" (where the mod has to run); EMF loads it everywhere";
    }
    return m;
}

std::string emf_core_version(const std::wstring& dll) { return core_version(dll); }

bool is_game_folder(const std::wstring& dir) {
    if (dir.empty()) return false;
    const std::wstring d = trim_slashes(dir);
    return exists(d + L"\\enshrouded.exe") || exists(d + L"\\enshrouded_server.exe");
}

FolderState scan_mod_folder(const std::wstring& game_in, ScanDepth depth) {
    ++g_folder_reads;
    FolderState s;
    s.game = trim_slashes(game_in);
    if (s.game.empty() || !is_dir(s.game)) {
        warn(s, "error", s.game.empty() ? "No game folder chosen yet." : "The folder " + utf8(s.game) + " does not exist.");
        return s;
    }
    const bool client = exists(s.game + L"\\enshrouded.exe"), server = exists(s.game + L"\\enshrouded_server.exe");
    if (!client && !server) {
        warn(s, "error", "There is no enshrouded.exe or enshrouded_server.exe in " + utf8(s.game) +
                             ". Choose the folder the game (or a dedicated server) is installed in.");
        return s;
    }
    s.game_found = true;
    s.server = !client;

    Context c;
    c.game = s.game;
    c.light = depth == ScanDepth::Profiles;
    // the light scan reads DLLs only to tell whether ShroudForge is the active loader (EML packages are read-only then)
    const bool sf_folder = is_dir(s.game + L"\\shroudforge");
    const bool read_dlls = !c.light || sf_folder;
    if (!read_file(s.game + L"\\emf\\known_tools.txt", c.table, 1u << 20)) c.table = g_fallback_tools;
    c.tools = core::parse_known_tools(c.table);
    for (const auto& t : c.tools)
        if (t.word == "shroudtopia") c.shroudtopia_name = wide(t.name);

    // the proxy DLLs and the start-up report EMF's core writes (installed_report)
    core::GameFolderView view;
    std::vector<std::string> words;  // tools whose DLL runs: proxies and chained DLLs
    for (const auto& name : core::proxy_dll_names()) {
        const std::wstring path = s.game + L"\\" + wide(name);
        if (!exists(path)) continue;
        const std::string word = read_dlls ? core::tool_for_dll_text(c.tools, dll_signature(path, c.tools, c.table)) : "";
        view.proxies.emplace_back(name, word);
        s.proxies.emplace_back(name, word);
        c.proxies.emplace_back(wide(name), word);
        words.push_back(word);
    }
    view.chain_status = c.light ? std::string() : read_text(s.game + L"\\emf\\chain_status.txt");
    // a DLL the last start skipped (Play without any mods, loader/play_once.h) runs again at the next normal start: its
    // tool counts as running, so the report does not say another tool took its file name
    std::vector<std::string> running = core::started_chain_names(view.chain_status);
    for (const auto& name : skipped_chain_names(view.chain_status)) running.push_back(name);
    for (const auto& name : running) {
        const std::wstring file = loader::widen_chain(name);
        if (loader::inside_game_folder(file)) view.chained_tools.push_back(tool_of(c, file));
    }
    s.chained_tools = view.chained_tools;
    s.tools_table = c.table;
    for (const auto& t : c.tools)
        for (const auto& b : t.beside)
            if (exists(s.game + L"\\" + wide(b))) view.beside.push_back(b);
    // dinput8-chain.ini: one that is there but cannot be read is said so (its DLLs are not listed), never taken as empty
    std::string chain_bytes;
    const std::wstring chain_path = s.game + L"\\" + kChainFile;
    if (!read_file(chain_path, chain_bytes, 1u << 20) && exists(chain_path)) {
        s.read_error = "Could not read dinput8-chain.ini, so the DLL mods EMF starts are not listed. Is another program "
                       "using it?";
        warn(s, "error", s.read_error);
    }
    c.chain_text = decode_chain(chain_bytes).text;
    c.chain_active = loader::parse_chain_list(c.chain_text);
    for (const auto& n : c.chain_active) {
        const std::string w = read_dlls ? dll_tool(c, n) : std::string();
        view.chain_listed.emplace_back(utf8(n), w);  // what EMF starts at the next game start
        if (!w.empty()) words.push_back(w);
    }

    // ModLoader Proxy (a proxy DLL, or started by EMF: listed in dinput8-chain.ini or started at the last game start)
    // starts the DLLs in its chainloader.ini: EMF's loader itself, ShroudForge, ...
    core::FolderLook look;
    look.read_text = [&](const std::string& rel) { return read_text(s.game + L"\\" + wide(rel)); };
    look.exists = [&](const std::string& rel) {  // a file, not a folder
        const std::wstring path = s.game + L"\\" + wide(rel);
        return exists(path) && !is_dir(path);
    };
    look.tool = [&](const std::string& rel) { return read_dlls ? dll_tool(c, wide(rel)) : std::string(); };
    look.dlls_in = [&](const std::string& rel) {
        std::vector<std::string> names;
        for (const auto& d : list_dir(s.game + L"\\" + wide(rel)))
            if (!d.dir && ends_with_nocase(d.name, L".dll")) names.push_back(utf8(d.name));
        return names;
    };
    std::string self;
    c.modloader_proxy = core::modloader_proxy_runs(view, &self);
    for (const auto& p : view.proxies)
        if (c.modloader_proxy && p.first == self) c.modloader_proxy_as = self;
    if (c.modloader_proxy) {
        core::add_loader_started(view, look);
        for (const auto& d : view.loader_started)
            if (d.found && !d.outside && !d.word.empty()) words.push_back(d.word);
    }
    const std::string list_of = c.modloader_proxy ? self : std::string();  // the ini next to its DLL, else the game's
    const size_t slash = list_of.find_last_of("\\/");
    c.chainloader_ini = exists(s.game + L"\\" + wide(slash == std::string::npos ? "" : list_of.substr(0, slash + 1)) +
                               L"chainloader.ini");
    if (c.chainloader_ini)
        c.chainloader = c.modloader_proxy ? view.loader_started : core::chainloader_entries("", look);
    if (!c.light)
        for (const auto& line : core::installed_report(c.tools, view)) {
            s.report.push_back(line);
            if (line.rfind("next to the game:", 0) != 0 && line.rfind("started by ", 0) != 0) warn(s, "warn", line);
        }

    const auto runs = [&](const char* w) { return std::find(words.begin(), words.end(), w) != words.end(); };
    s.emf_installed = runs("emf") && exists(s.game + L"\\emf\\emf_core.dll");
    // the loader that reads Play's flag: dinput8.dll when it is EMF's, else EMF's loader that ModLoader Proxy starts
    std::string emf_loader = "dinput8.dll";
    for (const auto& d : view.loader_started)
        if (d.found && !d.outside && d.word == "emf") {
            emf_loader = d.name;
            break;
        }
    for (const auto& p : view.proxies)
        if (p.second == "emf") emf_loader = p.first;
    s.play_once = s.emf_installed && !c.light && loader_plays_once(s.game + L"\\" + wide(emf_loader));
    if (!c.light) s.emf_version = exists(s.game + L"\\emf\\emf_core.dll") ? core_version(s.game + L"\\emf\\emf_core.dll") : "";
    s.shroudforge_active = sf_folder && runs("shroudforge") && !shroudforge_hands_to_eml(s.game);
    c.eml_loader = runs("eml") || runs("shroudforge");
    // EMF builds the EML mods' content in the game's folder when EMF and emf_content.dll are installed: the EMF tab's
    // card says what the next start does, with the loader's own rules (another tool, [content] mode, the mods, the base,
    // the last build), and the list follows it
    if (!c.light && s.emf_installed && exists(s.game + L"\\emf\\emf_content.dll")) {
        c.content = content_panel(s.game, s.server, false);  // without walking the copy: this scan runs every few seconds
        c.emf_content = c.content->status == ContentStatus::Ready || c.content->status == ContentStatus::BuildsNext;
        c.content_failed = c.content->status == ContentStatus::Failed;
        c.content_off = c.content->status == ContentStatus::Off;
    }

    scan_emf_mods(s, c);
    scan_eml_mods(s, c);
    scan_chain(s, c);
    scan_other(s, c);
    assign_keys(s);
    if (c.light) return s;  // no warnings: profiles read the switches only
    note_shared_keys(s);

    // warnings that need the whole list
    std::vector<Warning> first;
    if (!s.emf_installed)
        first.push_back({"warn", "EMF is not installed in this folder, so EMF mods and the DLLs in dinput8-chain.ini do not run. "
                                 "Install it with install.cmd from the EMF package."});
    bool eml_mods = false;
    for (const auto& m : s.mods) {
        if (m.source == "eml") eml_mods = true;
        if (m.error.empty()) continue;
        const char* group = m.source == "emf" ? "EMF mod" : m.source == "eml" ? "EML mod" : m.source == "chain" ? "DLL mod" : "mod";
        first.push_back({"warn", m.name + " (" + group + "): " + m.error});
    }
    if (eml_mods && c.content_failed)
        first.push_back({"warn", "EML content could not be built: " +
                                     (c.content->last_build && !c.content->last_build->message.empty()
                                          ? c.content->last_build->message
                                          : std::string("see the EMF tab")) +
                                     ". The game starts without it; EMF tries again when a mod, the game or emf_content.dll changes."});
    else if (eml_mods && !c.eml_loader && c.content_off)
        first.push_back({"warn", "There are EML mods in mods\\, but EMF does not build them (" +
                                     (c.content->detail.empty() || c.content->detail.back() != '.'
                                          ? c.content->detail
                                          : c.content->detail.substr(0, c.content->detail.size() - 1)) +
                                     ") and neither EML nor ShroudForge is next to the game, so nothing loads them."});
    else if (eml_mods && !c.eml_loader && !c.emf_content && !c.content_failed)
        first.push_back({"warn", "There are EML mods in mods\\, but neither EML nor ShroudForge is next to the game, so nothing loads them."});
    s.warnings.insert(s.warnings.begin(), first.begin(), first.end());
    return s;
}

// ---- changes ----

namespace {

Change fail(const std::string& why) { return {false, why}; }

// Finds the EMF mod `id` in emf\mods: its manifest (nullopt for a loose DLL); false when there is none.
bool find_emf_mod(const std::wstring& game, const std::string& id, std::optional<core::Manifest>& manifest) {
    ++g_folder_reads;
    const std::wstring dir = game + L"\\emf\\mods";
    for (const auto& d : list_dir(dir)) {
        if (!d.dir) {
            if (ends_with_nocase(d.name, L".dll") && make_id(utf8(d.name.substr(0, d.name.size() - 4))) == id) {
                manifest.reset();
                return true;
            }
            continue;
        }
        std::string text;
        if (!read_file(dir + L"\\" + d.name + L"\\mod.json", text, core::kMaxManifestBytes + 1)) continue;
        auto r = core::parse_manifest(text);
        if (r.manifest && r.manifest->id == id) {
            manifest = std::move(r.manifest);
            return true;
        }
    }
    return false;
}

// A plain file or folder name in mods\ (no path, no '.' in front: that is the switch).
bool plain_name(const std::string& id) {
    if (id.empty() || id == "." || id == ".." || id[0] == '.') return false;
    return id.find_first_of("\\/:*?\"<>|") == std::string::npos;
}

bool shroudforge_active(const std::wstring& game) { return scan_mod_folder(game, ScanDepth::Profiles).shroudforge_active; }

}  // namespace

Change write_file_whole(const std::wstring& path, const std::string& bytes, const std::string& shown) {
    const std::wstring tmp = path + L".emf-new";
    HANDLE h = CreateFileW(lp(tmp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return fail("Could not write " + shown + ": " + windows_error_text(GetLastError()));
    DWORD put = 0;
    const BOOL ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &put, nullptr) && put == bytes.size();
    const DWORD err = GetLastError();
    CloseHandle(h);
    // a virus scanner may hold either file for a moment: the rename waits for it
    if (!ok || !emf::move_retrying(lp(tmp), lp(path), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD e = ok ? GetLastError() : err;
        DeleteFileW(lp(tmp).c_str());  // only the new file this made
        return fail("Could not write " + shown + ": " + windows_error_text(e));
    }
    return {true, {}};
}

std::string known_tools_table(const std::wstring& game) {
    std::string table;
    if (!game.empty() && read_file(trim_slashes(game) + L"\\emf\\known_tools.txt", table, 1u << 20)) return table;
    return g_fallback_tools;
}

std::string dll_tool_word(const std::wstring& game, const std::wstring& rel) {
    const std::wstring path = trim_slashes(game) + L"\\" + rel;
    if (game.empty() || !exists(path) || is_dir(path)) return {};
    const std::string table = known_tools_table(game);
    const auto tools = core::parse_known_tools(table);
    return core::tool_for_dll_text(tools, dll_signature(path, tools, table));
}

Change edit_chain_file(const std::wstring& game_in, const std::function<std::optional<std::string>(const std::string&)>& edit,
                       bool* created) {
    if (created) *created = false;
    const std::wstring path = trim_slashes(game_in) + L"\\" + kChainFile;
    const size_t max = 1u << 20;
    std::string bytes;
    const bool there = read_file(path, bytes, max + 1);
    if (!there && exists(path)) return fail("Could not read dinput8-chain.ini.");
    if (bytes.size() > max)  // never write back a file that was not read whole
        return fail("dinput8-chain.ini is too large to edit here (over 1 MB). Change it in a text editor.");
    const ChainText file = decode_chain(bytes);
    const auto changed = edit(file.text);
    if (!changed) return {true, {}};
    const Change c = write_file_whole(path, encode_chain(*changed, file.utf16), "dinput8-chain.ini");
    if (c.ok && created) *created = !there;
    return c;
}

size_t folder_reads() { return g_folder_reads.load(); }

FolderFacts folder_facts(const FolderState& s) {
    FolderFacts f;
    f.shroudforge_active = s.shroudforge_active;
    for (const auto& m : s.mods) {
        if (m.source != "emf" || m.state_forced || !core::valid_mod_id(m.id)) continue;  // EMF finds no mod there
        std::vector<core::SettingDef> defs;
        for (const auto& st : m.settings) defs.push_back(st.def);
        f.emf_mods.emplace(m.id, std::move(defs));  // two folders with one id: the first, as EMF finds it
    }
    return f;
}

Change set_mod_enabled(const std::wstring& game, const std::string& source, const std::string& id, bool on) {
    return set_mod_enabled(game, source, id, on, nullptr);
}

Change set_mod_setting(const std::wstring& game, const std::string& id, const std::string& key, const std::string& value) {
    return set_mod_setting(game, id, key, value, nullptr);
}

Change set_mod_enabled(const std::wstring& game_in, const std::string& source, const std::string& id, bool on,
                       const FolderFacts* facts) {
    const std::wstring game = trim_slashes(game_in);
    if (!is_game_folder(game)) return fail("The game folder is not there any more.");
    if (source == "emf") {
        std::optional<core::Manifest> m;
        const bool there = core::valid_mod_id(id) && (facts ? facts->emf_mods.count(id) > 0 : find_emf_mod(game, id, m));
        if (!there) return fail("There is no EMF mod \"" + id + "\" in emf\\mods.");
        core::FileIniBackend ini(game + L"\\emf\\config");
        core::SettingsStore store(&ini);
        if (!store.set_enabled(id, on))
            return fail("Could not write emf\\config\\" + id + ".ini: " + windows_error_text(GetLastError()));
        return {true, {}};
    }
    if (source == "eml") {
        if (!plain_name(id)) return fail("\"" + id + "\" is not a mod in mods\\.");
        const bool zip = ends_with_nocase(wide(id), L".zip");  // ShroudForge ignores zips: EML's rule applies
        if (!zip && (facts ? facts->shroudforge_active : shroudforge_active(game)))
            return fail("ShroudForge is the active loader here: switch it in ShroudForge (F9).");
        const std::wstring dir = game + L"\\mods\\", on_name = wide(id), off_name = L"." + wide(id);
        const std::wstring from = dir + (on ? off_name : on_name), to = dir + (on ? on_name : off_name);
        if (!exists(from)) {
            if (exists(to)) return {true, {}};  // already switched that way
            return fail("mods\\" + id + " is not there any more.");
        }
        if (exists(to))
            return fail("Cannot rename: mods\\" + utf8(on ? on_name : off_name) +
                        " already exists (two copies of this mod?). Remove the one you do not want first.");
        if (!emf::move_retrying(from, to, 0)) {
            const DWORD e = GetLastError();
            return fail("Could not rename mods\\" + utf8(on ? off_name : on_name) + ": " + windows_error_text(e) +
                        (e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED ? ". Is a program using its files?" : ""));
        }
        return {true, {}};
    }
    if (source == "chain") {
        const std::wstring name = wide(id);
        if (name.empty() || !loader::inside_game_folder(name) || !ends_with_nocase(name, L".dll"))
            return fail("\"" + id + "\" is not a DLL in the game folder.");
        const std::wstring path = game + L"\\" + kChainFile;
        const size_t max = 1u << 20;
        std::string bytes;
        if (!read_file(path, bytes, max + 1)) return fail("dinput8-chain.ini is not there any more.");
        if (bytes.size() > max)  // never write back a file that was not read whole
            return fail("dinput8-chain.ini is too large to edit here (over 1 MB). Change it in a text editor.");
        const ChainText file = decode_chain(bytes);
        const auto changed = switch_chain_line(file.text, name, on);
        if (!changed) {
            bool active = false, off = false;
            for (const auto& n : loader::parse_chain_list(file.text)) active = active || loader::same_name(n, name);
            for (const auto& n : switched_off_chain_entries(file.text)) off = off || loader::same_name(n, name);
            if (active == on && (active || off)) return {true, {}};  // already so
            return fail(id + " is not listed in dinput8-chain.ini.");
        }
        return write_file_whole(path, encode_chain(*changed, file.utf16), "dinput8-chain.ini");  // in the file's own encoding
    }
    return fail("Its own loader manages this mod; EMF does not switch it.");
}

Change set_mod_setting(const std::wstring& game_in, const std::string& id, const std::string& key, const std::string& value,
                       const FolderFacts* facts) {
    const std::wstring game = trim_slashes(game_in);
    if (!is_game_folder(game)) return fail("The game folder is not there any more.");
    std::vector<core::SettingDef> defs;
    if (facts) {
        auto it = core::valid_mod_id(id) ? facts->emf_mods.find(id) : facts->emf_mods.end();
        if (it == facts->emf_mods.end()) return fail("There is no EMF mod \"" + id + "\" with settings.");
        defs = it->second;
    } else {
        std::optional<core::Manifest> m;
        if (!core::valid_mod_id(id) || !find_emf_mod(game, id, m) || !m) return fail("There is no EMF mod \"" + id + "\" with settings.");
        defs = m->settings;
    }
    SaveOneIni ini(game + L"\\emf\\config");
    core::SettingsStore store(&ini);
    for (const auto& def : defs) store.declare(id, def);
    ini.writing = true;
    std::string why;
    switch (store.set(id, key, value, &why)) {
        case core::SettingsStore::SetResult::Ok:
            return {true, {}};
        case core::SettingsStore::SetResult::Unchanged:
            return {true, {}, true};
        case core::SettingsStore::SetResult::WriteFailed:
            return fail(why + ": " + windows_error_text(GetLastError()));
        default:
            return fail(why.empty() ? "That value is not valid." : why);
    }
}

Change reset_mod_setting(const std::wstring& game_in, const std::string& id, const std::string& key) {
    const std::wstring game = trim_slashes(game_in);
    if (!is_game_folder(game)) return fail("The game folder is not there any more.");
    std::optional<core::Manifest> m;
    if (!core::valid_mod_id(id) || !find_emf_mod(game, id, m) || !m) return fail("There is no EMF mod \"" + id + "\" with settings.");
    SaveOneIni ini(game + L"\\emf\\config");
    core::SettingsStore store(&ini);
    for (const auto& def : m->settings) store.declare(id, def);
    ini.writing = true;
    std::vector<std::string> keys;
    if (key.empty())
        for (const auto& d : store.defs(id)) keys.push_back(d.key);
    else
        keys.push_back(key);
    bool changed = false;
    for (const auto& k : keys) {
        std::string why;
        switch (store.reset(id, k, &why)) {
            case core::SettingsStore::SetResult::Ok: changed = true; break;
            case core::SettingsStore::SetResult::Unchanged: break;
            case core::SettingsStore::SetResult::WriteFailed: return fail(why + ": " + windows_error_text(GetLastError()));
            default: return fail(why);
        }
    }
    return {true, {}, !changed};
}

// ---- dinput8-chain.ini lines ----

// The note above a new [EMF] section (the installer's Add-ChainEntry writes the same).
const char* const kEmfChainNote =
    "; [EMF]: DLLs EMF renamed, started by EMF only. [Chain]: read by every chain loader.";

namespace {
// Calls fn(line_start, body) for each line that counts, as the loader reads the file: lines before any section and
// lines in [Chain] and [EMF] (other sections are another tool's). body is the line without its line break (and without the BOM
// on the first line).
template <class Fn>
void each_chain_line(const std::string& text, Fn fn) {
    bool counted = true;
    size_t from = 0;
    while (from < text.size()) {
        size_t nl = text.find('\n', from);
        const size_t end = nl == std::string::npos ? text.size() : nl;
        size_t start = from;
        if (from == 0 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) start = 3;
        std::string body = text.substr(start, end - start);
        if (!body.empty() && body.back() == '\r') body.pop_back();
        from = nl == std::string::npos ? text.size() : nl + 1;
        const std::string t = loader::trim_chain(body);
        if (!t.empty() && t[0] == '[') {
            counted = _stricmp(t.c_str(), "[chain]") == 0 || _stricmp(t.c_str(), "[emf]") == 0;
            continue;
        }
        if (counted) fn(start, body);
    }
}

bool names(const std::string& line, const std::wstring& dll) {
    const auto n = loader::parse_chain_list(line);
    return n.size() == 1 && loader::same_name(n[0], dll);
}

// How many bytes at the start of a switched-off line make it a comment: "; " (what EMF writes), or the ';' (and
// spaces) another tool or the player wrote. 0 when the line is not one.
size_t off_prefix(const std::string& body) {
    if (body.compare(0, 2, "; ") == 0) return 2;
    size_t i = 0;
    while (i < body.size() && (body[i] == ' ' || body[i] == '\t')) ++i;
    if (i >= body.size() || body[i] != ';') return 0;
    while (i < body.size() && (body[i] == ';' || body[i] == ' ' || body[i] == '\t')) ++i;
    return i;
}
}  // namespace

std::optional<std::string> switch_chain_line(const std::string& text, const std::wstring& dll, bool on) {
    // (offset, bytes) where "; " goes in (off) or the comment prefix comes out (on), applied from the back so earlier
    // offsets stay right
    std::vector<std::pair<size_t, size_t>> at;
    each_chain_line(text, [&](size_t start, const std::string& body) {
        if (!on && names(body, dll)) at.emplace_back(start, 0);
        const size_t p = on ? off_prefix(body) : 0;
        if (p && names(body.substr(p), dll)) at.emplace_back(start, p);
    });
    if (at.empty()) return std::nullopt;
    std::string out = text;
    for (auto it = at.rbegin(); it != at.rend(); ++it) {
        if (on) out.erase(it->first, it->second);
        else out.insert(it->first, "; ");
    }
    return out;
}

std::vector<std::wstring> switched_off_chain_entries(const std::string& text) {
    std::vector<std::wstring> out;
    each_chain_line(text, [&](size_t, const std::string& body) {
        const size_t p = off_prefix(body);
        if (!p) return;
        const auto n = loader::parse_chain_list(body.substr(p));
        if (n.size() != 1) return;
        for (const auto& o : out)
            if (loader::same_name(o, n[0])) return;
        out.push_back(n[0]);
    });
    return out;
}

std::optional<std::string> add_chain_line(const std::string& text, const std::wstring& dll) {
    bool listed = false;
    each_chain_line(text, [&](size_t, const std::string& body) { listed = listed || names(body, dll); });
    if (listed) return std::nullopt;
    if (auto on = switch_chain_line(text, dll, true)) return on;  // a switched-off line: on again

    // the file's own line breaks; Windows' for a file that has none yet
    const std::string nl = text.find('\n') == std::string::npos || text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    const std::string name = utf8(dll);
    struct Line {
        size_t start;
        std::string trimmed;
    };
    std::vector<Line> lines;
    for (size_t from = 0; from < text.size();) {
        const size_t nlat = text.find('\n', from);
        const size_t end = nlat == std::string::npos ? text.size() : nlat;
        std::string body = text.substr(from, end - from);
        if (from == 0 && body.compare(0, 3, "\xEF\xBB\xBF") == 0) body.erase(0, 3);
        lines.push_back({from, loader::trim_chain(body)});
        from = nlat == std::string::npos ? text.size() : nlat + 1;
    }
    // EMF's lines go into its own [EMF] section: only EMF's loader reads it (ShroudForge reports [Chain] lines outside
    // mods\<folder>\ as failed); a new [EMF] comes at the end, after a note for players
    int header = -1;
    for (size_t i = 0; i < lines.size(); ++i)
        if (_stricmp(lines[i].trimmed.c_str(), "[emf]") == 0) header = static_cast<int>(i);
    const auto at_end = [&](const std::string& add) {
        std::string out = text;
        if (!out.empty() && out.back() != '\n') out += nl;
        return out + add;
    };
    if (header < 0)
        return at_end((text.empty() ? "" : nl) + std::string(kEmfChainNote) + nl + "[EMF]" + nl + name + nl);
    size_t at = static_cast<size_t>(header) + 1;
    while (at < lines.size() && !(!lines[at].trimmed.empty() && lines[at].trimmed[0] == '[')) ++at;
    while (at > static_cast<size_t>(header) + 1 && lines[at - 1].trimmed.empty()) --at;  // before the section's blank lines
    if (at >= lines.size()) return at_end(name + nl);
    return text.substr(0, lines[at].start) + name + nl + text.substr(lines[at].start);
}

namespace {
// An [EMF] with no line left goes, with EMF's note just above it and one blank line before that: a file EMF added
// [EMF] to is as it was once EMF's last line is out.
std::string drop_empty_emf_section(const std::string& text) {
    struct Line {
        size_t start, end;  // end: after the line break
        std::string trimmed;
    };
    std::vector<Line> lines;
    for (size_t from = 0; from < text.size();) {
        const size_t nlat = text.find('\n', from);
        const size_t end = nlat == std::string::npos ? text.size() : nlat + 1;
        std::string body = text.substr(from, end - from);
        if (from == 0 && body.compare(0, 3, "\xEF\xBB\xBF") == 0) body.erase(0, 3);
        lines.push_back({from, end, loader::trim_chain(body)});
        from = end;
    }
    for (size_t h = 0; h < lines.size(); ++h) {
        if (_stricmp(lines[h].trimmed.c_str(), "[emf]") != 0) continue;
        size_t end = h + 1;
        bool empty = true;
        for (; end < lines.size() && !(!lines[end].trimmed.empty() && lines[end].trimmed[0] == '['); ++end)
            empty = empty && lines[end].trimmed.empty();
        if (!empty) return text;
        size_t from = h;
        if (from > 0 && lines[from - 1].trimmed == kEmfChainNote) --from;
        if (from > 0 && lines[from - 1].trimmed.empty()) --from;
        // a BOM on the first line stays
        const size_t cut = from == 0 && text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : lines[from].start;
        const size_t until = end < lines.size() ? lines[end].start : text.size();
        return text.substr(0, cut) + text.substr(until);
    }
    return text;
}
}  // namespace

std::optional<std::string> remove_chain_line(const std::string& text, const std::wstring& dll) {
    std::vector<std::pair<size_t, size_t>> drop;  // (line start, length with its line break)
    each_chain_line(text, [&](size_t start, const std::string& body) {
        const size_t p = off_prefix(body);
        if (!names(body, dll) && !(p && names(body.substr(p), dll))) return;
        const size_t nlat = text.find('\n', start);  // `start` is after a BOM, which stays
        drop.emplace_back(start, (nlat == std::string::npos ? text.size() : nlat + 1) - start);
    });
    if (drop.empty()) return std::nullopt;
    std::string out = text;
    for (auto it = drop.rbegin(); it != drop.rend(); ++it) out.erase(it->first, it->second);
    return drop_empty_emf_section(out);
}

}  // namespace emf::app
