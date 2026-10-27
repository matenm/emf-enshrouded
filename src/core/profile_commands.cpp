#include "profile_commands.h"

#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>

#include "app/game_folder.h"
#include "app/profiles.h"
#include "commands.h"
#include "profiles.h"
#include "settings.h"

namespace emf::core {

namespace {

// The folder scan keeps a cache of DLL facts that is not thread-safe, and two loads must not interleave: the command,
// the picker's reads and its actions take turns.
std::mutex g_profile_lock;

const char* kUsage = "usage: profile | profile save <name> [--settings] [--replace] | profile load <name>";

// In game EMF mods' switches and the settings a profile carries go through the live store: the Mods tab shows them at
// once, live settings apply at once and mods hear about the change (as with "set <mod> enabled 1"). A setting the store
// does not know (a mod the loader did not declare) is written to its ini as EMF.exe would; EML and chain switches are
// EMF.exe's own (set_mod_enabled), with what the scan knew.
class LiveSwitcher : public app::FolderSwitcher {
public:
    LiveSwitcher(std::wstring game, app::FolderFacts facts, SettingsStore* store)
        : FolderSwitcher(std::move(game), facts), store_(store) {}
    Result set_enabled(const std::string& source, const std::string& id, bool on) override {
        if (source != "emf" || !store_) return FolderSwitcher::set_enabled(source, id, on);
        if (!facts_ || !facts_->emf_mods.count(id)) return {false, "There is no EMF mod \"" + id + "\" in emf\\mods."};
        if (!store_->set_enabled(id, on)) return {false, "Could not write emf\\config\\" + id + ".ini."};
        return {true, {}};
    }
    Result set_setting(const std::string& mod, const std::string& key, const std::string& value) override {
        if (!store_ || !store_->has(mod, key)) return FolderSwitcher::set_setting(mod, key, value);
        std::string why;
        switch (store_->set(mod, key, value, &why)) {
            case SettingsStore::SetResult::Ok:
                return {true, {}};
            case SettingsStore::SetResult::Unchanged:
                return {true, {}, true};
            default:
                return {false, why.empty() ? "refused" : why};
        }
    }

private:
    SettingsStore* store_;
};

std::string now_of(const ProfileCommandContext& ctx) { return ctx.now ? ctx.now() : app::profile_time_now(); }

std::string mods_text(size_t n) { return std::to_string(n) + (n == 1 ? " mod" : " mods"); }

app::FolderState scan(const ProfileCommandContext& ctx) { return app::scan_mod_folder(ctx.game, app::ScanDepth::Profiles); }

std::vector<std::string> list_lines(ProfileCommandContext& ctx) {
    const app::FolderState s = scan(ctx);
    if (!s.game_found) return {"profile: EMF does not see the game folder here"};
    const app::ProfilesOverview o = app::profiles_overview(ctx.game, s);
    if (o.list.empty())
        return {"no profiles yet: profile save <name> saves the mods' switches as one (--settings with their settings)"};
    std::vector<std::string> out;
    out.push_back(std::to_string(o.list.size()) + " profile(s) in emf\\profiles:");
    for (const auto& p : o.list) {
        if (!p.error.empty()) {
            out.push_back("  " + p.slug + ".json: not usable (" + p.error + ")");
            continue;
        }
        std::string line = "  " + p.name;
        if (p.slug == o.current) line += o.changed ? " (current, changed)" : " (current)";
        line += ": " + mods_text(p.mods);
        if (p.with_settings) line += ", with settings";
        if (!p.created.empty()) line += ", saved " + p.created;
        out.push_back(line);
    }
    if (!s.read_error.empty()) out.push_back("note: " + s.read_error);
    out.push_back("profile load <name> switches to one (at the next game start); profile save <name> saves one");
    return out;
}

ProfileAnswer save_answer(ProfileCommandContext& ctx, const std::string& name, bool settings, bool replace) {
    ProfileAnswer a;
    const std::string why = profile_name_problem(name);
    if (!why.empty()) {
        a.ok = false;
        a.lines = {"refused: " + why};
        return a;
    }
    const app::FolderState s = scan(ctx);
    const ProfileSave r = app::save_folder_profile(ctx.game, s, name, settings, replace, now_of(ctx), ctx.emf_version);
    if (r.exists) {
        a.ok = false;
        a.exists = true;
        a.lines = {"a profile named \"" + r.existing_name + "\" exists already: profile save " + trim_profile_name(name) +
                   " --replace replaces it"};
        return a;
    }
    if (!r.ok) {
        a.ok = false;
        a.lines = {"refused: " + r.error};
        return a;
    }
    const auto p = ProfileStore(app::profiles_dir(ctx.game)).read(r.slug);
    const size_t n = p.profile ? p.profile->mods.size() : 0;
    a.lines = {"saved profile \"" + trim_profile_name(name) + "\" (" + mods_text(n) + (settings ? ", with settings" : "") +
               ") in emf\\profiles\\" + r.slug + ".json"};
    return a;
}

// The last line of a load: what happens at the next start, and what will still not be as the profile has it.
std::string closing_line(const ApplyReport& r) {
    const size_t mods = r.failed.size() + r.missing.size() + r.read_only.size() + r.recorded_read_only.size();
    const size_t settings = r.settings_failed.size();
    std::string still;
    if (mods) still = mods_text(mods) + " will still differ from the profile";
    if (settings)
        still += (still.empty() ? "" : ", and ") + std::to_string(settings) + (settings == 1 ? " setting" : " settings") +
                 " could not be set";
    if (r.switched.empty() && r.settings_set == 0)
        return still.empty() ? "nothing to switch: the mods are as the profile has them"
                             : "nothing was switched; " + still + " (see above)";
    return "restart the game to apply (EMF and the other loaders read the switches at the start)" +
           (still.empty() ? std::string() : "; " + still + " (see above)");
}

ProfileAnswer load_answer(ProfileCommandContext& ctx, const std::string& name) {
    ProfileAnswer out;
    const ProfileStore store(app::profiles_dir(ctx.game));
    const auto slug = store.find(name);
    if (!slug) {
        out.ok = false;
        out.lines = {"no profile \"" + trim_profile_name(name) + "\" (profile lists them)"};
        return out;
    }
    const app::FolderState s = scan(ctx);
    LiveSwitcher sw(ctx.game, app::folder_facts(s), ctx.settings);
    const app::ProfileApply a = app::apply_saved_profile(ctx.game, s, *slug, sw);
    if (!a.ok) {
        out.ok = false;
        out.lines = {"profile: " + a.error};
        return out;
    }
    out.ok = a.report.ok();
    out.switched = a.report.switched.size() + a.report.settings_set;
    out.lines = apply_report_lines(a.report);
    out.lines.push_back(closing_line(a.report));
    return out;
}

// Runs `fn` with the lock, turning an exception into an answer.
template <class Fn>
ProfileAnswer guarded(Fn fn) {
    ProfileAnswer failed;
    failed.ok = false;
    try {
        std::lock_guard<std::mutex> l(g_profile_lock);
        return fn();
    } catch (const std::exception& e) {
        failed.lines = {std::string("profile: stopped with an error: ") + e.what()};
    } catch (...) {
        failed.lines = {"profile: stopped with an error"};
    }
    return failed;
}

void cmd_profile(EmfWorld*, int argc, const char* const* argv, void* user) {
    auto& ctx = *static_cast<ProfileCommandContext*>(user);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    if (!ctx.run) {
        for (const auto& line : run_profile_command(ctx, args)) command_reply_line(line);
        return;
    }
    // the folder is read and written on the worker; the answer goes where the command came from
    ctx.run([&ctx, args, reply = current_reply()] {
        const auto lines = run_profile_command(ctx, args);
        if (reply)
            for (const auto& line : lines) reply(line);
    });
}

// One thread for the whole game, made with the first job and never ended (it may outlive everything else at exit).
class ProfileWorker {
public:
    void post(std::function<void()> job) {
        std::lock_guard<std::mutex> l(m_);
        jobs_.push_back(std::move(job));
        if (!started_) {
            started_ = true;
            std::thread([this] { loop(); }).detach();
        }
        cv_.notify_one();
    }

private:
    void loop() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [this] { return !jobs_.empty(); });
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            try {
                job();
            } catch (...) {  // a job's own failure; the next one still runs
            }
        }
    }
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    bool started_ = false;
};

}  // namespace

