#include "core/save_commands.h"

#include <windows.h>

#include "core/commands.h"
#include "saves/save_info.h"

namespace emf::core {

namespace {

const char* kind_word(const std::string& k) {
    return k == "auto" ? "automatic" : k == "before-restore" ? "before restore" : k.c_str();
}

constexpr size_t kListed = 20;

void cmd_backup(EmfWorld* world, int argc, const char* const* argv, void* user) {
    auto& ctx = *static_cast<SaveCommandContext*>(user);
    std::string label;
    for (int i = 1; i < argc; ++i) label += (label.empty() ? "" : " ") + std::string(argv[i]);
    // run_pending passes the world a host runs; someone else's game runs no command with a world, but its client world
    // ticks and the game writes the character file then: only truly at the main menu
    const bool running = world != nullptr || (ctx.in_a_world && ctx.in_a_world());
    if (!ctx.run || running) {
        for (const auto& line : run_saves_backup(ctx, label, running)) command_reply_line(line);
        return;
    }
    ctx.run([&ctx, label, reply = current_reply()] {
        const auto lines = run_saves_backup(ctx, label, false);
        if (reply)
            for (const auto& line : lines) reply(line);
    });
}

// The list names characters, worlds and levels: its lines go to the console, never to emf_core.log (PrivateReplies).
void cmd_list(EmfWorld*, int, const char* const*, void* user) {
    auto& ctx = *static_cast<SaveCommandContext*>(user);
    if (!ctx.run) {
        const PrivateReplies quiet;
        for (const auto& line : run_saves_list(ctx)) command_reply_line(line);
        return;
    }
    ctx.run([&ctx, reply = private_reply(current_reply())] {
        const auto lines = run_saves_list(ctx);
        if (reply)
            for (const auto& line : lines) reply(line);
    });
}

}  // namespace

std::vector<std::string> run_saves_backup(SaveCommandContext& ctx, const std::string& label, bool world_running) {
    if (ctx.roots.store.empty()) return {"saves.backup: EMF has no folder for backups on this PC"};
    if (world_running)
        return {"saves.backup: not now: a world is running, and the game writes its saves while it runs (every 30 "
                "seconds). Quit to the main menu, then run saves.backup again."};
    saves::BackupOptions o;
    o.kind = "manual";
    o.label = label.size() > 80 ? label.substr(0, 80) : label;
    o.made_by = "console";
    o.emf_version = ctx.emf_version;
    o.info = saves::read_save_info(ctx.roots.info).slots;
    o.now = ctx.now;
    const saves::BackupResult r = saves::make_backup(ctx.roots, o);
    if (r.nothing) return {"saves.backup: there are no save files to back up yet"};
    if (!r.ok) return {"saves.backup: failed: " + r.error};
    std::vector<std::string> out{"saves.backup: backed up " + std::to_string(r.backup.files.size()) + " save files (" +
                                 saves::size_text(r.backup.bytes()) + ") as " + r.backup.id};
    for (const auto& p : r.backup.problems) out.push_back("  not copied: " + p);
    out.push_back("saves.list shows every backup");
    return out;
}

std::vector<std::string> run_saves_list(SaveCommandContext& ctx) {
    if (ctx.roots.store.empty()) return {"saves.list: EMF has no folder for backups on this PC"};
    const auto list = saves::list_backups(ctx.roots.store);
    if (list.empty()) return {"saves.list: no backups yet (saves.backup makes one, at the main menu)"};
    uint64_t total = 0;
    for (const auto& b : list) total += b.bytes();
    std::vector<std::string> out{std::to_string(list.size()) + (list.size() == 1 ? " backup, " : " backups, ") +
                                 saves::size_text(total) + " in all, in " + saves::display_path(ctx.roots.store) + ":"};
    for (size_t i = 0; i < list.size() && i < kListed; ++i) {
        const auto& b = list[i];
        if (b.damaged) {
            out.push_back("  " + b.id + ": damaged (" + b.error + ")");
            continue;
        }
        std::string line = "  " + (b.time_local.empty() ? saves::local_time_text(b.time) : b.time_local) + "  " + kind_word(b.kind);
        if (!b.label.empty()) line += "  \"" + b.label + "\"";
        line += "  " + saves::size_text(b.bytes());
        const std::string summary = saves::backup_summary(b);
        if (!summary.empty()) line += ": " + summary;
        if (!b.problems.empty()) line += " (incomplete)";
        out.push_back(line);
    }
    if (list.size() > kListed) out.push_back("  ... and " + std::to_string(list.size() - kListed) + " older");
    out.push_back("To restore: close the game, then use EMF.exe's Saves tab, or copy the files from a backup's folder back "
                  "by hand (docs\\saves.md says how).");
    return out;
}

bool add_save_commands(CommandCenter& center, SaveCommandContext& ctx) {
    bool ok = center.add("saves.backup", "[label]: back up every save file now (at the main menu)", cmd_backup, &ctx, nullptr,
                         true);
    ok &= center.add("saves.list", ": the save backups, newest first, and where they are", cmd_list, &ctx, nullptr, true);
    return ok;
}

}  // namespace emf::core
