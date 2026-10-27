#pragma once
// Small text helpers for EMF.exe: UTF-8 <-> UTF-16, case-insensitive compares, reading a file into memory.
#include <windows.h>
#include <share.h>

#include <cstdio>
#include <string>

namespace emf::app {

inline std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

inline std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

inline bool iequals(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

inline bool istarts_with(const std::wstring& s, const std::wstring& prefix) {
    return s.size() >= prefix.size() && _wcsnicmp(s.c_str(), prefix.c_str(), prefix.size()) == 0;
}

// A full path as Windows' long form (\\?\C:\... or \\?\UNC\server\...): no MAX_PATH limit, and names with trailing dots
// or spaces stay as they are. '/' becomes '\'. Relative paths, device paths and paths already in that form are returned
// unchanged. A path with "." or ".." parts is made full by Windows first (the long form is not normalised).
inline std::wstring lp(const std::wstring& path) {
    if (path.size() < 3 || path.compare(0, 4, L"\\\\?\\") == 0 || path.compare(0, 4, L"\\\\.\\") == 0) return path;
    std::wstring p = path;
    for (auto& c : p)
        if (c == L'/') c = L'\\';
    const auto dots = [&] {
        for (size_t from = 0; from <= p.size();) {
            size_t to = p.find(L'\\', from);
            if (to == std::wstring::npos) to = p.size();
            const std::wstring part = p.substr(from, to - from);
            if (part == L"." || part == L"..") return true;
            from = to + 1;
        }
        return false;
    };
    if (dots()) {
        std::wstring buf(32768, L'\0');
        const DWORD n = GetFullPathNameW(p.c_str(), 32768, buf.data(), nullptr);
        if (n == 0 || n >= 32768) return path;
        p = buf.substr(0, n);
    }
    if (p.size() >= 3 && p[1] == L':' && p[2] == L'\\') return L"\\\\?\\" + p;
    if (p.compare(0, 2, L"\\\\") == 0) return L"\\\\?\\UNC\\" + p.substr(2);
    return path;
}

inline bool exists(const std::wstring& path) { return GetFileAttributesW(lp(path).c_str()) != INVALID_FILE_ATTRIBUTES; }

inline bool is_dir(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(lp(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// Opens a file for reading without keeping anyone else from writing it: the game holds its logs open while it runs.
inline FILE* open_shared(const std::wstring& path) { return _wfsopen(lp(path).c_str(), L"rb", _SH_DENYNO); }

// The file's bytes (at most `max`); false when it cannot be opened.
inline bool read_file(const std::wstring& path, std::string& out, size_t max = 64u << 20) {
    out.clear();
    FILE* f = open_shared(path);
    if (!f) return false;
    char buf[65536];
    size_t n = 0;
    while (out.size() < max && (n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    if (out.size() > max) out.resize(max);
    return true;
}

// The last `max` bytes of a file (all of a smaller one); false when it cannot be opened. For logs that grow.
inline bool read_file_tail(const std::wstring& path, std::string& out, size_t max) {
    out.clear();
    FILE* f = open_shared(path);
    if (!f) return false;
    if (_fseeki64(f, 0, SEEK_END) == 0) {
        const long long size = _ftelli64(f);
        if (size > static_cast<long long>(max)) _fseeki64(f, size - static_cast<long long>(max), SEEK_SET);
        else _fseeki64(f, 0, SEEK_SET);
    }
    char buf[65536];
    size_t n = 0;
    while (out.size() < max && (n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    if (out.size() > max) out.resize(max);
    return true;
}

// Valid UTF-8 as it is; anything else is read in the system's ANSI code page (a file saved by an old Notepad).
inline std::string to_utf8_text(const std::string& bytes) {
    if (bytes.empty()) return bytes;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0) > 0)
        return bytes;
    const int n = MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), w.data(), n);
    return utf8(w);
}

// The Windows message for an error code ("Access is denied."), with the number.
inline std::string windows_error_text(DWORD error) {
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, error, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring w = n && buf ? std::wstring(buf, n) : std::wstring();
    if (buf) LocalFree(buf);
    while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' || w.back() == L' ')) w.pop_back();
    return (w.empty() ? std::string("error") : utf8(w)) + " (Windows error " + std::to_string(error) + ")";
}

}  // namespace emf::app
