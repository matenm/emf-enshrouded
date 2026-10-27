// The in-game Mods tab's profile picker (src/core/overlay/profiles_ui.h): reading the profiles, Load, Save as with the
// Replace question, over a fake game folder; the work runs at once here (in game: off the render thread). And the bar
// drawn headless.
#include <windows.h>

#include <doctest.h>

#include <functional>
#include <string>
#include <vector>

#include "app/game_folder.h"
#include "app/profiles.h"
#include "app_test_files.h"
#include "core/overlay/profiles_ui.h"
#include "core/profile_commands.h"
#include "core/profiles.h"
#include "core/settings.h"
#include "imgui.h"

using namespace emf::core;
using app_test::TempGame;

namespace {

struct BarFixture {
    TempGame g{L"profbar"};
    FileIniBackend ini;
    SettingsStore store;
    ProfileCommandContext ctx;
    std::vector<std::function<void()>> queued;  // work waiting (deferred runs)
    bool defer = false;
    ProfileBar bar;
    BarFixture()
        : ini(g.dir + L"\\emf\\config"), store(&ini), bar(ctx, [this](std::function<void()> w) {
              if (defer) queued.push_back(std::move(w));
              else w();
          }) {
        g.write(L"dinput8.dll", "MZ EMF-LOADER-7d3c9f fake");
        g.write(L"emf\\emf_core.dll", std::string("MZ emf_core 0.3.0\0 more", 23));
        g.write(L"dbghelp.dll", "EML_LOG_FILE_PATH");
        g.write(L"emf\\known_tools.txt", "eml | EML (Enshrouded Mod Loader) | text:EML_LOG_FILE_PATH\n");
        g.write(L"mods\\foo\\mod.json", R"({"id":"foo","name":"Foo","version":"1.0.0"})");
        g.write(L"dinput8-chain.ini", "[Chain]\r\ncreative.dll\r\n");
        g.write(L"creative.dll", "x");
        ctx.game = g.dir;
        ctx.settings = &store;
        ctx.emf_version = "0.3.0";
        ctx.now = [] { return std::string("2026-10-26 21:00"); };
    }
    void run_queued() {
        auto q = std::move(queued);
        queued.clear();
        for (auto& w : q) w();
    }
};

bool foo_on(const std::wstring& game) {
    for (const auto& m : emf::app::scan_game_folder(game).mods)
        if (m.source == "eml" && m.id == "foo") return m.enabled;
    FAIL("no foo");
    return false;
}

std::vector<std::string> names_of(const ProfilePicker& p) {
    std::vector<std::string> out;
    for (const auto& e : p.entries) out.push_back(e.name);
    return out;
}

bool has(const std::vector<std::string>& lines, const std::string& part) {
    for (const auto& l : lines)
        if (l.find(part) != std::string::npos) return true;
    return false;
}

}  // namespace

TEST_CASE("profile bar: reads the profiles once, again only when older than the limit, never while busy") {
    BarFixture f;
    CHECK_FALSE(f.bar.view().read);
    f.bar.refresh(1000);
    ProfileBar::View v = f.bar.view();
    CHECK(v.read);
    CHECK(v.picker.entries.empty());
    REQUIRE(save_profile_now(f.ctx, "Testing", false, false).ok);  // saved elsewhere (the console)
    f.bar.refresh(2000);  // too soon
    CHECK(f.bar.view().picker.entries.empty());
    f.bar.refresh(4500);
    v = f.bar.view();
    CHECK(names_of(v.picker) == std::vector<std::string>{"Testing"});
    CHECK(v.picker.current == "Testing");
    f.defer = true;
    f.bar.refresh(9000);
    CHECK(f.bar.view().busy);
    f.bar.load("Testing");  // ignored while busy
    CHECK(f.queued.size() == 1);
    f.run_queued();
    CHECK_FALSE(f.bar.view().busy);
}

TEST_CASE("profile bar: Save as, then Load switches back and says to restart; the picker follows") {
    BarFixture f;
    f.bar.save("Vanilla+", false, false);
    ProfileBar::View v = f.bar.view();
    CHECK_FALSE(v.failed);
    CHECK(has(v.answer, "saved profile \"Vanilla+\""));
    CHECK(names_of(v.picker) == std::vector<std::string>{"Vanilla+"});
    CHECK(v.picker.current == "Vanilla+");
    CHECK_FALSE(v.picker.changed);
    REQUIRE(emf::app::set_mod_enabled(f.g.dir, "eml", "foo", false).ok);
    f.bar.refresh(100000);
    CHECK(f.bar.view().picker.changed);
    f.bar.load("Vanilla+");
    v = f.bar.view();
    CHECK_FALSE(v.failed);
    CHECK(has(v.answer, "1 mod switched"));
    CHECK(has(v.answer, "restart the game to apply"));
    CHECK(foo_on(f.g.dir));
    CHECK_FALSE(v.picker.changed);
    f.bar.dismiss();
    CHECK(f.bar.view().answer.empty());
}

