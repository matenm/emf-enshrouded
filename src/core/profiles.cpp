#include "profiles.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <set>
#include <unordered_map>

#include "common/fnv.h"
#include "json.h"
#include "manifest.h"

namespace emf::core {

namespace {

using json::Value;
using VT = Value::Type;

std::atomic<size_t> g_files_read{0};

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// How a mod is found in a profile or a folder: its source and its id, case ignored (folder and DLL names).
std::string key_of(const std::string& source, const std::string& id) { return source + '\x1f' + lower(id); }

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// Characters in valid UTF-8 text, or -1 when the bytes are not valid UTF-8.
long utf8_count(const std::string& s) {
    long n = 0;
    for (size_t i = 0; i < s.size(); ++n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (!len || i + len > s.size()) return -1;
        for (size_t k = 1; k < len; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return -1;
        if (len == 2 && c < 0xC2) return -1;
        if (len == 3 && c == 0xE0 && static_cast<unsigned char>(s[i + 1]) < 0xA0) return -1;
        if (len == 3 && c == 0xED && static_cast<unsigned char>(s[i + 1]) >= 0xA0) return -1;
        if (len == 4 && (c > 0xF4 || (c == 0xF0 && static_cast<unsigned char>(s[i + 1]) < 0x90) ||
                         (c == 0xF4 && static_cast<unsigned char>(s[i + 1]) >= 0x90)))
            return -1;
        i += len;
    }
    return n;
}

bool has_control(const std::string& s) {
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7F) return true;
    return false;
}

// A short text from a file: valid UTF-8, no control characters, at most `max` bytes.
bool text_ok(const std::string& s, size_t max) { return s.size() <= max && !has_control(s) && utf8_count(s) >= 0; }

// ---- writing JSON (core has no writer: a profile is small and flat) ----
std::string quote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    out += b;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out + "\"";
}

const Value* member(const Value& v, const char* key, VT type) {
    const Value* x = v.get(key);
    return x && x->is(type) ? x : nullptr;
}

std::string text_of(const Value& v, const char* key) {
    const Value* x = member(v, key, VT::String);
    return x ? x->s : std::string();
}

// A Nexus id as digits (files from other tools may give a number).
std::string id_text(const Value& v, const char* key) {
    const Value* x = v.get(key);
    if (!x) return {};
    if (x->is(VT::String)) return x->s.find_first_not_of("0123456789") == std::string::npos && x->s.size() < 16 ? x->s : "";
    if (x->is(VT::Number) && x->n > 0 && x->n < 1e15 && x->n == std::floor(x->n))
        return std::to_string(static_cast<long long>(x->n));
    return {};
}

bool source_ok(const std::string& s) {
    if (s.empty() || s.size() > 16) return false;
    for (char c : s)
        if (c < 'a' || c > 'z') return false;
    return true;
}

bool id_ok(const std::string& id) { return !id.empty() && id.size() <= 300 && !has_control(id) && utf8_count(id) > 0; }

// Index of a folder's mods and a profile's entries by key_of (the first of two copies wins).
std::unordered_map<std::string, const FolderMod*> index_folder(const std::vector<FolderMod>& folder) {
    std::unordered_map<std::string, const FolderMod*> out;
    out.reserve(folder.size());
    for (const auto& m : folder) out.emplace(key_of(m.source, m.id), &m);
    return out;
}

std::unordered_map<std::string, const ProfileMod*> index_profile(const Profile& p) {
    std::unordered_map<std::string, const ProfileMod*> out;
    out.reserve(p.mods.size());
    for (const auto& m : p.mods) out.emplace(key_of(m.source, m.id), &m);
    return out;
}

template <class Map>
auto look(const Map& m, const std::string& source, const std::string& id) -> typename Map::mapped_type {
    auto it = m.find(key_of(source, id));
    return it == m.end() ? nullptr : it->second;
}

// The profile's value as EMF would store it for this setting (nullopt: the setting does not accept it). Without a
// definition the value is compared as it is.
std::optional<std::string> normalised(const FolderSetting& here, const std::string& value) {
    if (!here.def) return value;
    return setting_value(*here.def, value, false, nullptr);
}

const FolderSetting* setting_here(const FolderMod& m, const std::string& key) {
    for (const auto& s : m.settings)
        if (s.key == key) return &s;
    return nullptr;
}

std::string shown(const ApplyItem& i) { return i.name.empty() ? i.id : i.name; }

std::string join_names(const std::vector<ApplyItem>& items) {
    std::string out;
    for (const auto& i : items) out += (out.empty() ? "" : ", ") + shown(i);
    return out;
}

std::string plural(size_t n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }

// ---- files ----
bool read_whole(const std::wstring& path, std::string& out, size_t max) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    const size_t want = static_cast<size_t>(std::min<long long>(size.QuadPart, static_cast<long long>(max)));
    out.assign(want, '\0');
    DWORD got = 0;
    const BOOL ok = want == 0 || ReadFile(h, out.data(), static_cast<DWORD>(want), &got, nullptr);
    CloseHandle(h);
    out.resize(got);
    return ok != 0;
}

