#pragma once
// Save backups (docs/saves.md): copies of the game's save files in %LOCALAPPDATA%\EMF\save-backups, made
// by EMF.exe, the player installer (package\emf_saves.ps1 writes the same format), and EMF's core at game start and
// from the console. Shared by EMF.exe and emf_core.dll; plain file work, so emf_tests covers it with temp folders.
//
// What a backup holds: every Steam user's <Steam>\userdata\<id>\1203620\remote\ save files and the Local
// <Saved Games>\keen games\Enshrouded\default\ ones (character*.ksd, world*.ksd, global.ksd, user_settings.json), plus
// the Local local_settings.json one folder up. Never the game's own backup\ and migrated\ folders.
//
// One backup is one folder, <store>\<yyyy-MM-dd_HH-mm-ss>_<kind>\, with backup.json (the manifest) and the files under
// steam\<id>\, local\default\ and local\. It is written into <store>\.tmp-...\ first, every copy is checked against the
// hash of what was read, and only then is the folder renamed into place: the list never shows half a backup. A source
// file that is locked or changes while it is copied is tried once more, then named in the backup's problems.
//
// Nothing here writes into a save folder except restore_backup, and that only the files it restores: never a file with
// another name in the Steam remote root or the Local default root (the game's migration scan moves unknown files).
// Starter, manual and before-restore backups are never deleted automatically; prune_auto keeps the newest automatic
// ones.
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace emf::saves {

// Where the saves and the backups are. An empty path means that place is not there (no Steam, a test that hides it).
struct SaveRoots {
    std::wstring steam;  // the Steam folder: its userdata\<id>\1203620\remote\ holds Steam Cloud saves
    std::wstring local;  // <Saved Games>\keen games\Enshrouded: local_settings.json, and default\ with the Local saves
    std::wstring store;  // %LOCALAPPDATA%\EMF\save-backups
    std::wstring info;   // %LOCALAPPDATA%\EMF\save-info.json (names and levels the core saw in the game's menu)
};

// The places for EMF's own data folder `app_data` (%LOCALAPPDATA%\EMF): store and info.
SaveRoots with_app_data(SaveRoots roots, const std::wstring& app_data);

// How the real places are found; each returns "" when there is none.
struct RealPlaces {
    std::function<std::wstring()> steam;        // Steam's folder (registry)
    std::function<std::wstring()> saved_games;  // the Saved Games known folder
};
// The roots: EMF_TEST_STEAM_ROOT (a fake Steam folder) and EMF_TEST_SAVED_GAMES (a fake Saved Games folder) win when
// set. A test run (`test`: EMF.exe's smoke runs, or EMF_TEST_NO_STEAM set) never uses a real place: what no variable
// names stays empty. `env` reads an environment variable ("" when unset). Store and info stay empty (with_app_data).
SaveRoots roots_from(const std::function<std::wstring(const wchar_t*)>& env, bool test, const RealPlaces& real);
// The same with the process's environment and Windows' real places (registry, known folder).
SaveRoots default_roots(bool test);
// %LOCALAPPDATA%\EMF ("" when Windows does not say where %LOCALAPPDATA% is).
std::wstring default_app_data();

// One save file on this PC.
struct SaveFile {
    std::string storage;   // "steam" or "local"
    std::string steam_id;  // Steam's userdata folder name (steam only)
    std::string type;      // "character", "world", "global", "user_settings", "local_settings"
    int slot = -1;         // the number in the file name (character0.ksd: 0); -1 for the others
    std::string name;      // the file name as it is on disk
    std::string path;      // its place in a backup: "steam/<id>/<name>", "local/default/<name>", "local/local_settings.json"
    std::wstring source;   // where it is now
    uint64_t size = 0;
    int64_t modified = 0;  // the file's own time (Unix seconds)
};
// Every save file under the roots, Steam users first (sorted by id), then Local; in each, by type and slot. A save
// folder that cannot be read to its end sets *error (the list is then incomplete and must not be backed up as whole); a
// folder that is not there is no error.
std::vector<SaveFile> find_saves(const SaveRoots& roots, std::string* error = nullptr);
// The Steam user folders (userdata\<id>) that have Enshrouded saves (a remote\ folder with at least one save file).
std::vector<std::string> steam_users_with_saves(const SaveRoots& roots);

// What the game's menu showed for a slot, as EMF's core saw it (save-info.json; save_info.h merges it).
struct SlotInfo {
    std::string storage;       // "steam" or "local"
    std::string steam_id;      // steam: the Steam account (its userdata folder) the menu showed it for ("" unknown)
    std::string type;          // "character" or "world"
    int slot = -1;             // 0-based, as in the file name
    std::string name;
    std::optional<int> level;  // characters, when known
    int64_t seen = 0;          // when the core read it (Unix seconds)
    int64_t last_loaded = 0;   // when the core saw it load (0: not seen)
    int64_t played = 0;        // worlds: when it was last played, from the game's own slot record (Unix seconds; 0 unknown)
    int64_t play_seconds = 0;  // worlds: its total play time, from the same record (0 unknown)
};

// One file in a backup.
struct BackupFile {
    std::string storage, steam_id, type, name;
    std::string path;  // inside the backup folder, '/' between parts
    int slot = -1;
    uint64_t size = 0;
    std::string sha256;    // lower-case hex
    int64_t modified = 0;  // the file's own time when it was copied (Unix seconds)
};

constexpr int kManifestFormat = 1;
inline constexpr const char* kKinds[] = {"starter", "manual", "auto", "before-restore"};
constexpr const char* kStarterLabel = "Before EMF";

struct Backup {
    std::string id;  // the folder's name
    int format = kManifestFormat;
    std::string kind;  // starter | manual | auto | before-restore
    std::string label;
    std::string emf_version;
    std::string made_by;     // "EMF.exe", "install", "game" (automatic, at game start), "console"
    int64_t time = 0;        // when it was made (Unix seconds)
    std::string time_local;  // the same as local time, "2026-10-27 12:00:05"
    std::vector<BackupFile> files;
    std::vector<SlotInfo> info;         // names and levels of the slots in it, when known
    std::vector<std::string> problems;  // source files that could not be copied, and why
    int64_t created = 0;                // its folder's creation time (100 ns units): orders backups of the same second
    bool damaged = false;               // backup.json is missing or unreadable (listed so it can be deleted)
    std::string error;                  // why it is damaged
    uint64_t bytes() const;
};

bool valid_kind(std::string_view kind);
std::string manifest_json(const Backup& b);
// A manifest back (id is not in it: the caller sets it). nullopt and *error for text that is not one.
std::optional<Backup> parse_manifest(std::string_view text, std::string* error = nullptr);

// "Steam: Ember (level 8), world Embervale · Local: Test Character (level 1), world Test World": the characters and worlds
// in a backup, with their names and levels when known, else their slots ("character slot 2"). Settings files are not
// named. "" for a backup without character or world files.
std::string backup_summary(const Backup& b);
// "character slot 1" / "Ember (level 8)" / "world Test World" / "global.ksd": one file as backup_summary names it.
std::string file_label(const Backup& b, const BackupFile& f);
// "Steam" / "Local"
std::string storage_label(const std::string& storage);

// For tests: called while a source file is copied, so a test can lock it or change it.
struct CopyHooks {
    std::function<void(const std::wstring& source, int attempt)> before_open;  // before the file is opened
    std::function<void(const std::wstring& source, int attempt)> opened;       // open, before it is read
    std::function<void(const std::wstring& source, int attempt)> read_done;    // read and closed, before the checks
};

struct BackupOptions {
    std::string kind = "manual";
    std::string label;
    std::string made_by;
    std::string emf_version;
    std::vector<SlotInfo> info;     // the current save-info: the entries for slots in the backup are copied in
    std::vector<std::wstring> only;  // copy only these source files (a before-restore backup); empty: every save file
    std::function<int64_t()> now;   // Unix seconds (tests); the clock when empty
    CopyHooks hooks;
    int retry_ms = 250;             // the wait before a file is tried again
};

struct BackupResult {
    bool ok = false;       // a backup was made (it may name problems)
    bool nothing = false;  // there are no save files to back up (no backup was made)
    std::string error;     // why no backup was made
    Backup backup;         // the backup made
};
BackupResult make_backup(const SaveRoots& roots, const BackupOptions& o);

// Every backup in the store, newest first; folders whose backup.json cannot be read are listed as damaged. Folders
// being written (.tmp-...) are never listed.
std::vector<Backup> list_backups(const std::wstring& store);
std::optional<Backup> read_backup(const std::wstring& store, const std::string& id, std::string* error = nullptr);
// A starter backup that holds every file is there (one with problems does not count: the next chance makes another).
bool has_starter(const std::wstring& store);
// The backup's folder ("" for an id that is not a backup folder's name).
std::wstring backup_folder(const std::wstring& store, const std::string& id);

struct Outcome {
    bool ok = false;
    std::string error;
};
// Removes one backup (any kind: the player chose it). Refuses an id that is not a backup folder in the store.
Outcome delete_backup(const std::wstring& store, const std::string& id);
// Deletes the oldest automatic backups beyond `keep` (at least 1 is kept). Other kinds are never touched. Returns
// how many it deleted.
size_t prune_auto(const std::wstring& store, size_t keep);
// Removes .tmp-... folders older than `max_age_s` that a backup cut short (the game closed while it was writing).
void clean_stale_temp(const std::wstring& store, int64_t max_age_s = 3600);

// The starter backup ("Before EMF"), when the store has none yet and there are saves to back up. ok with nothing set
// when there is nothing to do.
BackupResult ensure_starter(const SaveRoots& roots, BackupOptions o);

// Automatic backups at game start ([backup] in emf\emf_core.ini).
struct AutoSettings {
    bool on = true;              // on_start
    size_t keep = 10;            // automatic backups kept (1-100)
    int64_t min_minutes = 60;    // at most one automatic backup this often
};
AutoSettings read_auto_settings(const std::wstring& emf_core_ini);
struct AutoResult {
    enum class What { Made, Skipped, Failed } what = What::Skipped;
    std::string text;  // for the log: what happened, in plain words
    BackupResult made;
    size_t pruned = 0;
};
// Off -> skipped. No starter yet -> the starter. An automatic backup younger than min_minutes -> skipped. Nothing
// changed since the newest backup (the same files with the same hashes) -> skipped. Otherwise an automatic backup, then
// the oldest automatic ones beyond `keep` go.
AutoResult auto_backup(const SaveRoots& roots, const AutoSettings& s, BackupOptions base);

// Where a backup's file goes back to: "" when that place is not on this PC (no Steam, or that Steam user has no
// userdata folder here), or for a path EMF does not restore.
std::wstring restore_target(const SaveRoots& roots, const BackupFile& f);

struct RestoreOptions {
    std::string id;                    // the backup
    std::vector<std::string> paths;    // the backup's files to put back (BackupFile::path); empty: all of them
    std::function<std::string()> busy;  // why the saves must not change now ("" = go on): the game runs
    std::string emf_version;
    std::string made_by;
    std::vector<SlotInfo> info;        // for the before-restore backup
    std::function<int64_t()> now;
};
struct RestoreResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> restored;  // what was put back, as file_label names it, with its storage
    std::string before_id;              // the before-restore backup made first ("" when no file was replaced)
    bool steam = false;                 // Steam Cloud files were put back
};
// Checks the backup's copies against their hashes, makes a before-restore backup of the files it will replace, then
// copies each file back (through a temporary file next to it with a name the game's migration scan ignores) and checks
// its hash. Refused while busy() names a reason, before anything changes. The backup itself always stays.
RestoreResult restore_backup(const SaveRoots& roots, const RestoreOptions& o);

// A path as EMF writes it in logs and console replies: %LOCALAPPDATA% and %USERPROFILE% for those folders, so no user
// name ends up in a log someone posts.
std::string display_path(const std::wstring& path);
// The named mutex every EMF program takes while it writes into `store` (package\emf_common.ps1 computes the same name).
std::wstring store_lock_name(const std::wstring& store);

namespace testing {
// How long EMF waits for another EMF program's store lock before it refuses (0: the normal two minutes).
void set_store_lock_timeout_ms(unsigned ms);
// Makes listing a save folder fail at entry `index` (nullptr: off), as a disk error part-way would.
void set_list_fault(std::function<bool(const std::wstring& dir, size_t index)> fault);
}  // namespace testing

// "2026-10-27 12:00:05" in local time.
std::string local_time_text(int64_t unix_seconds);
// "1.2 MB"
std::string size_text(uint64_t bytes);

}  // namespace emf::saves
