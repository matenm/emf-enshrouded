#pragma once
// "Install a downloaded file" in EMF.exe: a plan from the unpacked archive (what goes where), shown to the player,
// applied only when they click Install, and recorded so that Uninstall removes exactly what it installed. Plain file
// work without a window, tested against temp folders.
//
// The plan follows what nexus::classify_archive finds (the classifier the Nexus listing uses), with the real mod.json
// read from the unpacked files:
// - EML or ShroudForge mod -> mods\<id>\; EMF mod -> emf\mods\<id>\ (its mod.json must be one EMF reads);
//   Greeneye mod -> greeneye\<id>\ (plus the Greeneye Loader's dbgcore.dll when it ships one and none is there);
//   Shroudtopia mod -> mods\<id>\. A mod folder that is already there is an update: the old folder moves to
//   emf\backup\<id>-<time>\ first.
// - DLL mod with dinput8.dll: the file is renamed as EMF's installer does (<tool>.dll from known_tools.txt, else its
//   version information's product, else the archive's name, else dinput8-previous.dll; -2, -3 when taken) and listed
//   inside [Chain] of dinput8-chain.ini (EMF must be installed); its other files are copied with their paths.
// - Other proxy names (dbghelp.dll, winmm.dll, ...) and the other loose files: copied only where nothing is.
// - EMF itself, ReShade presets, save files, Cheat Engine tables and unknown files: explained, not installed.
// Nothing is ever overwritten: a part that would replace another mod's file is refused with the reason, and the other
// parts still install. Files of the same install's earlier version move to emf\backup\ (never deleted).
// emf\installed_mods\<key>.json records every installed file with its SHA-256; the key is nexus-<id> when the Nexus
// mod is known, else the mod's id. An archive EMF downloaded from Nexus also records its Nexus file id.
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "game_folder.h"
#include "nexus/classify.h"

namespace emf::app {

struct CopyStep {
    std::wstring from;  // the file in the unpack folder
    std::string to;     // relative to the game folder, '\' between folders
    uint64_t size = 0;
};

struct PlanPart {
    enum class Action { Install, Update, Skip, Refused };
    Action action = Action::Install;
    std::string kind;    // nexus::kind_id ("eml-mod", "dll-dinput8", ...)
    std::string label;   // nexus::kind_label ("EML mod")
    std::string title;   // the mod's name, or what the part is
    std::string target;  // where it goes: "mods\\foo\\", "next to the game"
    std::string why;     // Skip or Refused: the reason
    std::vector<CopyStep> copies;
    std::string replace_folder;               // Update: the folder that moves to emf\backup\ first (relative)
    std::vector<std::string> replace_files;   // files of this install's earlier version that move to the backup first
    std::vector<std::string> chain_add;       // DLL names it lists in dinput8-chain.ini
    std::vector<std::string> chain_keep;      // DLLs its earlier version listed there, which it replaces
    std::vector<std::string> chain_off;       // older copies of the same tool it switches off there
    std::string root;                         // a mod folder it installs ("mods\\foo"), "" for loose files
    std::vector<std::string> deps;            // files it needs but did not install (already there shared, a kept loader)
    std::vector<std::string> lines;           // what goes where, in words
    std::string id, version;                  // from mod.json
    bool installs() const { return (action == Action::Install || action == Action::Update) && (!copies.empty() || !chain_add.empty()); }
};

struct InstallPlan {
    std::string archive;     // the archive's file name
    std::wstring folder;     // the unpack folder
    std::wstring game;       // the game folder the plan was built for (apply refuses a different one)
    std::string key;         // "nexus-170", or the mod's id
    std::string name, version, nexus_id;
    std::string nexus_file_id;  // the Nexus file it was downloaded as ("" when not known)
    std::string kind_label;  // what the archive is, as the classifier says
    std::string error;       // the whole archive is refused
    std::vector<PlanPart> parts;
    std::vector<std::string> notes;  // the classifier's notes and EMF's own
    bool update = false;             // an earlier install with this key is recorded
    bool installable() const;        // some part installs something
};

// An install a plan of a Nexus mod updates instead of starting nexus-<id> (updates.h update_keys), and why.
struct UpdateKey {
    std::string key;  // the install's key
    std::string why;  // "you linked Foo to this Nexus mod": the plan says it
};

struct PlanInput {
    std::wstring game;
    std::wstring folder;               // the unpacked archive
    std::vector<std::string> files;    // its files, relative, '/' between folders
    std::string archive;               // the archive's file name
    std::string nexus_id, nexus_name;  // when installed from a mod's Nexus page ("" otherwise)
    std::string nexus_file_id;         // when EMF downloaded it from Nexus ("" otherwise)
    bool nexus_certain = false;        // EMF downloaded it from that mod: nexus_id wins over mod.json and the file name
    // Nexus mod id -> an install the player tied to that mod (updates.h update_keys): a plan for that mod updates that
    // install instead of starting a second one under nexus-<id>, and its notes say why
    std::map<std::string, UpdateKey> update_keys;
    nexus::GameState state;            // what is next to the game
    std::string tools_table;           // known_tools.txt
    bool emf_installed = false;
};

InstallPlan build_plan(const PlanInput& in);

struct InstallResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> done;  // what happened, in words
};
// Applies a plan (the caller checks that the game is closed). CopyFile never overwrites; on a failure everything this
// call did is taken back (its own new files removed, backups moved back, dinput8-chain.ini restored).
InstallResult apply_plan(const std::wstring& game, const InstallPlan& plan);