std::string error_text(DWORD e) {
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, e, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring w = n && buf ? std::wstring(buf, n) : L"error " + std::to_wstring(e);
    if (buf) LocalFree(buf);
    while (!w.empty() && (w.back() == L'\n' || w.back() == L'\r' || w.back() == L' ' || w.back() == L'.')) w.pop_back();
    return narrow(w);
}

// The whole file through a new file next to it, so a failed write leaves the old one whole.
bool write_whole(const std::wstring& path, const std::string& bytes, std::string* error) {
    const std::wstring tmp = path + L".emf-new";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (error) *error = error_text(GetLastError());
        return false;
    }
    DWORD put = 0;
    const BOOL ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &put, nullptr) && put == bytes.size();
    const DWORD err = GetLastError();
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (error) *error = error_text(ok ? GetLastError() : err);
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

bool make_dirs(const std::wstring& dir) {
    if (dir.empty()) return false;
    const DWORD a = GetFileAttributesW(dir.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 2) make_dirs(dir.substr(0, slash));
    return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

// A file name in the profiles folder that is not a slug but can still be named for deletion: no folder, no wildcard,
// not "." or "..", at most 200 bytes.
bool plain_stem(const std::string& s) {
    if (s.empty() || s.size() > 200 || s == "." || s == ".." || has_control(s) || utf8_count(s) < 0) return false;
    return s.find_first_of("\\/:*?\"<>|") == std::string::npos;
}

// list(): what a file held when it was last parsed, by its full path, size and time (the state and the picker list the
// folder every few seconds; a profile file is parsed again only when it changed).
struct CachedInfo {
    uint64_t size = 0;
    FILETIME time{};
    ProfileInfo info;
};
std::mutex g_cache_m;
std::unordered_map<std::wstring, CachedInfo> g_cache;

}  // namespace

size_t profile_files_read() { return g_files_read.load(); }

// ---- names ----

std::string trim_profile_name(const std::string& name) {
    size_t a = 0, b = name.size();
    while (a < b && (name[a] == ' ' || name[a] == '\t')) ++a;
    while (b > a && (name[b - 1] == ' ' || name[b - 1] == '\t')) --b;
    return name.substr(a, b - a);
}

std::string profile_name_problem(const std::string& raw) {
    const std::string name = trim_profile_name(raw);
    if (name.empty()) return "A profile needs a name.";
    const long n = utf8_count(name);
    if (n < 0) return "The name is not valid text.";
    if (static_cast<size_t>(n) > kProfileNameMax)
        return "A profile's name has at most " + std::to_string(kProfileNameMax) + " characters.";
    if (has_control(name)) return "The name has a character that cannot be typed.";
    if (name.find_first_of("\\/:*?\"<>|") != std::string::npos)
        return "A profile's name cannot have any of these: \\ / : * ? \" < > |";
    if (name.find_first_not_of('.') == std::string::npos) return "A profile's name cannot be only dots.";
    return {};
}

