#include "locate.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cctype>
#include <cstdlib>

#include "text.h"

namespace emf::app {

namespace {

// The quoted value of `"key" "value"` lines (Valve's text format), in order.
std::vector<std::string> vdf_values(const std::string& text, const std::string& key) {
    std::vector<std::string> out;
    const std::string quoted = "\"" + key + "\"";
    size_t from = 0;
    while (from < text.size()) {
        size_t to = text.find('\n', from);
        if (to == std::string::npos) to = text.size();
        std::string line = text.substr(from, to - from);
        from = to + 1;
        size_t a = line.find_first_not_of(" \t");
        if (a == std::string::npos || line.compare(a, quoted.size(), quoted) != 0) continue;
        a = line.find('"', a + quoted.size());
        const size_t b = line.rfind('"');
        if (a == std::string::npos || b == std::string::npos || b <= a) continue;
        std::string v;
        const std::string raw = line.substr(a + 1, b - a - 1);
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == '\\') ++i;  // "\\" is one backslash
            v += raw[i];
        }
        out.push_back(v);
    }
    return out;
}

// The full path with "." and ".." resolved; "" when Windows cannot make one (then the callers fail closed).
std::wstring full_path(const std::wstring& p) {
    if (p.empty()) return {};
    const DWORD need = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
    if (need == 0 || need > 32768) return {};
    std::wstring f(need, L'\0');
    const DWORD n = GetFullPathNameW(p.c_str(), need, f.data(), nullptr);
    if (n == 0 || n >= need) return {};
    f.resize(n);
    while (f.size() > 3 && (f.back() == L'\\' || f.back() == L'/')) f.pop_back();
    return f;
}

