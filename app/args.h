#pragma once
// EMF.exe's command line, and the hand-off of an nxm:// link to an EMF.exe that already runs. --smoke, --capture,
// --eval, --devtools, --nexus-fixture and --probe-wait are test switches: they need EMF_TEST=1 (finalize_args).
//   EMF.exe [--game <dir>] [--nxm <link> | <link>] [--smoke <out.json>] [--capture <out.png>] [--select <source>:<id>]
//           [--page <tab>] [--eval <js>] [--devtools] [--nexus-fixture <dir>] [--probe-wait <ms>]
//   EMF.exe --release-nxm      (no window) when this EMF.exe opens nxm:// links, give them back; uninstall.ps1 runs it
//   EMF.exe --stop-server <pid> (no window) send Ctrl+C to that enshrouded_server.exe's console (server_process.h);
//                              EMF.exe runs it as a process of its own to stop a dedicated server cleanly
//   EMF.exe --stop-server-break <pid>   the same with Ctrl+Break (when Ctrl+C did not take)
//   ... --shared-ok            send it even when other programs share the server's console (they get it too)
// Windows starts `EMF.exe --nxm "<link>"` when the player clicks Mod Manager Download and EMF is the nxm handler
// (nxm_handler.h); a bare nxm:// argument works too. When another EMF.exe already runs, the new one sends it the link
// in a WM_COPYDATA message (dwData = kHandoffTag, the link as UTF-16) and exits.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace emf::app {

struct AppArgs {
    std::wstring game, nxm, smoke, capture, select, page, eval, nexus_fixture;
    bool devtools = false;
    bool release_nxm = false;    // --release-nxm
    bool stop_server = false;    // --stop-server <pid> or --stop-server-break <pid>
    bool stop_break = false;     // --stop-server-break: Ctrl+Break instead of Ctrl+C
    bool stop_shared_ok = false; // --shared-ok: send it although other programs share the server's console
    unsigned long stop_pid = 0;  // its pid (0 when the value is not a number)
    unsigned probe_wait = 1000;  // ms, 100 to 10000
    bool link_seen = false;      // an --nxm switch (with any value, also an empty one) or a bare nxm: argument came
};

// The arguments after the program's name. Unknown switches are ignored; a switch without its value is ignored; an empty
// --nxm value is not taken as the link (but still counts as one: link_seen).
AppArgs parse_app_args(const std::vector<std::wstring>& args);
// When a link came (link_seen), everything but the link is dropped, whatever the link's value: a link with raw quotes
// in it could otherwise add switches to the command Windows runs ("EMF.exe" --nxm "%1").
void keep_only_link(AppArgs& args);
// What EMF.exe uses: without EMF_TEST=1 (`testing` false) keep_only_link, and the test switches are dropped: --smoke and
// --capture (they write files), --eval, --devtools, --nexus-fixture, --probe-wait. With EMF_TEST=1 everything stays
// (the smoke test combines a link with its own switches).
void finalize_args(AppArgs& args, bool testing);

constexpr uintptr_t kHandoffTag = 0x454D464E;  // "EMFN"
constexpr size_t kMaxHandoffChars = 4096;

// The link in a WM_COPYDATA hand-off: the right tag, whole UTF-16 characters, an nxm:// link of at most
// kMaxHandoffChars characters without a NUL inside (one at the end is allowed). nullopt for anything else.
std::optional<std::wstring> decode_handoff(uintptr_t tag, const void* data, size_t bytes);

}  // namespace emf::app
