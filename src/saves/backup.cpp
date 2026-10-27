#include "saves/backup.h"

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>
#include <set>

#include "core/json.h"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

namespace emf::saves {

namespace {

namespace fs = std::filesystem;
using core::json::Value;
using VT = Value::Type;

std::wstring wide(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool iequal(const std::wstring& a, const std::wstring& b) { return a.size() == b.size() && _wcsicmp(a.c_str(), b.c_str()) == 0; }

bool all_digits(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

int64_t clock_now(const std::function<int64_t()>& now) { return now ? now() : static_cast<int64_t>(std::time(nullptr)); }

int64_t filetime_unix(const FILETIME& ft) {
    ULARGE_INTEGER u{{ft.dwLowDateTime, ft.dwHighDateTime}};
    if (u.QuadPart < 116444736000000000ull) return 0;
    return static_cast<int64_t>((u.QuadPart - 116444736000000000ull) / 10000000ull);
}

std::wstring win_path(std::string_view rel) {
    std::wstring w = wide(rel);
    std::replace(w.begin(), w.end(), L'/', L'\\');
    return w;
}

// ---- SHA-256 (Windows CNG), streaming ----
class Sha256 {
public:
    Sha256() {
        if (BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) alg_ = nullptr;
        if (alg_ && BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0) != 0) hash_ = nullptr;
    }
    ~Sha256() {
        if (hash_) BCryptDestroyHash(hash_);
        if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
    }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void update(const void* p, size_t n) {
        if (hash_ && n) BCryptHashData(hash_, static_cast<PUCHAR>(const_cast<void*>(p)), static_cast<ULONG>(n), 0);
    }
    std::string hex() {
        unsigned char d[32];
        if (!hash_ || BCryptFinishHash(hash_, d, sizeof d, 0) != 0) return {};
        static const char* digits = "0123456789abcdef";
        std::string s;
        for (unsigned char c : d) {
            s += digits[c >> 4];
            s += digits[c & 15];
        }
        return s;
    }

private:
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
};

// Every read of a save file opens it with CreateFileW and all three share flags, never through the C runtime or a
// stream (their defaults deny writing): the game, or Steam, may write the file at that moment, and a write that fails
// could bring up the game's "corrupt save" question. Windows still refuses to replace (rename over) a file while any
// handle to it is open, even with FILE_SHARE_DELETE, so each handle is held only as long as one read takes: a save is
// read into memory in one go and closed before anything else happens (copy_checked). A file that changes meanwhile is
// noticed by its size and time and copied again, or named as a problem.
constexpr DWORD kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

// The file's SHA-256 ("" when it cannot be read). Opened with every share flag: the game may write it meanwhile.
std::string file_sha256(const std::wstring& path, uint64_t* size = nullptr) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, kShareAll, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    Sha256 sha;
    std::vector<char> buf(1 << 16);
    DWORD got = 0;
    uint64_t total = 0;
    bool ok = true;
    while ((ok = ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) != 0) && got > 0) {
        sha.update(buf.data(), got);
        total += got;
    }
    CloseHandle(h);
    if (!ok) return {};
    if (size) *size = total;
    return sha.hex();
}

bool read_small(const std::wstring& path, std::string& out, size_t max = 8u << 20) {
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, kShareAll, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[65536];
    DWORD got = 0;
    bool ok = true;
    while (out.size() <= max && (ok = ReadFile(h, buf, sizeof buf, &got, nullptr) != 0) && got > 0) out.append(buf, got);
    CloseHandle(h);
    return ok && out.size() <= max;
}

bool write_whole(const std::wstring& path, const std::string& bytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    const bool ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &put, nullptr) && put == bytes.size() &&
                    FlushFileBuffers(h);
    CloseHandle(h);
    return ok;
}

std::string win_error(DWORD e) {
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, e, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring w = n && buf ? std::wstring(buf, n) : std::wstring();
    if (buf) LocalFree(buf);
    while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' || w.back() == L' ' || w.back() == L'.')) w.pop_back();
    return (w.empty() ? std::string("error") : utf8(w)) + " (Windows error " + std::to_string(e) + ")";
}

// ---- which files are saves ----
struct SaveName {
    std::string type;
    int slot = -1;
};
// A save file name the game uses in a storage folder ("character3.ksd", "world0.ksd", "global.ksd",
// "user_settings.json"); local_settings.json lives one folder up and is matched apart.
std::optional<SaveName> save_name(const std::string& name) {
    const std::string n = lower(name);
    if (n == "global.ksd") return SaveName{"global", -1};
    if (n == "user_settings.json") return SaveName{"user_settings", -1};
    for (const char* type : {"character", "world"}) {
        const std::string t = type;
        if (n.size() > t.size() + 4 && n.compare(0, t.size(), t) == 0 && n.compare(n.size() - 4, 4, ".ksd") == 0) {
            const std::string digits = n.substr(t.size(), n.size() - t.size() - 4);
            if (digits.size() <= 3 && all_digits(digits)) return SaveName{t, std::stoi(digits)};
        }
    }
    return std::nullopt;
}

int type_order(const std::string& t) {
    return t == "character" ? 0 : t == "world" ? 1 : t == "global" ? 2 : t == "user_settings" ? 3 : 4;
}

bool file_facts(const std::wstring& path, uint64_t* size, int64_t* modified) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return false;
    *size = (static_cast<uint64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    *modified = filetime_unix(a.ftLastWriteTime);
    return true;
}

std::function<bool(const std::wstring&, size_t)> g_list_fault;  // tests: a listing error at an entry

// A folder that is there but cannot be read (to its end) is an error: the save list would be short without anyone
// noticing. A folder that is not there is none.
bool list_failed(const std::wstring& dir, const std::error_code& ec, std::string* error) {
    if (!ec) return false;
    if (ec == std::errc::no_such_file_or_directory) return false;
    const DWORD a = GetFileAttributesW(dir.c_str());
    if (a == INVALID_FILE_ATTRIBUTES && (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND)) return false;
    if (error && error->empty()) error->assign("EMF could not list the save folder " + display_path(dir) + " (" + ec.message() + ")");
    return true;
}

bool storage_files(const std::wstring& dir, const std::string& storage, const std::string& steam_id,
                   const std::string& prefix, std::vector<SaveFile>& out, std::string* error) {
    std::vector<SaveFile> found;
    std::error_code ec;
    size_t index = 0;
    fs::directory_iterator it(fs::path(dir), ec), end;
    for (; !ec && it != end; it.increment(ec), ++index) {
        if (g_list_fault && g_list_fault(dir, index)) {
            ec = std::make_error_code(std::errc::io_error);
            break;
        }
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        const std::string name = utf8(it->path().filename().wstring());
        const auto sn = save_name(name);
        if (!sn) continue;
        SaveFile f;
        f.storage = storage;
        f.steam_id = steam_id;
        f.type = sn->type;
        f.slot = sn->slot;
        f.name = name;
        f.path = prefix + name;
        f.source = it->path().wstring();
        if (!file_facts(f.source, &f.size, &f.modified)) continue;
        found.push_back(std::move(f));
    }
    if (list_failed(dir, ec, error)) return false;
    std::sort(found.begin(), found.end(), [](const SaveFile& a, const SaveFile& b) {
        if (type_order(a.type) != type_order(b.type)) return type_order(a.type) < type_order(b.type);
        if (a.slot != b.slot) return a.slot < b.slot;
        return lower(a.name) < lower(b.name);
    });
    for (auto& f : found) out.push_back(std::move(f));
    return true;
}

