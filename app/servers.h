#pragma once
// EMF.exe and dedicated servers (docs/app.md "Servers"). Plain file work without a window, so emf_tests covers
// it with temp folders:
// - the folders EMF.exe keeps: the game folder and server folders, in %LOCALAPPDATA%\EMF\folders.json;
// - server folders Steam has (the dedicated server is Steam app 2278520; a SteamCMD folder is picked by hand);
// - the server's own enshrouded_server.json (name, slots, port, user groups). Passwords never leave the parser except
//   as "set" or "not set"; the texts go only to redact_secrets, which takes them out of logs shown in the app;
// - EMF's chat settings in the server's emf\emf_core.ini ([chat] players, everyone, admins), changed one key at a time
//   so every other byte of the file stays;
// - where a mod runs: mod.json's optional "side" ("server", "client" or "both"), and the note a server's Mods tab shows;
// - the EMF package that installs EMF into a server folder (the package's own install.ps1, never a copy of its rules).
#include <atomic>
#include <string>
#include <vector>

#include "game_folder.h"

namespace emf::core::json {
struct Value;
}

namespace emf::app {

// ---- the folders EMF.exe keeps ----
struct FolderEntry {
    std::string kind;            // "game" or "server"
    std::wstring path;
    bool stop_on_close = false;  // a server EMF.exe started: stop it (cleanly) when EMF.exe closes; default: leave it running
};
struct FolderList {
    std::vector<FolderEntry> folders;  // the game first (at most one), then the servers in the order they were added
};
// "game" (enshrouded.exe is there), "server" (enshrouded_server.exe without enshrouded.exe) or "" (neither).
std::string folder_kind(const std::wstring& dir);
// The same folder: full paths compared with case ignored, a trailing backslash ignored.
bool same_folder(const std::wstring& a, const std::wstring& b);
// folders.json: {"version":1,"folders":[{"kind","path","stop_on_close"}]}. Entries that are not a game or a server, have
// no full path (a drive or \\server) or repeat a folder are dropped; the game comes first, only once.
FolderList parse_folder_list(const std::string& json);
std::string folder_list_text(const FolderList& list);
FolderList read_folder_list(const std::wstring& file);  // an empty list when there is none or it is broken
Change write_folder_list(const std::wstring& file, const FolderList& list);
// Adds `dir` as `kind`: a game replaces the game entry, a server is added once. True when the list changed.
bool remember_folder(FolderList& list, const std::wstring& dir, const std::string& kind);
bool forget_folder(FolderList& list, const std::wstring& dir);
const FolderEntry* find_folder(const FolderList& list, const std::wstring& dir);
FolderEntry* find_folder(FolderList& list, const std::wstring& dir);

// ---- server folders through Steam ----
constexpr const wchar_t* kServerAppManifest = L"appmanifest_2278520.acf";  // Enshrouded Dedicated Server
// The server folders (with enshrouded_server.exe) in the Steam install at `steam_root` and its libraries.
std::vector<std::wstring> find_servers_in_steam(const std::wstring& steam_root);

// ---- enshrouded_server.json ----
struct ServerGroup {
    std::string name;
    bool password_set = false;  // never the text
    bool can_kick_ban = false, can_access_inventories = false, can_edit_world = false, can_edit_base = false,
         can_extend_base = false;
    int reserved_slots = 0;
};
struct ServerConfig {
    bool found = false;   // the file is there
    std::string error;    // it could not be read (the rest is empty then)
    std::string name, ip, save_dir, log_dir;
    int slots = 0, query_port = 0, game_port = 0;  // 0 = not in the file
    bool text_chat = false, text_chat_known = false;
    bool voice_chat = false;
    bool legacy_password_set = false;  // the single "password" of servers before update 2 (2024-06-05)
    std::vector<ServerGroup> groups;
};
// `secrets`, when given, gets the non-empty password texts, for redact_secrets only: they are never shown, logged,
// stored or sent to the page. The values of every "password" key in the raw text go there too (a key given twice, a
// file that does not parse).
ServerConfig parse_server_config(const std::string& json_text, std::vector<std::string>* secrets = nullptr);
ServerConfig read_server_config(const std::wstring& folder, std::vector<std::string>* secrets = nullptr);
// The config as the page shows it: passwords only as password_set.
std::string server_config_json(const ServerConfig& c);
// The server's own log: <folder>\<logDirectory>\enshrouded_server.log; the server's default .\logs when logDirectory
// is missing or points outside the folder.
std::wstring server_log_path(const std::wstring& folder, const ServerConfig& c);

// `text` with whatever looks like a password replaced by ******: each of `secrets` of 4 or more characters wherever it
// appears (longest first; a shorter one would take ordinary words out of the log), then the value after a word with
// "passw" in it ("password": "x", password = x, Password: x, password is x).
std::string redact_secrets(const std::string& text, const std::vector<std::string>& secrets);
// A log's raw bytes as the app shows them: redact_secrets on the bytes, with each secret as UTF-8, as the ANSI code
// page writes it and JSON-escaped, then each line decoded on its own (UTF-8 as it is, else the ANSI code page), so one
// bad byte (a line the server is still writing) neither hides a password nor turns the rest of the log to ANSI.
std::string redact_log(const std::string& raw, const std::vector<std::string>& secrets);

// ---- EMF's chat settings (emf\emf_core.ini, docs/modding/server.md) ----
struct ChatSettings {
    bool players = false;   // a game you host: players' chat commands (a dedicated server always has them)
    bool everyone = false;  // every player may run commands
    std::vector<std::string> admins;  // account hashes as written, trimmed, in order
    bool has_players_key = false;     // the file has a players line (kept when saving)
};
ChatSettings parse_chat_settings(const std::string& ini_text);  // [chat], as the core reads it (GetPrivateProfile*)
// The value of `key` in the first [section] of an ini text, as GetPrivateProfileString gives it (spaces and one pair of
// quotes around it dropped; a second section of the same name is not read); nullopt when it has none.
std::optional<std::string> ini_value(const std::string& text, const std::string& section, const std::string& key);
// The admins as save_chat_settings writes them ("a, b"). The core reads at most kAdminsMaxChars of it.
std::string admins_value(const std::vector<std::string>& admins);
constexpr size_t kAdminsMaxChars = 2047;  // core.cpp reads [chat] admins into 2048 wide characters
ChatSettings read_chat_settings(const std::wstring& folder);
// `text` with `key` in [section] set to `value` (names compared with case ignored, as Windows does). An existing line
// is replaced; a new key goes after the section's last line (before its blank lines), a new section at the end. The
// file's own line breaks are used and every other byte stays.
std::string set_ini_value(const std::string& text, const std::string& section, const std::string& key, const std::string& value);
// An admin entry the core takes (parse_chat_policy in src/core/server_chat.cpp): 1-16 hex digits, "0x" optional.
bool valid_account_hash(const std::string& entry);
struct AdminList {
    std::vector<std::string> ok, bad;
};
// Admins as the page's box gives them: one per line (commas and spaces separate too); the same account once.
AdminList parse_admin_lines(const std::string& text);
// Writes admins, everyone (and players when it is on or the file has it) into emf\emf_core.ini, keeping its other keys,
// its comments and its encoding (UTF-16 stays UTF-16). The file is made when it is missing.
Change save_chat_settings(const std::wstring& folder, const ChatSettings& s);
// Accounts EMF refused, from emf_core.log: one per line the core wrote, "[<time> t<thread>] chat command refused from
// account 0x<16 hex digits>: <what was typed> (to allow it, ...)", the words right after the line's prefix, so a player
// cannot list an account by typing them. Each account once (its newest line), the newest last.
struct RefusedAccount {
    std::string account, time, command;  // "0x...", "11:06:20", what the player typed (without the slash)
};
std::vector<RefusedAccount> refused_accounts(const std::string& log_text);

// ---- where a mod runs ----
// ModSide and read_mod_side (mod.json's "side") are in game_folder.h: the scan reads them.
struct SideNote {
    std::string level;  // "info" or "warn"
    std::string text;
};
// The note on a mod in a server folder's Mods tab.
SideNote server_side_note(const std::string& side, bool declared);
// A server folder's scan (scan_game_folder calls it): each EMF and EML mod's server note, and warnings for a mod that is
// switched on but does nothing on a server and for a "side" EMF does not know.
void add_server_notes(FolderState& s);

// ---- installing EMF into a server folder ----
// EMF.exe installs the EMF it came with, with that package's own install.ps1 (the release package ships a copy of
// install.ps1 and emf_common.ps1 in game\emf\setup\), so the rules (another mod's dinput8.dll renamed, listed in
// dinput8-chain.ini and recorded in emf\moved.txt; the folder's emf\config kept) are the installer's, never a copy.
struct EmfPackage {
    bool ok = false;
    std::string error;          // why there is none
    std::wstring scripts;       // the folder with install.ps1 and emf_common.ps1
    std::wstring tree;          // the package's game\ folder (what goes next to the exe), or ""
    std::wstring installed_in;  // or: the game folder whose emf\installed.txt lists EMF's files (staged first)
    std::string version;        // of its emf\emf_core.dll
    struct Mod {
        std::string id, name, side;  // side: "server", "client", "both" (also when its mod.json does not say)
        bool on = true;              // starts switched on (its mod.json's "enabled", true when not given)
    };
    std::vector<Mod> mods;  // the mods it brings (emf\mods\<id>\mod.json), by id
};
// The EMF of the EMF.exe in `exe_dir` (<x>\emf): an unpacked package (<package>\game\emf\EMF.exe) or a game or server
// folder EMF is installed in (<game>\emf\EMF.exe).
EmfPackage find_emf_package(const std::wstring& exe_dir);
// Copies the files emf\installed.txt of `game` lists into `stage`, made new, as a package's game\ folder: only what a
// package's game\ folder holds (dinput8.dll, emf\emf_core.dll, emf\EMF.exe, emf\known_tools.txt, emf\ui\, emf\vk\,
// emf\mods\, emf\setup\), never the user's settings, EMF's records or any other file. "" or what went wrong.
std::string stage_installed_emf(const std::wstring& game, const std::wstring& stage);
struct ScriptRun {
    bool ok = false;
    int exit_code = -1;
    std::vector<std::string> lines;  // what the script printed (its errors too)
    std::string error;               // why it did not run or failed, in one line
};
// Runs the package's install.ps1 -Game <server> -From <tree> in Windows PowerShell: no window, in a job that ends with
// EMF.exe, at most five minutes. A package found through installed.txt is staged in %TEMP%\emf-setup\ first.
ScriptRun install_emf_into(const EmfPackage& pkg, const std::wstring& server, const std::atomic<bool>* cancel = nullptr);
// One argument of a command line, in quotes, as CommandLineToArgvW reads it back: backslashes before the closing quote
// are doubled ("D:\" would otherwise end in an escaped quote).
std::wstring command_line_arg(const std::wstring& arg);

}  // namespace emf::app
