#pragma once
// dinput8-chain.ini: the DLL mods that one dinput8.dll starts for the others, because only one file can be called
// dinput8.dll. XHL-Core and Auto Loot Plus read the same file ("[Chain]", one DLL name per line); EMF's installer adds
// the mods it renames. (chainloader.ini is ModLoader Proxy's own file, with settings such as delayed loading: EMF
// leaves it to ModLoader Proxy, which it starts.)
//
// Lines in [Chain] (and lines before any section) count when, after a comment and an optional "key=" are removed, they
// name a .dll inside the game folder. Other sections are another tool's business. Absolute paths, drives, UNC paths
// and ".." are refused (a chain file must not reach outside the game), and so is "dinput8.dll" (EMF's loader itself).
// The file may be UTF-8 (with or without BOM), the ANSI code page or UTF-16LE with BOM; at most kMaxChainEntries count.
#include <windows.h>

#include <cwctype>
#include <string>
#include <vector>

namespace emf::loader {

constexpr size_t kMaxChainEntries = 256;

inline std::string trim_chain(std::string s) {
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    size_t a = 0, b = s.size();
    while (a < b && space(s[a])) ++a;
    while (b > a && space(s[b - 1])) --b;
    return s.substr(a, b - a);
}

// UTF-8 when the bytes are valid UTF-8, else the system's ANSI code page (a file saved by Notepad on an old system)
inline std::wstring widen_chain(const std::string& s) {
    if (s.empty()) return {};
    for (UINT cp : {static_cast<UINT>(CP_UTF8), static_cast<UINT>(CP_ACP)}) {
        const DWORD flags = cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
        const int n = MultiByteToWideChar(cp, flags, s.data(), static_cast<int>(s.size()), nullptr, 0);
        if (n <= 0) continue;
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(cp, flags, s.data(), static_cast<int>(s.size()), w.data(), n);
        return w;
    }
    return {};
}

// A UTF-16LE file (BOM FF FE) as UTF-8; other text unchanged.
inline std::string chain_text_utf8(const std::string& bytes) {
    if (bytes.size() < 2 || static_cast<unsigned char>(bytes[0]) != 0xFF || static_cast<unsigned char>(bytes[1]) != 0xFE)
        return bytes;
    const auto* w = reinterpret_cast<const wchar_t*>(bytes.data() + 2);
    const int wn = static_cast<int>((bytes.size() - 2) / sizeof(wchar_t));
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, wn, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, wn, s.data(), n, nullptr, nullptr);
    return s;
}

inline bool same_name(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

// A relative path that stays in the game folder: no drive, no leading slash, no ".." part.
inline bool inside_game_folder(const std::wstring& p) {
    if (p.empty() || p[0] == L'\\' || p[0] == L'/' || p.find(L':') != std::wstring::npos) return false;
    size_t from = 0;
    while (from <= p.size()) {
        size_t to = p.find_first_of(L"\\/", from);
        if (to == std::wstring::npos) to = p.size();
        if (p.compare(from, to - from, L"..") == 0) return false;
        from = to + 1;
    }
    return true;
}

inline std::vector<std::wstring> parse_chain_list(const std::string& raw) {
    const std::string text = chain_text_utf8(raw);
    std::vector<std::wstring> out;
    size_t from = 0;
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) from = 3;
    bool counted = true;  // before any section, or in [Chain]
    while (from < text.size() && out.size() < kMaxChainEntries) {
        size_t to = text.find('\n', from);
        if (to == std::string::npos) to = text.size();
        std::string line = trim_chain(text.substr(from, to - from));
        from = to + 1;
        if (!line.empty() && line[0] == '[') {
            counted = _stricmp(trim_chain(line).c_str(), "[chain]") == 0;
            continue;
        }
        if (!counted || line.empty() || line[0] == ';' || line[0] == '#' || line.compare(0, 2, "//") == 0) continue;
        for (const char* mark : {" ;", "\t;", " #", "\t#"}) {  // a comment after the name
            const size_t at = line.find(mark);
            if (at != std::string::npos) line = trim_chain(line.substr(0, at));
        }
        if (const size_t eq = line.find('='); eq != std::string::npos) line = trim_chain(line.substr(eq + 1));
        if (line.size() >= 2 && line.front() == '"' && line.back() == '"') line = line.substr(1, line.size() - 2);
        if (line.size() < 5 || _stricmp(line.c_str() + line.size() - 4, ".dll") != 0) continue;  // a setting
        std::wstring name = widen_chain(line);
        for (wchar_t& c : name)
            if (c == L'/') c = L'\\';  // with '/', LoadLibrary does not search the DLL's own folder for what it needs
        if (name.empty() || !inside_game_folder(name) || same_name(name, L"dinput8.dll")) continue;
        bool listed = false;
        for (const auto& n : out) listed = listed || same_name(n, name);
        if (!listed) out.push_back(name);
    }
    return out;
}

}  // namespace emf::loader
