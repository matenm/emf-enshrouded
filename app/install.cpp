#include "install.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <map>
#include <set>

#include "archive.h"
#include "core/json.h"
#include "core/known_tools.h"
#include "core/manifest.h"
#include "emf/emf_api.h"
#include "json_out.h"
#include "loader/chain_list.h"
#include "loader/loader_id.h"
#include "servers.h"
#include "text.h"

namespace emf::app {

namespace {

using core::json::Value;
using VT = Value::Type;
using Action = PlanPart::Action;

constexpr const char* kRecords = "emf\\installed_mods";
constexpr size_t kMaxJson = 1u << 20;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool iprefix(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && _strnicmp(s.c_str(), p.c_str(), p.size()) == 0;
}

bool iends(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && _stricmp(s.c_str() + s.size() - tail.size(), tail.c_str()) == 0;
}

std::string win(std::string p) {
    std::replace(p.begin(), p.end(), '/', '\\');
    return p;
}

std::string base_name(const std::string& p) {
    const size_t at = p.find_last_of("/\\");
    return at == std::string::npos ? p : p.substr(at + 1);
}

size_t depth(const std::string& p) { return static_cast<size_t>(std::count(p.begin(), p.end(), '/')); }

std::string join(const std::vector<std::string>& v, size_t most = 5) {
    std::string s;
    for (size_t i = 0; i < v.size() && i < most; ++i) s += (i ? ", " : "") + v[i];
    if (v.size() > most) s += " and " + std::to_string(v.size() - most) + " more";
    return s;
}

std::string count_files(size_t n) { return std::to_string(n) + (n == 1 ? " file" : " files"); }

// Files classify_archive ignores as well: macOS and Explorer leftovers.
bool junk(const std::string& path) {
    const std::string low = lower(path);
    if (low.rfind("__macosx/", 0) == 0) return true;
    const std::string name = base_name(low);
    return name == ".ds_store" || name == "thumbs.db" || name == "desktop.ini";
}

// Read-me, licence and picture files: skipped (not refused) when another mod's file of that name is already there.
bool is_doc(const std::string& name) {
    const std::string low = lower(name);
    for (const char* ext : {".txt", ".md", ".pdf", ".url", ".htm", ".html", ".rtf", ".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp"})
        if (iends(low, ext)) return true;
    for (const char* stem : {"readme", "license", "licence", "changelog", "credits", "copying"})
        if (low.rfind(stem, 0) == 0) return true;
    return false;
}

bool is_proxy_name(const std::string& low_name) {
    for (const auto& p : core::proxy_dll_names())
        if (low_name == p) return true;
    return false;
}

// "3 files go to X" / "1 file goes to X" (verb agrees with the count).
std::string files_go(size_t n, const std::string& where) {
    return (n == 1 ? "1 file goes " : std::to_string(n) + " files go ") + where;
}

std::wstring system32() {
    wchar_t sys[MAX_PATH + 1] = {};
    const UINT n = GetSystemDirectoryW(sys, MAX_PATH + 1);
    return n && n <= MAX_PATH ? std::wstring(sys, n) : std::wstring();
}

// `<base>.dll` is a loader's proxy name or a DLL that Windows ships in System32 (dxgi, winmm, version, d3d11, ...): a
// foreign dinput8.dll must never be renamed to one, or the game would load it as that system DLL.
bool reserved_dll_base(const std::string& base) {
    const std::string dll = lower(base) + ".dll";
    if (is_proxy_name(dll)) return true;
    const std::wstring sys = system32();
    return !sys.empty() && exists(sys + L"\\" + wide(dll));
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    size_t from = 0;
    while (from < text.size()) {
        size_t to = text.find('\n', from);
        if (to == std::string::npos) to = text.size();
        std::string line = text.substr(from, to - from);
        from = to + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (!line.empty()) out.push_back(line);
    }
    return out;
}

// EMF's own files, lower-case with backslashes: emf\installed.txt lists what EMF's installer shipped. An install must
// never overwrite or remove one.
std::set<std::string> emf_own_files(const std::wstring& game) {
    std::set<std::string> s;
    std::string text;
    if (read_file(game + L"\\emf\\installed.txt", text, 1u << 20))
        for (const auto& line : split_lines(to_utf8_text(text)))
            if (!nexus::unsafe_archive_path(line)) s.insert(lower(win(line)));
    return s;
}

// A loose file (not inside a mod's own folder) an archive must not create: anything under emf\ that is not inside an
// emf\mods\<id>\ folder this archive installs, the chain files, the game's own exe/data and DLLs. Stops an archive from
// planting a forged install record, EMF's files, or a file the game loads by name.
bool blocked_loose_target(const std::string& rel_win, const std::set<std::string>& emf_mod_dirs) {
    const std::string low = lower(rel_win);
    const std::string name = base_name(low);
    if (iprefix(low, "emf\\")) {
        for (const auto& d : emf_mod_dirs)
            if (iprefix(low, d + "\\")) return false;  // emf\mods\<id>\... of a mod the archive really installs
        return true;
    }
    if (name == "dinput8-chain.ini" || name == "chainloader.ini") return true;
    if (iprefix(name, "enshrouded") && (iends(name, ".exe") || iends(name, ".dat"))) return true;
    if (low.find(".kfc") != std::string::npos) return true;
    if (name == "steam_api64.dll" || name == "steam_api.dll" || iprefix(name, "gfsdk_") || iprefix(name, "nvngx_")) return true;
    return false;
}

// A file uninstall and the update-backup step must never touch, however a record names it: the game's own exe, data and
// DLLs, EMF's own loader, app, page, tables and records, the chain files, and anything emf\installed.txt lists.
bool protected_from_removal(const std::string& rel_win, const std::set<std::string>& emf_own) {
    const std::string low = lower(rel_win);
    const std::string name = base_name(low);
    if (emf_own.count(low)) return true;
    if (name == "dinput8.dll" || name == "dinput8-chain.ini" || name == "chainloader.ini") return true;
    if (iprefix(name, "enshrouded") && (iends(name, ".exe") || iends(name, ".dat"))) return true;
    if (low.find(".kfc") != std::string::npos) return true;
    if (name == "steam_api64.dll" || name == "steam_api.dll" || iprefix(name, "gfsdk_") || iprefix(name, "nvngx_")) return true;
    if (low == "emf\\emf_core.dll" || low == "emf\\emf.exe" || low == "emf\\known_tools.txt") return true;
    if (iprefix(low, "emf\\ui\\") || iprefix(low, "emf\\installed_mods\\")) return true;
    return false;
}

// The mod id Nexus puts in a download's name, "<name>-<modId>-<v>-<v>-<v>-<timestamp>.zip": the name ends in a run of
// dash-separated all-number parts that finishes with a long timestamp; the first of that run is the mod id. A plain
// "2-handed-mod.zip" has no timestamp, so nothing is taken (and the version digits inside the name, "1.1.8", are not a
// '-' part, so they never look like the id).
std::string filename_nexus_id(const std::string& archive) {
    std::string stem = base_name(archive);
    const size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos) stem.resize(dot);
    std::vector<std::string> parts;
    size_t from = 0;
    while (from <= stem.size()) {
        size_t to = stem.find('-', from);
        if (to == std::string::npos) to = stem.size();
        parts.push_back(stem.substr(from, to - from));
        from = to + 1;
    }
    const auto all_digits = [](const std::string& s) { return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos; };
    size_t first = parts.size();  // start of the trailing all-number run
    while (first > 0 && all_digits(parts[first - 1])) --first;
    if (first >= parts.size() || parts.back().size() < 9) return {};  // no trailing number run ending in a timestamp
    const std::string& id = parts[first];
    return id.size() >= 1 && id.size() <= 7 ? id : std::string();
}

// A folder name taken from an archive or a mod.json: one safe part, not starting with '.' (EML's switch).
bool usable_folder_name(const std::string& n) {
    return !n.empty() && n.size() <= 64 && n.find_first_of("/\\") == std::string::npos && n[0] != '.' && n.back() != '.' &&
           n.back() != ' ' && !nexus::unsafe_archive_path(n);
}

std::string now_text(const char* fmt) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof buf, fmt, &tm);
    return buf;
}

std::optional<Value> parse_object(const std::string& text) {
    std::string t = text;
    if (t.compare(0, 3, "\xEF\xBB\xBF") == 0) t.erase(0, 3);
    auto r = core::json::parse(to_utf8_text(t));
    if (!r.value || !r.value->is(VT::Object)) return std::nullopt;
    return std::move(r.value);
}

std::string jstr(const Value& v, const char* key) {
    const Value* x = v.get(key);
    return x && x->is(VT::String) ? x->s : std::string();
}