std::vector<std::string> steam_ids(const SaveRoots& roots, std::string* error = nullptr) {
    std::vector<std::string> ids;
    if (roots.steam.empty()) return ids;
    std::error_code ec;
    const fs::path userdata = fs::path(roots.steam) / L"userdata";
    size_t index = 0;
    for (fs::directory_iterator it(userdata, ec), end; !ec && it != end; it.increment(ec), ++index) {
        if (g_list_fault && g_list_fault(userdata.wstring(), index)) {
            ec = std::make_error_code(std::errc::io_error);
            break;
        }
        std::error_code e2;
        const std::string id = utf8(it->path().filename().wstring());
        if (it->is_directory(e2) && all_digits(id) && fs::is_directory(it->path() / L"1203620" / L"remote", e2)) ids.push_back(id);
    }
    list_failed(userdata.wstring(), ec, error);
    std::sort(ids.begin(), ids.end(), [](const std::string& a, const std::string& b) {
        return a.size() != b.size() ? a.size() < b.size() : a < b;
    });
    return ids;
}

std::wstring remote_dir(const SaveRoots& roots, const std::string& id) {
    return roots.steam + L"\\userdata\\" + wide(id) + L"\\1203620\\remote";
}

// ---- a lock shared by every EMF process that writes into the same store ----
std::atomic<unsigned> g_lock_timeout_ms{0};  // tests; 0: two minutes
constexpr const char* kLockBusy =
    "Another EMF program (EMF.exe, the game or the installer) is working on the save backups right now. Try again in a "
    "moment.";

// Held while a backup, a restore, a delete or a prune writes into the store. When another EMF program holds it past
// the timeout, ok() is false and the caller refuses: it never goes on without the lock.
class StoreLock {
public:
    explicit StoreLock(const std::wstring& store) {
        m_ = CreateMutexW(nullptr, FALSE, store_lock_name(store).c_str());
        if (m_) {
            const unsigned t = g_lock_timeout_ms.load();
            const DWORD w = WaitForSingleObject(m_, t ? t : 120000);
            owned_ = w == WAIT_OBJECT_0 || w == WAIT_ABANDONED;
        }
    }
    ~StoreLock() {
        if (owned_) ReleaseMutex(m_);
        if (m_) CloseHandle(m_);
    }
    bool ok() const { return owned_; }
    StoreLock(const StoreLock&) = delete;
    StoreLock& operator=(const StoreLock&) = delete;

private:
    HANDLE m_ = nullptr;
    bool owned_ = false;
};

// Removes a folder and what is in it, without ever following a junction or a symbolic link inside it (the link itself
// goes, never what it points at). True when nothing is left.
bool remove_tree(const std::wstring& dir) {
    const DWORD a = GetFileAttributesW(dir.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return true;
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) return (a & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(dir.c_str()) != 0 : DeleteFileW(dir.c_str()) != 0;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_NORMAL);
        return DeleteFileW(dir.c_str()) != 0;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring p = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(p.c_str());
                else DeleteFileW(p.c_str());
            } else {
                remove_tree(p);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_DIRECTORY);
    RemoveDirectoryW(dir.c_str());
    return GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES;
}

// ---- JSON out ----
size_t utf8_len(const std::string& s, size_t i) {
    const auto b = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = b(i);
    const size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    if (n == 0 || i + n > s.size()) return 0;
    for (size_t k = 1; k < n; ++k)
        if ((b(i + k) & 0xC0) != 0x80) return 0;
    if (n == 2 && c < 0xC2) return 0;
    if (n == 3 && c == 0xE0 && b(i + 1) < 0xA0) return 0;
    if (n == 3 && c == 0xED && b(i + 1) >= 0xA0) return 0;
    if (n == 4 && (c > 0xF4 || (c == 0xF0 && b(i + 1) < 0x90) || (c == 0xF4 && b(i + 1) >= 0x90))) return 0;
    return n;
}

std::string quote(const std::string& s) {
    std::string out = "\"";
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0x80) {
            const size_t n = utf8_len(s, i);
            if (n == 0) {
                out += "\xEF\xBF\xBD";
                ++i;
            } else {
                out.append(s, i, n);
                i += n;
            }
            continue;
        }
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
        ++i;
    }
    return out + "\"";
}

std::string num(int64_t v) { return std::to_string(v); }

std::string slot_info_json(const SlotInfo& i, const char* indent) {
    std::string s = std::string(indent) + "{\"storage\": " + quote(i.storage);
    if (!i.steam_id.empty()) s += ", \"steam_id\": " + quote(i.steam_id);
    s += ", \"type\": " + quote(i.type) + ", \"slot\": " + num(i.slot) + ", \"name\": " + quote(i.name);
    if (i.level) s += ", \"level\": " + num(*i.level);
    if (i.seen) s += ", \"seen\": " + num(i.seen);
    if (i.last_loaded) s += ", \"last_loaded\": " + num(i.last_loaded);
    if (i.played) s += ", \"played\": " + num(i.played);
    if (i.play_seconds) s += ", \"play_seconds\": " + num(i.play_seconds);
    return s + "}";
}

// ---- JSON in ----
std::string jstr(const Value& v, const char* k) {
    const Value* x = v.get(k);
    return x && x->is(VT::String) ? x->s : std::string();
}
std::optional<int64_t> jint(const Value& v, const char* k) {
    const Value* x = v.get(k);
    if (!x || !x->is(VT::Number) || x->n != x->n || x->n < -9e15 || x->n > 9e15) return std::nullopt;
    return static_cast<int64_t>(x->n);
}

