// The console's "profile" command over a fake game folder: list, save (never replacing without --replace, settings
// only with --settings), load (switches, then "restart the game to apply"; settings through the live store), and the
// in-game picker's view.
#include <windows.h>

#include <doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <string>
#include <vector>

#include "app/game_folder.h"
#include "app/profiles.h"
#include "app_test_files.h"
#include "core/commands.h"
#include "core/manifest.h"
#include "core/profile_commands.h"
#include "core/profiles.h"
#include "core/settings.h"

using namespace emf::core;
using app_test::TempGame;

namespace {

const char* kClock = R"({"id":"clock","name":"Clock","version":"1.1.0",
  "settings":[{"key":"size","type":"int","default":10,"min":1,"max":20,"live":true}]})";

struct Fixture {
    TempGame g{L"profcmd"};
    CommandCenter center;
    FileIniBackend ini;
    SettingsStore store;
    ProfileCommandContext ctx;
    std::vector<std::string> out;
    Fixture() : ini(g.dir + L"\\emf\\config"), store(&ini) {
        g.write(L"dinput8.dll", "MZ EMF-LOADER-7d3c9f fake");
        g.write(L"emf\\emf_core.dll", std::string("MZ emf_core 0.3.0\0 more", 23));
        g.write(L"dbghelp.dll", "EML_LOG_FILE_PATH");
        g.write(L"emf\\known_tools.txt", "eml | EML (Enshrouded Mod Loader) | text:EML_LOG_FILE_PATH\n");
        g.write(L"emf\\mods\\clock\\mod.json", kClock);
        g.write(L"emf\\mods\\clock\\clock.dll", "dll");
        g.write(L"mods\\foo\\mod.json", R"({"id":"foo","name":"Foo","version":"1.0.0"})");
        g.write(L"dinput8-chain.ini", "[Chain]\r\ncreative.dll\r\n");
        g.write(L"creative.dll", "x");
        const auto m = parse_manifest(kClock);
        REQUIRE(m.manifest);
        for (const auto& d : m.manifest->settings) store.declare("clock", d);  // as the loader does at the start
        ctx.game = g.dir;
        ctx.settings = &store;
        ctx.emf_version = "0.3.0";
        ctx.now = [] { return std::string("2026-10-26 21:00"); };
        REQUIRE(add_profile_commands(center, ctx));
    }
    // A line typed on the main menu (no world): "profile" runs at once.
    void run(const std::string& line) {
        out.clear();
        center.run_without_world(line, [this](const std::string& s) { out.push_back(s); }, "not run");
    }
    bool said(const std::string& part) const {
        for (const auto& l : out)
            if (l.find(part) != std::string::npos) return true;
        return false;
    }
    std::string all() const {
        std::string s;
        for (const auto& l : out) s += l + "\n";
        return s;
    }
};

std::vector<std::string> names_of(const ProfilePicker& p) {
    std::vector<std::string> out;
    for (const auto& e : p.entries) out.push_back(e.name);
    return out;
}

bool is_on(const std::wstring& game, const std::string& source, const std::string& id) {
    for (const auto& m : emf::app::scan_game_folder(game).mods)
        if (m.source == source && m.id == id) return m.enabled;
    FAIL("no such mod");
    return false;
}

}  // namespace

TEST_CASE("profile command: lists nothing at first and says how to save one") {
    Fixture f;
    f.run("profile");
    CHECK_MESSAGE(f.said("no profiles yet"), f.all());
    CHECK(f.said("profile save <name>"));
}

TEST_CASE("profile command: save writes the profile; the same name again needs --replace") {
    Fixture f;
    f.run("profile save Building");
    CHECK_MESSAGE(f.said("saved profile \"Building\""), f.all());
    CHECK(f.said("3 mods"));
    REQUIRE(f.g.has(L"emf\\profiles\\building.json"));
    const auto p = ProfileStore(emf::app::profiles_dir(f.g.dir)).read("building");
    REQUIRE(p.profile);
    CHECK(p.profile->created == "2026-10-26 21:00");
    CHECK(p.profile->emf_version == "0.3.0");
    CHECK_FALSE(p.profile->with_settings);
    REQUIRE(emf::app::set_mod_enabled(f.g.dir, "eml", "foo", false).ok);
    f.run("profile save building");
    CHECK_MESSAGE(f.said("exists already"), f.all());
    CHECK(f.said("--replace"));
    CHECK(ProfileStore(emf::app::profiles_dir(f.g.dir)).read("building").profile->mods[1].on);  // not replaced
    f.run("profile save building --replace");
    CHECK_MESSAGE(f.said("saved profile \"building\""), f.all());
    bool foo_on = true;
    const ProfileRead replaced = ProfileStore(emf::app::profiles_dir(f.g.dir)).read("building");
    REQUIRE(replaced.profile);
    for (const auto& m : replaced.profile->mods)
        if (m.id == "foo") foo_on = m.on;
    CHECK_FALSE(foo_on);
    f.run("profile save \"Big Base\" --settings");
    CHECK(f.said("with settings"));
    CHECK(ProfileStore(emf::app::profiles_dir(f.g.dir)).read("big-base").profile->with_settings);
    f.run("profile save a|b");
    CHECK(f.said("refused"));
    f.run("profile save");
    CHECK(f.said("usage"));
}