std::string profile_slug(const std::string& raw) {
    const std::string name = trim_profile_name(raw);
    std::string slug;
    bool dash = false, other = false;
    for (char ch : name) {
        if (static_cast<unsigned char>(ch) >= 0x80) other = true;
        const char c = (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (dash && !slug.empty()) slug += '-';
            dash = false;
            slug += c;
        } else {
            dash = true;
        }
    }
    char hash[16];
    std::snprintf(hash, sizeof hash, "%08x", emf::fnv1a32(lower(name)));
    if (slug.empty()) return std::string("profile-") + hash;
    if (other) return slug + "-" + hash;  // the letters outside ASCII tell such names apart
    static const std::set<std::string> devices = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5",
                                                  "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4",
                                                  "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    if (devices.count(slug)) slug += "-profile";
    return slug;
}

bool valid_profile_slug(const std::string& slug) {
    if (slug.empty() || slug.size() > 60 || slug.front() == '-' || slug.back() == '-') return false;
    for (char c : slug)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    return true;
}

// ---- the file ----

std::string profile_to_json(const Profile& p) {
    std::string out = "{\n  \"emf_profile\": " + std::to_string(kProfileFormat) + ",\n";
    out += "  \"name\": " + quote(p.name) + ",\n";
    out += "  \"created\": " + quote(p.created) + ",\n";
    out += "  \"emf_version\": " + quote(p.emf_version) + ",\n";
    out += "  \"mods\": [";
    for (size_t i = 0; i < p.mods.size(); ++i) {
        const ProfileMod& m = p.mods[i];
        out += std::string(i ? "," : "") + "\n    {\"source\": " + quote(m.source) + ", \"id\": " + quote(m.id) +
               ", \"on\": " + (m.on ? "true" : "false");
        if (!m.name.empty()) out += ", \"name\": " + quote(m.name);
        if (!m.nexus_id.empty()) out += ", \"nexus_id\": " + quote(m.nexus_id);
        if (m.read_only) out += ", \"read_only\": true";
        out += "}";
    }
    out += p.mods.empty() ? "]" : "\n  ]";
    if (p.with_settings) {
        out += ",\n  \"settings\": {";
        size_t n = 0;
        for (const auto& [mod, list] : p.settings) {
            out += std::string(n++ ? "," : "") + "\n    " + quote(mod) + ": {";
            for (size_t i = 0; i < list.size(); ++i)
                out += std::string(i ? ", " : "") + quote(list[i].key) + ": " + quote(list[i].value);
            out += "}";
        }
        out += n ? "\n  }" : "}";
    }
    return out + "\n}\n";
}

