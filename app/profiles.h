#pragma once
// Mod profiles over a game folder (docs/app.md "Profiles"): what EMF.exe's Mods tab and EMF's core (the
// console's "profile" command) share on top of core/profiles.h. The folder is read with the Mods tab's own scan
// (scan_mod_folder: EMF mods, EML mods, DLL mods in dinput8-chain.ini, other loaders' mods), and every switch goes
// through set_mod_enabled, so the chain file's encoding, EML's renames and ShroudForge's read-only packages stay exactly
// as a click in the Mods tab does them. Built into EMF.exe and into the core (no window, no installer here).
#include <optional>
#include <string>
#include <vector>

#include "core/profiles.h"
#include "game_folder.h"

namespace emf::app {

// <game>\emf\profiles
std::wstring profiles_dir(const std::wstring& game);

// The folder's mods as a profile sees them: every listed mod (the ones EMF cannot switch there as read-only, with the
// reason; other loaders' own DLLs are not mods and are left out), an EMF mod's settings with their values, and the
// Nexus id from mod.json or EMF.exe's install record.
std::vector<core::FolderMod> profile_folder(const FolderState& s);

// Switches through set_mod_enabled and set_mod_setting on `game` (each one checked as a click in the Mods tab is). With
// the facts of the scan the profile is applied over (folder_facts), no switch reads the folder again.
class FolderSwitcher : public core::ProfileSwitcher {
public:
    explicit FolderSwitcher(std::wstring game, std::optional<FolderFacts> facts = std::nullopt)
        : game_(std::move(game)), facts_(std::move(facts)) {}
    Result set_enabled(const std::string& source, const std::string& id, bool on) override;
    Result set_setting(const std::string& mod, const std::string& key, const std::string& value) override;

protected:
    std::wstring game_;
    std::optional<FolderFacts> facts_;
};

struct ProfilesOverview {
    std::vector<core::ProfileInfo> list;
    std::string current;       // the slug of the profile last saved or switched to ("" when none)
    std::string current_name;  // its display name
    bool changed = false;      // the folder no longer matches it (the picker shows "(changed)")
};
ProfilesOverview profiles_overview(const std::wstring& game, const FolderState& s);

// Saves the folder's switches (and, with `with_settings`, the EMF mods' settings) as profile `name`; another profile
// with the same file name is replaced only with `replace` (ProfileSave::exists says it is there). The saved profile
// becomes the current one. Refused when the scan could not read a file it needs (FolderState::read_error).
core::ProfileSave save_folder_profile(const std::wstring& game, const FolderState& s, const std::string& name,
                                      bool with_settings, bool replace, const std::string& created,
                                      const std::string& emf_version);

struct ProfileApply {
    bool ok = false;     // the profile was read and applied (the report may still list failures)
    std::string error;   // why it could not be read
    core::ApplyReport report;
};
// Applies the saved profile `slug` to the folder through `switcher` and makes it the current one. The caller checks
// that the game is closed where that matters (EMF.exe); in game the switches apply at the next start. Refused when the
// scan could not read a file it needs (FolderState::read_error).
ProfileApply apply_saved_profile(const std::wstring& game, const FolderState& s, const std::string& slug,
                                 core::ProfileSwitcher& switcher);

// A profile file from elsewhere (Import): checked, with the mods this folder lacks.
struct ProfileImport {
    std::optional<core::Profile> profile;
    std::string error;
    std::string slug;           // where it would be saved
    bool exists = false;        // a profile with that file name is there (Import asks before replacing it)
    std::string existing_name;
    std::vector<core::ProfileMod> missing;  // its mods that are not in this folder (with Nexus ids where known)
    core::ApplyReport preview;               // what Switch would do here (nothing is done)
    std::vector<core::ApplyItem> settings_to_set;  // ... and the settings it would rewrite (id "mod.key", why = value)
};
ProfileImport read_profile_file(const std::wstring& path, const std::wstring& game, const FolderState& s);

// Local time as "2026-10-26 20:00" (a profile's "created").
std::string profile_time_now();

}  // namespace emf::app
