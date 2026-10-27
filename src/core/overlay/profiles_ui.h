#pragma once
// The in-game Mods tab's profile picker (docs/app.md "Profiles"): the game folder's profiles with the current
// one ("(changed)" when a switch it lists differs), Load and Save as (with "with settings", and a Replace question for a
// name that is taken), the same as the console's "profile" command (profile_commands.h). Reading the folder and
// switching run on the core's profile worker (profile_worker_runner), never on the render thread; the bar shows the
// last answer. A load applies at the next game start.
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "core/profile_commands.h"

namespace emf::core {

class ProfileBar {
public:
    // Runs work away from the render thread (the profile worker in game; tests run it at once).
    using Runner = ProfileRunner;
    ProfileBar(ProfileCommandContext& ctx, Runner run) : ctx_(ctx), run_(std::move(run)) {}

    struct View {
        ProfilePicker picker;
        bool read = false;                // the profiles were read at least once
        bool busy = false;                // a read, a load or a save runs
        std::vector<std::string> answer;  // the last load's or save's lines
        bool failed = false;              // ... and something was refused or failed
        std::string replace;              // a save found this name taken: ask, then save it with replace
        bool replace_settings = false;
        bool restart_pending = false;     // a load switched something this session: it applies at the next start
    };
    View view() const;
    // Reads the profiles again when the last read is older than `max_age_ms` (and nothing runs).
    void refresh(uint64_t now_ms, uint64_t max_age_ms = 3000);
    // Ignored while something runs (the bar greys its buttons out then). `name`: a display name or a file name (slug).
    void load(const std::string& name);
    void save(const std::string& name, bool settings, bool replace);
    void dismiss();  // forgets the answer and the question

private:
    bool begin();  // false while something runs
    // Runs `work` on the runner; whatever it throws, busy is cleared and the answer says it stopped.
    void start(std::function<void(View&)> work, bool with_answer);
    void finish(View next_fields, bool with_answer);
    ProfileCommandContext& ctx_;
    Runner run_;
    mutable std::mutex m_;
    View v_;
    uint64_t read_ms_ = 0;
};

// What the bar keeps between frames (render thread).
struct ProfileBarUi {
    std::string pick;      // the profile the combo shows, by its file name ("": the current one)
    char name[161] = {};   // Save as: 40 characters of any script, 4 bytes each, and the end
    bool settings = false;
};
void draw_profile_bar(ProfileBar& bar, ProfileBarUi& ui, uint64_t now_ms);

// The bar the Mods tab draws at its top (set once by the core; none: nothing is drawn).
void set_mods_tab_profile_bar(ProfileBar* bar);
// Draws it, if there is one, with the tab's own UI state (render thread).
void draw_mods_tab_profile_bar(uint64_t now_ms);

}  // namespace emf::core