bool is_hex64(const std::string& s) {
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// "steam/<id>/<name>", "local/default/<name>" or "local/local_settings.json": parts without "." or "..", no
// backslashes, colons or empty parts.
bool safe_backup_path(const std::string& p, const std::string& storage) {
    if (p.empty() || p.find('\\') != std::string::npos || p.find(':') != std::string::npos) return false;
    std::vector<std::string> parts;
    size_t from = 0;
    while (true) {
        const size_t to = p.find('/', from);
        parts.push_back(p.substr(from, to == std::string::npos ? std::string::npos : to - from));
        if (to == std::string::npos) break;
        from = to + 1;
    }
    for (const auto& x : parts)
        if (x.empty() || x == "." || x == "..") return false;
    if (parts[0] != storage) return false;
    if (storage == "steam") return parts.size() == 3 && all_digits(parts[1]);
    if (storage == "local") return (parts.size() == 3 && parts[1] == "default") || (parts.size() == 2 && parts[1] == "local_settings.json");
    return false;
}

std::optional<SlotInfo> slot_info_from(const Value& v) {
    if (!v.is(VT::Object)) return std::nullopt;
    SlotInfo i;
    i.storage = jstr(v, "storage");
    i.steam_id = jstr(v, "steam_id");
    if (!i.steam_id.empty() && (i.storage != "steam" || !all_digits(i.steam_id))) return std::nullopt;
    i.type = jstr(v, "type");
    i.name = jstr(v, "name");
    const auto slot = jint(v, "slot");
    if ((i.storage != "steam" && i.storage != "local") || (i.type != "character" && i.type != "world") || !slot || *slot < 0 ||
        *slot > 999)
        return std::nullopt;
    i.slot = static_cast<int>(*slot);
    if (const auto lv = jint(v, "level"); lv && *lv >= 0 && *lv < 100000) i.level = static_cast<int>(*lv);
    i.seen = jint(v, "seen").value_or(0);
    i.last_loaded = jint(v, "last_loaded").value_or(0);
    i.played = jint(v, "played").value_or(0);
    i.play_seconds = jint(v, "play_seconds").value_or(0);
    return i;
}

// ---- backup folders ----
bool id_ok(const std::string& id) {
    if (id.empty() || id.size() > 120 || id[0] == '.' || id == "..") return false;
    for (char c : id)
        if (c == '/' || c == '\\' || c == ':' || static_cast<unsigned char>(c) < 0x20 || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|')
            return false;
    return true;
}

// <yyyy-MM-dd_HH-mm-ss>_<kind>[_<n>]
bool backup_like_name(const std::string& id) {
    if (id.size() < 21) return false;
    const std::string t = id.substr(0, 19);
    for (size_t i = 0; i < t.size(); ++i) {
        const char c = t[i];
        const bool sep = i == 4 || i == 7 || i == 13 || i == 16;
        if (i == 10) {
            if (c != '_') return false;
        } else if (sep) {
            if (c != '-') return false;
        } else if (c < '0' || c > '9') {
            return false;
        }
    }
    if (id[19] != '_') return false;
    std::string rest = id.substr(20);
    const size_t us = rest.rfind('_');
    if (us != std::string::npos && all_digits(rest.substr(us + 1))) rest = rest.substr(0, us);
    return valid_kind(rest);
}

std::string folder_time_text(int64_t t) {
    const std::time_t tt = static_cast<std::time_t>(t);
    std::tm tm{};
    localtime_s(&tm, &tt);
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%d_%H-%M-%S", &tm);
    return b;
}

bool same_files(const std::vector<SaveFile>& now, const std::vector<std::string>& hashes, const Backup& b) {
    if (b.damaged || now.size() != b.files.size()) return false;
    std::map<std::string, std::string> have;
    for (const auto& f : b.files) have[lower(f.path)] = f.sha256;
    for (size_t i = 0; i < now.size(); ++i) {
        const auto it = have.find(lower(now[i].path));
        if (it == have.end() || hashes[i].empty() || it->second != hashes[i]) return false;
    }
    return true;
}

std::wstring reg_string(HKEY root, const wchar_t* key, const wchar_t* value) {
    wchar_t buf[1024];
    DWORD size = sizeof buf;
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    std::wstring s(buf);
    std::replace(s.begin(), s.end(), L'/', L'\\');
    return s;
}

}  // namespace

// ---- roots ----
SaveRoots with_app_data(SaveRoots roots, const std::wstring& app_data) {
    if (app_data.empty()) {
        roots.store.clear();
        roots.info.clear();
    } else {
        roots.store = app_data + L"\\save-backups";
        roots.info = app_data + L"\\save-info.json";
    }
    return roots;
}

SaveRoots roots_from(const std::function<std::wstring(const wchar_t*)>& env, bool test, const RealPlaces& real) {
    test = test || !env(L"EMF_TEST_NO_STEAM").empty();
    SaveRoots r;
    r.steam = env(L"EMF_TEST_STEAM_ROOT");
    if (r.steam.empty() && !test && real.steam) r.steam = real.steam();
    std::wstring saved = env(L"EMF_TEST_SAVED_GAMES");
    if (saved.empty() && !test && real.saved_games) saved = real.saved_games();
    while (!saved.empty() && (saved.back() == L'\\' || saved.back() == L'/')) saved.pop_back();
    if (!saved.empty()) r.local = saved + L"\\keen games\\Enshrouded";
    while (!r.steam.empty() && (r.steam.back() == L'\\' || r.steam.back() == L'/')) r.steam.pop_back();
    return r;
}

SaveRoots default_roots(bool test) {
    auto env = [](const wchar_t* name) {
        wchar_t v[1024] = {};
        const DWORD n = GetEnvironmentVariableW(name, v, 1024);
        return n > 0 && n < 1024 ? std::wstring(v, n) : std::wstring();
    };
    RealPlaces real;
    real.steam = [] {
        std::wstring s = reg_string(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
        if (s.empty()) s = reg_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
        return s;
    };
    real.saved_games = [] {
        PWSTR p = nullptr;
        std::wstring dir;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SavedGames, 0, nullptr, &p)) && p) dir = p;
        CoTaskMemFree(p);
        return dir;
    };
    return roots_from(env, test, real);
}

std::wstring default_app_data() {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)) && p && *p) dir = std::wstring(p) + L"\\EMF";
    CoTaskMemFree(p);
    return dir;
}

// ---- finding saves ----
std::vector<SaveFile> find_saves(const SaveRoots& roots, std::string* error) {
    std::string why;
    std::vector<SaveFile> out;
    for (const auto& id : steam_ids(roots, &why)) storage_files(remote_dir(roots, id), "steam", id, "steam/" + id + "/", out, &why);
    if (!roots.local.empty()) {
        storage_files(roots.local + L"\\default", "local", "", "local/default/", out, &why);
        SaveFile f;
        f.storage = "local";
        f.type = "local_settings";
        f.name = "local_settings.json";
        f.path = "local/local_settings.json";
        f.source = roots.local + L"\\local_settings.json";
        if (file_facts(f.source, &f.size, &f.modified)) out.push_back(std::move(f));
    }
    if (error) *error = why;
    return out;
}

std::vector<std::string> steam_users_with_saves(const SaveRoots& roots) {
    std::vector<std::string> ids;
    for (const auto& id : steam_ids(roots)) {
        std::vector<SaveFile> f;
        storage_files(remote_dir(roots, id), "steam", id, "", f, nullptr);
        if (!f.empty()) ids.push_back(id);
    }
    return ids;
}

// ---- the manifest ----
uint64_t Backup::bytes() const {
    uint64_t n = 0;
    for (const auto& f : files) n += f.size;
    return n;
}

