#pragma once
// What a download is, from the paths inside its archive (Nexus lists them without a download: api.h,
// contents_request), and what the player should know before installing it. Pure; unit-tested with real listings.
//
// Rules (paths compared without case; '\' read as '/'; trailing dots and spaces of a name dropped, as Windows does;
// __MACOSX/ and folder entries ignored; unsafe paths (unsafe_archive_path) reported and left out):
// - Wrapper: when every path starts with the same folder and that folder is not one the game folder uses (emf, mods,
//   greeneye, shroudforge, reshade-shaders, aldrich-modmenu), it is a wrapper ("ModName/...") and is stripped, up to
//   two levels. Everything below is relative to what is left (the "root").
// - The kind is the first that matches:
//   1. EMF itself: emf/emf_core.dll, or game/emf/emf_core.dll (EMF's release zip).
//   2. Another loader, by its own folder or file next to a proxy DLL (known_tools.txt's markers): shroudforge/
//      (ShroudForge), shroudtopia.json (Shroudtopia/Shroudweave), chainloader.ini (ModLoader Proxy),
//      aldrich-modmenu/ (Aldrich Mod Menu), emm.exe or kfc-parser.exe (EML's release).
//   3. EMF mod: emf/mods/<id>/ or a DLL directly in emf/mods/.
//   4. Greeneye mod: greeneye/<id>/ (its dbgcore.dll is the Greeneye Loader).
//   5. DLL mod using dinput8: dinput8.dll at the root.
//   6. ReShade preset: a .ini with "reshade" in its name, or a reshade-shaders/ folder (with or without ReShade's
//      own dxgi.dll).
//   7. Another loader or proxy DLL at the root: dbghelp, dbgcore, winmm, powrprof, version, xinput1_4, dxgi, umpdc,
//      cfgmgr32 (.dll). dbgcore.dll with a greeneye/ folder is the Greeneye Loader.
//   8. Shroudtopia mod: shroudtopia.json, a DLL directly in mods/, or mods/<id>/ with a DLL in it.
//   9. ShroudForge package: an EML mod folder with extended.mod.json in it.
//  10. EML mod: a folder with mod.json and src/mod.lua: mods/<id>/, <id>/, or the root itself.
//  11. EMF mod by layout: <id>/mod.json next to <id>/main.lua (an EMF Lua mod), and no src/mod.lua (a wrapper
//      that is the mod's own folder counts as <id>). EMF checks its mod.json when installing. <id>/mod.json next
//      to a DLL stays Unknown: it could be an EMF, Greeneye or Shroudtopia mod packed without its parent folder.
//  12. Save files: *.ksd, the characters file (characters, characters-N), or a world save (8 hex digits with its
//      _info file, or a -N backup of it). An 8-hex-digit name alone is not enough.
//  13. Cheat Engine table: *.ct, *.cetrainer.
//  14. Unknown.
// - `also` lists the unambiguous layouts found besides the kind (EML mod, ShroudForge package, EMF mod, Greeneye
//   mod): Auto Loot Plus is a dinput8.dll DLL mod and also an EML mod that needs EML.
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nexus/api.h"

namespace emf::nexus {

enum class ArchiveKind {
    Unknown,
    EmfFramework,  // EMF itself
    EmfMod,
    EmlMod,
    ShroudForgePackage,  // an EML mod with ShroudForge's extended.mod.json
    DllModDinput8,
    ProxyDll,  // another loader or a DLL mod under another proxy name
    GreeneyeMod,
    ShroudtopiaMod,
    ReshadePreset,
    SaveFiles,
    CheatTable,
};
const char* kind_id(ArchiveKind k);     // "emf-mod", "eml-mod", ... (stable, for the app's page)
const char* kind_label(ArchiveKind k);  // "EMF mod", "EML mod", ... (shown to players)

// What is next to the game now, the same shape and tool words as core::GameFolderView::proxies (known_tools.txt):
// proxy DLL file name -> tool word ("emf", "eml", "shroudforge", "shroudtopia", "greeneye", "aldrich-modmenu",
// "modloader-proxy"; "" = a tool EMF does not know). `known` = the app looked; without it nothing is said about
// what is installed. `chained`: tool words of the DLLs EMF starts through dinput8-chain.ini
// (core::GameFolderView::chained_tools); EML running as a chained eml.dll counts as installed.
struct GameState {
    std::vector<std::pair<std::string, std::string>> proxies;
    bool known = false;
    std::vector<std::string> chained;
};

// A mod folder in the archive, of the kind or one of `also`. The installer copies `path` to the kind's place:
// emf\mods\<id>\ (EMF), mods\<id>\ (EML, ShroudForge, Shroudtopia), greeneye\<id>\ (Greeneye). Paths here and in
// strip_prefix are normalised: '/' between folders, trailing dots and spaces of each name dropped.
struct ModFolder {
    ArchiveKind kind = ArchiveKind::Unknown;
    std::string id;    // the folder's name; "" when mod.json is at the top of the archive itself
    std::string path;  // where it is in the archive, wrapper included, '/' at the end ("" = the whole archive)
};

struct Classification {
    ArchiveKind kind = ArchiveKind::Unknown;
    std::vector<ArchiveKind> also;
    std::string loader;                   // tool word of the loader the archive brings, when its layout says
    std::string strip_prefix;             // the wrapper folder(s) to drop when unpacking ("ModName/"); "" = none
    std::vector<ModFolder> mods;
    std::vector<std::string> proxy_dlls;  // proxy-named DLLs at the root, lower case, sorted
    std::vector<std::string> notes;       // plain English, for the page before install
};

Classification classify_archive(const std::vector<std::string>& paths, const GameState& game = {});
Classification classify_archive(const std::vector<NexusArchiveEntry>& entries, const GameState& game = {});

// A path that must not be unpacked: absolute ('/', '\', a drive), "." or ".." segments, ':' (drives and alternate
// streams), NUL or other control characters, <>"|?*, a segment that is empty after dropping trailing dots and spaces,
// or a reserved device name (CON, PRN, AUX, NUL, COM1-9, LPT1-9, also with an extension). The installer uses it too.
bool unsafe_archive_path(std::string_view path);

}  // namespace emf::nexus
