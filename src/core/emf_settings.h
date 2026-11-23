#pragma once
// EMF's own settings (in-game look phase 2): the keys of emf_core.ini that the console's Settings tab shows, as
// SettingDefs under the reserved id "emf" (kEmfSettingsId) with keys "<section>.<name>". The values are read by the
// same store as mods' settings and kept in emf_core.ini (RoutingIniBackend). No ImGui here (unit-tested).
#include <string>
#include <vector>

#include "manifest.h"
#include "overlay/look_settings.h"
#include "settings.h"

namespace emf::core {

// The Settings tab's pages, in order (SettingDef::group).
constexpr const char* kPageText = "Text on screen";
constexpr const char* kPageConsole = "Console";
constexpr const char* kPageHud = "HUD";
constexpr const char* kPageStart = "Start-up";

// Every setting of the Settings tab, in the order the tab shows them; `font_files` (from emf_font_files) become
// "file:<name>" choices of the font settings.
std::vector<SettingDef> emf_setting_defs(const std::vector<std::string>& font_files);

// The .ttf and .otf files in `fonts_dir` (emf\fonts) by name (UTF-8), sorted, without the three EMF ships.
std::vector<std::string> emf_font_files(const std::wstring& fonts_dir);

// Declares every setting under kEmfSettingsId (Declare::Keep: what the player typed stays in emf_core.ini). Returns the
// lines worth logging (a value that had to be clamped, a definition refused).
std::vector<std::string> declare_emf_settings(SettingsStore& s, const std::wstring& fonts_dir);

// The value one of EMF's settings has while nobody set it: under hud.style "boxed", hud.font, hud.colour,
// hud.notes_place and overlay.tabs_font take that style's own (read_look_settings); any other its definition's default.
std::string emf_unset_value(const SettingsStore& s, const std::string& key);

// The look as the store holds EMF's settings now ([hud] and [overlay] keys; unset or undeclared ones: the defaults).
LookSettings look_from_store(const SettingsStore& s, std::vector<std::string>* problems);

}  // namespace emf::core
