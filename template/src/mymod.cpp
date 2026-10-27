// mymod: a starting point for your own EMF mod. Rename it in CMakeLists.txt (project), then replace the examples.
// Docs: docs/modding/README.md (first mod), guide.md (how the game and EMF work), cookbook.md (recipes),
// reference.md (every API function).
#define NOMINMAX
#include <windows.h>  // GetTickCount64

#include <cstdio>
#include <mutex>
#include <string>

#include "emf/emf.hpp"
#include "emf/generated/components.hpp"
#include "emf/keys.hpp"

#ifndef MOD_ID
#define MOD_ID "mymod"  // CMakeLists.txt passes the project name
#endif

namespace gen = emf::gen::ecs;

namespace {
constexpr const char* kMod = MOD_ID;
const std::string kLine = std::string(MOD_ID) + ".position";  // HUD line ids start with the mod's id

// Settings from mod.json (changed in the console's Mods tab or in EMF.exe): read again when they change.
struct Settings {
    bool show = true;
    uint32_t corner = EMF_HUD_BOTTOM_LEFT;
    uint32_t rgba = 0xFFFFFFFFu;
    std::string prefix;
    double every = 1.0;          // seconds
    uint32_t toggle_vk = VK_F8;  // 0: no key
};
std::mutex g_settings_lock;  // read on the world threads, written by the change callback
Settings g_settings;

Settings settings() {
    std::lock_guard<std::mutex> l(g_settings_lock);
    return g_settings;
}

void read_settings() {
    Settings s;
    s.show = emf::setting_bool(kMod, "show", true);
    const std::string corner = emf::setting_text(kMod, "corner", "bottom_left");  // a select: its option's value
    s.corner = corner == "top_right"      ? EMF_HUD_TOP_RIGHT
               : corner == "bottom_right" ? EMF_HUD_BOTTOM_RIGHT
                                          : EMF_HUD_BOTTOM_LEFT;
    s.rgba = emf::setting_rgba(kMod, "color", 0xFFFFFFFFu);  // a color: "#rrggbb" as 0xRRGGBBAA
    s.prefix = emf::setting_text(kMod, "prefix");             // text, at most 24 characters
    s.every = emf::setting_float(kMod, "every", 1.0);         // a slider in steps of 0.25
    const std::string key = emf::setting_text(kMod, "toggle_key", "F8");  // a key bind: "F8", "G", "" (none)
    s.toggle_vk = key.empty() ? 0 : emf::keys::parse_key(std::wstring(key.begin(), key.end()));
    std::lock_guard<std::mutex> l(g_settings_lock);
    g_settings = s;
}

double meters(int64_t fixed) { return fixed / 4294967296.0; }  // positions are fixed point: 1 m = 2^32

void show_position(emf::World w) {
    if (!w.is_client()) return;  // the client world exists for host and guests
    const Settings s = settings();
    static emf::keys::Edge toggle;
    static uint64_t last = 0;
    if (toggle.pressed(emf::keys::down(s.toggle_vk))) {
        emf::set_setting(kMod, "show", s.show ? "0" : "1");  // saved, so the Mods tab shows it too
        last = 0;                                             // redraw at once
        return;
    }
    if (GetTickCount64() - last < static_cast<uint64_t>(s.every * 1000.0)) return;
    last = GetTickCount64();
    const auto* t = w.get<gen::CurrentTransform>(emf::local_player());
    if (!s.show || !t) {
        emf::hud(kLine.c_str(), "");  // empty text removes the line
        return;
    }
    char where[96];
    std::snprintf(where, sizeof where, "%.0f, %.0f, %.0f", meters(t->transform.position.x),
                  meters(t->transform.position.y), meters(t->transform.position.z));
    emf::hud(kLine.c_str(), s.prefix.empty() ? std::string(where) : s.prefix + " " + where, s.corner, s.rgba);
}
}  // namespace

extern "C" __declspec(dllexport) bool emf_mod_load(const EmfApi* api) {
    if (!emf::init(api)) return false;  // the core is too old: the log says why
    if (!emf::has_v13()) {
        EMF_LOG(kMod, "needs EMF API v13 or newer");
        return false;
    }

    // Type "<mod id>.hello" in the in-game console (the key under Esc).
    const std::string hello = std::string(kMod) + ".hello";
    emf::add_command(hello.c_str(), ": say hello", [](emf::World, const std::vector<std::string>&) {
        EMF_REPLY("hello from %s", kMod);
        EMF_NOTIFY("hello from %s", kMod);  // a note at the top of the screen
    });

    read_settings();
    emf::on_setting_changed(kMod, [](emf::World, const std::string&) { read_settings(); });

    // Your position in a corner; the key in the "toggle_key" setting hides and shows it.
    emf::on_tick(show_position);

    EMF_LOG(kMod, "loaded");
    return true;
}