// The path with junctions, symbolic links and subst drives resolved ("" when it cannot be opened).
std::wstring final_path(const std::wstring& p) {
    HANDLE h = CreateFileW(p.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    wchar_t buf[4096];
    const DWORD n = GetFinalPathNameByHandleW(h, buf, 4096, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (n == 0 || n >= 4096) return {};
    std::wstring f(buf, n);
    if (f.compare(0, 8, L"\\\\?\\UNC\\") == 0) f = L"\\\\" + f.substr(8);
    else if (f.compare(0, 4, L"\\\\?\\") == 0) f = f.substr(4);
    return f;
}

}  // namespace

std::vector<std::wstring> steam_library_paths(const std::string& vdf_text) {
    std::vector<std::wstring> out;
    for (const auto& v : vdf_values(vdf_text, "path")) out.push_back(wide(v));
    return out;
}

std::string acf_installdir(const std::string& acf_text) {
    const auto v = vdf_values(acf_text, "installdir");
    return v.empty() ? std::string() : v.front();
}

std::vector<std::wstring> steam_libraries(const std::wstring& steam_root) {
    if (steam_root.empty()) return {};
    std::vector<std::wstring> libs{steam_root};
    std::string vdf;
    if (read_file(steam_root + L"\\steamapps\\libraryfolders.vdf", vdf, 1u << 20))
        for (const auto& p : steam_library_paths(vdf)) {
            bool listed = false;
            for (const auto& l : libs) listed = listed || iequals(full_path(l), full_path(p));
            if (!listed) libs.push_back(p);
        }
    return libs;
}

std::wstring find_game_in_steam(const std::wstring& steam_root) {
    for (const auto& lib : steam_libraries(steam_root)) {
        std::string acf;
        if (!read_file(lib + L"\\steamapps\\appmanifest_1203620.acf", acf, 1u << 20)) continue;
        std::string dir = acf_installdir(acf);
        if (dir.empty()) dir = "Enshrouded";
        const std::wstring game = lib + L"\\steamapps\\common\\" + wide(dir);
        if (exists(game + L"\\enshrouded.exe")) return game;
    }
    return {};
}

std::wstring steam_root_from_registry() {
    wchar_t env[8];
    if (GetEnvironmentVariableW(L"EMF_TEST_NO_STEAM", env, 8) > 0) return {};  // tests: never find the real game
    struct Key {
        HKEY root;
        const wchar_t* path;
        const wchar_t* value;
    };
    for (const Key& k : {Key{HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"},
                         Key{HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath"}}) {
        wchar_t buf[1024];
        DWORD size = sizeof buf;
        if (RegGetValueW(k.root, k.path, k.value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) continue;
        std::wstring p = buf;
        for (auto& ch : p)
            if (ch == L'/') ch = L'\\';
        if (!p.empty()) return p;
    }
    return {};
}

std::vector<ProcessInfo> game_processes() {
    std::vector<ProcessInfo> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        const std::wstring name = pe.szExeFile;
        if (!iequals(name, L"enshrouded.exe") && !iequals(name, L"enshrouded_server.exe")) continue;
        ProcessInfo p{name, L"", pe.th32ProcessID};
        if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID)) {
            wchar_t buf[4096];
            DWORD n = 4096;
            if (QueryFullProcessImageNameW(h, 0, buf, &n)) p.path.assign(buf, n);
            CloseHandle(h);
        }
        out.push_back(std::move(p));
    }
    CloseHandle(snap);
    return out;
}

std::string game_running_reason(const std::wstring& game, const std::vector<ProcessInfo>& processes) {
    const std::wstring folder = full_path(game), real_folder = final_path(game);
    if (folder.empty() && !processes.empty())  // fail closed: switches stay locked when the folder cannot be compared
        return utf8(processes[0].name) + " is running, and EMF cannot tell whether it runs from this folder (Windows "
                                         "could not read the folder's full path). Close it first.";
    for (const auto& p : processes) {
        const std::string what = utf8(p.name) + " (process " + std::to_string(p.pid) + ")";
        if (p.path.empty())
            return what + " is running and Windows does not show its folder (it probably runs as administrator), so it "
                          "may be the one in this folder. Close it first (or run EMF.exe as administrator).";
        if (path_inside(folder, p.path)) return what + " is running from this folder";
        const std::wstring real = final_path(p.path);  // the folder may be reached through a junction or link
        if (!real_folder.empty() && !real.empty() && path_inside(real_folder, real)) return what + " is running from this folder";
    }
    return {};
}

bool nexus_url_allowed(const std::string& url) {
    const std::string scheme = "https://";
    if (url.size() <= scheme.size() || url.size() > 2048 || url.compare(0, scheme.size(), scheme) != 0) return false;
    for (unsigned char ch : url)
        if (ch <= 0x20 || ch >= 0x7F || std::string("\"<>\\^`{|}").find(static_cast<char>(ch)) != std::string::npos) return false;
    const size_t end = url.find_first_of("/?#", scheme.size());
    std::string host = url.substr(scheme.size(), end == std::string::npos ? std::string::npos : end - scheme.size());
    if (host.find_first_of("@:") != std::string::npos) return false;  // no user name, no port
    for (char& ch : host) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const std::string domain = "nexusmods.com";
    if (host == domain) return true;
    return host.size() > domain.size() + 1 && host.compare(host.size() - domain.size() - 1, std::string::npos, "." + domain) == 0;
}

bool web_url_allowed(const std::string& url) {
    const std::string scheme = "https://";
    if (url.size() <= scheme.size() || url.size() > 2048 || url.compare(0, scheme.size(), scheme) != 0) return false;
    for (unsigned char ch : url)
        if (ch <= 0x20 || ch >= 0x7F || std::string("\"<>\\^`{|}").find(static_cast<char>(ch)) != std::string::npos) return false;
    const size_t end = url.find_first_of("/?#", scheme.size());
    const std::string host = url.substr(scheme.size(), end == std::string::npos ? std::string::npos : end - scheme.size());
    if (host.empty() || host.size() > 253 || host.find('.') == std::string::npos || host.front() == '.' || host.back() == '.' ||
        host.front() == '-')
        return false;
    for (char ch : host)  // no user name ('@'), no port (':'), no IPv6 literal
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '.')) return false;
    return host.find("..") == std::string::npos;
}

bool path_inside(const std::wstring& folder, const std::wstring& path) {
    if (folder.empty() || path.empty()) return false;
    const std::wstring f = full_path(folder), p = full_path(path);
    if (f.empty() || p.empty()) return false;  // fails closed
    if (iequals(f, p)) return true;
    const std::wstring prefix = f.back() == L'\\' ? f : f + L"\\";
    return istarts_with(p, prefix);
}

bool same_place(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return false;
    if (path_inside(a, b) && path_inside(b, a)) return true;
    const std::wstring ra = final_path(a), rb = final_path(b);
    return !ra.empty() && !rb.empty() && iequals(ra, rb);
}

}  // namespace emf::app