// Digits of a Nexus id given as a number or text ("" for anything else).
std::string jid(const Value& v, const char* key) {
    const Value* x = v.get(key);
    std::string s;
    if (x && x->is(VT::String)) s = x->s;
    else if (x && x->is(VT::Number) && x->n > 0 && x->n < 1e12) s = std::to_string(static_cast<long long>(x->n));
    return !s.empty() && s.size() <= 12 && s.find_first_not_of("0123456789") == std::string::npos ? s : "";
}

std::vector<std::string> jstrs(const Value& v, const char* key) {
    std::vector<std::string> out;
    if (const Value* x = v.get(key); x && x->is(VT::Array))
        for (const Value& e : x->arr)
            if (e.is(VT::String)) out.push_back(e.s);
    return out;
}

std::string tool_name(const std::vector<core::KnownTool>& tools, const std::string& word) {
    if (word == "emf") return "EMF";
    for (const auto& t : tools)
        if (t.word == word) return t.name;
    return word;
}

// The tool of a DLL from its bytes (known_tools.txt's texts; "emf" for EMF's loader); "" when unknown.
std::string tool_of_bytes(const std::vector<core::KnownTool>& tools, const std::string& bytes) {
    if (bytes.find(EMF_LOADER_MARKER) != std::string::npos) return "emf";
    return core::tool_for_dll_text(tools, bytes);
}

// ---- version resource, read from the file's bytes (the DLL is never loaded) ----
uint32_t rd(const std::string& b, size_t at, size_t n, bool& ok) {
    if (at + n > b.size() || at + n < at) {
        ok = false;
        return 0;
    }
    uint32_t v = 0;
    for (size_t i = 0; i < n; ++i) v |= static_cast<uint32_t>(static_cast<unsigned char>(b[at + i])) << (8 * i);
    return v;
}

struct VerBlock {
    std::wstring key;
    size_t value = 0, end = 0, children = 0;
};

// One VS_VERSIONINFO block at `at` (offsets inside `v`): wLength, wValueLength, wType, the key (UTF-16), padding to 4,
// the value, padding to 4, the children.
bool ver_block(const std::string& v, size_t at, size_t limit, VerBlock& out) {
    bool ok = true;
    const uint32_t len = rd(v, at, 2, ok), vlen = rd(v, at + 2, 2, ok), type = rd(v, at + 4, 2, ok);
    if (!ok || len < 6 || at + len > limit) return false;
    out.end = at + len;
    out.key.clear();
    size_t p = at + 6;
    for (; p + 1 < out.end; p += 2) {
        const wchar_t c = static_cast<wchar_t>(rd(v, p, 2, ok));
        if (c == 0) break;
        out.key += c;
    }
    p = (p + 2 + 3) & ~static_cast<size_t>(3);
    out.value = p;
    out.children = (p + (type == 1 ? vlen * 2u : vlen) + 3) & ~static_cast<size_t>(3);
    return true;
}

std::string ver_string(const std::string& v, const VerBlock& b) {
    std::wstring w;
    bool ok = true;
    for (size_t p = b.value; p + 1 < b.end; p += 2) {
        const wchar_t c = static_cast<wchar_t>(rd(v, p, 2, ok));
        if (c == 0) break;
        w += c;
    }
    return utf8(w);
}

// The planned copies of a part, checked against the game folder: a file of this install's earlier version is replaced
// (it moves to the backup first), the same bytes already there are left, a read-me is skipped, anything else stops the
// part. Returns the names that stop it.
struct Placer {
    const PlanInput& in;
    const std::set<std::string>& owned;      // lower-case paths the earlier version installed
    const std::set<std::string>& own;        // EMF's own files (emf\installed.txt)
    const std::set<std::string>& emf_mod_dirs;  // emf\mods\<id> folders the archive installs
    std::vector<std::string> conflicts, same, skipped, blocked, game_files;

    bool place(PlanPart& part, const std::string& from_rel, const std::string& to_rel, bool doc_ok = true) {
        const std::wstring src = in.folder + L"\\" + wide(win(from_rel));
        const std::wstring dst = in.game + L"\\" + wide(to_rel);
        if (blocked_loose_target(to_rel, emf_mod_dirs)) {
            blocked.push_back(to_rel);
            return false;
        }
        if (exists(dst)) {
            if (is_dir(dst)) {
                conflicts.push_back(to_rel + " (a folder)");
                return false;
            }
            if (owned.count(lower(to_rel))) {
                part.replace_files.push_back(to_rel);
            } else if (file_sha256(dst) == file_sha256(src)) {
                same.push_back(to_rel);
                part.deps.push_back(win(to_rel));  // already there, same bytes: a shared file, kept on uninstall
                return true;
            } else if (doc_ok && is_doc(base_name(to_rel))) {
                skipped.push_back(to_rel);
                return true;
            } else if (protected_from_removal(win(to_rel), own)) {
                game_files.push_back(to_rel);  // the game's or EMF's own file: never replaced
                return false;
            } else {
                conflicts.push_back(to_rel);
                return false;
            }
        }
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        uint64_t size = 0;
        if (GetFileAttributesExW(src.c_str(), GetFileExInfoStandard, &fa)) size = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
        part.copies.push_back({src, to_rel, size});
        return true;
    }
};

// A mod folder of the archive and what its mod.json says.
struct FolderInfo {
    nexus::ModFolder mod;
    std::string folder;  // the folder name it gets
    std::string name, version, nexus_id, error;
    std::optional<core::Manifest> manifest;
    ModSide side;  // where it has to run (mod.json "side")
};

const char* place_of(nexus::ArchiveKind k) {
    switch (k) {
        case nexus::ArchiveKind::EmfMod: return "emf\\mods\\";
        case nexus::ArchiveKind::GreeneyeMod: return "greeneye\\";
        default: return "mods\\";
    }
}

std::string sanitize_key(const std::string& text) {
    std::string k;
    for (char c : lower(text)) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
        k += ok ? c : '-';
    }
    while (!k.empty() && (k[0] == '-' || k[0] == '.')) k.erase(0, 1);
    if (k.size() > 64) k.resize(64);
    while (!k.empty() && (k.back() == '-' || k.back() == '.')) k.pop_back();
    return valid_install_key(k) ? k : std::string();
}

std::string record_path(const std::string& key) { return std::string(kRecords) + "\\" + key + ".json"; }

// mods\foo\... of a record, for an EML mod switched off since (mods\.foo\...).
std::string switched_path(const std::wstring& game, const std::string& rel) {
    if (!iprefix(rel, "mods\\")) return rel;
    const size_t end = rel.find('\\', 5);
    const std::string folder = rel.substr(5, end == std::string::npos ? std::string::npos : end - 5);
    if (folder.empty() || folder[0] == '.') return rel;
    if (exists(game + L"\\mods\\" + wide(folder)) || !exists(game + L"\\mods\\." + wide(folder))) return rel;
    return "mods\\." + rel.substr(5);
}

// Makes the folders up to `rel_dir` (relative), adding the ones it made to `made` (outermost first).
bool make_folders(const std::wstring& game, const std::string& rel_dir, std::vector<std::string>& made) {
    if (rel_dir.empty()) return true;
    size_t from = 0;
    while (from <= rel_dir.size()) {
        size_t to = rel_dir.find('\\', from);
        if (to == std::string::npos) to = rel_dir.size();
        const std::string sub = rel_dir.substr(0, to);
        const std::wstring p = game + L"\\" + wide(sub);
        if (!is_dir(p)) {
            if (exists(p) || !CreateDirectoryW(lp(p).c_str(), nullptr)) return false;
            made.push_back(sub);
        }
        from = to + 1;
    }
    return true;
}

std::string parent_of(const std::string& rel) {
    const size_t at = rel.find_last_of('\\');
    return at == std::string::npos ? std::string() : rel.substr(0, at);
}

// Moves a file or folder of the game folder to `to` (relative), making its folders. No overwriting.
bool move_into(const std::wstring& game, const std::string& from, const std::string& to, std::vector<std::string>& made) {
    if (!make_folders(game, parent_of(to), made)) return false;
    return MoveFileExW(lp(game + L"\\" + wide(from)).c_str(), lp(game + L"\\" + wide(to)).c_str(), 0) != 0;
}

// Removes the folders in `dirs` that are empty, deepest first.
void remove_empty(const std::wstring& game, std::vector<std::string> dirs) {
    std::sort(dirs.begin(), dirs.end(), [](const std::string& a, const std::string& b) {
        const auto da = std::count(a.begin(), a.end(), '\\'), db = std::count(b.begin(), b.end(), '\\');
        return da != db ? da > db : a > b;
    });
    for (const auto& d : dirs) RemoveDirectoryW(lp(game + L"\\" + wide(d)).c_str());  // only succeeds when empty
}