TEST_CASE("profile command: load switches everything back and says to restart the game") {
    Fixture f;
    f.run("profile save Vanilla+");
    REQUIRE(emf::app::set_mod_enabled(f.g.dir, "eml", "foo", false).ok);
    REQUIRE(emf::app::set_mod_enabled(f.g.dir, "chain", "creative.dll", false).ok);
    REQUIRE(emf::app::set_mod_enabled(f.g.dir, "emf", "clock", false).ok);
    f.run("profile");
    CHECK_MESSAGE(f.said("Vanilla+ (current, changed)"), f.all());
    f.run("profile load vanilla+");
    CHECK_MESSAGE(f.said("3 mods switched"), f.all());
    CHECK(f.said("restart the game to apply"));
    CHECK(is_on(f.g.dir, "eml", "foo"));
    CHECK(is_on(f.g.dir, "chain", "creative.dll"));
    CHECK(is_on(f.g.dir, "emf", "clock"));
    CHECK(f.g.read(L"dinput8-chain.ini") == "[Chain]\r\ncreative.dll\r\n");
    f.run("profile");
    CHECK(f.said("Vanilla+ (current)"));
    CHECK_FALSE(f.said("changed"));
    f.run("profile load nope");
    CHECK(f.said("no profile \"nope\""));
    f.run("profile frobnicate");
    CHECK(f.said("usage"));
}

TEST_CASE("profile command: a profile's settings go through the live store (the Mods tab shows them at once)") {
    Fixture f;
    REQUIRE(f.store.set("clock", "size", "15") == SettingsStore::SetResult::Ok);
    f.run("profile save Big --settings");
    REQUIRE(f.store.set("clock", "size", "4") == SettingsStore::SetResult::Ok);
    f.store.take_changes();
    f.run("profile load big");
    CHECK_MESSAGE(f.said("1 setting set"), f.all());
    CHECK(f.store.get("clock", "size") == "15");  // the live value
    const auto changes = f.store.take_changes();
    REQUIRE(changes.size() == 1);  // mods' on_setting callbacks hear about it
    CHECK(changes[0].second == "size");
    CHECK(f.g.read(L"emf\\config\\clock.ini").find("size=15") != std::string::npos);
}

TEST_CASE("profile command: the in-game picker's view: names, the current one, changed") {
    Fixture f;
    ProfilePicker p = read_profile_picker(f.ctx);
    CHECK(p.entries.empty());
    CHECK(p.current.empty());
    f.run("profile save Testing");
    f.run("profile save Building");
    p = read_profile_picker(f.ctx);
    CHECK(names_of(p) == std::vector<std::string>{"Building", "Testing"});
    CHECK(p.current == "Building");
    CHECK_FALSE(p.changed);
    REQUIRE(emf::app::set_mod_enabled(f.g.dir, "eml", "foo", false).ok);
    CHECK(read_profile_picker(f.ctx).changed);
}

// ---- edge cases ----

TEST_CASE("profile command: with a worker the command returns at once; the answer comes from the worker to the same place") {
    Fixture f;
    std::vector<std::function<void()>> jobs;
    f.ctx.run = [&](std::function<void()> job) { jobs.push_back(std::move(job)); };
    f.run("profile save Later");
    CHECK(f.out == std::vector<std::string>{"> profile save Later"});  // nothing read or written on this thread
    CHECK_FALSE(f.g.has(L"emf\\profiles\\later.json"));
    REQUIRE(jobs.size() == 1);
    jobs[0]();
    CHECK_MESSAGE(f.said("saved profile \"Later\""), f.all());
    CHECK(f.g.has(L"emf\\profiles\\later.json"));
}

