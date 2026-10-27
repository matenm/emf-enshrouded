// UTF-8 file names for the Lua library (see lua_files.h; this file is compiled without that header forced in).
#include <windows.h>

#include <stdio.h>

#include <string>

namespace {

// The UTF-16 form of a UTF-8 string; false when it is not valid UTF-8 (the ANSI function then gets it as before).
bool to_wide(const char* s, std::wstring& out) {
    if (!s) return false;
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, nullptr, 0);
    if (n <= 0) return false;
    out.assign(static_cast<size_t>(n), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, out.data(), n) != n) return false;
    out.pop_back();  // the terminator
    return true;
}

}  // namespace

FILE* emf_lua_fopen(const char* name, const char* mode) {
    std::wstring w, m;
    if (to_wide(name, w) && to_wide(mode, m)) return _wfopen(w.c_str(), m.c_str());
    return fopen(name, mode);
}

FILE* emf_lua_freopen(const char* name, const char* mode, FILE* f) {
    std::wstring w, m;
    if (to_wide(name, w) && to_wide(mode, m)) return _wfreopen(w.c_str(), m.c_str(), f);
    return freopen(name, mode, f);
}

int emf_lua_remove(const char* name) {
    std::wstring w;
    if (to_wide(name, w)) return _wremove(w.c_str());
    return remove(name);
}

int emf_lua_rename(const char* from, const char* to) {
    std::wstring a, b;
    if (to_wide(from, a) && to_wide(to, b)) return _wrename(a.c_str(), b.c_str());
    return rename(from, to);
}