std::string record_json(const InstallRecord& r) {
    std::vector<std::string> files;
    for (const auto& [path, hash] : r.files) files.push_back(Obj().str("path", path).str("sha256", hash).text());
    return Obj()
               .str("key", r.key).str("name", r.name).str("version", r.version).str("nexus_id", r.nexus_id)
               .str("nexus_file_id", r.nexus_file_id).str("archive", r.archive).str("installed_at", r.installed_at).strs("kinds", r.kinds)
               .strs("roots", r.roots).strs("chain", r.chain).strs("chain_off", r.chain_off)
               .strs("folders", r.folders).strs("backups", r.backups).strs("deps", r.deps).raw("files", json_list(files))
               .text() +
           "\n";
}

std::optional<InstallRecord> parse_record(const std::string& text, const std::string& key) {
    const auto v = parse_object(text);
    if (!v || jstr(*v, "key") != key) return std::nullopt;
    InstallRecord r;
    r.key = key;
    r.name = jstr(*v, "name");
    r.version = jstr(*v, "version");
    r.nexus_id = jid(*v, "nexus_id");
    r.nexus_file_id = jid(*v, "nexus_file_id");
    r.archive = jstr(*v, "archive");
    r.installed_at = jstr(*v, "installed_at");
    r.kinds = jstrs(*v, "kinds");
    r.backups = jstrs(*v, "backups");
    const auto safe = [](std::vector<std::string> in) {  // a changed record must not reach outside the game folder
        std::vector<std::string> out;
        for (auto& p : in)
            if (!nexus::unsafe_archive_path(p)) out.push_back(win(p));
        return out;
    };
    r.roots = safe(jstrs(*v, "roots"));
    r.folders = safe(jstrs(*v, "folders"));
    r.deps = safe(jstrs(*v, "deps"));
    for (auto& n : safe(jstrs(*v, "chain")))
        if (n.find('\\') == std::string::npos) r.chain.push_back(n);
    for (auto& n : safe(jstrs(*v, "chain_off")))
        if (n.find('\\') == std::string::npos) r.chain_off.push_back(n);
    if (const Value* f = v->get("files"); f && f->is(VT::Array))
        for (const Value& e : f->arr) {
            if (!e.is(VT::Object)) continue;
            const std::string p = jstr(e, "path"), h = jstr(e, "sha256");
            if (!nexus::unsafe_archive_path(p) && h.size() == 64) r.files.emplace_back(win(p), lower(h));
        }
    return r;
}

const char* action_name(Action a) {
    switch (a) {
        case Action::Install: return "install";
        case Action::Update: return "update";
        case Action::Skip: return "skip";
        case Action::Refused: break;
    }
    return "refused";
}

}  // namespace

// ---- helpers ----

std::string name_slug(const std::string& text) {
    std::string s;
    for (char ch : lower(text)) {
        const bool keep = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
        if (keep) s += ch;
        else if (!s.empty() && s.back() != '-') s += '-';
    }
    while (!s.empty() && s.back() == '-') s.pop_back();
    if (s.size() > 40) {
        s.resize(40);
        while (!s.empty() && s.back() == '-') s.pop_back();
    }
    return s;
}

std::string archive_slug(const std::string& file_name) {
    std::string stem = base_name(file_name);
    for (const char* ext : {".zip", ".7z", ".rar", ".tar", ".gz"})
        if (iends(stem, ext)) stem.resize(stem.size() - strlen(ext));
    std::string s = name_slug(stem);
    // Nexus names downloads "<name>-<mod id>-<version>-<time>": drop the trailing parts that are only digits
    for (;;) {
        const size_t dash = s.find_last_of('-');
        const std::string last = dash == std::string::npos ? s : s.substr(dash + 1);
        if (last.empty() || last.find_first_not_of("0123456789") != std::string::npos || dash == std::string::npos) break;
        s.resize(dash);
    }
    return s;
}

std::vector<std::string> dll_version_strings(const std::string& b) {
    std::vector<std::string> out(3);
    bool ok = true;
    if (b.size() < 0x40 || b[0] != 'M' || b[1] != 'Z') return out;
    const uint32_t pe = rd(b, 0x3C, 4, ok);
    if (!ok || rd(b, pe, 4, ok) != 0x00004550) return out;
    const uint32_t sections = rd(b, pe + 6, 2, ok), opt_size = rd(b, pe + 20, 2, ok);
    const size_t opt = pe + 24;
    const uint32_t magic = rd(b, opt, 2, ok);
    const size_t dirs = magic == 0x20b ? opt + 112 : magic == 0x10b ? opt + 96 : 0;
    if (!ok || !dirs) return out;
    const uint32_t rsrc_rva = rd(b, dirs + 2 * 8, 4, ok);
    if (!ok || !rsrc_rva) return out;
    const size_t table = opt + opt_size;
    const auto offset_of = [&](uint32_t rva) -> size_t {
        for (uint32_t i = 0; i < sections && i < 96; ++i) {
            const size_t s = table + 40 * i;
            const uint32_t vsize = rd(b, s + 8, 4, ok), va = rd(b, s + 12, 4, ok), raw_size = rd(b, s + 16, 4, ok), raw = rd(b, s + 20, 4, ok);
            if (rva >= va && rva < va + (std::max)(vsize, raw_size)) return raw + (rva - va);
        }
        return static_cast<size_t>(-1);
    };
    const size_t root = offset_of(rsrc_rva);
    if (root == static_cast<size_t>(-1) || !ok) return out;
    // the directory tree: type RT_VERSION (16), then the first name, then the first language
    size_t dir = root;
    uint32_t data_entry = 0;
    for (int level = 0; level < 3; ++level) {
        const uint32_t named = rd(b, dir + 12, 2, ok), ids = rd(b, dir + 14, 2, ok);
        if (!ok || named + ids == 0 || named + ids > 4096) return out;
        size_t pick = static_cast<size_t>(-1);
        for (uint32_t i = 0; i < named + ids; ++i) {
            const size_t e = dir + 16 + 8 * i;
            const uint32_t name = rd(b, e, 4, ok);
            if (level > 0 || (!(name & 0x80000000u) && name == 16)) {
                pick = e;
                break;
            }
        }
        if (pick == static_cast<size_t>(-1) || !ok) return out;
        const uint32_t to = rd(b, pick + 4, 4, ok);
        if (level < 2) {
            if (!(to & 0x80000000u)) return out;
            dir = root + (to & 0x7FFFFFFFu);
        } else {
            if (to & 0x80000000u) return out;
            data_entry = to;
        }
    }
    const uint32_t data_rva = rd(b, root + data_entry, 4, ok), data_size = rd(b, root + data_entry + 4, 4, ok);
    const size_t at = offset_of(data_rva);
    if (!ok || at == static_cast<size_t>(-1) || at >= b.size()) return out;
    const std::string v = b.substr(at, (std::min)(static_cast<size_t>(data_size), b.size() - at));
    VerBlock top;
    if (!ver_block(v, 0, v.size(), top) || top.key != L"VS_VERSION_INFO") return out;
    for (size_t p = top.children; p < top.end;) {
        VerBlock sfi;
        if (!ver_block(v, p, top.end, sfi)) break;
        if (sfi.key == L"StringFileInfo") {
            VerBlock st;  // the first string table
            if (ver_block(v, sfi.children, sfi.end, st)) {
                for (size_t q = st.children; q < st.end;) {
                    VerBlock s;
                    if (!ver_block(v, q, st.end, s)) break;
                    if (s.key == L"ProductName") out[0] = ver_string(v, s);
                    else if (s.key == L"FileDescription") out[1] = ver_string(v, s);
                    else if (s.key == L"OriginalFilename") out[2] = ver_string(v, s);
                    q = (s.end + 3) & ~static_cast<size_t>(3);
                }
            }
            break;
        }
        p = (sfi.end + 3) & ~static_cast<size_t>(3);
    }
    return out;
}

std::string product_slug(const std::vector<std::string>& strings) {
    for (std::string v : strings) {
        if (v.empty() || lower(v).find("microsoft") != std::string::npos) continue;  // proxies copy the real DLL's
        if (iends(v, ".dll")) v.resize(v.size() - 4);
        const std::string slug = name_slug(v);
        if (!slug.empty() && slug != "dinput8") return slug;
    }
    return "";
}

std::string filename_nexus_id_of(const std::string& archive_name) { return filename_nexus_id(archive_name); }

bool valid_install_key(const std::string& key) {
    if (key.empty() || key.size() > 64 || key[0] == '.' || key[0] == '-') return false;
    for (char c : key)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')) return false;
    return key.find("..") == std::string::npos;
}