ProfileRead profile_from_json(const std::string& text) {
    ProfileRead r;
    const auto fail = [&](const std::string& why) {
        r.profile.reset();
        r.error = why;
        return r;
    };
    if (text.size() > kProfileFileMax) return fail("The file is larger than 1 MB, too large for a profile.");
    const auto parsed = json::parse(text);
    if (!parsed.value) return fail("The file is not valid JSON: " + parsed.error);
    const Value& v = *parsed.value;
    if (!v.is(VT::Object)) return fail("A profile file holds one JSON object ({...}).");
    if (const Value* f = v.get("emf_profile")) {
        if (!f->is(VT::Number) || !(f->n >= 1 && f->n < 1000000)) return fail("\"emf_profile\" is not a format EMF knows.");
        if (f->n > kProfileFormat)
            return fail("A newer EMF made this profile (format " + std::to_string(static_cast<int>(f->n)) +
                        "): update EMF to use it.");
    }
    Profile p;
    p.name = trim_profile_name(text_of(v, "name"));
    if (!member(v, "name", VT::String)) return fail("The profile has no \"name\".");
    if (const std::string why = profile_name_problem(p.name); !why.empty()) return fail("Its name: " + why);
    p.created = text_of(v, "created");
    p.emf_version = text_of(v, "emf_version");
    if (!text_ok(p.created, 40)) return fail("\"created\" is not a short line of text.");
    if (!text_ok(p.emf_version, 40)) return fail("\"emf_version\" is not a short line of text.");
    const Value* mods = member(v, "mods", VT::Array);
    if (!mods) return fail("The profile has no list of mods (\"mods\": [...]).");
    if (mods->arr.size() > kProfileModsMax)
        return fail("The profile lists " + std::to_string(mods->arr.size()) + " mods; a profile has at most " +
                    std::to_string(kProfileModsMax) + ".");
    std::set<std::string> seen;  // source and id, case ignored: a mod listed twice
    p.mods.reserve(mods->arr.size());
    for (size_t i = 0; i < mods->arr.size(); ++i) {
        const Value& e = mods->arr[i];
        const std::string at = "mod " + std::to_string(i + 1);
        if (!e.is(VT::Object)) return fail(at + " in \"mods\" is not an object ({\"source\", \"id\", \"on\"}).");
        ProfileMod m;
        m.source = text_of(e, "source");
        m.id = text_of(e, "id");
        if (!source_ok(m.source)) return fail(at + " has no valid \"source\" (emf, eml, chain or other).");
        if (!id_ok(m.id)) return fail(at + " has no valid \"id\".");
        const Value* on = member(e, "on", VT::Bool);
        if (!on) return fail(at + " (" + m.id + ") has no \"on\": true or false.");
        m.on = on->b;
        m.name = text_of(e, "name");
        if (!text_ok(m.name, kProfileTextMax)) return fail(at + " (" + m.id + ") has a \"name\" that is not a short line of text.");
        m.nexus_id = id_text(e, "nexus_id");
        const Value* ro = member(e, "read_only", VT::Bool);
        m.read_only = ro && ro->b;
        if (!seen.insert(key_of(m.source, m.id)).second) return fail(m.source + " mod \"" + m.id + "\" is listed twice.");
        p.mods.push_back(std::move(m));
    }
    if (const Value* s = v.get("settings")) {
        if (!s->is(VT::Object)) return fail("\"settings\" must be an object ({mod id: {key: value}}).");
        p.with_settings = true;
        size_t count = 0;
        for (const auto& [mod, list] : s->obj) {
            if (!valid_mod_id(mod)) return fail("\"settings\" names \"" + mod + "\", which is not an EMF mod id.");
            if (!list.is(VT::Object)) return fail("\"settings\" of " + mod + " must be an object ({key: value}).");
            auto& out = p.settings[mod];
            for (const auto& [key, value] : list.obj) {
                if (++count > kProfileSettingsMax)
                    return fail("The profile holds more than " + std::to_string(kProfileSettingsMax) + " settings.");
                if (!valid_setting_key(key)) return fail("\"settings\" of " + mod + " has a bad key \"" + key + "\".");
                if (!value.is(VT::String))
                    return fail("\"settings\" of " + mod + ": the value of " + key + " must be text (as EMF writes it).");
                if (value.s.size() > kProfileValueMax)
                    return fail("\"settings\" of " + mod + ": the value of " + key + " is too long.");
                out.push_back({key, value.s});
            }
        }
    }
    r.profile = std::move(p);
    return r;
}

// ---- capture, compare, apply ----

Profile capture_profile(const std::string& name, const std::vector<FolderMod>& folder, bool with_settings) {
    Profile p;
    p.name = trim_profile_name(name);
    p.with_settings = with_settings;
    std::set<std::string> seen;
    for (const auto& m : folder) {
        if (m.forced) continue;                                // its state is not a switch: no say on it
        if (!seen.insert(key_of(m.source, m.id)).second) continue;  // two copies: the folder side merges them first
        p.mods.push_back({m.source, m.id, m.on, m.name, m.nexus_id, m.read_only});
        if (with_settings && m.source == "emf" && !m.settings.empty() && valid_mod_id(m.id)) {
            auto& out = p.settings[m.id];
            for (const auto& s : m.settings) out.push_back({s.key, s.value});
        }
    }
    return p;
}