TEST_CASE("profile bar: saving over another profile asks; Replace saves it, a bad name is refused") {
    BarFixture f;
    f.bar.save("Building", false, false);
    REQUIRE(emf::app::set_mod_enabled(f.g.dir, "eml", "foo", false).ok);
    f.bar.save("building", true, false);
    ProfileBar::View v = f.bar.view();
    CHECK(v.replace == "building");
    CHECK(v.replace_settings);
    f.bar.save(v.replace, v.replace_settings, true);
    v = f.bar.view();
    CHECK(v.replace.empty());
    CHECK(has(v.answer, "with settings"));
    f.bar.save("a|b", false, false);
    v = f.bar.view();
    CHECK(v.failed);
    CHECK(has(v.answer, "refused"));
    f.bar.load("nope");
    CHECK(f.bar.view().failed);
}

TEST_CASE("profile bar: drawn headless (no profiles, profiles, an answer, the Replace question) without ImGui errors") {
    BarFixture f;
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    ProfileBarUi ui;
    uint64_t now = 1000;
    auto frame = [&] {
        ImGui::NewFrame();
        ImGui::Begin("Mods");
        draw_profile_bar(f.bar, ui, now);
        ImGui::End();
        ImGui::Render();
        now += 16;
    };
    for (int i = 0; i < 3; ++i) frame();
    f.bar.save("Vanilla+", false, false);
    f.bar.save("Building", false, false);
    for (int i = 0; i < 3; ++i) frame();
    CHECK(ui.pick == "building");  // the current one (by its file name) until the player picks another
    f.bar.save("building", false, false);  // the question
    for (int i = 0; i < 3; ++i) frame();
    f.bar.load("Vanilla+");
    for (int i = 0; i < 3; ++i) frame();
    ImGui::DestroyContext(ctx);
    CHECK(emf_imgui_last_assert()[0] == '\0');  // a failed IM_ASSERT would also have raised (imgui_emf_config.h)
}

// ---- edge cases ----

TEST_CASE("profile bar: a switch that fails is shown as failed, and the status says the switches wait for a restart") {
    BarFixture f;
    f.bar.save("On", false, false);
    REQUIRE(emf::app::set_mod_enabled(f.g.dir, "chain", "creative.dll", false).ok);
    REQUIRE(emf::app::set_mod_enabled(f.g.dir, "eml", "foo", false).ok);
    const std::wstring chain = f.g.dir + L"\\dinput8-chain.ini";
    REQUIRE(SetFileAttributesW(chain.c_str(), FILE_ATTRIBUTE_READONLY));
    f.bar.load("on");
    SetFileAttributesW(chain.c_str(), FILE_ATTRIBUTE_NORMAL);
    const ProfileBar::View v = f.bar.view();
    CHECK(v.failed);
    CHECK(has(v.answer, "FAILED on: creative.dll"));
    CHECK_FALSE(has(v.answer, "nothing to switch"));
    CHECK(v.restart_pending);  // foo was switched: it takes effect at the next start
}

TEST_CASE("profile bar: a job that throws leaves the bar usable (busy is cleared, the answer says so)") {
    BarFixture f;
    f.ctx.now = []() -> std::string { throw 42; };  // not a std::exception: it reaches the bar's own guard
    f.bar.save("Boom", false, false);
    ProfileBar::View v = f.bar.view();
    CHECK_FALSE(v.busy);
    CHECK(v.failed);
    CHECK(has(v.answer, "stopped"));
    f.ctx.now = [] { return std::string("2026-10-26 21:00"); };
    f.bar.save("Fine", false, false);
    v = f.bar.view();
    CHECK_FALSE(v.failed);
    CHECK(has(v.answer, "saved profile \"Fine\""));
}

TEST_CASE("profile bar: profiles are told apart by their file, also two with the same name inside") {
    BarFixture f;
    f.g.write(L"emf\\profiles\\one.json", R"({"name":"Twin","mods":[]})");
    f.g.write(L"emf\\profiles\\two.json", R"({"name":"Twin","mods":[]})");
    f.bar.refresh(1000);
    const ProfileBar::View v = f.bar.view();
    REQUIRE(v.picker.entries.size() == 2);
    CHECK(v.picker.entries[0].slug == "one");
    CHECK(v.picker.entries[1].slug == "two");
    f.bar.load("two");  // by its file
    CHECK(has(f.bar.view().answer, "Twin"));
    CHECK(ProfileStore(emf::app::profiles_dir(f.g.dir)).current() == "two");
}

TEST_CASE("profile bar: the Save as name holds 40 characters of any script") {
    ProfileBarUi ui;
    CHECK(sizeof ui.name >= 161);
}

TEST_CASE("profile bar: with the core's own worker (as core.cpp wires it) the bar reads off the calling thread and settles") {
    BarFixture f;
    REQUIRE(save_profile_now(f.ctx, "Wired", false, false).ok);
    ProfileBar bar(f.ctx, profile_worker_runner());
    bar.refresh(1000);
    bool read = false;
    for (int i = 0; i < 500 && !read; ++i) {
        const ProfileBar::View v = bar.view();
        read = v.read && !v.busy;
        if (!read) Sleep(10);
    }
    REQUIRE(read);
    CHECK(names_of(bar.view().picker) == std::vector<std::string>{"Wired"});
    bar.load("wired");
    bool done = false;
    for (int i = 0; i < 500 && !done; ++i) {
        const ProfileBar::View v = bar.view();
        done = !v.busy && !v.answer.empty();
        if (!done) Sleep(10);
    }
    REQUIRE(done);
    CHECK(has(bar.view().answer, "nothing to switch"));
}
