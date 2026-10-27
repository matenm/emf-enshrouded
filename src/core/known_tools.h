#pragma once
// Which loaders and DLL mods sit next to the game. emf\known_tools.txt (shipped with EMF, read by the installer too)
// has one line per tool: "word | name | text:<in its DLL> | beside:<file or folder next to the exe> |
// same-loader:<another proxy name the same loader also uses> | helper:<a proxy name of its own that does not start it
// alone>". The core reports at start-up which tool owns each proxy DLL, what EMF chain-loaded, and a loader whose
// files another tool replaced (and which tool holds the name it wants).
#include <string>
#include <utility>
#include <vector>

namespace emf::core {

struct KnownTool {
    std::string word;                      // short file-name word (the installer names a renamed DLL <word>.dll)
    std::string name;                      // shown to players
    std::vector<std::string> texts;        // text found inside its DLLs (case ignored; stored in lower case)
    std::vector<std::string> beside;       // a file or folder of its own next to the exe
    std::vector<std::string> same_loader;  // proxy names through which this one loader also runs
    std::vector<std::string> helpers;      // its proxy names that do not start it alone (helpers of its loader)
};

std::vector<KnownTool> parse_known_tools(const std::string& text);

// The word of the first tool whose text the DLL contains; "emf" for EMF's own loader (EMF_LOADER_MARKER); "" when
// unknown.
std::string tool_for_dll_text(const std::vector<KnownTool>& tools, const std::string& dll_text);

// The DLLs chain_status.txt reports as started.
std::vector<std::string> started_chain_names(const std::string& chain_status);

struct GameFolderView {
    std::vector<std::pair<std::string, std::string>> proxies;  // proxy-named DLLs next to the exe: name, tool word
    std::vector<std::string> chained_tools;                    // tool words of the DLLs EMF chain-loaded
    std::vector<std::string> beside;                           // tools' own files/folders that exist
    std::string chain_status;                                  // emf\chain_status.txt
};

std::vector<std::string> installed_report(const std::vector<KnownTool>& tools, const GameFolderView& view);

// File names DLL mods use to get into the game (the game imports them, or a DLL it imports forwards to them).
const std::vector<std::string>& proxy_dll_names();

}  // namespace emf::core