bool InstallPlan::installable() const {
    return error.empty() && std::any_of(parts.begin(), parts.end(), [](const PlanPart& p) { return p.installs(); });
}

// ---- the plan ----

InstallPlan build_plan(const PlanInput& in) {
    InstallPlan plan;
    plan.archive = in.archive;
    plan.folder = in.folder;
    plan.game = in.game;
    std::string page_id;
    if (!in.nexus_id.empty() && in.nexus_id.size() <= 12 && in.nexus_id.find_first_not_of("0123456789") == std::string::npos)
        page_id = in.nexus_id;
    const std::set<std::string> own = emf_own_files(in.game);
    const bool server = folder_kind(in.game) == "server";

    std::vector<std::string> files;
    for (const auto& f : in.files) {
        if (junk(f)) continue;
        if (nexus::unsafe_archive_path(f)) {
            plan.error = "EMF does not install this archive: the path " + f + " is not safe to install.";
            return plan;
        }
        files.push_back(f);
    }
    if (files.empty()) {
        plan.error = "The archive has no files to install.";
        return plan;
    }
    const nexus::Classification c = nexus::classify_archive(files, in.state);
    plan.kind_label = nexus::kind_label(c.kind);
    plan.notes = c.notes;
    if (c.kind == nexus::ArchiveKind::EmfFramework) {
        plan.error = "This is EMF itself. Install or update EMF with install.cmd from its package, not as a mod.";
        return plan;
    }
    const auto tools = core::parse_known_tools(in.tools_table);
    std::map<std::string, std::string> by_low;  // lower-case path -> path
    for (const auto& f : files) by_low[lower(f)] = f;
    const auto read_rel = [&](const std::string& rel, size_t max) {
        std::string s;
        read_file(in.folder + L"\\" + wide(win(rel)), s, max);
        return s;
    };

    // ---- the mod folders and their mod.json ----
    std::vector<FolderInfo> folders;
    for (const auto& m : c.mods) {
        FolderInfo fi;
        fi.mod = m;
        const auto mj = by_low.find(lower(m.path) + "mod.json");
        std::optional<Value> json;
        if (mj != by_low.end()) json = parse_object(read_rel(mj->second, core::kMaxManifestBytes + 1));
        if (json) {
            fi.name = jstr(*json, "name");
            fi.version = jstr(*json, "version");
            fi.nexus_id = jid(*json, "nexus_id");
            fi.side = read_mod_side(*json);
        }
        if (m.kind == nexus::ArchiveKind::EmfMod) {
            if (mj == by_low.end()) {
                fi.error = "there is no mod.json in its folder, and EMF loads no mod folder without one";
            } else {
                auto r = core::parse_manifest(read_rel(mj->second, core::kMaxManifestBytes + 1));
                if (!r.manifest) fi.error = "its mod.json is not one EMF reads (" + r.error + ")";
                else {
                    fi.manifest = r.manifest;
                    fi.folder = r.manifest->id;
                    fi.name = r.manifest->name;
                    fi.version = r.manifest->version_text;
                }
            }
        } else {
            fi.folder = !m.id.empty() ? m.id : json ? jstr(*json, "id") : std::string();
            if (fi.folder.empty()) fi.folder = archive_slug(in.archive);
        }
        if (fi.error.empty() && !usable_folder_name(fi.folder))
            fi.error = "its folder name \"" + fi.folder + "\" is not one EMF can use";
        if (fi.name.empty()) fi.name = !fi.folder.empty() ? fi.folder : m.id;
        folders.push_back(std::move(fi));
    }

    // the emf\mods\<id> folders this archive legitimately installs: loose files are allowed under emf\ only there
    std::set<std::string> emf_mod_dirs;
    for (const auto& f : folders)
        if (f.mod.kind == nexus::ArchiveKind::EmfMod && f.error.empty()) emf_mod_dirs.insert(lower("emf\\mods\\" + f.folder));

    // ---- the Nexus id: the archive's mod.json, then the id in its Nexus file name, then the page it came from ----
    std::string mod_json_id;
    for (const auto& f : folders)
        if (mod_json_id.empty() && !f.nexus_id.empty()) mod_json_id = f.nexus_id;
    const std::string file_id = filename_nexus_id(in.archive);
    std::string nexus = !mod_json_id.empty() ? mod_json_id : !file_id.empty() ? file_id : page_id;
    if (in.nexus_certain && !page_id.empty()) nexus = page_id;  // downloaded from that mod: no guess
    if (!in.nexus_file_id.empty() && in.nexus_file_id.size() <= 12 &&
        in.nexus_file_id.find_first_not_of("0123456789") == std::string::npos)
        plan.nexus_file_id = in.nexus_file_id;
    plan.nexus_id = nexus;
    std::vector<std::string> ids;  // the sources that gave an id, to warn when they disagree
    for (const auto& id : {mod_json_id, file_id, page_id})
        if (!id.empty() && std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    if (ids.size() > 1)
        plan.notes.push_back("The Nexus id is not certain: its mod.json, file name and page do not agree (" + join(ids) +
                             "). EMF uses " + nexus + "; check it is the mod you mean.");
    std::string linked_note;
    if (!nexus.empty()) {
        const auto linked = in.update_keys.find(nexus);
        if (linked != in.update_keys.end() && valid_install_key(linked->second.key)) {
            plan.key = linked->second.key;
            if (!linked->second.why.empty())
                linked_note = "EMF plans this as an update of the install " + plan.key + ", because " + linked->second.why + ".";
        } else {
            plan.key = "nexus-" + nexus;
        }
    }
    for (const auto& f : folders)
        if (plan.key.empty() && f.error.empty()) plan.key = sanitize_key(f.folder);
    if (plan.key.empty()) plan.key = sanitize_key(archive_slug(in.archive));
    if (plan.key.empty()) plan.key = "mod";
    const auto earlier = read_install_record(in.game, plan.key);
    plan.update = earlier.has_value();
    std::set<std::string> owned;
    if (earlier) {
        for (const auto& [path, hash] : earlier->files) owned.insert(lower(path));
        // the same key can come from two archives with a generic name: say which install this replaces
        plan.notes.insert(plan.notes.begin(),
                          "This replaces " + (earlier->name.empty() ? plan.key : earlier->name) +
                              (earlier->version.empty() ? "" : " " + earlier->version) + ", which EMF installed" +
                              (earlier->installed_at.empty() ? "" : " on " + earlier->installed_at) +
                              (earlier->archive.empty() ? "" : " from " + earlier->archive) +
                              ": its files move to emf\\backup\\ first.");
    }
    if (!linked_note.empty()) plan.notes.insert(plan.notes.begin(), linked_note);

    std::vector<bool> claimed(files.size(), false);
    std::set<std::string> targets;
    for (auto& fi : folders) {
        PlanPart part;
        part.kind = nexus::kind_id(fi.mod.kind);
        part.label = nexus::kind_label(fi.mod.kind);
        part.title = fi.name;
        part.version = fi.version;
        const std::string prefix = fi.mod.path;
        for (size_t i = 0; i < files.size(); ++i)
            if (iprefix(files[i], prefix)) claimed[i] = true;
        if (!fi.error.empty()) {
            part.action = Action::Refused;
            part.target = place_of(fi.mod.kind);
            part.why = "Not installed: " + fi.error + ".";
            plan.parts.push_back(std::move(part));
            continue;
        }
        part.id = fi.manifest ? fi.manifest->id : fi.folder;
        const std::string place = place_of(fi.mod.kind);
        part.target = place + fi.folder + "\\";
        const std::string target = place + fi.folder;
        part.root = target;
        if (!targets.insert(lower(target)).second) {
            part.action = Action::Refused;
            part.why = "Not installed: the archive has two mods for " + part.target + ".";
            plan.parts.push_back(std::move(part));
            continue;
        }
        const bool here = exists(in.game + L"\\" + wide(target));
        const bool off_here = place == std::string("mods\\") && exists(in.game + L"\\mods\\." + wide(fi.folder));
        if (here && off_here) {
            part.action = Action::Refused;
            part.why = "Not installed: both mods\\" + fi.folder + " and mods\\." + fi.folder +
                       " exist (two copies of this mod). Remove the one you do not want first.";
            plan.parts.push_back(std::move(part));
            continue;
        }
        size_t n = 0;
        for (size_t i = 0; i < files.size(); ++i) {
            if (!iprefix(files[i], prefix)) continue;
            const std::wstring src = in.folder + L"\\" + wide(win(files[i]));
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            uint64_t size = 0;
            if (GetFileAttributesExW(src.c_str(), GetFileExInfoStandard, &fa)) size = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
            part.copies.push_back({src, target + "\\" + win(files[i].substr(prefix.size())), size});
            ++n;
        }
        if (here || off_here) {
            part.action = Action::Update;
            part.replace_folder = here ? target : "mods\\." + fi.folder;
            if (off_here) {
                part.lines.push_back("Replaces the switched-off mods\\." + fi.folder + "\\: it moves to emf\\backup\\ first, then " +
                                     files_go(n, "to " + part.target) + ". The new version is switched on.");
            } else {
                part.lines.push_back(files_go(n, "to " + part.target) + "; the old versions of those files move to emf\\backup\\ first.");
                part.lines.push_back("Your data and settings in " + part.target + " (files the mod made, not part of the download) are kept.");
            }
        } else {
            part.lines.push_back(files_go(n, "to " + part.target));
        }
        if (fi.manifest && fi.manifest->emf > EMF_API_VERSION)
            part.lines.push_back("It needs EMF API " + std::to_string(fi.manifest->emf) + "; this EMF.exe knows API " +
                                 std::to_string(EMF_API_VERSION) + ". Update EMF if it does not load.");
        if (server) {  // a dedicated server's folder: where the mod has to run (servers.h)
            part.lines.push_back(server_side_note(fi.side.side, fi.side.declared).text);
            if (fi.side.side == "client")
                plan.notes.push_back(fi.name + " does nothing on a server (its mod.json says \"side\": \"client\"): it only "
                                               "needs to be in players' games.");
        }
        plan.parts.push_back(std::move(part));
    }

    // ---- the files outside the mod folders ----
    const std::string& strip = c.strip_prefix;
    std::vector<std::string> loose, not_copied;  // relative to the wrapper
    for (size_t i = 0; i < files.size(); ++i)
        if (!claimed[i]) loose.push_back(iprefix(files[i], strip) ? files[i].substr(strip.size()) : files[i]);

    const auto from_of = [&](const std::string& rel) { return strip + rel; };
    const auto owner_text = [&](const std::string& low_name) {
        for (const auto& [file, word] : in.state.proxies)
            if (lower(file) == low_name) return word.empty() ? std::string("a tool EMF does not know") : tool_name(tools, word);
        return std::string("another mod");
    };
    const auto clear_part = [](PlanPart& part) {
        part.copies.clear();
        part.chain_add.clear();
        part.chain_keep.clear();
        part.chain_off.clear();
        part.replace_files.clear();
        part.deps.clear();
    };
    const auto finish = [&](PlanPart& part, Placer& p) {
        if (!p.blocked.empty()) {
            part.action = Action::Refused;
            part.why = "Not installed: an archive may not write " + join(p.blocked) +
                       " (that is EMF's own folder or the game's own file). Only a mod's own folders are installed.";
            clear_part(part);
            return;
        }
        if (!p.game_files.empty()) {
            part.action = Action::Refused;
            part.why = "Not installed: " + join(p.game_files) + (p.game_files.size() == 1 ? " is" : " are") +
                       " the game's own file, and EMF never replaces it.";
            clear_part(part);
            return;
        }
        if (!p.conflicts.empty()) {
            std::vector<std::string> said;
            for (const auto& f : p.conflicts)
                said.push_back(is_proxy_name(lower(f)) ? f + " (" + owner_text(lower(f)) + "'s)" : f);
            part.action = Action::Refused;
            part.why = "Not installed: " + join(said) + (p.conflicts.size() == 1 ? " is" : " are") +
                       " already in the game folder, and EMF never overwrites another mod's files.";
            clear_part(part);
            return;
        }
        if (!p.same.empty()) part.lines.push_back("Already there with the same contents, and kept: " + join(p.same) + ".");
        if (!p.skipped.empty()) part.lines.push_back("Not copied, another mod's file of that name is there: " + join(p.skipped) + ".");
        if (!part.replace_files.empty())
            part.lines.push_back("Replaces its earlier version's " + join(part.replace_files) + " (moved to emf\\backup\\ first).");
        if (!part.installs() && part.action != Action::Refused && part.action != Action::Skip) {
            part.action = Action::Skip;
            if (part.why.empty()) part.why = "Nothing to copy: it is installed already.";
        } else if (part.action == Action::Install && !part.replace_files.empty()) {
            part.action = Action::Update;
        }
    };

    using K = nexus::ArchiveKind;
    if (c.kind == K::DllModDinput8 || c.kind == K::ProxyDll) {
        PlanPart part;
        part.kind = nexus::kind_id(c.kind);
        part.label = nexus::kind_label(c.kind);
        part.title = !c.loader.empty() ? tool_name(tools, c.loader) : !in.nexus_name.empty() ? in.nexus_name : in.archive;
        part.target = "next to the game";
        Placer p{in, owned, own, emf_mod_dirs};
        size_t dll_copies = 0;
        for (const auto& rel : loose) {
            const std::string low = lower(rel);
            if (low != "dinput8.dll") {
                if (depth(rel) == 0 && is_proxy_name(low)) p.place(part, from_of(rel), win(rel), false);
                else p.place(part, from_of(rel), win(rel));
                continue;
            }
            // ---- dinput8.dll: renamed and started by EMF through dinput8-chain.ini ----
            if (c.loader == "eml" || c.loader == "shroudforge") {
                part.lines.push_back("Its dinput8.dll is not installed: it is a second copy of " + tool_name(tools, c.loader) +
                                     "'s loader, which would start it twice.");
                continue;
            }
            if (!in.emf_installed) {
                part.action = Action::Refused;
                part.why = "Not installed: EMF is not installed in this folder, and EMF starts a dinput8.dll mod through "
                           "dinput8-chain.ini. Install EMF first (install.cmd), then install this mod.";
                continue;
            }
            const std::string bytes = read_rel(from_of(rel), 256u << 20);
            const std::string tool = tool_of_bytes(tools, bytes);
            if (tool == "emf") {
                part.lines.push_back("Its dinput8.dll is a copy of EMF's own loader: not installed.");
                continue;
            }
            std::string runs_as;  // the same loader already runs through another proxy name it uses
            for (const auto& t : tools)
                if (!tool.empty() && t.word == tool)
                    for (const auto& other : t.same_loader)
                        for (const auto& [file, word] : in.state.proxies)
                            if (lower(file) == other && word == tool) runs_as = other;
            if (!runs_as.empty()) {
                part.lines.push_back("Its dinput8.dll is not installed: " + tool_name(tools, tool) + " already runs as " +
                                     runs_as + ", and a second copy would start it twice.");
                continue;
            }
            std::string base = tool;
            if (base.empty()) base = product_slug(dll_version_strings(bytes));
            if (base.empty()) base = archive_slug(in.archive);
            if (base.empty() || base == "dinput8") base = "dinput8-previous";
            // never a loader's or Windows' own DLL name: the game would load the renamed file as that system DLL
            for (int guard = 0; guard < 10 && reserved_dll_base(base); ++guard) base += "-mod";
            const std::string src_hash = file_sha256(in.folder + L"\\" + wide(win(from_of(rel))));
            std::string chain_text;
            read_file(in.game + L"\\dinput8-chain.ini", chain_text, 1u << 20);
            const auto listed = loader::parse_chain_list(chain_text);
            const auto is_listed = [&](const std::string& n) {
                return std::any_of(listed.begin(), listed.end(), [&](const std::wstring& l) { return loader::same_name(l, wide(n)); });
            };
            std::string chosen;
            bool identical = false, replace = false;
            std::vector<std::string> earlier_copies;
            for (int i = 1; i < 100 && chosen.empty(); ++i) {
                const std::string n = i == 1 ? base + ".dll" : base + "-" + std::to_string(i) + ".dll";
                const std::wstring at = in.game + L"\\" + wide(n);
                if (!exists(at)) chosen = n;
                else if (owned.count(lower(n))) {
                    chosen = n;
                    replace = true;
                } else if (!src_hash.empty() && file_sha256(at) == src_hash) {
                    chosen = n;
                    identical = true;
                } else if (is_listed(n)) {
                    if (!tool.empty()) part.chain_off.push_back(n);
                    else earlier_copies.push_back(n);
                }
            }
            if (chosen.empty()) {
                part.action = Action::Refused;
                part.why = "Not installed: there are too many copies of " + base + ".dll in the game folder.";
                continue;
            }
            if (!identical) {
                if (replace) part.replace_files.push_back(chosen);
                WIN32_FILE_ATTRIBUTE_DATA fa{};
                uint64_t size = 0;
                const std::wstring src = in.folder + L"\\" + wide(win(from_of(rel)));
                if (GetFileAttributesExW(src.c_str(), GetFileExInfoStandard, &fa)) size = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
                part.copies.push_back({src, chosen, size});
                ++dll_copies;
            }
            if (!is_listed(chosen)) part.chain_add.push_back(chosen);
            else if (replace) part.chain_keep.push_back(chosen);
            part.lines.push_back(identical ? "dinput8.dll is already there as " + chosen + " (the same file)" +
                                                 (part.chain_add.empty() ? ", and listed in dinput8-chain.ini." : "; it gets listed in dinput8-chain.ini.")
                                           : "dinput8.dll is installed as " + chosen +
                                                 " and listed in dinput8-chain.ini: EMF starts it before the game runs.");
            for (const auto& o : part.chain_off)
                part.lines.push_back("The older copy " + o + " of " + tool_name(tools, tool) +
                                     " is switched off in dinput8-chain.ini (kept, not deleted).");
            for (const auto& e : earlier_copies)
                part.lines.push_back("dinput8-chain.ini also starts " + e + ", an earlier dinput8.dll EMF could not identify. If " +
                                     chosen + " is a newer copy of the same mod, switch " + e + " off in the Mods tab.");
        }
        if (part.action != Action::Refused) {
            const size_t others = part.copies.size() - dll_copies;
            if (others) part.lines.insert(part.lines.begin(), files_go(others, "next to the game, with their folders"));
            finish(part, p);
        } else {
            part.copies.clear();
            part.chain_add.clear();
            part.chain_keep.clear();
            part.chain_off.clear();
            part.replace_files.clear();
            part.lines.clear();
        }
        plan.parts.push_back(std::move(part));
    } else if (c.kind == K::GreeneyeMod || c.kind == K::EmfMod || c.kind == K::ShroudtopiaMod) {
        // the loose files that belong to the kind: the Greeneye Loader's dbgcore.dll, files under emf\ for an EMF mod
        // (a loose DLL in emf\mods, a settings ini that is never overwritten), DLLs directly in mods\ for Shroudtopia
        PlanPart part;
        part.kind = nexus::kind_id(c.kind);
        Placer p{in, owned, own, emf_mod_dirs};
        std::vector<std::string> kept_settings;
        for (const auto& rel : loose) {
            const std::string low = lower(rel);
            if (c.kind == K::GreeneyeMod && low == "dbgcore.dll") {
                part.label = "Greeneye Loader";
                part.title = "Greeneye Loader (dbgcore.dll)";
                const std::string* word = nullptr;
                for (const auto& pr : in.state.proxies)
                    if (lower(pr.first) == "dbgcore.dll") word = &pr.second;
                if (word && *word == "greeneye" && !owned.count("dbgcore.dll")) {
                    part.lines.push_back("The Greeneye Loader (dbgcore.dll) is installed already: kept as it is.");
                    part.deps.push_back("dbgcore.dll");  // shared: uninstall must not delete it if another mod needs it
                    continue;
                }
                p.place(part, from_of(rel), "dbgcore.dll", false);
            } else if (c.kind == K::EmfMod && low.rfind("emf/", 0) == 0) {
                part.label = "EMF mod files";
                part.title = "Files in emf\\";
                if (low.rfind("emf/config/", 0) == 0 && exists(in.game + L"\\" + wide(win(rel))) && !owned.count(lower(win(rel)))) {
                    kept_settings.push_back(win(rel));
                    continue;
                }
                p.place(part, from_of(rel), win(rel), false);
            } else if (c.kind == K::ShroudtopiaMod && low.rfind("mods/", 0) == 0 && depth(low) == 1 && iends(low, ".dll")) {
                part.label = "Shroudtopia mod";
                part.title = base_name(rel);
                p.place(part, from_of(rel), win(rel), false);
            } else {
                not_copied.push_back(rel);
            }
        }
        if (!kept_settings.empty()) part.lines.push_back("Your settings are kept: " + join(kept_settings) + " is not replaced.");
        if (!part.label.empty()) {
            part.target = c.kind == K::EmfMod ? "emf\\" : c.kind == K::ShroudtopiaMod ? "mods\\" : "next to the game";
            if (!part.copies.empty()) part.lines.insert(part.lines.begin(), files_go(part.copies.size(), "to " + part.target));
            finish(part, p);
            plan.parts.push_back(std::move(part));
        }
    } else if (c.kind == K::EmlMod || c.kind == K::ShroudForgePackage) {
        not_copied = loose;
    } else {
        // explained, not installed
        PlanPart part;
        part.kind = nexus::kind_id(c.kind);
        part.label = nexus::kind_label(c.kind);
        part.title = part.label;
        part.action = Action::Skip;
        switch (c.kind) {
            case K::ReshadePreset:
                part.target = "next to the game, for ReShade";
                part.why = "A ReShade preset: install ReShade itself first, then copy the preset next to the game as the mod's "
                           "page says. EMF does not install ReShade files.";
                break;
            case K::SaveFiles:
                part.target = "Saved Games\\Enshrouded";
                part.why = "Save files replace a character or a world. EMF never touches saves: back up Saved "
                           "Games\\Enshrouded first and copy them by hand if you want them.";
                break;
            case K::CheatTable:
                part.target = "Cheat Engine";
                part.why = "A Cheat Engine table: open it in Cheat Engine while the game runs. There is nothing to install.";
                break;
            default:
                part.target = "unknown";
                part.why = "EMF cannot tell where these files go. Read the mod's page and install it by hand.";
                break;
        }
        plan.parts.push_back(std::move(part));
    }
    if (!not_copied.empty()) plan.notes.push_back("Not copied (they are not part of the mod): " + join(not_copied) + ".");

    // ---- the name shown ----
    for (const auto& f : folders)
        if (plan.name.empty() && f.error.empty()) {
            plan.name = f.name;
            plan.version = f.version;
        }
    if (plan.name.empty()) plan.name = !in.nexus_name.empty() ? in.nexus_name : in.archive;
    // programs and scripts EMF copies but never runs
    std::vector<std::string> runnable;
    for (const auto& part : plan.parts)
        if (part.installs())
            for (const auto& cp : part.copies) {
                const std::string low = lower(cp.to);
                for (const char* ext : {".exe", ".bat", ".cmd", ".ps1", ".lnk", ".com", ".scr"})
                    if (iends(low, ext) && std::find(runnable.begin(), runnable.end(), cp.to) == runnable.end())
                        runnable.push_back(cp.to);
            }
    if (!runnable.empty())
        plan.notes.push_back("It installs programs or scripts (" + join(runnable) +
                             "): EMF copies them but never runs them. Run them only if the mod's page says to.");
    if (plan.parts.empty()) plan.error = "EMF found nothing to install in this archive.";
    return plan;
}

// ---- applying ----

InstallResult apply_plan(const std::wstring& game, const InstallPlan& plan) {
    InstallResult res;
    if (!plan.error.empty()) {
        res.error = plan.error;
        return res;
    }
    if (!plan.installable()) {
        res.error = "There is nothing to install.";
        return res;
    }
    if (!valid_install_key(plan.key)) {
        res.error = "The install has no usable name.";
        return res;
    }
    if (!plan.game.empty() && !iequals(plan.game, game)) {
        res.error = "The game folder changed since this plan was made. Choose the file again.";
        return res;
    }
    const std::set<std::string> own = emf_own_files(game);
    std::set<std::string> emf_mod_dirs;
    for (const auto& part : plan.parts)
        if (part.kind == std::string("emf-mod") && !part.root.empty()) emf_mod_dirs.insert(lower(part.root));
    const auto earlier = read_install_record(game, plan.key);
    const std::string stamp = now_text("%Y%m%d-%H%M%S");
    const auto backup_dir = [&](const std::string& name) {
        std::string d = "emf\\backup\\" + name + "-" + stamp;
        for (int i = 2; exists(game + L"\\" + wide(d)); ++i) d = "emf\\backup\\" + name + "-" + stamp + "-" + std::to_string(i);
        return d;
    };

    // what was done, to take it back on a failure
    std::vector<std::pair<std::string, std::string>> moved;  // from, to (relative)
    std::vector<std::string> made, copied;
    std::string chain_before;
    const bool chain_existed = read_file(game + L"\\dinput8-chain.ini", chain_before, 1u << 20);
    bool chain_changed = false;
    std::vector<std::string> backups;
    const auto undo = [&](const std::string& why) {
        // the new files first, then the empty folders they made, so a backup can move back into its old place
        for (auto it = copied.rbegin(); it != copied.rend(); ++it) {
            SetFileAttributesW(lp(game + L"\\" + wide(*it)).c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(lp(game + L"\\" + wide(*it)).c_str());  // only the new files this install made
        }
        remove_empty(game, made);
        std::vector<std::string> stuck;  // backups that could not be moved back
        for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
            std::vector<std::string> ignore;
            make_folders(game, parent_of(it->first), ignore);  // recreate the parent the new files' removal may have left empty
            if (!MoveFileExW(lp(game + L"\\" + wide(it->second)).c_str(), lp(game + L"\\" + wide(it->first)).c_str(), 0))
                stuck.push_back(it->first + " (your copy is in " + it->second + ")");
        }
        if (chain_changed) {
            if (chain_existed) write_file_whole(game + L"\\dinput8-chain.ini", chain_before, "dinput8-chain.ini");
            else DeleteFileW(lp(game + L"\\dinput8-chain.ini").c_str());
        }
        res.ok = false;
        res.done.clear();
        res.error = stuck.empty() ? why + " Nothing was changed."
                                  : why + " EMF could not put everything back: " + join(stuck, 8) + ".";
        return res;
    };

    // 1. what the new version replaces moves to emf\backup\ first
    std::string files_backup;
    const auto backup_file = [&](const std::string& rel) {
        if (protected_from_removal(rel, own)) return true;  // never move the game's or EMF's own files, however a record names them
        if (!exists(game + L"\\" + wide(rel))) return true;
        if (files_backup.empty()) {
            files_backup = backup_dir(plan.key);
            backups.push_back(files_backup);
        }
        const std::string to = files_backup + "\\" + rel;
        if (!move_into(game, rel, to, made)) return false;
        moved.emplace_back(rel, to);
        return true;
    };
    for (const auto& part : plan.parts) {
        if (!part.installs()) continue;
        if (!part.replace_folder.empty() && exists(game + L"\\" + wide(part.replace_folder))) {
            const bool in_place = iequals(wide(part.replace_folder), wide(part.root));
            if (!in_place) {
                // the switched-off mods\.foo being replaced: the whole old folder (its data too) goes to the backup
                std::string name = base_name(part.replace_folder);
                if (!name.empty() && name[0] == '.') name.erase(0, 1);
                const std::string to = backup_dir(name);
                if (!move_into(game, part.replace_folder, to, made)) {
                    const DWORD e = GetLastError();
                    return undo("Could not move " + part.replace_folder + " to " + to + ": " + windows_error_text(e) +
                                (e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED ? " Is a program using its files?" : ""));
                }
                moved.emplace_back(part.replace_folder, to);
                backups.push_back(to);
                res.done.push_back("The old " + part.replace_folder + "\\ is kept in " + to + "\\.");
            } else {
                // an in-place update: back up only the files the new version ships or the old install put here; the
                // rest (the mod's own saved data, emf.dir, the player's edits) stays, carried into the new version
                const std::string folder = part.root;
                std::set<std::string> ships;  // lower-case paths the new version writes under the folder
                for (const auto& cp : part.copies) ships.insert(lower(cp.to));
                std::set<std::string> old_installed;
                if (earlier)
                    for (const auto& [path, hash] : earlier->files)
                        if (iprefix(path, folder + "\\")) old_installed.insert(lower(path));
                std::vector<std::string> here_files;
                uint64_t ignore_bytes = 0;
                std::string ignore_why;
                list_tree(game + L"\\" + wide(folder), here_files, ignore_bytes, ignore_why, 100000);
                size_t carried = 0;
                for (const auto& sub : here_files) {
                    const std::string rel = folder + "\\" + win(sub);
                    if (ships.count(lower(rel)) || old_installed.count(lower(rel))) {
                        if (!backup_file(rel)) return undo("Could not move " + rel + " to the backup: " + windows_error_text(GetLastError()));
                    } else {
                        ++carried;  // the mod's own data: left in place
                    }
                }
                if (carried) res.done.push_back("Kept " + count_files(carried) + " of your data in " + folder + "\\.");
            }
        }
        for (const auto& f : part.replace_files)
            if (!backup_file(f)) return undo("Could not move " + f + " to the backup: " + windows_error_text(GetLastError()));
    }
    if (earlier)  // the earlier version's other files go too (kept in the backup, never deleted)
        for (const auto& [path, hash] : earlier->files)
            if (!backup_file(switched_path(game, path)))
                return undo("Could not move " + path + " to the backup: " + windows_error_text(GetLastError()));
    if (!files_backup.empty()) res.done.push_back("The earlier version's files are kept in " + files_backup + "\\.");

    // 2. the new files: CopyFile never overwrites
    InstallRecord rec;
    rec.key = plan.key;
    rec.name = plan.name;
    rec.version = plan.version;
    rec.nexus_id = plan.nexus_id;
    rec.nexus_file_id = plan.nexus_file_id;
    rec.archive = plan.archive;
    rec.installed_at = now_text("%Y-%m-%d %H:%M:%S");
    // a part that only keeps a shared file (e.g. the Greeneye loader already there) installs nothing but still depends
    // on it: collect deps from every part that was not refused, so uninstall knows not to delete it
    for (const auto& part : plan.parts)
        if (part.action != Action::Refused)
            for (const auto& d : part.deps)
                if (std::find(rec.deps.begin(), rec.deps.end(), d) == rec.deps.end()) rec.deps.push_back(d);
    std::vector<std::string> chain_add, chain_off;
    size_t n = 0;
    for (const auto& part : plan.parts) {
        if (!part.installs()) continue;
        if (std::find(rec.kinds.begin(), rec.kinds.end(), part.kind) == rec.kinds.end()) rec.kinds.push_back(part.kind);
        if (!part.root.empty()) rec.roots.push_back(part.root);
        for (const auto& cp : part.copies) {
            if (nexus::unsafe_archive_path(cp.to) || blocked_loose_target(cp.to, emf_mod_dirs))
                return undo("The path " + cp.to + " is not one EMF installs.");
            if (!make_folders(game, parent_of(cp.to), made))
                return undo("Could not make the folder for " + cp.to + ": " + windows_error_text(GetLastError()));
            const std::wstring dst = game + L"\\" + wide(cp.to);
            if (!CopyFileW(lp(cp.from).c_str(), lp(dst).c_str(), TRUE)) {
                const DWORD e = GetLastError();
                return undo(e == ERROR_FILE_EXISTS ? cp.to + " appeared in the game folder since the plan was made; EMF never overwrites it."
                                                   : "Could not copy " + cp.to + ": " + windows_error_text(e));
            }
            copied.push_back(cp.to);
            rec.files.emplace_back(cp.to, file_sha256(dst));
            ++n;
        }
        for (const auto& a : part.chain_add) chain_add.push_back(a);
        for (const auto& o : part.chain_off) chain_off.push_back(o);
        rec.chain.insert(rec.chain.end(), part.chain_add.begin(), part.chain_add.end());
        rec.chain.insert(rec.chain.end(), part.chain_keep.begin(), part.chain_keep.end());
        rec.chain_off.insert(rec.chain_off.end(), part.chain_off.begin(), part.chain_off.end());
    }

    // 3. dinput8-chain.ini: the earlier version's lines out, the new ones in, older copies of the same tool off
    std::vector<std::string> chain_drop;
    if (earlier)
        for (const auto& old : earlier->chain)
            if (std::none_of(rec.chain.begin(), rec.chain.end(), [&](const std::string& a) { return _stricmp(a.c_str(), old.c_str()) == 0; }))
                chain_drop.push_back(old);
    if (!chain_add.empty() || !chain_off.empty() || !chain_drop.empty()) {
        bool created = false;
        const Change c = edit_chain_file(game, [&](const std::string& text) -> std::optional<std::string> {
            std::string t = text;
            bool any = false;
            for (const auto& d : chain_drop)
                if (auto r = remove_chain_line(t, wide(d))) t = *r, any = true;
            for (const auto& a : chain_add)
                if (auto r = add_chain_line(t, wide(a))) t = *r, any = true;
            for (const auto& o : chain_off)
                if (auto r = switch_chain_line(t, wide(o), false)) t = *r, any = true;
            if (!any) return std::nullopt;
            if (text.empty() && !chain_existed) t = "[Chain]\r\n" + t;  // a new file names its section, as the installer's
            return t;
        }, &created);
        if (!c.ok) return undo(c.error);
        chain_changed = true;
        for (const auto& a : chain_add) res.done.push_back(a + " is listed in dinput8-chain.ini: EMF starts it at the next game start.");
        for (const auto& o : chain_off) res.done.push_back("The older copy " + o + " is switched off in dinput8-chain.ini.");
    }

    // 4. the record (the backup folders are not the install's: uninstall leaves them)
    for (const auto& f : made)
        if (!iprefix(f + "\\", "emf\\backup\\")) rec.folders.push_back(f);
    rec.backups = backups;
    if (earlier) {
        for (const auto& b : earlier->backups) rec.backups.push_back(b);
        // folders the earlier version made that the new one still uses stay the install's
        for (const auto& f : earlier->folders)
            if (is_dir(game + L"\\" + wide(f)) && std::find(rec.folders.begin(), rec.folders.end(), f) == rec.folders.end())
                rec.folders.push_back(f);
    }
    std::vector<std::string> record_made;
    if (!make_folders(game, kRecords, record_made)) return undo("Could not make " + std::string(kRecords) + ".");
    const Change w = write_file_whole(game + L"\\" + wide(record_path(plan.key)), record_json(rec), record_path(plan.key));
    if (!w.ok) {
        remove_empty(game, record_made);
        return undo(w.error);
    }
    if (earlier) remove_empty(game, earlier->folders);  // the earlier version's folders that are empty now
    res.ok = true;
    res.done.insert(res.done.begin(), "Installed " + plan.name + (plan.version.empty() ? "" : " " + plan.version) + ": " +
                                          count_files(n) + " copied. It is loaded at the next game start.");
    return res;
}

// ---- records ----

std::vector<InstallRecord> read_install_records(const std::wstring& game) {
    std::vector<InstallRecord> out;
    WIN32_FIND_DATAW fd;
    const std::wstring dir = game + L"\\" + wide(kRecords);
    HANDLE h = FindFirstFileW((dir + L"\\*.json").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::string name = utf8(fd.cFileName);
        if (!iends(name, ".json")) continue;
        const std::string key = name.substr(0, name.size() - 5);
        if (!valid_install_key(key)) continue;
        std::string text;
        if (!read_file(dir + L"\\" + fd.cFileName, text, kMaxJson)) continue;
        if (auto r = parse_record(text, key)) out.push_back(std::move(*r));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const InstallRecord& a, const InstallRecord& b) { return a.key < b.key; });
    return out;
}