TEST_CASE("profile command: a switch that fails is said, and the last line never claims there was nothing to switch") {
    Fixture f;
    f.run("profile save On");
    REQUIRE(emf::app::set_mod_enabled(f.g.dir, "chain", "creative.dll", false).ok);
    const std::wstring chain = f.g.dir + L"\\dinput8-chain.ini";
    REQUIRE(SetFileAttributesW(chain.c_str(), FILE_ATTRIBUTE_READONLY));  // the write fails
    f.run("profile load On");
    SetFileAttributesW(chain.c_str(), FILE_ATTRIBUTE_NORMAL);
    CHECK_MESSAGE(f.said("FAILED on: creative.dll"), f.all());
    CHECK_FALSE(f.said("nothing to switch"));
    CHECK(f.said("1 mod will still differ from the profile"));
    // only a missing mod: nothing was switched, and the line says the folder still differs
    f.g.write(L"emf\\profiles\\gone.json", R"({"name":"Gone","mods":[{"source":"eml","id":"gone","on":true}]})");
    f.run("profile load Gone");
    CHECK_FALSE(f.said("nothing to switch"));
    CHECK_MESSAGE(f.said("nothing was switched"), f.all());
    CHECK(f.said("1 mod will still differ from the profile"));
    // all as the profile has it
    f.run("profile load On");
    f.run("profile load On");
    CHECK_MESSAGE(f.said("nothing to switch: the mods are as the profile has them"), f.all());
}

TEST_CASE("profile command: an EMF mod switched by a profile goes through the live store (mods hear the switch)") {
    Fixture f;
    f.run("profile save On");
    REQUIRE(f.store.set_enabled("clock", false));
    f.store.take_changes();
    f.run("profile load On");
    CHECK_MESSAGE(f.said("switched on: Clock"), f.all());
    const auto changes = f.store.take_changes();
    REQUIRE(changes.size() == 1);
    CHECK(changes[0] == std::make_pair(std::string("clock"), std::string("enabled")));
    CHECK(f.store.enabled("clock", false));
}

TEST_CASE("profile command: settings of a mod the live store does not know (switched off, not declared) go to its ini") {
    Fixture f;
    f.g.write(L"emf\\mods\\extra\\mod.json", R"({"id":"extra","name":"Extra","version":"1.0.0",
      "settings":[{"key":"size","type":"int","default":3,"min":1,"max":9}]})");
    f.g.write(L"emf\\mods\\extra\\extra.dll", "dll");
    REQUIRE(emf::app::set_mod_setting(f.g.dir, "extra", "size", "7").ok);
    f.run("profile save Extra --settings");
    REQUIRE(emf::app::set_mod_setting(f.g.dir, "extra", "size", "2").ok);
    REQUIRE_FALSE(f.store.has("extra", "size"));
    f.run("profile load Extra");
    CHECK_MESSAGE(f.said("1 setting set"), f.all());
    CHECK(f.g.read(L"emf\\config\\extra.ini").find("size=7") != std::string::npos);
}

TEST_CASE("profile command: many threads at once take turns (the folder scan's cache is shared)") {
    Fixture f;
    f.g.mkdir(L"shroudforge");  // the scan then reads DLLs (its cache) also in the light mode
    f.g.write(L"emf\\known_tools.txt",
              "shroudforge | ShroudForge | text:shroudforge | beside:shroudforge | same-loader:dbghelp.dll\n"
              "eml | EML (Enshrouded Mod Loader) | text:EML_LOG_FILE_PATH\n");
    REQUIRE(save_profile_now(f.ctx, "A", false, false).ok);
    std::atomic<int> bad{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&, t] {
            for (int i = 0; i < 8; ++i) {
                if (t % 2) {
                    if (read_profile_picker(f.ctx).entries.empty()) ++bad;
                } else if (!load_profile_now(f.ctx, "A").ok) {
                    ++bad;
                }
            }
        });
    for (auto& th : threads) th.join();
    CHECK(bad == 0);
    CHECK(is_on(f.g.dir, "eml", "foo"));
}

TEST_CASE("profile worker: runs jobs one after another on one thread of its own, also after a job throws") {
    const auto run = profile_worker_runner();
    std::mutex m;
    std::condition_variable cv;
    std::vector<int> order;
    std::set<std::thread::id> threads;
    for (int i = 0; i < 5; ++i)
        run([&, i] {
            std::lock_guard<std::mutex> l(m);
            order.push_back(i);
            threads.insert(std::this_thread::get_id());
            cv.notify_all();
        });
    run([] { throw 42; });  // a job that throws does not end the worker
    run([&] {
        std::lock_guard<std::mutex> l(m);
        order.push_back(99);
        cv.notify_all();
    });
    std::unique_lock<std::mutex> l(m);
    REQUIRE(cv.wait_for(l, std::chrono::seconds(5), [&] { return order.size() == 6; }));
    CHECK(order == std::vector<int>{0, 1, 2, 3, 4, 99});
    CHECK(threads.size() == 1);
    CHECK(threads.count(std::this_thread::get_id()) == 0);
}
