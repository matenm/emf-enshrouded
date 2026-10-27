#pragma once
// The console's "profile" command (docs/app.md "Profiles"): lists, saves and loads mod profiles of the game
// folder EMF runs in, with the files and rules of EMF.exe's Mods tab (app/profiles.h): the folder is read with its scan
// (the light one: ScanDepth::Profiles) and every switch goes through set_mod_enabled with what that scan knew. Loading
// takes effect at the next game start ("restart the game to apply"). EMF mods' switches and the settings a profile
// carries go through the live settings store, so the Mods tab shows them, live settings apply at once and mods hear
// about the change. Works on the main menu too.
//   profile                                        the profiles, the current one and whether the mods changed since
//   profile save <name> [--settings] [--replace]   the mods' switches (and settings) as a profile; never replaces
//                                                  another one without --replace
//   profile load <name>                            switches every mod to the profile's state, then reports
// In game the work runs on a worker of its own (ProfileCommandContext::run), never on the render thread or a world's
// thread, and the answer goes back to where the command came from (the console, the log, a chat player).
#include <functional>
#include <string>
#include <vector>

namespace emf::core {

class CommandCenter;
class SettingsStore;

// Runs a job away from the caller's thread.
using ProfileRunner = std::function<void(std::function<void()>)>;

struct ProfileCommandContext {
    std::wstring game;                    // the game folder (enshrouded.exe's)
    SettingsStore* settings = nullptr;    // the live store (nullptr: switches and settings go to the ini only)
    std::string emf_version;
    std::function<std::string()> now;     // "2026-10-26 20:00" (local time when empty)
    ProfileRunner run;                    // where the command's work runs (empty: on the caller's thread; tests)
};

// Registers "profile" (it runs without a world too); `ctx` must stay alive as long as it can run.
bool add_profile_commands(CommandCenter& center, ProfileCommandContext& ctx);

// What "profile <args>" answers, line by line, on the caller's thread. One at a time: a second caller waits.
std::vector<std::string> run_profile_command(ProfileCommandContext& ctx, const std::vector<std::string>& args);

// A save or a load with what it answered (the command's lines), for the in-game Mods tab's picker. One at a time.
struct ProfileAnswer {
    std::vector<std::string> lines;
    bool exists = false;   // a save found another profile with that name: nothing was written (ask, then replace)
    bool ok = true;        // nothing was refused or failed
    size_t switched = 0;   // a load: mods switched and settings set (they take effect at the next start)
};
ProfileAnswer save_profile_now(ProfileCommandContext& ctx, const std::string& name, bool settings, bool replace);
// `name`: a display name or a profile's file name (slug).
ProfileAnswer load_profile_now(ProfileCommandContext& ctx, const std::string& name);

// For the in-game Mods tab's picker: the profiles (by file, with their display names), the current one and whether the
// mods changed since it was saved or loaded. Reads the folder (call it off the render thread).
struct ProfilePickerEntry {
    std::string slug, name;
};
struct ProfilePicker {
    std::vector<ProfilePickerEntry> entries;  // usable profiles, sorted by name
    std::string current;                      // its display name ("" none)
    std::string current_slug;
    bool changed = false;
};
ProfilePicker read_profile_picker(ProfileCommandContext& ctx);

// The worker EMF's core runs profile work on: one thread of its own for the whole game (made with the first job, never
// ended), jobs in the order they came; a job that throws is dropped and the next one runs.
ProfileRunner profile_worker_runner();

}  // namespace emf::core