bool profile_differs(const Profile& p, const std::vector<FolderMod>& folder) {
    const auto entries = index_profile(p);
    for (const auto& m : folder) {
        if (!m.can_switch) continue;
        const ProfileMod* e = look(entries, m.source, m.id);
        // a mod it does not list, or records read-only: it has no say on it
        if (e && !e->read_only && e->on != m.on) return true;
    }
    if (!p.with_settings) return false;
    const auto mods = index_folder(folder);
    for (const auto& [mod, list] : p.settings) {
        const FolderMod* m = look(mods, "emf", mod);
        if (!m) continue;
        for (const auto& s : list) {
            const FolderSetting* here = setting_here(*m, s.key);
            if (!here) continue;
            const auto want = normalised(*here, s.value);
            if (want && *want != here->value) return true;  // a value the mod does not take cannot be set: no say
        }
    }
    return false;
}

ApplyReport apply_profile(const Profile& p, const std::vector<FolderMod>& folder, ProfileSwitcher& switcher) {
    ApplyReport r;
    r.profile = p.name;
    const auto mods = index_folder(folder);
    for (const auto& e : p.mods) {
        const FolderMod* m = look(mods, e.source, e.id);
        ApplyItem item{e.source, m ? m->id : e.id, m && !m->name.empty() ? m->name : e.name, e.nexus_id, e.on, {}};
        if (!m) {
            r.missing.push_back(item);
            continue;
        }
        if (item.nexus_id.empty()) item.nexus_id = m->nexus_id;
        if (m->on == e.on) {
            r.unchanged.push_back(item);
            continue;
        }
        if (e.read_only) {  // its own loader switched it where the profile was saved: never EMF's to switch
            item.why = "recorded read-only where the profile was saved";
            r.recorded_read_only.push_back(item);
            continue;
        }
        if (!m->can_switch) {
            item.why = m->why_not;
            r.read_only.push_back(item);
            continue;
        }
        const ProfileSwitcher::Result done = switcher.set_enabled(m->source, m->id, e.on);
        if (done.ok) {
            r.switched.push_back(item);
        } else {
            item.why = done.error;
            r.failed.push_back(item);
        }
    }
    const auto entries = index_profile(p);
    for (const auto& m : folder)
        if (m.can_switch && !look(entries, m.source, m.id))
            r.not_in_profile.push_back({m.source, m.id, m.name, m.nexus_id, m.on, {}});
    if (p.with_settings)
        for (const auto& [mod, list] : p.settings) {
            const FolderMod* m = look(mods, "emf", mod);
            for (const auto& s : list) {
                ApplyItem item{"emf", mod + "." + s.key, m ? m->name : mod, {}, true, {}};
                const FolderSetting* here = m ? setting_here(*m, s.key) : nullptr;
                if (!here) {
                    item.why = m ? "the mod has no setting \"" + s.key + "\" now" : "the mod is not in this folder";
                    r.settings_skipped.push_back(item);
                    continue;
                }
                const auto want = normalised(*here, s.value);
                if (want && *want == here->value) {
                    ++r.settings_unchanged;
                    continue;
                }
                const ProfileSwitcher::Result done = switcher.set_setting(m->id, s.key, s.value);
                if (done.ok && done.unchanged) {
                    ++r.settings_unchanged;
                } else if (done.ok) {
                    ++r.settings_set;
                } else {
                    item.why = done.error;
                    r.settings_failed.push_back(item);
                }
            }
        }
    return r;
}

