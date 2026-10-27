#pragma once
// Finding the game through Steam (the same lookup as Find-EnshroudedDir in package\emf_common.ps1), telling whether
// the game or a server runs from a folder (Get-GameRunning there), and which links the app may open.
#include <string>
#include <vector>

namespace emf::app {

// "path" entries of steamapps\libraryfolders.vdf, in file order ("\\" in the file is one backslash).
std::vector<std::wstring> steam_library_paths(const std::string& vdf_text);
// "installdir" of an appmanifest_<id>.acf ("" when it has none).
std::string acf_installdir(const std::string& acf_text);
// The Steam install at `steam_root` and each library in its steamapps\libraryfolders.vdf, each once.
std::vector<std::wstring> steam_libraries(const std::wstring& steam_root);
// The folder of Enshrouded (app 1203620) in the Steam install at `steam_root` and its libraries, or "".
std::wstring find_game_in_steam(const std::wstring& steam_root);
// SteamPath from the registry (HKCU, then the 32-bit HKLM key), or "" (always "" with EMF_TEST_NO_STEAM set).
std::wstring steam_root_from_registry();

struct ProcessInfo {
    std::wstring name;  // "enshrouded.exe"
    std::wstring path;  // "" when Windows does not show it (the game runs as administrator)
    unsigned long pid = 0;
};
// The running enshrouded.exe and enshrouded_server.exe processes.
std::vector<ProcessInfo> game_processes();
// Why the app must not switch mods in `game` now ("" when nothing runs from it). A process whose path cannot be read
// counts as running from it: EMF cannot tell which folder it uses.
std::string game_running_reason(const std::wstring& game, const std::vector<ProcessInfo>& processes);

// https links to nexusmods.com (or a subdomain) only, without a user name, port or odd characters.
bool nexus_url_allowed(const std::string& url);
// Any https link with a plain host name (letters, digits, '-' and '.', with a dot), without a user name, a port,
// backslashes or odd characters: a link the player clicked in a mod's description.
bool web_url_allowed(const std::string& url);

// `path` is `folder` or inside it (full paths, case ignored; "..\" tricks resolved first).
bool path_inside(const std::wstring& folder, const std::wstring& path);
// The same folder or file reached through any path: the paths as written, or where they lead with junctions, symbolic
// links and subst drives resolved (both must exist for that).
bool same_place(const std::wstring& a, const std::wstring& b);

}  // namespace emf::app
