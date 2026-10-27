#pragma once
// Mod profiles (docs/app.md "Profiles"): a player saves the on/off state of their mods under a name ("Vanilla+", "Building") and
// switches between them in one go. A profile lists every mod EMF can switch (EMF and Lua mods, EML mods, DLL mods in
// dinput8-chain.ini) with its state, and the read-only ones (ShroudForge's, other loaders') for the record; it may also
// hold each EMF mod's settings. Shared by EMF.exe (app/profiles.h, the Mods tab) and EMF's core (the console's
// "profile" command, the in-game Mods tab): this part is the file format, names, the store and the rules, without the
// game folder (app/profiles.h reads the folder and switches through set_mod_enabled).
//
// Storage: <game>\emf\profiles\<slug>.json, the display name inside; <game>\emf\profiles\current.txt names the profile
// last saved or switched to. Applying a profile switches each listed mod to its state and reports; nothing is rolled
// back, nothing is deleted, and it takes effect at the next game start.
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "manifest.h"

namespace emf::core {

constexpr size_t kProfileNameMax = 40;        // characters
constexpr size_t kProfileFileMax = 1u << 20;  // bytes a profile file may have
constexpr int kProfileFormat = 1;             // "emf_profile" in the file
constexpr size_t kProfileModsMax = 2000;      // mods a profile may list
constexpr size_t kProfileSettingsMax = 5000;  // settings a profile may hold, all mods together
constexpr size_t kProfileValueMax = 4096;     // bytes of one setting's value
constexpr size_t kProfileTextMax = 200;       // bytes of a mod's name

struct ProfileMod {
    std::string source;      // "emf", "eml", "chain" or "other" (as EMF.exe lists them)
    std::string id;          // the mod's id in its source (EML: the folder or zip name; chain: the DLL name)
    bool on = true;
    std::string name;        // the mod's name when the profile was saved (for the reader)
    std::string nexus_id;    // its Nexus mod when known: an import names where to get a missing mod
    bool read_only = false;  // its own loader switched it where the profile was saved: recorded, never switched
};

struct ProfileSetting {
    std::string key, value;
};

struct Profile {
    std::string name;         // the display name, 1-40 characters
    std::string created;      // "2026-10-26 20:00", local time
    std::string emf_version;  // the EMF that saved it
    std::vector<ProfileMod> mods;
    bool with_settings = false;  // the player chose "include settings"
    std::map<std::string, std::vector<ProfileSetting>> settings;  // EMF mod id -> its settings, in mod.json order
};

// ---- names ----
// Trims spaces at both ends (what the player meant).
std::string trim_profile_name(const std::string& name);
// Why `name` (trimmed) cannot be a profile's name, "" when it can: 1-40 characters of valid UTF-8, no control
// characters, none of \ / : * ? " < > | (nothing that is not safe in a file name), not only dots.
std::string profile_name_problem(const std::string& name);
// The file name for a profile (without .json): lower-case ASCII letters and digits, other runs as one '-'
// ("Vanilla+" -> "vanilla", "Building 2" -> "building-2"). A name with letters outside ASCII gets "-<8 hex>" of the
// whole name after that ("Caf\u00e9" -> "caf-1a2b3c4d"), or is "profile-<8 hex>" when it has no ASCII letter or
// digit, so such names never meet on their ASCII part alone; Windows' device names (con, nul, com1, ...) get "-profile"
// after them. Two names with one slug are one profile: saving the second asks first.
std::string profile_slug(const std::string& name);
// A slug as profile_slug makes them (1-60 of a-z 0-9 '-'): the only file names the store opens.
bool valid_profile_slug(const std::string& slug);

// ---- the file ----
std::string profile_to_json(const Profile& p);
struct ProfileRead {
    std::optional<Profile> profile;
    std::string error;  // why the file is not a profile EMF can use
};
// A profile file's text: a JSON object with "name", "mods" (a list of {source, id, on}, optional name, nexus_id,
// read_only), optional "created", "emf_version" and "settings" ({mod id: {key: value}}). Anything else in it is an
// error with the reason (a mod listed twice, a bad name, a file over 1 MB, a newer format, more than 2000 mods or 5000
// settings, a text with control characters or too long).
ProfileRead profile_from_json(const std::string& text);

// ---- the folder, as a profile sees it ----
struct FolderSetting {
    std::string key, value;         // the value as EMF stores it (normalised)
    std::optional<SettingDef> def;  // its definition: a profile's value is normalised with it before it is compared
};

struct FolderMod {
    std::string source, id, name, nexus_id;
    bool on = true;
    bool can_switch = false;  // EMF switches it here
    std::string why_not;      // why it cannot be switched here
    bool read_only = false;   // its own loader switches it (ShroudForge, another loader, a DLL kept aside): recorded so
    bool forced = false;      // its state is not a switch (a broken mod.json): a profile does not record it
    std::vector<FolderSetting> settings;  // an EMF mod's settings with their current values, in mod.json order
};

// The folder's state as a profile named `name` (created and emf_version are the caller's). Every mod is listed (the
// ones whose own loader switches them as read-only) except those whose state is forced; with `with_settings`, the
// settings of every EMF mod.
Profile capture_profile(const std::string& name, const std::vector<FolderMod>& folder, bool with_settings);

// Whether the switches differ from the profile (the picker shows "(changed)"): a mod the profile lists (not as
// read-only) that EMF can switch here and whose state is not the profile's; with settings, a setting the profile holds
// whose value here is another (both normalised: "true" is 1). Mods the profile does not list (installed since: it has
// no say on them, and a switch to it leaves them), mods it lists that are not here, entries it recorded read-only,
// mods EMF cannot switch here, and saved values the mod does not accept (a switch cannot set them) do not count.
bool profile_differs(const Profile& p, const std::vector<FolderMod>& folder);

// How applying a profile changes the folder: the game folder side (app/profiles.h) switches through set_mod_enabled.
class ProfileSwitcher {
public:
    struct Result {
        bool ok = false;
        std::string error;
        bool unchanged = false;  // a setting already had that value (the store found nothing to write)
    };
    virtual ~ProfileSwitcher() = default;
    virtual Result set_enabled(const std::string& source, const std::string& id, bool on) = 0;
    virtual Result set_setting(const std::string& mod, const std::string& key, const std::string& value) = 0;
};

struct ApplyItem {
    std::string source, id, name, nexus_id;
    bool on = true;   // the state the profile asks for (not_in_profile: the state it has and keeps)
    std::string why;  // failed / read_only / a setting's reason
};

struct ApplyReport {
    std::string profile;                  // the profile's name
    std::vector<ApplyItem> switched;      // switched to the profile's state
    std::vector<ApplyItem> unchanged;     // already so
    std::vector<ApplyItem> failed;        // the switch failed (why); the others still applied
    std::vector<ApplyItem> missing;       // in the profile, not in this folder
    std::vector<ApplyItem> read_only;     // here in another state, but EMF does not switch it here (why)
    std::vector<ApplyItem> recorded_read_only;  // here in another state, recorded read-only: not switched (never)
    std::vector<ApplyItem> not_in_profile;  // here and switchable, not in the profile: left as they are
    size_t settings_set = 0, settings_unchanged = 0;
    std::vector<ApplyItem> settings_failed;   // id = "mod.key": refused or not written (why)
    std::vector<ApplyItem> settings_skipped;  // id = "mod.key": the mod is not here, or has no such setting now
    bool ok() const { return failed.empty() && settings_failed.empty(); }
    // Mods and settings that will still not be as the profile has them at the next start.
    size_t still_different() const {
        return failed.size() + missing.size() + read_only.size() + recorded_read_only.size() + settings_failed.size();
    }
};

// Switches each listed mod to its state (all-or-report: one that fails is reported and the others still switch;
// nothing is rolled back), then sets the profile's settings. Mods are matched by source and id (case ignored). Entries
// recorded read-only are never switched, nor mods EMF cannot switch here.
ApplyReport apply_profile(const Profile& p, const std::vector<FolderMod>& folder, ProfileSwitcher& switcher);

// The report in short lines, for the console: a summary first, then what failed, is missing, read-only, not in the
// profile, and the settings.
std::vector<std::string> apply_report_lines(const ApplyReport& r);

// ---- the store: <game>\emf\profiles ----
struct ProfileInfo {
    std::string slug;  // the file's name without .json, lower case (a file EMF did not name: as it is)
    std::string name, created, emf_version;
    size_t mods = 0;
    bool with_settings = false;
    std::string error;  // a file that is not a usable profile (listed so the player can delete it)
};

struct ProfileSave {
    bool ok = false;
    bool exists = false;        // another profile has this slug: nothing was written (ask, then save with replace)
    std::string existing_name;  // its display name
    std::string slug;
    std::string error;
};

class ProfileStore {
public:
    explicit ProfileStore(std::wstring dir) : dir_(std::move(dir)) {}
    const std::wstring& dir() const { return dir_; }

