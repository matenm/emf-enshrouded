#include "mods.h"

#include <algorithm>
#include <fstream>
#include <map>

#include "manifest.h"
#include "mod_order.h"
#include "mod_registry.h"
#include "settings.h"

namespace emf::core {
namespace {

std::string utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring wide(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// A file or folder name as a mod id: lower case, other characters become '_'
std::string make_id(const std::string& name) {
    std::string id;
    for (char c : lower(name)) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
        id += ok ? c : '_';
        if (id.size() == 48) break;
    }
    return id.empty() ? "mod" : id;
}

// 1 = loaded, 0 = mod returned false, -1 = faulted
int guarded_load(EmfModLoadFn fn, const EmfApi* api) {
    __try {
        return fn(api) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Reads at most `max` bytes (a larger file gives max bytes; its parser refuses it as too large).
bool read_file(const std::wstring& path, std::string& out, size_t max) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(max, '\0');
    f.read(out.data(), static_cast<std::streamsize>(max));
    out.resize(static_cast<size_t>(f.gcount()));
    return true;
}

// The script host mod that runs a script mod's entry file ("" when no known host runs it: then it is not checked).
std::string script_host_of(const std::string& main) {
    if (main.size() > 4 && lower(main.substr(main.size() - 4)) == ".lua") return "lua";
    return "";
}

struct Found {
    ModRecord rec;
    std::wstring dll;  // empty for script mods and rejected ones
    std::optional<Manifest> manifest;
    bool rejected = false;
    std::string sort_name;
    std::string host;               // a script mod's host mod ("lua"), "" otherwise
    std::vector<Dependency> deps;   // the manifest's dependencies, plus the host for a script mod
};

std::vector<Found> discover(const std::wstring& dir) {
    std::vector<Found> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    std::vector<std::wstring> files, folders;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) folders.push_back(name);
        else if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".dll") == 0) files.push_back(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    for (const auto& f : files) {
        Found x;
        const std::string name = utf8(f);
        x.rec.id = make_id(name.substr(0, name.size() - 4));
        x.rec.name = name.substr(0, name.size() - 4);
        x.rec.version = "";
        x.rec.source = name;
        x.dll = dir + L"\\" + f;
        x.sort_name = name;
        out.push_back(std::move(x));
    }
    for (const auto& folder : folders) {
        const std::wstring base = dir + L"\\" + folder;
        std::string text;
        // not a mod folder (data, a script mod's files...) without one; one byte over the cap reads as too large
        if (!read_file(base + L"\\mod.json", text, kMaxManifestBytes + 1)) continue;
        Found x;
        const std::string fname = utf8(folder);
        x.sort_name = fname;
        x.rec.source = fname + "\\";
        x.rec.has_manifest = true;
        auto r = parse_manifest(text);
        if (!r.manifest) {
            x.rec.id = make_id(fname);
            x.rec.name = fname;
            x.rec.status = ModStatus::Rejected;
            x.rec.reason = r.error;
            x.rejected = true;
            out.push_back(std::move(x));
            continue;
        }
        const Manifest& m = *r.manifest;
        x.rec.id = m.id;
        x.rec.name = m.name;
        x.rec.version = m.version_text;
        x.rec.description = m.description;
        for (size_t i = 0; i < m.authors.size(); ++i) x.rec.authors += (i ? ", " : "") + m.authors[i];
        x.rec.enabled_default = m.enabled_default;
        x.rec.main = m.main;
        if (m.main.empty()) {
            std::string rel = m.dll;
            std::replace(rel.begin(), rel.end(), '/', '\\');
            x.dll = base + L"\\" + wide(rel);
            if (GetFileAttributesW(x.dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
                x.rec.status = ModStatus::Rejected;
                x.rec.reason = m.dll + " not found in the mod's folder";
                x.rejected = true;
            }
        }
        // A script mod needs its host: ordered after it, and not run (nor counted as a dependency) without it.
        if (!m.main.empty()) x.host = script_host_of(m.main);
        if (x.host == m.id) x.host.clear();
        x.deps = with_script_host(m.depends, x.host);
        x.manifest = m;
        out.push_back(std::move(x));
    }
    return out;
}

}  // namespace

std::vector<LoadedMod> load_mods(const std::wstring& dir, const EmfApi* api,
                                 const std::function<void(const std::string&)>& report, const LoadOptions& opt) {
    std::vector<LoadedMod> mods;
    std::vector<Found> found = discover(dir);

    // Order the mods that can load; the others keep their reason
    std::vector<size_t> cand_of;  // candidate index -> found index
    std::vector<ModCandidate> cands;
    for (size_t i = 0; i < found.size(); ++i) {
        if (found[i].rejected) continue;
        ModCandidate c;
        c.id = found[i].rec.id;
        c.sort_name = found[i].sort_name;
        if (found[i].manifest) {
            c.version = found[i].manifest->version;
            c.depends = found[i].deps;
            c.emf = found[i].manifest->emf;
        }
        cand_of.push_back(i);
        cands.push_back(std::move(c));
    }
    const OrderResult order = order_mods(cands, opt.api_version);
    for (const auto& [ci, why] : order.rejected) {
        Found& f = found[cand_of[ci]];
        f.rejected = true;
        f.rec.status = ModStatus::Rejected;
        f.rec.reason = why;
    }
    for (const auto& f : found)
        if (f.rejected) report("mod " + f.rec.source + ": not loaded: " + f.rec.reason);
    for (const auto& [ci, note] : order.notes) report("mod " + found[cand_of[ci]].rec.source + ": " + note);
    // Every mod is in the registry before the first emf_mod_load (mod_info works from there); each is updated as it
    // loads. Sorted by name; among equal names the mod that is not rejected first, then by folder.
    auto publish = [&] {
        if (!opt.registry) return;
        std::vector<ModRecord> records;
        for (auto& f : found) records.push_back(f.rec);
        std::stable_sort(records.begin(), records.end(), [](const ModRecord& a, const ModRecord& b) {
            const std::string la = lower(a.name), lb = lower(b.name);
            if (la != lb) return la < lb;
            const bool ra = a.status == ModStatus::Rejected, rb = b.status == ModStatus::Rejected;
            if (ra != rb) return rb;  // the mod that owns the name first, rejected copies after
            return a.source < b.source;
        });
        opt.registry->set(std::move(records));
    };
    // The settings of every mod that can load are declared now, switched on or not, before any emf_mod_load: the Mods
    // tab and `set` can change a switched-off mod's settings before it is switched on. Declaring is not a change (no
    // change callbacks run for it).
    if (opt.settings)
        for (size_t ci : order.order) {
            const Found& f = found[cand_of[ci]];
            if (!f.manifest) continue;
            for (const SettingDef& d : f.manifest->settings) {
                std::string why;
                if (!opt.settings->declare(f.rec.id, d, &why)) report("mod " + f.rec.source + ": setting refused: " + why);
                else if (!why.empty()) report("mod " + f.rec.source + ": " + why);
            }
        }
    // Script mods are run by a script host mod, not by the core: their switch is settled here, before the
    // first emf_mod_load, so a host that loads early already sees them as scripts.
    std::map<std::string, ModStatus> status;  // id -> what became of it (for dependants)
    for (size_t ci : order.order) {
        Found& f = found[cand_of[ci]];
        ModRecord& rec = f.rec;
        if (rec.main.empty()) {
            rec.status = ModStatus::Pending;
            continue;
        }
        const bool on = opt.settings ? opt.settings->enabled(rec.id, rec.enabled_default) : true;
        if (!on) {
            rec.status = ModStatus::Disabled;
            rec.reason = rec.enabled_default ? "enabled=0 in emf\\config\\" + rec.id + ".ini"
                                             : "off by default (enabled=1 in emf\\config\\" + rec.id + ".ini turns it on)";
            report("mod " + rec.source + ": switched off (" + rec.reason + ")");
        } else {
            rec.status = ModStatus::Script;
            report("mod " + rec.source + ": script mod (" + rec.main + "), for the script host " +
                   (f.host.empty() ? std::string("that runs it") : f.host));
        }
        status[rec.id] = rec.status;
    }
    publish();

    // Why a dependency (or a script mod's host) that was switched off or failed keeps `f` from running; "" if none.
    // Every dependency comes earlier in the order, so its fate is known here.
    std::map<std::string, Version> versions;  // id -> the version of the mod that owns it
    for (size_t ci : order.order)
        if (found[cand_of[ci]].manifest) versions[cands[ci].id] = cands[ci].version;
    auto unmet = [&](const Found& f) -> std::string {
        return unmet_dependency(
            f.deps, f.host,
            [&](const std::string& id) -> std::optional<ModFate> {
                auto it = status.find(id);
                if (it == status.end()) return std::nullopt;
                if (it->second == ModStatus::Loaded || it->second == ModStatus::Script) return ModFate::Runs;
                return it->second == ModStatus::Disabled ? ModFate::Off : ModFate::NotLoaded;
            },
            [&](const std::string& id) -> std::optional<Version> {
                auto it = versions.find(id);
                return it == versions.end() ? std::nullopt : std::optional<Version>(it->second);
            });
    };
    for (size_t ci : order.order) {
        Found& f = found[cand_of[ci]];
        ModRecord& rec = f.rec;
        if (!rec.main.empty()) {
            // A script mod (its switch settled above): now that its dependencies and host have loaded or not, a script
            // mod that cannot run is rejected (a host that already took it stops it on its first tick), so the mods
            // after it that need it see that.
            if (rec.status != ModStatus::Script) continue;
            const std::string why = unmet(f);
            if (why.empty()) continue;
            rec.status = ModStatus::Rejected;
            rec.reason = why;
            status[rec.id] = rec.status;
            report("mod " + rec.source + ": not run: " + rec.reason);
            if (opt.registry) opt.registry->update(rec);
            continue;
        }
        // a dependency that was disabled or failed at run time
        if (const std::string why = unmet(f); !why.empty()) {
            rec.status = ModStatus::Rejected;
            rec.reason = why;
            status[rec.id] = rec.status;
            report("mod " + rec.source + ": not loaded: " + rec.reason);
            if (opt.registry) opt.registry->update(rec);
            continue;
        }
        const bool on = opt.settings ? opt.settings->enabled(rec.id, rec.enabled_default) : true;
        if (!on) {
            rec.status = ModStatus::Disabled;
            rec.reason = rec.enabled_default ? "enabled=0 in emf\\config\\" + rec.id + ".ini"
                                             : "off by default (enabled=1 in emf\\config\\" + rec.id + ".ini turns it on)";
            status[rec.id] = rec.status;
            report("mod " + rec.source + ": switched off (" + rec.reason + ")");
            if (opt.registry) opt.registry->update(rec);
            continue;
        }
        const std::string name = utf8(f.dll);
        // The mod's own folder is searched for the DLLs it imports (helper DLLs shipped next to it), then the usual places.
        HMODULE m = LoadLibraryExW(f.dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!m) {
            rec.status = ModStatus::Failed;
            rec.reason = "LoadLibrary failed (error " + std::to_string(GetLastError()) + ")";
            status[rec.id] = rec.status;
            report("mod " + name + ": " + rec.reason);
            if (opt.registry) opt.registry->update(rec);
            continue;
        }
        auto fn = reinterpret_cast<EmfModLoadFn>(GetProcAddress(m, "emf_mod_load"));
        if (!fn) {
            rec.status = ModStatus::Failed;
            rec.reason = "no emf_mod_load export";
            status[rec.id] = rec.status;
            report("mod " + name + ": no emf_mod_load export, skipped");
            FreeLibrary(m);
            if (opt.registry) opt.registry->update(rec);
            continue;
        }
        const int r = guarded_load(fn, api);
        if (opt.kept_loaded) opt.kept_loaded->push_back(m);  // mapped from here on, whatever emf_mod_load answered
        if (r == 1) {
            rec.status = ModStatus::Loaded;
            report("mod " + name + ": loaded");
            mods.push_back({f.dll, m, rec.id});
        } else {
            // Never unload once emf_mod_load ran: hooks or callbacks it registered still point into the DLL.
            rec.status = ModStatus::Failed;
            rec.reason = r == 0 ? "emf_mod_load returned false (see the log)" : "emf_mod_load faulted";
            report("mod " + name + (r == 0 ? ": emf_mod_load returned false (kept loaded; anything it registered stays active)"
                                           : ": emf_mod_load faulted (kept loaded; anything it registered stays active)"));
        }
        status[rec.id] = rec.status;
        if (opt.registry) opt.registry->update(rec);
    }
    publish();
    return mods;
}

std::vector<std::string> ManifestWatch::poll(const std::wstring& dir, const std::vector<ModRecord>& mods,
                                             SettingsStore& store) {
    std::lock_guard<std::mutex> l(m_);
    std::vector<std::string> lines;
    for (const ModRecord& rec : mods) {
        if (!rec.has_manifest || rec.source.empty() || rec.source.back() != '\\') continue;  // folder mods only
        if (rec.status == ModStatus::Rejected) {
            if (store.defs(rec.id).empty()) continue;  // never declared
            // a second folder with an id another mod has: its mod.json must not redefine that mod's settings
            size_t same = 0;
            for (const ModRecord& other : mods) same += other.id == rec.id ? 1 : 0;
            if (same > 1) continue;
        }
        const std::wstring path = dir + L"\\" + wide(rec.source.substr(0, rec.source.size() - 1)) + L"\\mod.json";
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) continue;
        const uint64_t written = (static_cast<uint64_t>(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime;
        const uint64_t stamp = written ^ (static_cast<uint64_t>(fa.nFileSizeLow) << 40);  // its time and size
        auto it = std::find_if(seen_.begin(), seen_.end(), [&](const auto& s) { return s.first == path; });
        if (it == seen_.end()) {  // first look: only note it
            seen_.emplace_back(path, stamp);
            continue;
        }
        if (it->second == stamp) continue;
        std::string text;
        if (!read_file(path, text, kMaxManifestBytes + 1)) continue;  // locked by the editor: look again next time
        it->second = stamp;
        const auto r = parse_manifest(text);
        if (!r.manifest) {
            lines.push_back("mod " + rec.source + ": mod.json changed but does not read (" + r.error +
                            "); its settings stay as they were");
            continue;
        }
        if (r.manifest->id != rec.id) {
            lines.push_back("mod " + rec.source + ": mod.json now says id \"" + r.manifest->id +
                            "\"; a new id takes effect at the next start");
            continue;
        }
        size_t declared = 0;
        for (const SettingDef& d : r.manifest->settings) {
            std::string why;
            if (store.declare(rec.id, d, &why, SettingsStore::Declare::Live)) {
                ++declared;
                if (!why.empty()) lines.push_back("mod " + rec.source + ": " + why);
            } else {
                lines.push_back("mod " + rec.source + ": setting refused: " + why);
            }
        }
        lines.push_back("mod " + rec.source + ": mod.json changed: " + std::to_string(declared) +
                        " setting(s) declared again (the Mods tab shows them now)");
    }
    return lines;
}

}  // namespace emf::core