bool valid_kind(std::string_view kind) {
    for (const char* k : kKinds)
        if (kind == k) return true;
    return false;
}

std::string manifest_json(const Backup& b) {
    std::string s = "{\n";
    s += "  \"format\": " + num(b.format > 0 ? b.format : kManifestFormat) + ",\n";
    s += "  \"kind\": " + quote(b.kind) + ",\n";
    s += "  \"label\": " + quote(b.label) + ",\n";
    s += "  \"time\": " + num(b.time) + ",\n";
    s += "  \"time_local\": " + quote(b.time_local) + ",\n";
    s += "  \"emf_version\": " + quote(b.emf_version) + ",\n";
    s += "  \"made_by\": " + quote(b.made_by) + ",\n";
    s += "  \"files\": [";
    for (size_t i = 0; i < b.files.size(); ++i) {
        const auto& f = b.files[i];
        s += std::string(i ? "," : "") + "\n    {\"storage\": " + quote(f.storage);
        if (!f.steam_id.empty()) s += ", \"steam_id\": " + quote(f.steam_id);
        s += ", \"type\": " + quote(f.type) + ", \"slot\": " + num(f.slot) + ", \"name\": " + quote(f.name) +
             ", \"path\": " + quote(f.path) + ", \"size\": " + num(static_cast<int64_t>(f.size)) +
             ", \"sha256\": " + quote(f.sha256) + ", \"modified\": " + num(f.modified) + "}";
    }
    s += b.files.empty() ? "],\n" : "\n  ],\n";
    s += "  \"info\": [";
    for (size_t i = 0; i < b.info.size(); ++i) s += std::string(i ? "," : "") + "\n" + slot_info_json(b.info[i], "    ");
    s += b.info.empty() ? "],\n" : "\n  ],\n";
    s += "  \"problems\": [";
    for (size_t i = 0; i < b.problems.size(); ++i) s += std::string(i ? ", " : "") + quote(b.problems[i]);
    s += "]\n}\n";
    return s;
}

std::optional<Backup> parse_manifest(std::string_view text, std::string* error) {
    auto fail = [error](const std::string& why) -> std::optional<Backup> {
        if (error) *error = why;
        return std::nullopt;
    };
    const auto parsed = core::json::parse(text);
    if (!parsed.value) return fail("backup.json is not valid JSON: " + parsed.error);
    const Value& v = *parsed.value;
    if (!v.is(VT::Object)) return fail("backup.json is not a JSON object");
    Backup b;
    const auto format = jint(v, "format");
    if (!format || *format < 1) return fail("backup.json has no format number");
    b.format = static_cast<int>(*format);
    b.kind = jstr(v, "kind");
    if (!valid_kind(b.kind)) return fail("backup.json names no kind EMF knows (\"" + b.kind + "\")");
    const auto time = jint(v, "time");
    if (!time) return fail("backup.json has no time");
    b.time = *time;
    b.label = jstr(v, "label");
    b.time_local = jstr(v, "time_local");
    b.emf_version = jstr(v, "emf_version");
    b.made_by = jstr(v, "made_by");
    const Value* files = v.get("files");
    if (!files || !files->is(VT::Array)) return fail("backup.json lists no files");
    for (const Value& x : files->arr) {
        if (!x.is(VT::Object)) return fail("backup.json has a file entry that is not an object");
        BackupFile f;
        f.storage = jstr(x, "storage");
        f.steam_id = jstr(x, "steam_id");
        f.type = jstr(x, "type");
        f.name = jstr(x, "name");
        f.path = jstr(x, "path");
        f.sha256 = lower(jstr(x, "sha256"));
        f.slot = static_cast<int>(jint(x, "slot").value_or(-1));
        const auto size = jint(x, "size");
        f.modified = jint(x, "modified").value_or(0);
        if (!safe_backup_path(f.path, f.storage)) return fail("backup.json has a file path EMF does not accept: " + f.path);
        if (f.name.empty() || f.path.size() < f.name.size() || lower(f.path.substr(f.path.size() - f.name.size())) != lower(f.name))
            return fail("backup.json has a file whose name does not match its path: " + f.path);
        if (!size || *size < 0) return fail("backup.json has a file without its size: " + f.path);
        f.size = static_cast<uint64_t>(*size);
        if (!is_hex64(f.sha256)) return fail("backup.json has a file without its SHA-256: " + f.path);
        b.files.push_back(std::move(f));
    }
    if (const Value* info = v.get("info"); info && info->is(VT::Array))
        for (const Value& x : info->arr)
            if (auto i = slot_info_from(x)) b.info.push_back(std::move(*i));
    if (const Value* p = v.get("problems"); p && p->is(VT::Array))
        for (const Value& x : p->arr)
            if (x.is(VT::String)) b.problems.push_back(x.s);
    return b;
}

// ---- labels ----
std::string storage_label(const std::string& storage) { return storage == "steam" ? "Steam" : storage == "local" ? "Local" : storage; }

std::string file_label(const Backup& b, const BackupFile& f) {
    if (f.type != "character" && f.type != "world") return f.name;
    for (const auto& i : b.info) {
        if (i.storage != f.storage || i.type != f.type || i.slot != f.slot || i.name.empty()) continue;
        if (f.storage == "steam" && !i.steam_id.empty() && i.steam_id != f.steam_id) continue;  // another account's
        if (f.type == "world") return "world " + i.name;
        return i.name + (i.level ? " (level " + std::to_string(*i.level) + ")" : "");
    }
    return f.type + " slot " + std::to_string(f.slot + 1);
}

std::string backup_summary(const Backup& b) {
    std::string out;
    for (const char* storage : {"steam", "local"}) {
        std::vector<const BackupFile*> mine;
        for (const auto& f : b.files)
            if (f.storage == storage && (f.type == "character" || f.type == "world")) mine.push_back(&f);
        if (mine.empty()) continue;
        std::stable_sort(mine.begin(), mine.end(), [](const BackupFile* x, const BackupFile* y) {
            if (x->type != y->type) return x->type == "character";
            return x->slot < y->slot;
        });
        std::string part = storage_label(storage) + ": ";
        for (size_t i = 0; i < mine.size(); ++i) part += (i ? ", " : "") + file_label(b, *mine[i]);
        out += (out.empty() ? "" : " \xC2\xB7 ") + part;
    }
    return out;
}

std::string local_time_text(int64_t t) {
    const std::time_t tt = static_cast<std::time_t>(t);
    std::tm tm{};
    localtime_s(&tm, &tt);
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm);
    return b;
}

std::string size_text(uint64_t bytes) {
    auto one = [](double v, const char* unit) {
        char b[32];
        if (v < 10) {
            std::snprintf(b, sizeof b, "%.1f", v);
            std::string s = b;
            if (s.size() > 2 && s.compare(s.size() - 2, 2, ".0") == 0) s.resize(s.size() - 2);
            return s + " " + unit;
        }
        std::snprintf(b, sizeof b, "%.0f %s", v, unit);
        return std::string(b);
    };
    const double kb = bytes / 1024.0, mb = kb / 1024.0, gb = mb / 1024.0;
    if (gb >= 1) return one(gb, "GB");
    if (mb >= 1) return one(mb, "MB");
    return one(kb, "KB");
}