std::optional<InstallRecord> read_install_record(const std::wstring& game, const std::string& key) {
    if (!valid_install_key(key)) return std::nullopt;
    std::string text;
    if (!read_file(game + L"\\" + wide(record_path(key)), text, kMaxJson)) return std::nullopt;
    return parse_record(text, key);
}

InstallResult uninstall_mod(const std::wstring& game, const std::string& key) {
    InstallResult res;
    const auto rec = read_install_record(game, key);
    if (!rec) {
        res.error = "EMF has no record of installing \"" + key + "\", so it cannot uninstall it.";
        return res;
    }
    // files the game or EMF owns (a forged record may name them), and files another install still needs (shared)
    const std::set<std::string> own = emf_own_files(game);
    std::set<std::string> needed;       // other records' files and deps, lower-case
    std::set<std::string> others_own;   // files another record installed itself
    std::map<std::string, std::string> heir;  // a file another install depends on -> the first such install (by key)
    for (const auto& r : read_install_records(game)) {
        if (r.key == key) continue;
        for (const auto& [p, h] : r.files) needed.insert(lower(p)), others_own.insert(lower(p));
        for (const auto& d : r.deps) needed.insert(lower(d)), heir.emplace(lower(d), r.key);
    }
    for (const auto& d : rec->deps) needed.insert(lower(d));  // its own deps are never deleted either

    std::vector<std::string> left, gone, shared, protectedf;
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> handed;  // heir key -> (path, SHA-256)
    size_t removed = 0;
    for (const auto& [path, hash] : rec->files) {
        if (protected_from_removal(path, own)) {
            protectedf.push_back(path);  // never delete the game's or EMF's own files
            continue;
        }
        if (needed.count(lower(path))) {
            shared.push_back(path);  // another mod uses it too
            // EMF put it there for this install: an install that only depends on it takes it over, so the last one
            // that needs it removes it (else it would stay once both are gone)
            const auto h = heir.find(lower(path));
            if (h != heir.end() && !others_own.count(lower(path))) handed[h->second].emplace_back(path, hash);
            continue;
        }
        const std::string rel = switched_path(game, path);
        const std::wstring p = game + L"\\" + wide(rel);
        if (!exists(p) || is_dir(p)) {
            gone.push_back(path);
            continue;
        }
        if (file_sha256(p) != hash) {
            left.push_back(rel);
            continue;
        }
        SetFileAttributesW(lp(p).c_str(), FILE_ATTRIBUTE_NORMAL);
        if (DeleteFileW(lp(p).c_str())) ++removed;
        else left.push_back(rel + " (" + windows_error_text(GetLastError()) + ")");
    }
    for (const auto& [heir_key, files] : handed) {  // the heir's record: those files are its own now
        auto r = read_install_record(game, heir_key);
        if (!r) continue;
        for (const auto& [path, hash] : files) {
            r->deps.erase(std::remove_if(r->deps.begin(), r->deps.end(), [&](const std::string& d) { return lower(d) == lower(path); }),
                          r->deps.end());
            r->files.emplace_back(path, hash);
        }
        write_file_whole(game + L"\\" + wide(record_path(heir_key)), record_json(*r), record_path(heir_key));
    }
    std::vector<std::string> dirs;
    for (const auto& f : rec->folders) dirs.push_back(switched_path(game, f));
    for (const auto& r : rec->roots) dirs.push_back(switched_path(game, r));  // the mod's own folder, when empty now
    remove_empty(game, dirs);

    // dinput8-chain.ini: its lines out (a DLL that was left in place keeps its line), copies it switched off on again
    std::vector<std::string> kept_lines;
    const Change c = edit_chain_file(game, [&](const std::string& text) -> std::optional<std::string> {
        if (text.empty()) return std::nullopt;
        std::string t = text;
        bool any = false;
        for (const auto& n : rec->chain) {
            if (exists(game + L"\\" + wide(n))) {
                kept_lines.push_back(n);
                continue;
            }
            if (auto r = remove_chain_line(t, wide(n))) t = *r, any = true;
        }
        for (const auto& n : rec->chain_off)
            if (exists(game + L"\\" + wide(n)))
                if (auto r = switch_chain_line(t, wide(n), true)) t = *r, any = true;
        return any ? std::optional<std::string>(t) : std::nullopt;
    });
    DeleteFileW(lp(game + L"\\" + wide(record_path(key))).c_str());  // EMF's own record
    RemoveDirectoryW(lp(game + L"\\" + wide(kRecords)).c_str());     // when it was the last one

    res.ok = true;
    res.done.push_back("Uninstalled " + rec->name + ": " + count_files(removed) + " removed.");
    if (!shared.empty()) res.done.push_back("Kept, also used by another mod: " + join(shared, 8) + ".");
    if (!protectedf.empty()) res.done.push_back("Kept (the game's or EMF's own file): " + join(protectedf, 8) + ".");
    if (!left.empty())
        res.done.push_back("Left in place because they changed since they were installed: " + join(left, 8) + ".");
    if (!gone.empty()) res.done.push_back("Already gone: " + join(gone) + ".");
    if (!kept_lines.empty()) res.done.push_back(join(kept_lines) + " stays listed in dinput8-chain.ini (the DLL was left in place).");
    if (!rec->chain_off.empty()) res.done.push_back("Switched on again in dinput8-chain.ini: " + join(rec->chain_off) + ".");
    if (!c.ok) res.done.push_back("dinput8-chain.ini was not changed: " + c.error);
    if (!rec->backups.empty()) res.done.push_back("Earlier versions stay in " + join(rec->backups) + ".");
    return res;
}

