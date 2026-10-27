#pragma once
// emf/keys.hpp — hotkeys for mods: read a key setting from an ini, see each push once, and only while the game
// window is in front (not while alt-tabbed or typing in another window).
//
//   uint32_t vk = emf::keys::parse_key(L"G", &name);        // from GetPrivateProfileStringW
//   emf::keys::Edge edge;
//   // every server tick:
//   if (edge.pressed(emf::keys::down(vk))) { ... }
//
// While EMF's console or a mod's wheel holds the input, mods' GetAsyncKeyState reads keys as up (and a wheel's hold
// key stays up until released after the wheel closed), so hotkeys pause by themselves.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>

namespace emf::keys {

namespace detail {
struct NamedKey {
    const char* name;
    uint32_t vk;
};
// Keys with a name (besides letters, digits and F1-F24), matched ignoring case.
inline const NamedKey* named_keys(size_t* count) {
    static const NamedKey k[] = {
        {"Space", VK_SPACE},      {"Tab", VK_TAB},          {"Enter", VK_RETURN},       {"Backspace", VK_BACK},
        {"Insert", VK_INSERT},    {"Delete", VK_DELETE},    {"Home", VK_HOME},          {"End", VK_END},
        {"PageUp", VK_PRIOR},     {"PageDown", VK_NEXT},    {"Up", VK_UP},              {"Down", VK_DOWN},
        {"Left", VK_LEFT},        {"Right", VK_RIGHT},      {"Shift", VK_SHIFT},        {"Ctrl", VK_CONTROL},
        {"Alt", VK_MENU},         {"CapsLock", VK_CAPITAL}, {"Pause", VK_PAUSE},        {"Numpad0", VK_NUMPAD0},
        {"Numpad1", VK_NUMPAD1},  {"Numpad2", VK_NUMPAD2},  {"Numpad3", VK_NUMPAD3},    {"Numpad4", VK_NUMPAD4},
        {"Numpad5", VK_NUMPAD5},  {"Numpad6", VK_NUMPAD6},  {"Numpad7", VK_NUMPAD7},    {"Numpad8", VK_NUMPAD8},
        {"Numpad9", VK_NUMPAD9},  {"NumpadAdd", VK_ADD},    {"NumpadSubtract", VK_SUBTRACT},
        {"NumpadMultiply", VK_MULTIPLY}, {"NumpadDivide", VK_DIVIDE}, {"NumpadDecimal", VK_DECIMAL},
    };
    *count = sizeof(k) / sizeof(k[0]);
    return k;
}
inline bool same_ignoring_case(const std::wstring& a, const char* b) {
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i) {
        wchar_t x = a[i];
        char y = b[i];
        if (x >= L'A' && x <= L'Z') x = static_cast<wchar_t>(x + 32);
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + 32);
        if (x != static_cast<wchar_t>(y)) return false;
    }
    return i == a.size() && b[i] == '\0';
}
// The name a key shows: "G", "F2", "Space", or "key 0xba" for keys without a name.
inline std::string key_name(uint32_t vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, static_cast<char>(vk));
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    size_t n = 0;
    const NamedKey* k = named_keys(&n);
    for (size_t i = 0; i < n; ++i)
        if (k[i].vk == vk) return k[i].name;
    char b[16];
    snprintf(b, sizeof(b), "key 0x%02x", vk);
    return b;
}
}  // namespace detail

// A key setting: one letter or digit, a key name ("F2", "Space", "PageUp", "Numpad7", "Shift"; ignoring case), or the
// virtual-key number of a keyboard key ("0x71" or "113" = F2; without 0x the number is decimal). Mouse buttons and the left/right modifier codes are refused
// (0): the wheel opened with it as its hold key could never see it released. *name (optional) gets what notes show
// ("K", "F2", "key 0xba").
inline uint32_t parse_key(const std::wstring& text, std::string* name = nullptr) {
    uint32_t vk = 0;
    if (text.size() == 1 && ((text[0] >= L'A' && text[0] <= L'Z') || (text[0] >= L'a' && text[0] <= L'z') ||
                             (text[0] >= L'0' && text[0] <= L'9'))) {
        vk = text[0] >= L'a' && text[0] <= L'z' ? static_cast<uint32_t>(text[0] - 32) : static_cast<uint32_t>(text[0]);
    } else if (text.size() >= 2 && text.size() <= 3 && (text[0] == L'F' || text[0] == L'f') && text[1] >= L'1' &&
               text[1] <= L'9') {
        const unsigned long f = std::wcstoul(text.c_str() + 1, nullptr, 10);
        const bool digits = text.size() == 2 || (text[2] >= L'0' && text[2] <= L'9');
        if (!digits || f < 1 || f > 24) return 0;
        vk = VK_F1 + static_cast<uint32_t>(f) - 1;
    } else if (!text.empty() && text[0] >= L'0' && text[0] <= L'9') {  // "0x71" hex, "113" decimal ("071" too)
        const bool hex = text.size() > 2 && text[0] == L'0' && (text[1] == L'x' || text[1] == L'X');
        const wchar_t* digits = text.c_str() + (hex ? 2 : 0);
        for (const wchar_t* p = digits; *p; ++p)
            if (!((*p >= L'0' && *p <= L'9') ||
                  (hex && ((*p >= L'a' && *p <= L'f') || (*p >= L'A' && *p <= L'F')))))
                return 0;
        wchar_t* end = nullptr;
        const unsigned long v = std::wcstoul(digits, &end, hex ? 16 : 10);
        if (!end || *end || v == 0 || v > 0xFE) return 0;
        if (v <= 0x06 || (v >= 0xA0 && v <= 0xA5)) return 0;  // mouse buttons; VK_LSHIFT..VK_RMENU
        vk = static_cast<uint32_t>(v);
    } else {
        size_t n = 0;
        const detail::NamedKey* k = detail::named_keys(&n);
        for (size_t i = 0; i < n && !vk; ++i)
            if (detail::same_ignoring_case(text, k[i].name)) vk = k[i].vk;
        if (!vk) return 0;
    }
    if (name) *name = detail::key_name(vk);
    return vk;
}

// The foreground window belongs to the game process.
inline bool game_in_front() {
    DWORD pid = 0;
    const HWND fg = GetForegroundWindow();
    return fg && GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId();
}

// The key is held now and the game is in front (0 = no key: never).
inline bool down(uint32_t vk) {
    return vk != 0 && game_in_front() && (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
}

// Turns "is it down" samples into one press per push.
struct Edge {
    bool was_down = false;
    bool pressed(bool is_down) {
        const bool p = is_down && !was_down;
        was_down = is_down;
        return p;
    }
};

}  // namespace emf::keys
