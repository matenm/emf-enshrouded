#include "emf_settings.h"

#define NOMINMAX
#include <windows.h>

#include <algorithm>

namespace emf::core {

namespace {

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

SettingDef base(const char* key, SettingType type, const char* label, const char* dflt, const char* page, bool live,
                const char* help = "") {
    SettingDef d;
    d.key = key;
    d.type = type;
    d.label = label;
    d.default_value = dflt;
    d.group = page;
    d.live = live;
    d.help = help;
    return d;
}

SettingDef number(const char* key, SettingType type, const char* label, const char* dflt, double mn, double mx,
                  double step, bool slider, const char* page, bool live, const char* help = "") {
    SettingDef d = base(key, type, label, dflt, page, live, help);
    d.min = mn;
    d.max = mx;
    d.has_min = d.has_max = true;
    d.step = step;
    d.slider = slider;
    return d;
}

SettingDef select(const char* key, const char* label, const char* dflt, std::vector<SettingOption> options,
                  const char* page, bool live, const char* help = "") {
    SettingDef d = base(key, SettingType::Select, label, dflt, page, live, help);
    d.options = std::move(options);
    return d;
}

void add_files(std::vector<SettingOption>& options, const std::vector<std::string>& font_files) {
    for (const auto& f : font_files) options.push_back({"file:" + f, f});
}

}  // namespace

std::vector<std::string> emf_font_files(const std::wstring& fonts_dir) {
    static const char* const kShipped[] = {"alegreya-regular.ttf", "alegreya-bold.ttf", "cascadiamono-regular.ttf"};
    std::vector<std::string> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((fonts_dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::string name = utf8(fd.cFileName);
        const std::string l = lower(name);
        const bool font = l.size() > 4 && (l.ends_with(".ttf") || l.ends_with(".otf"));
        if (!font || std::find(std::begin(kShipped), std::end(kShipped), l) != std::end(kShipped)) continue;
        out.push_back(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<SettingDef> emf_setting_defs(const std::vector<std::string>& font_files) {
    std::vector<SettingDef> d;
    // [hud]: HUD lines, notes and wheels (live)
    d.push_back(select("hud.style", "Look", "game",
                       {{"game", "Like the game (serif, soft shadow)"}, {"boxed", "Boxed (EMF's earlier look)"},
                        {"plain", "Plain text"}},
                       kPageText, true));
    std::vector<SettingOption> hud_fonts = {{"serif", "Serif (Alegreya)"}, {"console", "The console's font"}};
    add_files(hud_fonts, font_files);
    d.push_back(select("hud.font", "Font", "serif", hud_fonts, kPageText, true,
                       "Your own: put a .ttf or .otf file in emf\\fonts (listed from the next start)."));
    d.push_back(number("hud.size", SettingType::Int, "Text size (%)", "100", 60, 200, 10, true, kPageText, true));
    d.push_back(base("hud.colour", SettingType::Color, "Text colour", "#fffff4", kPageText, true,
                     "For lines whose mod chose no colour, and for notes."));
    d.push_back(base("hud.shadow", SettingType::Bool, "Soft shadow", "1", kPageText, true));
    d.push_back(select("hud.notes_place", "Notes", "top_centre",
                       {{"top_centre", "Top centre"}, {"top_left", "Top left"}, {"right", "Right"}}, kPageText, true));
    d.push_back(number("hud.notes_seconds", SettingType::Int, "Notes stay (seconds)", "6", 2, 20, 1, true, kPageText,
                       true));
    // [overlay]: the console
    SettingDef key = base("overlay.key", SettingType::ScanCode, "Console key", "0x29", kPageConsole, false,
                          "Takes two presses of the same key. Default: the key under Esc.");
    d.push_back(key);
    d.push_back(number("overlay.mouse_speed", SettingType::Float, "Mouse speed", "1", 0.1, 10, 0.0, true, kPageConsole,
                       true));
    d.push_back(base("overlay.chat", SettingType::Bool, "Commands in the game's chat", "1", kPageConsole, false,
                     "Type a / and an EMF command into the game's chat."));
    std::vector<SettingOption> console_fonts = {{"consolas", "Consolas"}, {"cascadia", "Cascadia Mono"},
                                                {"serif", "Serif (Alegreya)"}};
    add_files(console_fonts, font_files);
    d.push_back(select("overlay.font", "Font", "consolas", console_fonts, kPageConsole, true));
    d.push_back(select("overlay.tabs_font", "Tabs and Mods tab font", "serif", console_fonts, kPageConsole, true));
    d.push_back(number("overlay.text_size", SettingType::Int, "Text size (%)", "100", 60, 200, 10, true, kPageConsole,
                       true));
    d.push_back(number("overlay.height", SettingType::Int, "Height (% of the screen)", "50", 25, 90, 1, true,
                       kPageConsole, true));
    d.push_back(number("overlay.opacity", SettingType::Int, "Background opacity (%)", "80", 30, 100, 1, true,
                       kPageConsole, true));
    d.push_back(base("overlay.background", SettingType::Color, "Background", "#080a0e", kPageConsole, true));
    d.push_back(base("overlay.accent", SettingType::Color, "Accent", "#c0b074", kPageConsole, true,
                     "Tabs, the selection, the scrollbar and the wheel's ring."));
    d.push_back(base("overlay.colour_command", SettingType::Color, "Commands you type", "#ffffff", kPageConsole, true));
    d.push_back(base("overlay.colour_reply", SettingType::Color, "Replies", "#b3bac2", kPageConsole, true));
    d.push_back(base("overlay.colour_error", SettingType::Color, "Errors", "#ff7a73", kPageConsole, true));
    d.push_back(base("overlay.colour_success", SettingType::Color, "Done", "#7de887", kPageConsole, true));
    d.push_back(base("overlay.suggest", SettingType::Bool, "Suggestions while typing", "1", kPageConsole, true));
    SettingDef rows = number("overlay.suggest_rows", SettingType::Int, "Suggestion rows", "8", 3, 12, 1, true,
                             kPageConsole, true);
    rows.when_key = "overlay.suggest";
    rows.when_value = "1";
    d.push_back(rows);
    d.push_back(base("overlay.timestamps", SettingType::Bool, "A time on each line", "0", kPageConsole, true,
                     "21:04:17 in front of each console line (not in the notes on screen)."));
    d.push_back(number("overlay.scrollback", SettingType::Int, "Lines the console keeps", "2000", 500, 20000, 500, true,
                       kPageConsole, false, "From the next start."));
    // [hud_editor]: the HUD editor (src/core/hud_editor); the active layout has no page: the layout bar changes it
    d.push_back(base("hud_editor.enabled", SettingType::Bool, "HUD editor", "1", kPageHud, true,
                     "Off: EMF leaves the game's HUD alone."));
    d.push_back(base("hud_editor.show_outlines", SettingType::Bool, "Outline the HUD parts while this page is open", "1",
                     kPageHud, true, "The game's own outline; the selected part blinks."));
    d.push_back(base("hud_editor.allow_with_shroudhud", SettingType::Bool, "Use anyway with another HUD mod", "0", kPageHud,
                     true,
                     "With ShroudHud installed the editor stays off so the two do not fight. On: both run, at your own "
                     "risk."));
    d.push_back(base("hud_editor.key_page", SettingType::Key, "Key: open this page", "F9", kPageHud, true,
                     "Press it again to close the console."));
    d.push_back(base("hud_editor.key_reset", SettingType::Key, "Key: reset the whole HUD", "Pause", kPageHud, true,
                     "Puts every part back where the game draws it; the layout before is kept as \"Before reset\"."));
    SettingDef layout = base("hud_editor.active_layout", SettingType::Text, "HUD layout", "Default", "", true,
                             "The layout bar on the HUD page changes it.");
    layout.max_length = 40;
    d.push_back(layout);
    // [console], [backup]: read at start
    d.push_back(base("console.window", SettingType::Bool, "A Windows console window with EMF's log", "0", kPageStart,
                     false));
    d.push_back(base("backup.on_start", SettingType::Bool, "Back up the saves when the game starts", "1", kPageStart,
                     false));
    d.push_back(number("backup.keep", SettingType::Int, "Backups kept", "10", 1, 100, 1, false, kPageStart, false));
    d.push_back(number("backup.min_minutes", SettingType::Int, "At most one backup every (minutes)", "60", 0, 10080, 1,
                       false, kPageStart, false));
    return d;
}

std::vector<std::string> declare_emf_settings(SettingsStore& s, const std::wstring& fonts_dir) {
    std::vector<std::string> lines;
    for (SettingDef& def : emf_setting_defs(emf_font_files(fonts_dir))) {
        const std::string key = def.key;
        std::string why;
        if (!s.declare(kEmfSettingsId, std::move(def), &why, SettingsStore::Declare::Keep))
            lines.push_back("EMF's setting " + key + " was refused: " + why);
        else if (!why.empty())
            lines.push_back("emf_core.ini: " + why);
    }
    return lines;
}

std::string emf_unset_value(const SettingsStore& s, const std::string& key) {
    if (s.get(kEmfSettingsId, "hud.style") == "boxed") {  // the values read_look_settings gives the boxed style
        if (key == "hud.font") return "console";
        if (key == "hud.colour") return "#dbdbcc";
        if (key == "hud.notes_place") return "top_left";
        if (key == "overlay.tabs_font") return "consolas";
    }
    for (const auto& d : s.defs(kEmfSettingsId))
        if (d.key == key) return d.default_value;
    return {};
}

LookSettings look_from_store(const SettingsStore& s, std::vector<std::string>* problems) {
    return read_look_settings(
        [&s](const char* section, const char* key) {
            // only what someone set: an unset key is the reader's own default (style=boxed has defaults of its own)
            const std::string k = std::string(section) + "." + key;
            return s.stored(kEmfSettingsId, k) ? s.get(kEmfSettingsId, k) : std::string();
        },
        problems);
}

}  // namespace emf::core
