#pragma once
// EMF.exe's card "EML content" (docs/engine_parts/content_loader.md "What the player sees"): who applies the content
// of the EML mods at the next start ("Play" on the game client) and why, the last build's outcome, which mods were
// built and which are left out, the shadow copy's disk use, and EML's dbghelp.dll set aside by EMF (E5).
//
// It is computed from the game folder with the loader's own rules (src/loader/: content_owner.h, eml_mods.h,
// content_base.h, content_state.h, content_fingerprint.h, content_shadow.h, the redirect's shadow.txt check), so
// EMF.exe and the loader cannot disagree. It reads sizes and times, the mods' mod.json, the state and the record; it
// never loads emf_content.dll (the licence boundary: only the loader does, at the held start) and never writes.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace emf::app {

// The loader's constants the panel needs, copied (EMF.exe never includes content_build.h or content_start.h, which
// hold the DLL's LoadLibrary); a test pins them to the loader's.
constexpr int kContentBuildRunning = -7;     // content_build.h kBuildRunning: a build that never came back
constexpr int kContentMaxBuildAttempts = 2;  // content_start.h kMaxBuildAttempts

enum class ContentStatus {
    Ready,         // the copy is current: the next start uses it without building
    BuildsNext,    // the next start builds (never built, a mod, the game or emf_content.dll changed, the copy is gone)
    Failed,        // a build with the same files failed: not tried again until something changes
    Off,           // [content] mode=off, no EML mod EMF can build, no base to build from
    OtherTool,     // EML or ShroudForge applies the content
    NotInstalled,  // no emf\emf_content.dll
};
const char* content_status_name(ContentStatus s);  // "ready", "builds_next", "failed", "off", "other_tool", ...

// EML's dbghelp.dll and EMF (E5): EMF may set it aside (offer) or has (given); never ShroudForge's.
enum class TakeOver { None, Offer, Given };
const char* take_over_name(TakeOver t);  // "none", "offer", "given"

struct ContentPanel {
    ContentStatus status = ContentStatus::NotInstalled;
    std::string headline, detail;
    std::string why;  // decide_content's reason for the next start (emf_loader.log words its own line around it)
    std::string mode = "auto";  // [content] mode: auto, on or off
    struct Dll {
        bool present = false;
        uint64_t size = 0;
        int64_t time = 0;  // last written, Unix seconds
    } dll;
    struct Mod {
        std::string folder, id, name, version;
        bool built = false;  // the last build has it at this version
    };
    std::vector<Mod> mods;  // the usable EML mods, in the loader's order
    struct LeftOut {
        std::string folder, why;
    };
    std::vector<LeftOut> left_out;  // the loader's reasons
    struct LastBuild {
        int code = 0;
        std::string message;
        std::vector<std::string> errors, mods;  // errors: the first kMaxErrors
        size_t errors_more = 0;                 // how many more the state lists
        std::vector<std::string> warnings;      // a good build's mod warnings ("<id>: <text>"), the first kMaxErrors
        size_t warnings_more = 0;
        uint64_t ms = 0;
        std::string built_at;  // UTC, "YYYY-MM-DDTHH:MM:SSZ"
    };
    std::optional<LastBuild> last_build;  // emf\content\client\emf_content_state.json, when it reads
    bool copy_there = false;  // emf\content\client\ is there
    uint64_t copy_bytes = 0;  // its own files (one name each), not its links to the game's files
    bool patched_in_place = false;  // the game's .kfc was patched in place (the base is the .kfc.bak): verify advice
    TakeOver take_over = TakeOver::None;
    std::string other_tool;  // OtherTool: the tool as players know it
    std::string set_aside;   // Given: the name EML's dbghelp.dll has now (in the game folder)
    // Offer: the EML mods EML loads now that EMF would leave out (needs EML's runtime loader, a .zip, exports only, ...):
    // they stop working until EML's dbghelp.dll is given back. Its mods and left_out are then what EMF would build.
    std::vector<LeftOut> take_over_loses;
};

constexpr size_t kMaxErrors = 20;  // the last build's errors the card lists

// The panel for `game` at its next start; `server_folder`: a dedicated server's folder (its own data files and
// emf\content\server, as the loader there takes them). `disk_use`: walk
// the copy for its own bytes (the Mods tab's scan, every few seconds, does not).
ContentPanel content_panel(const std::wstring& game, bool server_folder, bool disk_use = true);
// The panel as a JSON object for the page (every text as data).
std::string content_panel_json(const ContentPanel& p);

// EML's dbghelp.dll set aside by EMF (E5) is recorded in emf\moved.txt as "set-aside<TAB><name><TAB>dbghelp.dll" (CRLF
// lines, next to the installer's "moved<TAB><name>" lines), so that EMF's uninstall (package/uninstall.ps1) puts it
// back. The names those lines give (in the game folder), whether or not the file is still there.
std::vector<std::string> content_set_aside_names(const std::wstring& game);

// "" when EMF can take over from EML in `game` (E5): its dbghelp.dll is EML's (by the folder's emf\known_tools.txt, as
// the loader reads it), emf_content.dll is installed, [content] mode is not off, and no EML or ShroudForge DLL in
// dinput8-chain.ini would still apply the content; else why not, for the player.
std::string content_take_over_problem(const std::wstring& game);

}  // namespace emf::app