// ---- copying ----
namespace {

struct Copied {
    bool ok = false;
    std::string sha256, problem;
    uint64_t size = 0;
    int64_t modified = 0;
};

Copied copy_checked(const SaveFile& f, const std::wstring& dst, const BackupOptions& o) {
    Copied c;
    const std::string who = f.name + " (" + storage_label(f.storage) + ")";
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt) Sleep(static_cast<DWORD>(o.retry_ms > 0 ? o.retry_ms : 0));
        if (o.hooks.before_open) o.hooks.before_open(f.source, attempt);
        HANDLE in = CreateFileW(f.source.c_str(), GENERIC_READ, kShareAll, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (in == INVALID_HANDLE_VALUE) {
            const DWORD e = GetLastError();
            c.problem = who + (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND
                                   ? ": it was removed while EMF copied it"
                                   : ": it is in use by another program (" + win_error(e) + ")");
            continue;
        }
        // the whole file into memory with one read, and the handle closed at once (see kShareAll)
        BY_HANDLE_FILE_INFORMATION i1{}, i2{};
        GetFileInformationByHandle(in, &i1);
        if (o.hooks.opened) o.hooks.opened(f.source, attempt);
        const uint64_t size1 = (static_cast<uint64_t>(i1.nFileSizeHigh) << 32) | i1.nFileSizeLow;
        if (size1 > (1ull << 30)) {
            CloseHandle(in);
            c.problem = who + ": it is bigger than 1 GB, which is no Enshrouded save";
            break;
        }
        std::string data(static_cast<size_t>(size1) + 1, '\0');  // one byte more: a file that grew is noticed
        size_t total = 0;
        bool io = true;
        while (io && total < data.size()) {
            DWORD got = 0;
            io = ReadFile(in, data.data() + total, static_cast<DWORD>((std::min<size_t>)(data.size() - total, 1u << 30)), &got,
                          nullptr) != 0;
            if (got == 0) break;
            total += got;
        }
        GetFileInformationByHandle(in, &i2);
        CloseHandle(in);
        if (o.hooks.read_done) o.hooks.read_done(f.source, attempt);
        if (!io) {
            c.problem = who + ": reading it failed";
            continue;
        }
        const uint64_t size2 = (static_cast<uint64_t>(i2.nFileSizeHigh) << 32) | i2.nFileSizeLow;
        const bool same_time = CompareFileTime(&i1.ftLastWriteTime, &i2.ftLastWriteTime) == 0;
        WIN32_FILE_ATTRIBUTE_DATA now{};  // the path may name another file now (one renamed over it)
        const bool still = GetFileAttributesExW(f.source.c_str(), GetFileExInfoStandard, &now) &&
                           ((static_cast<uint64_t>(now.nFileSizeHigh) << 32) | now.nFileSizeLow) == size2 &&
                           CompareFileTime(&now.ftLastWriteTime, &i2.ftLastWriteTime) == 0;
        if (size1 != size2 || !same_time || total != size2 || !still) {
            c.problem = who + ": it changed while EMF copied it (the game was probably saving)";
            continue;
        }
        data.resize(total);
        Sha256 sha;
        sha.update(data.data(), data.size());
        const std::string hex = sha.hex();
        if (!write_whole(dst, data)) {
            c.problem = who + ": EMF could not write its copy (" + win_error(GetLastError()) + ")";
            continue;
        }
        uint64_t written = 0;
        if (hex.empty() || file_sha256(dst, &written) != hex || written != total) {
            c.problem = who + ": the copy did not match the file (a disk problem?)";
            continue;
        }
        c.ok = true;
        c.sha256 = hex;
        c.size = total;
        c.modified = filetime_unix(i2.ftLastWriteTime);
        c.problem.clear();
        return c;
    }
    DeleteFileW(dst.c_str());
    return c;
}

std::atomic<unsigned> g_tmp_counter{0};

}  // namespace