std::vector<std::string> run_profile_command(ProfileCommandContext& ctx, const std::vector<std::string>& args) {
    if (args.empty() || (args.size() == 1 && args[0] == "list"))
        return guarded([&] { return ProfileAnswer{list_lines(ctx), false, true, 0}; }).lines;
    const std::string& verb = args[0];
    bool settings = false, replace = false;
    std::string name;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--settings") settings = true;
        else if (args[i] == "--replace") replace = true;
        else name += (name.empty() ? "" : " ") + args[i];
    }
    if (verb == "save" && !name.empty()) return save_profile_now(ctx, name, settings, replace).lines;
    if (verb == "load" && !name.empty() && !settings && !replace) return load_profile_now(ctx, name).lines;
    return {kUsage};
}

ProfileAnswer save_profile_now(ProfileCommandContext& ctx, const std::string& name, bool settings, bool replace) {
    return guarded([&] { return save_answer(ctx, name, settings, replace); });
}

ProfileAnswer load_profile_now(ProfileCommandContext& ctx, const std::string& name) {
    return guarded([&] { return load_answer(ctx, name); });
}

ProfilePicker read_profile_picker(ProfileCommandContext& ctx) {
    ProfilePicker out;
    try {
        std::lock_guard<std::mutex> l(g_profile_lock);
        const app::FolderState s = scan(ctx);
        const app::ProfilesOverview o = app::profiles_overview(ctx.game, s);
        for (const auto& p : o.list)
            if (p.error.empty()) out.entries.push_back({p.slug, p.name});
        out.current = o.current_name;
        out.current_slug = o.current;
        out.changed = o.changed;
    } catch (...) {
    }
    return out;
}

ProfileRunner profile_worker_runner() {
    static ProfileWorker* worker = new ProfileWorker;  // never deleted: its thread runs until the game ends
    return [](std::function<void()> job) { worker->post(std::move(job)); };
}

bool add_profile_commands(CommandCenter& center, ProfileCommandContext& ctx) {
    return center.add("profile",
                      "[save <name> [--settings] [--replace] | load <name>]: mod profiles (the mods' switches by name; "
                      "a load applies at the next game start)",
                      cmd_profile, &ctx, nullptr, true);
}

}  // namespace emf::core
