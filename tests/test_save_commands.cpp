// The console's saves.backup and saves.list (src/core/save_commands.h) against a fake PC, and through the command
// center: refused while a world runs, made at the main menu, listed newest first.
#include <doctest.h>

#include <string>
#include <vector>

#include "core/commands.h"
#include "core/save_commands.h"
#include "save_test_files.h"
#include "saves/backup.h"
#include "saves/save_info.h"

using namespace emf::core;
using save_test::FakePc;

namespace {
std::string narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += static_cast<char>(c < 128 ? c : '?');
    return s;
}
std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += x + "\n";
    return s;
}
}  // namespace

TEST_CASE("save commands: saves.backup only at the main menu") {
    FakePc pc(L"cmd");
    pc.usual();
    SaveCommandContext ctx;
    ctx.roots = pc.roots;
    ctx.emf_version = "9.9.9";
    int64_t t = 1800000000;
    ctx.now = [&] { return t; };

    auto out = joined(run_saves_backup(ctx, "x", true));
    CHECK(out.find("not now") != std::string::npos);
    CHECK(out.find("main menu") != std::string::npos);
    CHECK(emf::saves::list_backups(pc.roots.store).empty());

    out = joined(run_saves_backup(ctx, "before the boss", false));
    CHECK(out.find("backed up 8 save files") != std::string::npos);
    const auto list = emf::saves::list_backups(pc.roots.store);
    REQUIRE(list.size() == 1);
    CHECK(list[0].kind == "manual");
    CHECK(list[0].label == "before the boss");
    CHECK(list[0].made_by == "console");

    t += 60;
    run_saves_backup(ctx, "", false);
    out = joined(run_saves_list(ctx));
    CHECK(out.find("2 backups") != std::string::npos);
    CHECK(out.find(emf::saves::display_path(pc.roots.store)) != std::string::npos);  // %LOCALAPPDATA%\... when it is there
    CHECK(out.find("manual") != std::string::npos);
    CHECK(out.find("\"before the boss\"") != std::string::npos);
    CHECK(out.find("Steam: character slot 1, world slot 1") != std::string::npos);
    // newest first
    CHECK(out.find("\"before the boss\"") > out.find("manual"));
    CHECK(out.find("restore") != std::string::npos);  // says how to restore
}

TEST_CASE("save commands: no saves, no store") {
    FakePc pc(L"cmd-none");
    SaveCommandContext ctx;
    ctx.roots = pc.roots;
    CHECK(joined(run_saves_backup(ctx, "", false)).find("no save files") != std::string::npos);
    CHECK(joined(run_saves_list(ctx)).find("no backups yet") != std::string::npos);
    SaveCommandContext none;
    CHECK(joined(run_saves_backup(none, "", false)).find("no folder") != std::string::npos);
}

TEST_CASE("save commands: through the command center") {
    FakePc pc(L"cmd-center");
    pc.usual();
    CommandCenter center;
    SaveCommandContext ctx;
    ctx.roots = pc.roots;
    REQUIRE(add_save_commands(center, ctx));
    std::vector<std::string> lines;
    // at the main menu (no world): it runs
    center.run_without_world("saves.backup \"my label\"", [&](const std::string& s) { lines.push_back(s); }, "needs a world");
    CHECK(joined(lines).find("backed up") != std::string::npos);
    REQUIRE(emf::saves::list_backups(pc.roots.store).size() == 1);
    CHECK(emf::saves::list_backups(pc.roots.store)[0].label == "my label");
    // with a world: refused
    lines.clear();
    center.enqueue("saves.backup");
    center.run_pending(reinterpret_cast<EmfWorld*>(0x1000), [&](const std::string& s) { lines.push_back(s); });
    CHECK(joined(lines).find("not now") != std::string::npos);
    CHECK(emf::saves::list_backups(pc.roots.store).size() == 1);
    lines.clear();
    center.run_without_world("saves.list", [&](const std::string& s) { lines.push_back(s); }, "needs a world");
    CHECK(joined(lines).find("1 backup") != std::string::npos);
}

TEST_CASE("save commands: saves.list stays out of the log, and no logged reply names a user folder or a character") {
    FakePc pc(L"cmd-private");
    pc.usual();
    emf::saves::SaveInfo info;
    emf::saves::SlotInfo astrid;
    astrid.storage = "steam";
    astrid.steam_id = "111";
    astrid.type = "character";
    astrid.slot = 0;
    astrid.name = "Astrid";
    astrid.level = 8;
    info.slots.push_back(astrid);
    REQUIRE(emf::saves::write_save_info(pc.roots.info, info));
    wchar_t profile_w[MAX_PATH] = {};
    GetEnvironmentVariableW(L"USERPROFILE", profile_w, MAX_PATH);
    const std::string profile = narrow(profile_w);
    for (bool worker : {false, true}) {
        CAPTURE(worker);
        SaveCommandContext ctx;
        ctx.roots = pc.roots;
        std::vector<std::function<void()>> jobs;
        if (worker) ctx.run = [&](std::function<void()> job) { jobs.push_back(std::move(job)); };
        CommandCenter center;
        REQUIRE(add_save_commands(center, ctx));
        struct Line {
            std::string text;
            bool logged;  // what core.cpp's sink writes to emf_core.log: every line that is not private
        };
        std::vector<Line> lines;
        const ReplyFn sink = [&](const std::string& s) { lines.push_back({s, !reply_is_private()}); };
        center.run_without_world("saves.backup", sink, "needs a world");
        center.run_without_world("saves.list", sink, "needs a world");
        for (auto& j : jobs) j();  // the worker's replies come later, on another call stack
        bool listed_name = false;
        for (const auto& l : lines) {
            if (l.text.find("Astrid") != std::string::npos) listed_name = true;
            if (!l.logged) continue;
            CHECK_MESSAGE(l.text.find("Astrid") == std::string::npos, l.text);
            if (!profile.empty()) CHECK_MESSAGE(l.text.find(profile) == std::string::npos, l.text);
        }
        CHECK(listed_name);  // the player still sees the names in the console
        size_t list_lines = 0;
        for (const auto& l : lines) list_lines += l.text.find("in all, in ") != std::string::npos && !l.logged;
        CHECK(list_lines == 1);
    }
}

TEST_CASE("save commands: saves.backup refuses in someone else's game too") {
    FakePc pc(L"cmd-client");
    pc.usual();
    SaveCommandContext ctx;
    ctx.roots = pc.roots;
    ctx.in_a_world = [] { return true; };  // a client world ticks: the game writes the character file
    CommandCenter center;
    REQUIRE(add_save_commands(center, ctx));
    std::vector<std::string> lines;
    center.run_without_world("saves.backup", [&](const std::string& s) { lines.push_back(s); }, "x");
    CHECK(joined(lines).find("not now") != std::string::npos);
    CHECK(emf::saves::list_backups(pc.roots.store).empty());
}