std::vector<std::string> apply_report_lines(const ApplyReport& r) {
    std::vector<std::string> out;
    std::string head = r.profile + ": " + plural(r.switched.size(), "mod switched", "mods switched") + ", " +
                       std::to_string(r.unchanged.size()) + " already so";
    if (!r.failed.empty()) head += ", " + plural(r.failed.size(), "failed", "failed");
    if (!r.missing.empty()) head += ", " + std::to_string(r.missing.size()) + " not in this folder";
    out.push_back(head);
    std::string on, off;
    for (const auto& i : r.switched) (i.on ? on : off) += ((i.on ? on : off).empty() ? "" : ", ") + shown(i);
    if (!on.empty()) out.push_back("  switched on: " + on);
    if (!off.empty()) out.push_back("  switched off: " + off);
    for (const auto& i : r.failed)
        out.push_back("  FAILED " + std::string(i.on ? "on" : "off") + ": " + shown(i) + " (" + i.why + ")");
    for (const auto& i : r.missing)
        out.push_back("  not in this folder: " + shown(i) + (i.nexus_id.empty() ? "" : " (Nexus " + i.nexus_id + ")"));
    for (const auto& i : r.read_only)
        out.push_back("  read-only here, left " + std::string(i.on ? "off" : "on") + ": " + shown(i) +
                      (i.why.empty() ? "" : " (" + i.why + ")"));
    for (const auto& i : r.recorded_read_only)
        out.push_back("  recorded read-only, not switched (left " + std::string(i.on ? "off" : "on") + "): " + shown(i));
    if (!r.not_in_profile.empty()) out.push_back("  not in this profile, left as they are: " + join_names(r.not_in_profile));
    if (r.settings_set || r.settings_unchanged || !r.settings_failed.empty() || !r.settings_skipped.empty()) {
        out.push_back("  " + plural(r.settings_set, "setting set", "settings set") + ", " + std::to_string(r.settings_unchanged) +
                      " already so");
        for (const auto& i : r.settings_failed) out.push_back("  setting FAILED: " + i.id + " (" + i.why + ")");
        for (const auto& i : r.settings_skipped) out.push_back("  setting skipped: " + i.id + " (" + i.why + ")");
    }
    return out;
}

// ---- the store ----

std::vector<ProfileInfo> ProfileStore::list() const {
    std::vector<ProfileInfo> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir_ + L"\\*.json").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring file = fd.cFileName;
        if (file.size() <= 5 || _wcsicmp(file.c_str() + file.size() - 5, L".json") != 0) continue;  // "*.json" also matches "x.jsonx"
        const std::string stem = narrow(file.substr(0, file.size() - 5));
        const std::string slug = lower(stem);
        const std::wstring path = dir_ + L"\\" + file;
        const uint64_t size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        {
            std::lock_guard<std::mutex> l(g_cache_m);
            auto it = g_cache.find(path);
            if (it != g_cache.end() && it->second.size == size && CompareFileTime(&it->second.time, &fd.ftLastWriteTime) == 0) {
                out.push_back(it->second.info);
                continue;
            }
        }
        ProfileInfo info;
        info.slug = valid_profile_slug(slug) ? slug : stem;
        info.name = info.slug;
        const ProfileRead r = valid_profile_slug(slug) ? read(slug)
                                                       : ProfileRead{std::nullopt, "EMF did not make this file name (not a profile's)."};
        if (r.profile) {
            info.name = r.profile->name;
            info.created = r.profile->created;
            info.emf_version = r.profile->emf_version;
            info.mods = r.profile->mods.size();
            info.with_settings = r.profile->with_settings;
        } else {
            info.error = r.error;
        }
        {
            std::lock_guard<std::mutex> l(g_cache_m);
            if (g_cache.size() > 1000) g_cache.clear();  // many folders in one session: start again
            g_cache[path] = CachedInfo{size, fd.ftLastWriteTime, info};
        }
        out.push_back(std::move(info));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const ProfileInfo& a, const ProfileInfo& b) {
        const std::string la = lower(a.name), lb = lower(b.name);
        return la != lb ? la < lb : a.slug < b.slug;
    });
    return out;
}

ProfileRead ProfileStore::read(const std::string& slug) const {
    if (!valid_profile_slug(slug)) return {std::nullopt, "\"" + slug + "\" is not a profile."};
    std::string text;
    if (!read_whole(dir_ + L"\\" + wide(slug) + L".json", text, kProfileFileMax + 1))
        return {std::nullopt, "There is no profile \"" + slug + "\"."};
    ++g_files_read;
    return profile_from_json(text);
}