// What one install recorded (emf\installed_mods\<key>.json). Paths are relative to the game folder; unsafe ones in a
// record are ignored when it is read.
struct InstallRecord {
    std::string key, name, version, nexus_id, archive, installed_at;
    std::string nexus_file_id;  // the Nexus file it came from, when EMF downloaded it
    std::vector<std::string> kinds, backups;
    std::vector<std::pair<std::string, std::string>> files;  // path, SHA-256
    std::vector<std::string> folders;    // folders the install made
    std::vector<std::string> roots;      // mod folders it installed ("mods\\foo")
    std::vector<std::string> chain;      // DLLs it listed in dinput8-chain.ini
    std::vector<std::string> chain_off;  // older copies it switched off there
    std::vector<std::string> deps;       // shared files it needs but did not install (uninstall never deletes these)
};
std::vector<InstallRecord> read_install_records(const std::wstring& game);
std::optional<InstallRecord> read_install_record(const std::wstring& game, const std::string& key);

// Removes what install `key` recorded: the files whose SHA-256 still matches (others are left and named), the folders
// it made once they are empty, and its lines in dinput8-chain.ini (copies it switched off are switched on again).
InstallResult uninstall_mod(const std::wstring& game, const std::string& key);

// The plan as the page shows it.
std::string plan_json(const InstallPlan& plan, const std::string& id, const std::string& blocked);

// Helpers, tested on their own.
std::string name_slug(const std::string& text);          // "Auto Loot Plus" -> "auto-loot-plus"
// The archive's name as a slug, without its extension and the numbers Nexus appends (version, mod id, time):
// "AutoLootPlus 1.1.8-170-1-1-8-1792790875.zip" -> "autolootplus".
std::string archive_slug(const std::string& file_name);
// ProductName, FileDescription and OriginalFilename from a DLL's version resource (read from the bytes; the DLL is
// never loaded). Empty strings for what it lacks.
std::vector<std::string> dll_version_strings(const std::string& dll_bytes);
// The installer's fallback name for an unknown dinput8.dll: the first of the strings that is not Microsoft's, as a
// slug of at most 40 characters ("" when none).
std::string product_slug(const std::vector<std::string>& version_strings);
bool valid_install_key(const std::string& key);
// The Nexus mod id in a download's file name ("AutoLootPlus 1.1.8-170-1-1-8-1792790875.zip" -> "170"); "" when the name
// does not carry one (no trailing mod-id/version/timestamp run).
std::string filename_nexus_id_of(const std::string& archive_name);

}  // namespace emf::app
