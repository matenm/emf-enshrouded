#pragma once
// EMF.exe's view of one game folder: every installed mod by who loads it (EMF, EML or ShroudForge, the DLL mods EMF
// starts from dinput8-chain.ini, other loaders), the settings of EMF mods, warnings, and the changes the app makes
// (a mod on or off, a setting saved). Plain file work without a window, so emf_tests covers it with temp folders.
//
// Switches, all taking effect at the next game start, and nothing is ever deleted:
// - EMF mods: "enabled" in emf\config\<id>.ini, section [<id>] (what EMF's core reads);
// - EML mods: the folder (or zip) in mods\ is renamed with a leading '.', which EML skips: mods\foo <-> mods\.foo;
// - DLL mods EMF starts: their line in dinput8-chain.ini gets a "; " in front (a comment), or loses it again;
// - ShroudForge packages (when ShroudForge is the active loader) and other loaders' mods: read-only here.
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/manifest.h"

namespace emf::core::json {
struct Value;
}

namespace emf::app {

struct ModSetting {
    core::SettingDef def;
    std::string value;      // the effective value, normalised as EMF's settings store does
    bool active = true;     // its "enabledWhen" holds (or it has none); the in-game Mods tab greys it out otherwise
    std::string when;       // "only while Show the line is on" ("" without "enabledWhen")
    bool controls = false;  // another setting's "enabledWhen" names this one (a change re-draws the others)
};

struct ModEntry {
    std::string key;     // unique in the list (source and path): the page selects rows by it
    std::string source;  // "emf", "eml", "chain" (started by EMF) or "other" (another loader's)
    std::string id;      // EMF: mod id; EML: folder or zip name without the '.'; chain: DLL name; other: a unique key
    std::string name, version, description;
    std::vector<std::string> authors;
    std::string loaded_by;    // "EMF", "EML", "ShroudForge", "Greeneye Loader", ...
    bool enabled = true;
    bool can_toggle = false;  // the app can switch it
    std::string toggle_note;  // why it cannot, or how it is switched
    std::wstring path;        // its folder or file
    std::string readme;       // the README next to it, if any
    std::string error;        // what is wrong (an unreadable mod.json, a missing DLL): shown, never hidden
    std::string status;       // chain: "started", "missing", "failed N", "kept aside", "" (not started yet)
    std::string note;
    std::string nexus_id, game_build;
    std::vector<std::string> depends;  // "lua ^1.3", "helper (optional)"
    std::vector<ModSetting> settings;  // in the order the Mods tab shows them (ungrouped first, then by group)
    bool is_loader = false;   // a loader's own DLL (other loaders' group)
    std::string install_key;  // EMF.exe installed it: the key of its record in emf\installed_mods ("" otherwise)
    // where it has to run (mod.json "side", servers.h): "server", "client" or "both" (also when mod.json does not say)
    std::string side = "both";
    bool side_declared = false;
    std::string side_error;                 // "side" has a value EMF does not know
    std::string server_note, server_level;  // in a server folder: what the side means there ("info" or "warn")
    // Its on/off is not a switch EMF sets: a mod.json EMF cannot read (EMF does not load the mod), or an EML folder without
    // mod.json (EML does not take it as a mod, and its '.' form is not listed). Profiles do not record it.
    bool state_forced = false;
};

// A mod EMF.exe installed from an archive (emf\installed_mods\<key>.json), as the page lists it.
struct InstalledMod {
    std::string key, name, version, nexus_id, archive, installed_at;
    size_t files = 0;
    std::vector<std::string> roots;    // the mod folders it installed ("mods\\foo")
    std::vector<std::string> chain;    // the DLLs it listed in dinput8-chain.ini
    std::vector<std::string> outside;  // files it installed that are not inside one of its mod folders (shown on uninstall)
};

struct Warning {
    std::string level;  // "error", "warn" or "info"
    std::string text;
};

struct FolderState {
    std::wstring game;
    bool game_found = false;  // enshrouded.exe or enshrouded_server.exe is there
    bool server = false;      // a dedicated server folder (enshrouded_server.exe only)
    bool emf_installed = false;
    std::string emf_version;  // from emf\emf_core.dll ("" when unknown)
    bool shroudforge_active = false;
    std::vector<ModEntry> mods;
    std::vector<Warning> warnings;
    std::vector<std::string> report;  // what EMF's core writes to emf_core.log at start-up (installed_report)
    std::vector<InstalledMod> installed;
    std::string read_error;  // a file the list needs could not be read (dinput8-chain.ini in use): profiles refuse then
    // for the Nexus classifier and the installer
    std::vector<std::pair<std::string, std::string>> proxies;  // proxy DLLs next to the game: file name, tool word
    std::vector<std::string> chained_tools;                    // tool words of the DLLs EMF started last time
    std::string tools_table;                                   // the known-tools table the scan used
};

// How much a scan reads. Full: the Mods tab. Profiles: what saving and applying a profile needs (no READMEs, no
// warnings, start-up report or Will-not-load check, no chain status, and no DLL is read unless ShroudForge's folder is
// there, to tell whether it is the active loader); the mods, their switches and settings are the same.
enum class ScanDepth { Full, Profiles };
FolderState scan_game_folder(const std::wstring& game, ScanDepth depth = ScanDepth::Full);
// The same without EMF.exe's install records (no install_key, no `installed`): what EMF's core reads in game
// (game_folder.cpp builds into the core too; the records are game_folder_installs.cpp's).
FolderState scan_mod_folder(const std::wstring& game, ScanDepth depth = ScanDepth::Full);
// Scans and EMF manifest lookups done so far in this process (tests: a run of switches reads the folder once).
size_t folder_reads();

// The known-tools table to use for a folder without emf\known_tools.txt (EMF.exe passes the copy next to itself).
void set_fallback_known_tools(const std::string& table);

// mod.json's "side" (where the mod has to run, docs/modding/reference.md): "server", "client" or "both"; without it a
// mod counts as "both", EMF cannot tell whether players need it too.
struct ModSide {
    std::string side = "both";
    bool declared = false;  // mod.json says it
    std::string error;      // "side" has a value EMF does not know (the mod still loads everywhere)
};
ModSide read_mod_side(const core::json::Value& mod_json);

// "emf_core <version>" that the build writes into emf_core.dll ("" when it has none).
std::string emf_core_version(const std::wstring& dll);

// A folder EMF can manage: it holds enshrouded.exe or enshrouded_server.exe.
bool is_game_folder(const std::wstring& dir);

struct Change {
    bool ok = false;
    std::string error;       // what went wrong, for the player
    bool unchanged = false;  // a setting already had that value
};
// source "emf", "eml" or "chain"; other sources are refused (their own loader switches them).
Change set_mod_enabled(const std::wstring& game, const std::string& source, const std::string& id, bool on);
// An EMF mod's setting (declared in its mod.json), checked like the in-game Mods tab does.
Change set_mod_setting(const std::wstring& game, const std::string& id, const std::string& key, const std::string& value);

// What a scan already knows, for a run of switches (a profile): with it, set_mod_enabled and set_mod_setting do not read
// the folder again for each one (whether ShroudForge is the active loader, which EMF mods are there).
struct FolderFacts {
    bool shroudforge_active = false;
    std::map<std::string, std::vector<core::SettingDef>> emf_mods;  // EMF mods by id (as EMF finds them), their settings
};
FolderFacts folder_facts(const FolderState& s);
// The same switches, with what the scan knew (nullptr: read the folder, as above).
Change set_mod_enabled(const std::wstring& game, const std::string& source, const std::string& id, bool on,
                       const FolderFacts* facts);
Change set_mod_setting(const std::wstring& game, const std::string& id, const std::string& key, const std::string& value,
                       const FolderFacts* facts);

// dinput8-chain.ini text with the lines naming `dll` commented out ("; " in front) when !on, or that prefix removed
// when on. Every other byte stays as it was. nullopt when no line changes.
std::optional<std::string> switch_chain_line(const std::string& text, const std::wstring& dll, bool on);
// DLLs switched off with "; " in front of their line (lines the chain parser skips), each once.
std::vector<std::wstring> switched_off_chain_entries(const std::string& text);
// dinput8-chain.ini text with `dll` added, as the installer's Add-ChainEntry does: a switched-off line for it is
// switched on again; otherwise the name goes after the last line of [Chain] (before its blank lines), at the end of a
// file without sections, or into a new [Chain] section at the end. The file's own line breaks are used; every other
// byte stays. nullopt when it is listed already.
std::optional<std::string> add_chain_line(const std::string& text, const std::wstring& dll);
// The text without the lines that name `dll`, switched on or off (lines that count: before any section and in
// [Chain]). nullopt when no line names it.
std::optional<std::string> remove_chain_line(const std::string& text, const std::wstring& dll);

// Changes dinput8-chain.ini: `edit` gets its text as UTF-8 ("" when there is no file) and returns the new text, or
// nullopt to leave it. The result is written in the file's own encoding (UTF-16 stays UTF-16, UTF-8 and ANSI bytes stay
// as they are) through a new file next to it; a new file is UTF-8. `created` says whether the file was made.
Change edit_chain_file(const std::wstring& game, const std::function<std::optional<std::string>(const std::string&)>& edit,
                       bool* created = nullptr);
// Replaces a file's contents through a new file next to it, so a failed write leaves the old one whole.
Change write_file_whole(const std::wstring& path, const std::string& bytes, const std::string& shown);
// The known-tools table for a folder: its emf\known_tools.txt, else the copy next to EMF.exe.
std::string known_tools_table(const std::wstring& game);

}  // namespace emf::app