std::optional<std::string> ProfileStore::find(const std::string& raw) const {
    const std::string name = lower(trim_profile_name(raw));
    if (name.empty()) return std::nullopt;
    const auto all = list();
    for (const auto& i : all)
        if (i.error.empty() && lower(i.name) == name) return i.slug;
    for (const auto& i : all)
        if (i.error.empty() && i.slug == name) return i.slug;
    return std::nullopt;
}

ProfileSave ProfileStore::save(const Profile& p, bool replace) const {
    ProfileSave s;
    if (const std::string why = profile_name_problem(p.name); !why.empty()) {
        s.error = why;
        return s;
    }
    s.slug = profile_slug(p.name);
    const std::wstring path = dir_ + L"\\" + wide(s.slug) + L".json";
    if (!replace && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const ProfileRead old = read(s.slug);
        s.exists = true;
        s.existing_name = old.profile ? old.profile->name : s.slug;
        s.error = "A profile named \"" + s.existing_name + "\" exists already.";
        return s;
    }
    if (!make_dirs(dir_)) {
        s.error = "Could not make the folder " + narrow(dir_) + ": " + error_text(GetLastError());
        return s;
    }
    Profile out = p;
    out.name = trim_profile_name(p.name);
    std::string why;
    if (!write_whole(path, profile_to_json(out), &why)) {
        s.error = "Could not write the profile " + s.slug + ".json: " + why;
        return s;
    }
    s.ok = true;
    return s;
}

ProfileSave ProfileStore::rename(const std::string& slug, const std::string& new_name) const {
    ProfileSave s;
    ProfileRead r = read(slug);
    if (!r.profile) {
        s.error = r.error;
        return s;
    }
    if (const std::string why = profile_name_problem(new_name); !why.empty()) {
        s.error = why;
        return s;
    }
    const bool was_current = current() == slug;
    Profile p = std::move(*r.profile);
    p.name = trim_profile_name(new_name);
    const std::string to = profile_slug(p.name);
    if (to != slug) {
        s = save(p, false);  // never onto another profile
        if (!s.ok) {
            if (s.exists) s.error = "A profile named \"" + s.existing_name + "\" exists already: choose another name.";
            return s;
        }
        if (!DeleteFileW((dir_ + L"\\" + wide(slug) + L".json").c_str())) {
            // the new file is there; keep the old one too rather than lose anything
            s.error = "Renamed, but the old file " + slug + ".json could not be removed: " + error_text(GetLastError());
        }
        if (was_current) set_current(to);
        return s;
    }
    return save(p, true);
}

bool ProfileStore::remove(const std::string& slug, std::string* error) const {
    if (!valid_profile_slug(slug) && !plain_stem(slug)) {
        if (error) *error = "\"" + slug + "\" is not a profile.";
        return false;
    }
    const bool was_current = current() == slug;
    if (!DeleteFileW((dir_ + L"\\" + wide(slug) + L".json").c_str())) {
        if (error) *error = "Could not delete the profile " + slug + ".json: " + error_text(GetLastError());
        return false;
    }
    if (was_current) DeleteFileW((dir_ + L"\\current.txt").c_str());
    return true;
}

std::string ProfileStore::current() const {
    std::string text;
    if (!read_whole(dir_ + L"\\current.txt", text, 256)) return {};
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    if (!valid_profile_slug(text)) return {};
    if (GetFileAttributesW((dir_ + L"\\" + wide(text) + L".json").c_str()) == INVALID_FILE_ATTRIBUTES) return {};
    return text;
}

bool ProfileStore::set_current(const std::string& slug) const {
    if (!valid_profile_slug(slug) || !make_dirs(dir_)) return false;
    return write_whole(dir_ + L"\\current.txt", slug + "\r\n", nullptr);
}

}  // namespace emf::core