// ---- for the page ----

std::string plan_json(const InstallPlan& plan, const std::string& id, const std::string& blocked) {
    std::vector<std::string> parts;
    for (const auto& p : plan.parts) {
        uint64_t bytes = 0;
        for (const auto& c : p.copies) bytes += c.size;
        std::vector<std::string> copies;
        for (size_t i = 0; i < p.copies.size() && i < 200; ++i) copies.push_back(p.copies[i].to);
        parts.push_back(Obj()
                            .str("action", action_name(p.action)).str("kind", p.kind).str("label", p.label)
                            .str("title", p.title).str("version", p.version).str("target", p.target).str("why", p.why)
                            .num("files", static_cast<double>(p.copies.size())).num("bytes", static_cast<double>(bytes))
                            .strs("copies", copies).strs("chain", p.chain_add).strs("lines", p.lines)
                            .str("replace_folder", p.replace_folder)
                            .text());
    }
    return Obj()
        .str("id", id).str("archive", plan.archive).str("game", utf8(plan.game)).str("key", plan.key).str("name", plan.name)
        .str("version", plan.version).str("nexus_id", plan.nexus_id).str("nexus_file_id", plan.nexus_file_id)
        .str("kind_label", plan.kind_label)
        .str("error", plan.error).boolean("update", plan.update).boolean("installable", plan.installable())
        .str("blocked", blocked).raw("parts", json_list(parts)).strs("notes", plan.notes)
        .text();
}

}  // namespace emf::app
