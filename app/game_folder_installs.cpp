// EMF.exe's view of a game folder with the installs it recorded (emf\installed_mods): scan_game_folder is
// scan_mod_folder (game_folder.cpp) plus the records, which bind Uninstall to the mod rows. Kept apart from
// game_folder.cpp because EMF's core builds that file too (the console's "profile" command reads the folder in game)
// and has no installer.
#include "game_folder.h"
#include "install.h"
#include "servers.h"
#include "loader/chain_list.h"
#include "text.h"

namespace emf::app {

namespace {

// The installs EMF.exe recorded (emf\installed_mods). A record binds to a mod row only through the mod folder it
// installed (its roots, also when an EML mod is switched off since: mods\.foo) or a DLL it listed in dinput8-chain.ini
// -- never through an arbitrary file path, so a forged record naming enshrouded.exe or dinput8.dll does not attach an
// Uninstall to the game's own files. `outside` lists the record's files that are not inside one of its mod folders, for
// the uninstall confirmation.
void link_installs(FolderState& s) {
    const auto under_a_root = [](const std::vector<std::string>& roots, const std::string& path) {
        for (const auto& root : roots) {
            if (_strnicmp(path.c_str(), (root + "\\").c_str(), root.size() + 1) == 0) return true;
            const size_t slash = root.find_last_of('\\');  // the switched-off variant mods\.foo\...
            if (slash != std::string::npos && _strnicmp(root.c_str(), "mods\\", 5) == 0) {
                const std::string off = root.substr(0, slash + 1) + "." + root.substr(slash + 1) + "\\";
                if (_strnicmp(path.c_str(), off.c_str(), off.size()) == 0) return true;
            }
        }
        return false;
    };
    for (const auto& r : read_install_records(s.game)) {
        std::vector<std::string> outside;
        for (const auto& [path, hash] : r.files)
            if (!under_a_root(r.roots, path)) outside.push_back(path);
        s.installed.push_back({r.key, r.name, r.version, r.nexus_id, r.archive, r.installed_at, r.files.size(), r.roots, r.chain, outside});
        for (auto& m : s.mods) {
            if (!m.install_key.empty()) continue;
            bool mine = false;
            for (const auto& root : r.roots) {
                if (iequals(m.path, s.game + L"\\" + wide(root))) mine = true;
                const size_t slash = root.find_last_of('\\');
                if (slash != std::string::npos && root.rfind("mods\\", 0) == 0 &&
                    iequals(m.path, s.game + L"\\" + wide(root.substr(0, slash + 1) + "." + root.substr(slash + 1))))
                    mine = true;
            }
            if (m.source == "chain")
                for (const auto& n : r.chain) mine = mine || loader::same_name(wide(m.id), wide(n));
            if (mine) m.install_key = r.key;
        }
    }
}

}  // namespace

FolderState scan_game_folder(const std::wstring& game, ScanDepth depth) {
    FolderState s = scan_mod_folder(game, depth);
    if (s.game_found) link_installs(s);
    if (s.game_found && s.server && depth != ScanDepth::Profiles) add_server_notes(s);  // where each mod has to run
    return s;
}

}  // namespace emf::app