BackupResult make_backup(const SaveRoots& roots, const BackupOptions& o) {
    BackupResult r;
    if (roots.store.empty()) {
        r.error = "EMF has no folder for backups.";
        return r;
    }
    if (!valid_kind(o.kind)) {
        r.error = "EMF does not know the backup kind \"" + o.kind + "\".";
        return r;
    }
    std::string list_error;
    std::vector<SaveFile> files = find_saves(roots, &list_error);
    if (!list_error.empty()) {  // a short list is never backed up as if it were whole
        r.error = list_error + ". No backup was made.";
        return r;
    }
    if (!o.only.empty()) {
        std::vector<SaveFile> keep;
        for (auto& f : files)
            if (std::any_of(o.only.begin(), o.only.end(), [&](const std::wstring& p) { return iequal(p, f.source); }))
                keep.push_back(std::move(f));
        files = std::move(keep);
    }
    if (files.empty()) {
        r.nothing = true;
        r.error = "There are no save files to back up.";
        return r;
    }
    StoreLock lock(roots.store);
    if (!lock.ok()) {
        r.error = kLockBusy;
        return r;
    }
    std::error_code ec;
    fs::create_directories(fs::path(roots.store), ec);
    clean_stale_temp(roots.store);
    const int64_t now = clock_now(o.now);
    const std::wstring tmp = roots.store + L"\\.tmp-" + std::to_wstring(now) + L"-" + std::to_wstring(GetCurrentProcessId()) +
                             L"-" + std::to_wstring(++g_tmp_counter);
    remove_tree(tmp);
    if (!fs::create_directories(fs::path(tmp), ec)) {
        r.error = "EMF could not make a folder in " + display_path(roots.store) + ".";
        return r;
    }
    Backup b;
    b.kind = o.kind;
    b.label = o.label;
    b.made_by = o.made_by;
    b.emf_version = o.emf_version;
    b.time = now;
    b.time_local = local_time_text(now);
    std::set<std::string> ids;
    for (const auto& f : files) {
        const std::wstring dst = tmp + L"\\" + win_path(f.path);
        fs::create_directories(fs::path(dst).parent_path(), ec);
        const Copied c = copy_checked(f, dst, o);
        if (!c.ok) {
            b.problems.push_back(c.problem);
            continue;
        }
        BackupFile x;
        x.storage = f.storage;
        x.steam_id = f.steam_id;
        x.type = f.type;
        x.slot = f.slot;
        x.name = f.name;
        x.path = f.path;
        x.size = c.size;
        x.sha256 = c.sha256;
        x.modified = c.modified;
        if (f.storage == "steam") ids.insert(f.steam_id);
        b.files.push_back(std::move(x));
    }
    if (b.files.empty()) {
        remove_tree(tmp);
        r.error = "No save file could be copied:";
        for (const auto& p : b.problems) r.error += " " + p + ".";
        return r;
    }
    // the menu's names for the slots in this backup. A Steam name goes with the account it was seen for; one without an
    // account (an older save-info.json) only when a single Steam user's files are in the backup.
    for (const auto& i : o.info) {
        if (i.storage == "steam" && i.steam_id.empty() && ids.size() != 1) continue;
        const bool here = std::any_of(b.files.begin(), b.files.end(), [&](const BackupFile& f) {
            return f.storage == i.storage && f.type == i.type && f.slot == i.slot &&
                   (f.storage != "steam" || i.steam_id.empty() || i.steam_id == f.steam_id);
        });
        if (here) b.info.push_back(i);
    }
    if (!write_whole(tmp + L"\\backup.json", manifest_json(b))) {
        remove_tree(tmp);
        r.error = "EMF could not write backup.json in " + display_path(roots.store) + ".";
        return r;
    }
    const std::string base = folder_time_text(now) + "_" + o.kind;
    for (int n = 1; n < 1000; ++n) {
        const std::string id = n == 1 ? base : base + "_" + std::to_string(n);
        const std::wstring final_dir = roots.store + L"\\" + wide(id);
        if (GetFileAttributesW(final_dir.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
        if (MoveFileExW(tmp.c_str(), final_dir.c_str(), 0)) {
            b.id = id;
            r.ok = true;
            r.backup = std::move(b);
            return r;
        }
        const DWORD e = GetLastError();
        if (e != ERROR_ALREADY_EXISTS && e != ERROR_FILE_EXISTS) {
            remove_tree(tmp);
            r.error = "EMF could not put the backup in place: " + win_error(e);
            return r;
        }
    }
    remove_tree(tmp);
    r.error = "EMF could not find a free name for the backup.";
    return r;
}

// ---- the list ----
std::wstring backup_folder(const std::wstring& store, const std::string& id) {
    if (store.empty() || !id_ok(id)) return {};
    const std::wstring dir = store + L"\\" + wide(id);
    const DWORD a = GetFileAttributesW(dir.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT)) return {};
    if (!backup_like_name(id) && GetFileAttributesW((dir + L"\\backup.json").c_str()) == INVALID_FILE_ATTRIBUTES) return {};
    return dir;
}

std::optional<Backup> read_backup(const std::wstring& store, const std::string& id, std::string* error) {
    const std::wstring dir = backup_folder(store, id);
    if (dir.empty()) {
        if (error) *error = "There is no backup \"" + id + "\".";
        return std::nullopt;
    }
    std::string text;
    if (!read_small(dir + L"\\backup.json", text, 4u << 20)) {
        if (error) *error = "Its backup.json is missing or cannot be read.";
        return std::nullopt;
    }
    auto b = parse_manifest(text, error);
    if (b) b->id = id;
    return b;
}

std::vector<Backup> list_backups(const std::wstring& store) {
    std::vector<Backup> out;
    if (store.empty()) return out;
    std::error_code ec;
    for (fs::directory_iterator it(fs::path(store), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_directory(e2)) continue;
        const std::string id = utf8(it->path().filename().wstring());
        if (backup_folder(store, id).empty()) continue;
        WIN32_FILE_ATTRIBUTE_DATA a{};
        const bool facts = GetFileAttributesExW(it->path().c_str(), GetFileExInfoStandard, &a) != 0;
        const int64_t created = facts ? static_cast<int64_t>((static_cast<uint64_t>(a.ftCreationTime.dwHighDateTime) << 32) |
                                                             a.ftCreationTime.dwLowDateTime)
                                      : 0;
        std::string why;
        if (auto b = read_backup(store, id, &why)) {
            b->created = created;
            out.push_back(std::move(*b));
            continue;
        }
        Backup d;
        d.id = id;
        d.damaged = true;
        d.error = why;
        d.created = created;
        if (facts) d.time = filetime_unix(a.ftLastWriteTime);
        d.time_local = local_time_text(d.time);
        out.push_back(std::move(d));
    }
    std::sort(out.begin(), out.end(), [](const Backup& a, const Backup& b) {
        if (a.time != b.time) return a.time > b.time;
        if (a.created != b.created) return a.created > b.created;
        return a.id > b.id;
    });
    return out;
}

bool has_starter(const std::wstring& store) {
    for (const auto& b : list_backups(store))
        if (b.kind == "starter") return true;  // also one with problems: later chances make automatic backups
    return false;
}

Outcome delete_backup(const std::wstring& store, const std::string& id) {
    Outcome o;
    const std::wstring dir = backup_folder(store, id);
    if (dir.empty()) {
        o.error = "There is no backup \"" + id + "\".";
        return o;
    }
    StoreLock lock(store);
    if (!lock.ok()) {
        o.error = kLockBusy;
        return o;
    }
    if (!remove_tree(dir)) {
        o.error = "EMF could not delete all of " + display_path(dir) + ". Is a file in it open in another program?";
        return o;
    }
    o.ok = true;
    return o;
}

size_t prune_auto(const std::wstring& store, size_t keep) {
    if (keep < 1) keep = 1;
    StoreLock lock(store);
    if (!lock.ok()) return 0;
    // newest first; only complete automatic backups count toward `keep`, so the newest complete copies always stay;
    // an incomplete one goes only once it is older than the last complete one kept
    size_t complete = 0, deleted = 0;
    for (const auto& b : list_backups(store)) {
        if (b.damaged || b.kind != "auto") continue;
        if (complete < keep) {
            if (b.problems.empty()) ++complete;
            continue;
        }
        if (delete_backup(store, b.id).ok) ++deleted;
    }
    return deleted;
}

void clean_stale_temp(const std::wstring& store, int64_t max_age_s) {
    if (store.empty()) return;
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    std::error_code ec;
    std::vector<fs::path> old;
    for (fs::directory_iterator it(fs::path(store), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_directory(e2) || it->path().filename().wstring().rfind(L".tmp-", 0) != 0) continue;
        WIN32_FILE_ATTRIBUTE_DATA a{};
        if (!GetFileAttributesExW(it->path().c_str(), GetFileExInfoStandard, &a)) continue;
        if (now - filetime_unix(a.ftLastWriteTime) >= max_age_s) old.push_back(it->path());
    }
    for (const auto& p : old) remove_tree(p.wstring());
}

BackupResult ensure_starter(const SaveRoots& roots, BackupOptions o) {
    if (has_starter(roots.store)) {
        BackupResult r;
        r.ok = true;
        r.nothing = true;
        return r;
    }
    o.kind = "starter";
    o.label = kStarterLabel;
    BackupResult r = make_backup(roots, o);
    if (!r.ok && r.nothing) r.ok = true;  // no saves yet: nothing to do
    return r;
}

AutoSettings read_auto_settings(const std::wstring& ini) {
    AutoSettings s;
    s.on = GetPrivateProfileIntW(L"backup", L"on_start", 1, ini.c_str()) != 0;
    const int keep = static_cast<int>(GetPrivateProfileIntW(L"backup", L"keep", 10, ini.c_str()));
    s.keep = static_cast<size_t>(std::clamp(keep, 1, 100));
    const int minutes = static_cast<int>(GetPrivateProfileIntW(L"backup", L"min_minutes", 60, ini.c_str()));
    s.min_minutes = std::clamp(minutes, 0, 10080);
    return s;
}

AutoResult auto_backup(const SaveRoots& roots, const AutoSettings& s, BackupOptions base) {
    AutoResult a;
    if (!s.on) {
        a.text = "automatic save backups are off ([backup] on_start=0 in emf\\emf_core.ini)";
        return a;
    }
    if (roots.store.empty()) {
        a.what = AutoResult::What::Failed;
        a.text = "no folder for save backups";
        return a;
    }
    std::string list_error;
    const std::vector<SaveFile> files = find_saves(roots, &list_error);
    if (!list_error.empty()) {
        a.what = AutoResult::What::Failed;
        a.text = list_error;
        return a;
    }
    if (files.empty()) {
        a.text = "no save files yet; nothing to back up";
        return a;
    }
    if (!has_starter(roots.store)) {  // the very first one; once any starter exists, the rules below apply
        a.made = ensure_starter(roots, base);
        a.what = a.made.ok && !a.made.nothing ? AutoResult::What::Made : AutoResult::What::Failed;
        a.text = a.what == AutoResult::What::Made ? "made the starter backup (" + a.made.backup.id + ")"
                                                  : "the starter backup failed: " + a.made.error;
        return a;
    }
    const int64_t now = clock_now(base.now);
    const auto list = list_backups(roots.store);
    for (const auto& b : list) {
        if (b.damaged || b.kind != "auto") continue;
        const int64_t age = now - b.time;
        if (age >= 0 && age < s.min_minutes * 60) {
            a.text = "the last automatic backup is " + std::to_string(age / 60) + " minute(s) old (at most one every " +
                     std::to_string(s.min_minutes) + " minutes); no new backup";
            return a;
        }
        break;  // the newest automatic one decides
    }
    for (const auto& b : list) {
        if (b.damaged) continue;
        std::vector<std::string> hashes;
        for (const auto& f : files) hashes.push_back(file_sha256(f.source));
        if (same_files(files, hashes, b)) {
            a.text = "saves unchanged since " + (b.time_local.empty() ? local_time_text(b.time) : b.time_local) + "; no new backup";
            return a;
        }
        break;  // only the newest backup counts
    }
    base.kind = "auto";
    a.made = make_backup(roots, base);
    if (!a.made.ok) {
        a.what = AutoResult::What::Failed;
        a.text = "the automatic backup failed: " + a.made.error;
        return a;
    }
    a.what = AutoResult::What::Made;
    // a backup with problems is no reason to let an older complete one go
    if (a.made.backup.problems.empty()) a.pruned = prune_auto(roots.store, s.keep);
    a.text = "made " + a.made.backup.id + " (" + std::to_string(a.made.backup.files.size()) + " files, " +
             size_text(a.made.backup.bytes()) + ")";
    if (a.pruned) a.text += "; removed " + std::to_string(a.pruned) + " older automatic backup(s)";
    return a;
}

// ---- restore ----
std::wstring restore_target(const SaveRoots& roots, const BackupFile& f) {
    if (!safe_backup_path(f.path, f.storage)) return {};
    if (f.storage == "local") {
        if (roots.local.empty()) return {};
        if (f.path == "local/local_settings.json") return lower(f.name) == "local_settings.json" ? roots.local + L"\\local_settings.json" : L"";
        const std::string name = f.path.substr(std::string("local/default/").size());
        if (lower(name) != lower(f.name) || !save_name(name)) return {};
        return roots.local + L"\\default\\" + wide(name);
    }
    if (f.storage == "steam") {
        if (roots.steam.empty()) return {};
        const size_t slash = f.path.find('/', 6);
        const std::string id = f.path.substr(6, slash - 6), name = f.path.substr(slash + 1);
        if (!all_digits(id) || (!f.steam_id.empty() && f.steam_id != id) || lower(name) != lower(f.name) || !save_name(name)) return {};
        const DWORD a = GetFileAttributesW((roots.steam + L"\\userdata\\" + wide(id)).c_str());
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return {};
        return remote_dir(roots, id) + L"\\" + wide(name);
    }
    return {};
}

RestoreResult restore_backup(const SaveRoots& roots, const RestoreOptions& o) {
    RestoreResult r;
    auto busy = [&]() { return o.busy ? o.busy() : std::string(); };
    if (const std::string why = busy(); !why.empty()) {
        r.error = "Saves are restored while the game is closed: " + why;
        return r;
    }
    std::string why;
    const auto b = read_backup(roots.store, o.id, &why);
    if (!b) {
        r.error = "EMF cannot restore that backup: " + why;
        return r;
    }
    const std::wstring dir = backup_folder(roots.store, o.id);
    std::vector<const BackupFile*> chosen;
    if (o.paths.empty()) {
        for (const auto& f : b->files) chosen.push_back(&f);
    } else {
        for (const auto& p : o.paths) {
            const auto it = std::find_if(b->files.begin(), b->files.end(), [&](const BackupFile& f) { return f.path == p; });
            if (it == b->files.end()) {
                r.error = "That backup has no file " + p + ".";
                return r;
            }
            if (std::find(chosen.begin(), chosen.end(), &*it) == chosen.end()) chosen.push_back(&*it);
        }
    }
    if (chosen.empty()) {
        r.error = "That backup holds no files.";
        return r;
    }
    // every copy is whole and every place is there, before anything changes
    std::vector<std::wstring> targets;
    for (const BackupFile* f : chosen) {
        const std::wstring copy = dir + L"\\" + win_path(f->path);
        uint64_t size = 0;
        if (file_sha256(copy, &size) != f->sha256 || size != f->size) {
            r.error = "The backup's copy of " + f->name + " (" + storage_label(f->storage) +
                      ") does not match its record: it was changed or damaged. Nothing was restored.";
            return r;
        }
        const std::wstring t = restore_target(roots, *f);
        if (t.empty()) {
            r.error = f->storage == "steam"
                          ? "There is no save folder on this PC for the Steam account of " + f->name +
                                " (Steam user " + f->steam_id + "). Nothing was restored."
                          : "EMF does not know where " + f->name + " goes on this PC. Nothing was restored.";
            return r;
        }
        targets.push_back(t);
    }
    StoreLock lock(roots.store);
    if (!lock.ok()) {
        r.error = std::string(kLockBusy) + " Nothing was restored.";
        return r;
    }
    // a backup of what is there now, first; it must hold every file the restore replaces
    std::vector<std::wstring> existing;
    std::vector<const BackupFile*> replaced;
    for (size_t i = 0; i < targets.size(); ++i)
        if (GetFileAttributesW(targets[i].c_str()) != INVALID_FILE_ATTRIBUTES) {
            existing.push_back(targets[i]);
            replaced.push_back(chosen[i]);
        }
    if (!existing.empty()) {
        BackupOptions bo;
        bo.kind = "before-restore";
        bo.label = "Before restoring " + (b->time_local.empty() ? b->id : b->time_local);
        bo.made_by = o.made_by;
        bo.emf_version = o.emf_version;
        bo.info = o.info;
        bo.only = existing;
        bo.now = o.now;
        const BackupResult before = make_backup(roots, bo);
        if (!before.ok || !before.backup.problems.empty()) {
            r.error = "EMF could not first keep a copy of the files it would replace, so it restored nothing: " +
                      (before.ok ? before.backup.problems.front() : before.error);
            return r;
        }
        r.before_id = before.backup.id;
        for (const BackupFile* f : replaced) {  // every replaced file, by its path, or no restore
            const bool kept = std::any_of(before.backup.files.begin(), before.backup.files.end(),
                                          [&](const BackupFile& k) { return lower(k.path) == lower(f->path); });
            if (!kept) {
                r.error = "EMF could not first keep a copy of " + f->name + " (" + storage_label(f->storage) +
                          ": what is there now is not a save file it can copy), so it restored nothing.";
                return r;
            }
        }
    }
    if (const std::string later = busy(); !later.empty()) {
        r.error = "Saves are restored while the game is closed: " + later + " Nothing was restored.";
        return r;
    }
    // what an earlier restore that was cut short (EMF closed by force, the PC off) left next to the saves: only EMF's own
    // temporary files, "<save file name>.emf-restore-in-progress-<digits>"
    std::set<std::wstring> folders;
    for (const auto& t : targets) folders.insert(fs::path(t).parent_path().wstring());
    for (const auto& folder : folders) {
        std::error_code ec;
        for (fs::directory_iterator it(fs::path(folder), ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = utf8(it->path().filename().wstring());
            const std::string mark = ".emf-restore-in-progress-";
            const size_t at = name.find(mark);
            if (at == std::string::npos) continue;
            const std::string base = name.substr(0, at), digits = name.substr(at + mark.size());
            if (!all_digits(digits) || (!save_name(base) && lower(base) != "local_settings.json")) continue;
            std::error_code e2;
            if (it->is_regular_file(e2)) DeleteFileW(it->path().c_str());
        }
    }
    for (size_t i = 0; i < chosen.size(); ++i) {
        const BackupFile& f = *chosen[i];
        const std::wstring& target = targets[i];
        const std::wstring copy = dir + L"\\" + win_path(f.path);
        std::error_code ec;
        fs::create_directories(fs::path(target).parent_path(), ec);
        // longer than 32 characters and not ending in .ksd: the game's migration scan leaves such a name alone
        const std::wstring tmp = target + L".emf-restore-in-progress-" + std::to_wstring(GetCurrentProcessId());
        const std::string who = f.name + " (" + storage_label(f.storage) + ")";
        auto stop = [&](const std::string& what) {
            DeleteFileW(tmp.c_str());
            r.error = what;
            if (!r.restored.empty()) {
                r.error += " Restored before that:";
                for (const auto& x : r.restored) r.error += " " + x + ";";
            }
            if (!r.before_id.empty()) r.error += " The files as they were are in the backup " + r.before_id + ".";
            return r;
        };
        if (!CopyFileW(copy.c_str(), tmp.c_str(), FALSE)) return stop("EMF could not write " + who + ": " + win_error(GetLastError()) + ".");
        SetFileAttributesW(tmp.c_str(), FILE_ATTRIBUTE_NORMAL);
        // a fresh time: Steam sees the restored file as the newest change
        if (HANDLE h = CreateFileW(tmp.c_str(), FILE_WRITE_ATTRIBUTES | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            h != INVALID_HANDLE_VALUE) {
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            SetFileTime(h, nullptr, nullptr, &ft);
            FlushFileBuffers(h);
            CloseHandle(h);
        }
        if (file_sha256(tmp) != f.sha256) return stop("EMF's copy of " + who + " did not match the backup (a disk problem?).");
        if (!MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return stop("EMF could not put " + who + " in place: " + win_error(GetLastError()) + ".");
        if (file_sha256(target) != f.sha256) return stop(who + " does not match the backup after restoring it.");
        r.restored.push_back(file_label(*b, f) + " (" + storage_label(f.storage) + ")");
        if (f.storage == "steam") r.steam = true;
    }
    r.ok = true;
    return r;
}

std::string display_path(const std::wstring& path) {
    const auto env = [](const wchar_t* name) {
        wchar_t v[1024] = {};
        const DWORD n = GetEnvironmentVariableW(name, v, 1024);
        std::wstring s = n > 0 && n < 1024 ? std::wstring(v, n) : std::wstring();
        while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
        return s;
    };
    for (const wchar_t* name : {L"LOCALAPPDATA", L"USERPROFILE"}) {
        const std::wstring v = env(name);
        if (v.size() < 3 || path.size() < v.size() || _wcsnicmp(path.c_str(), v.c_str(), v.size()) != 0) continue;
        if (path.size() > v.size() && path[v.size()] != L'\\' && path[v.size()] != L'/') continue;
        return "%" + utf8(name) + "%" + utf8(path.substr(v.size()));
    }
    return utf8(path);
}

std::wstring store_lock_name(const std::wstring& store) {
    // FNV-1a over the path's UTF-16 code units, A-Z lowered (the installer's PowerShell does the same)
    uint32_t h = 0x811c9dc5u;
    for (wchar_t c : store) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        h = (h ^ static_cast<uint32_t>(c)) * 0x01000193u;
    }
    wchar_t name[64];
    swprintf_s(name, L"Local\\EMF.SaveBackups.%08x", h);
    return name;
}

namespace testing {
void set_store_lock_timeout_ms(unsigned ms) { g_lock_timeout_ms = ms; }
void set_list_fault(std::function<bool(const std::wstring&, size_t)> fault) { g_list_fault = std::move(fault); }
}  // namespace testing

// ---- for the other files of src/saves (detail.h) ----
namespace detail {
std::wstring wide(std::string_view s) { return emf::saves::wide(s); }
std::string utf8(const std::wstring& w) { return emf::saves::utf8(w); }
std::string quote(const std::string& s) { return emf::saves::quote(s); }
std::string slot_info_json(const SlotInfo& i, const char* indent) { return emf::saves::slot_info_json(i, indent); }
std::optional<SlotInfo> slot_info_from(const core::json::Value& v) { return emf::saves::slot_info_from(v); }
bool read_small(const std::wstring& path, std::string& out, size_t max) { return emf::saves::read_small(path, out, max); }
bool write_whole(const std::wstring& path, const std::string& bytes) { return emf::saves::write_whole(path, bytes); }
}  // namespace detail

}  // namespace emf::saves