    // Every *.json in the folder, sorted by name (case ignored); unreadable ones with their error. A file is parsed
    // again only when its size or time changed (the state and the picker list them every few seconds).
    std::vector<ProfileInfo> list() const;
    ProfileRead read(const std::string& slug) const;
    // The profile whose display name or slug is `name_or_slug` (case ignored), or nullopt.
    std::optional<std::string> find(const std::string& name_or_slug) const;
    // Writes p under profile_slug(p.name); another profile with that slug is replaced only with `replace`. The folder is
    // made when missing; the file is written whole through a new file next to it.
    ProfileSave save(const Profile& p, bool replace) const;
    // A new display name (and so maybe a new file). Refused when another profile has the new slug.
    ProfileSave rename(const std::string& slug, const std::string& new_name) const;
    // Removes the profile's file (only that: never a mod): `slug` as list() gives it, also a file EMF did not name (a
    // plain name, no folder). The current mark goes with it.
    bool remove(const std::string& slug, std::string* error = nullptr) const;
    // The profile last saved or switched to ("" when none, or when its file is gone).
    std::string current() const;
    bool set_current(const std::string& slug) const;

private:
    std::wstring dir_;
};

// Profile files read from disk so far (tests: list() reads only what changed).
size_t profile_files_read();

}  // namespace emf::core
