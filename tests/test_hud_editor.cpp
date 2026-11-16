// The HUD editor: settings, layouts from emf\hud, the gate, hooks once, edits published to the hooks, saves.
#include <doctest.h>

#include <windows.h>

#include <fstream>

#include "core/hud_editor/layout_store.h"
#include "core/profiles.h"
#include "hud_editor_rig.h"

using namespace emf::core;
using namespace emf::core::hud;
using hud_test::HudRig;

namespace {
void write_layout(HudRig& r, const std::string& name, size_t part, float x) {
    HudLayout l;
    l.name = name;
    l.elements[part].x = x;
    std::string err;
    REQUIRE(write_layout_file(r.dir, l, 1287369, &err));
}
}  // namespace

TEST_CASE("editor: a first start is on with an empty Default, hooked once, logged once") {
    HudRig r;
    HudEditor& e = r.make();
    const HudView v = e.view();
    CHECK(v.gate.state == GateState::On);
    CHECK(v.layout_name == "Default");
    CHECK(v.layouts == std::vector<std::string>{"Default"});
    CHECK(r.installs == 1);
    CHECK(e.snapshots().current()->active);
    CHECK(r.count_logged("HUD editor: on (layout \"Default\", 19 parts)") == 1);
    CHECK(r.files().empty());
    CHECK(r.writes == 0);
}

TEST_CASE("editor: the active layout comes from the setting and emf\\hud") {
    HudRig r;
    write_layout(r, "Default", 0, 1);
    write_layout(r, "Building", 3, 12.5f);
    r.set("active_layout", "Building");
    HudEditor& e = r.make();
    const HudView v = e.view();
    CHECK(v.layout_name == "Building");
    CHECK(v.elements[3].x == 12.5f);
    CHECK(v.elements[0].x == 0.0f);
    CHECK(v.layouts == std::vector<std::string>{"Building", "Default"});
    CHECK(e.snapshots().current()->edits[3].x_percent == 12.5f);
}

TEST_CASE("editor: broken files and notes are logged, the rest still load") {
    HudRig r;
    std::filesystem::create_directories(r.dir);
    std::ofstream(std::filesystem::path(r.dir) / L"bad.json") << "{";
    write_layout(r, "Default", 0, 2);
    HudEditor& e = r.make();
    CHECK(r.logged("HUD editor: emf\\hud\\bad.json: not JSON"));
    CHECK(e.view().elements[0].x == 2.0f);
}

TEST_CASE("editor: an unknown build is off, never hooks, and still keeps edits") {
    HudRig r;
    r.gate.build_known = false;
    HudEditor& e = r.make();
    CHECK(e.view().gate.state == GateState::UnknownBuild);
    CHECK(r.installs == 0);
    CHECK_FALSE(e.snapshots().current()->active);
    e.set_offset(0, 5, 5, Save::Now);
    const auto files = r.files();
    REQUIRE(files.size() == 1);
    CHECK(files[0].layout->elements[0].x == 5.0f);
    CHECK_FALSE(e.snapshots().current()->active);
}

TEST_CASE("editor: switched off at start hooks nothing until switched on") {
    HudRig r;
    r.set("enabled", "0");
    HudEditor& e = r.make();
    CHECK(e.view().gate.state == GateState::Disabled);
    CHECK(r.installs == 0);
    r.set("enabled", "1");
    e.tick();
    CHECK(e.view().gate.state == GateState::On);
    CHECK(r.installs == 1);
    CHECK(e.snapshots().current()->active);
    r.set("enabled", "0");
    e.tick();
    CHECK_FALSE(e.snapshots().current()->active);
    CHECK(r.installs == 1);
}

TEST_CASE("editor: ShroudHud keeps it off; 'use anyway' turns it on; a HUD mod loaded later turns it off") {
    {
        HudRig r;
        r.other_mod = "ShroudHud";
        HudEditor& e = r.make();
        CHECK(e.view().gate.state == GateState::OtherHudMod);
        CHECK(r.installs == 0);
        r.set("allow_with_shroudhud", "1");
        e.tick();
        CHECK(e.view().gate.state == GateState::On);
        CHECK(r.installs == 1);
    }
    {
        HudRig r;
        HudEditor& e = r.make();
        CHECK(e.view().gate.state == GateState::On);
        r.other_mod = "ShroudHud";
        r.now += 4999;
        e.tick();
        CHECK(e.view().gate.state == GateState::On);
        r.now += 1;
        e.tick();
        CHECK(e.view().gate.state == GateState::OtherHudMod);
        CHECK_FALSE(e.snapshots().current()->active);
    }
}

TEST_CASE("editor: a fault turns it off and is logged once") {
    HudRig r;
    HudEditor& e = r.make();
    r.fault = 0xC0000005;
    e.tick();
    e.tick();
    CHECK(e.view().gate.state == GateState::Faulted);
    CHECK_FALSE(e.snapshots().current()->active);
    CHECK(r.count_logged("exception 0xC0000005") == 1);
}

TEST_CASE("editor: a hook that fails is not tried again") {
    HudRig r;
    r.install_ok = false;
    r.install_why = "x";
    HudEditor& e = r.make();
    CHECK(e.view().gate.state == GateState::HookFailed);
    CHECK(e.view().gate.message.find("(x)") != std::string::npos);
    e.tick();
    e.tick();
    e.tick();
    CHECK(r.installs == 1);
    CHECK_FALSE(e.snapshots().current()->active);
}

// The view read a failed install as "not hooked yet" and called every part working, with no note.
TEST_CASE("editor: after a hook install that failed every part says it is off, and why") {
    HudRig r;
    r.install_ok = false;
    r.install_why = "x";
    HudEditor& e = r.make();
    const HudView v = e.view();
    REQUIRE(v.gate.state == GateState::HookFailed);
    CHECK_FALSE(v.scale_works);
    CHECK_FALSE(v.bars_work);
    CHECK_FALSE(v.colours_work);
    CHECK_FALSE(v.panel_works);
    CHECK_FALSE(v.grow_works);
    for (const std::string* note : {&v.scale_note, &v.bars_note, &v.colours_note, &v.panel_note, &v.grow_note}) {
        CAPTURE(*note);
        CHECK(note->find("could not be installed") != std::string::npos);
    }
}

TEST_CASE("editor: edits clamp, publish at once and save now") {
    HudRig r;
    HudEditor& e = r.make();
    e.set_offset(0, 150, -2.5f, Save::Now);
    CHECK(e.view().elements[0].x == 100.0f);
    CHECK(e.view().elements[0].y == -2.5f);
    CHECK(e.snapshots().current()->edits[0].x_percent == 100.0f);
    CHECK(e.snapshots().current()->edits[0].y_percent == -2.5f);
    CHECK(r.writes == 1);
    REQUIRE(r.files().size() == 1);
    CHECK(r.files()[0].layout->elements[0].x == 100.0f);
    e.nudge(0, -0.5f, 0, Save::Now);
    CHECK(e.view().elements[0].x == 99.5f);
    e.set_hidden(3, true, Save::Now);
    CHECK(e.snapshots().current()->edits[3].hidden);
    e.set_scale(1, 1.234f, Save::Now);
    CHECK(e.view().elements[1].scale == doctest::Approx(1.23f));
    CHECK(e.snapshots().current()->edits[1].scale == doctest::Approx(1.23f));
    e.set_scale(1, 9, Save::Now);
    CHECK(e.view().elements[1].scale == 2.0f);
    e.reset(0, Save::Now);
    CHECK(e.view().elements[0].is_default());
    e.set_offset(99, 1, 1, Save::Now);  // no such part: nothing happens
}

TEST_CASE("editor: Later edits are written once, 500 ms after the last one") {
    HudRig r;
    HudEditor& e = r.make();
    const uint64_t t = r.now;
    e.set_offset(0, 1, 1, Save::Later);
    r.now = t + 100;
    e.set_offset(0, 2, 1, Save::Later);
    r.now = t + 200;
    e.set_offset(0, 3, 1, Save::Later);
    CHECK(r.writes == 0);
    r.now = t + 699;
    e.tick();
    CHECK(r.writes == 0);
    r.now = t + 700;
    e.tick();
    CHECK(r.writes == 1);
    e.tick();
    CHECK(r.writes == 1);
    CHECK(r.files()[0].layout->elements[0].x == 3.0f);
}

TEST_CASE("editor: flush writes a pending Later save at once") {
    HudRig r;
    HudEditor& e = r.make();
    e.set_offset(0, 1, 1, Save::Later);
    e.flush();
    CHECK(r.writes == 1);
    e.flush();
    CHECK(r.writes == 1);
}

TEST_CASE("editor: settings changed elsewhere are followed at the next tick") {  // Review Focus 5
    HudRig r;
    write_layout(r, "Building", 3, 12.5f);
    HudEditor& e = r.make();
    r.set("active_layout", "Nope");
    e.tick();
    CHECK(e.view().layout_name == "Default");
    CHECK(r.count_logged("\"Nope\"") == 1);
    e.tick();
    CHECK(r.count_logged("\"Nope\"") == 1);
    r.set("active_layout", "Building");
    e.tick();
    CHECK(e.view().layout_name == "Building");
    CHECK(e.view().elements[3].x == 12.5f);
}

TEST_CASE("editor: a missing layout at start falls back to Default with one log line") {
    HudRig r;
    r.set("active_layout", "Gone");
    HudEditor& e = r.make();
    CHECK(e.view().layout_name == "Default");
    CHECK(r.logged("HUD editor: layout \"Gone\" is not in emf\\hud; using \"Default\""));
    CHECK(r.store.get(kEmfSettingsId, "hud_editor.active_layout") == "Gone");  // never written back
}

TEST_CASE("editor: view shows what the hooks saw") {
    HudRig r;
    HudEditor& e = r.make();
    r.live[4] = LiveInfo{true, 99, {786, 56, 512, 30}};
    const HudView v = e.view();
    CHECK(v.live[4].seen);
    CHECK(v.live[4].last_seen_ms == 99);
    CHECK(v.live[4].rect[0] == 786.0f);
    CHECK(v.live[4].rect[3] == 30.0f);
    CHECK_FALSE(v.live[0].seen);
}

TEST_CASE("editor: scale works, is not available, or stops after a mismatch (logged once); its values are kept") {
    HudRig r;
    HudEditor& e = r.make();
    CHECK(e.view().scale_works);
    CHECK(e.view().scale_note.empty());
    r.scale = ScaleState{false, false};
    CHECK_FALSE(e.view().scale_works);
    CHECK(e.view().scale_note == "Scale is not available on this game build; move and hide work.");
    e.set_scale(2, 1.5f, Save::Now);
    CHECK(e.view().elements[2].scale == 1.5f);
    r.scale = ScaleState{true, true};
    e.tick();
    e.tick();
    CHECK_FALSE(e.view().scale_works);
    CHECK(e.view().scale_note ==
          "Scale stopped for this session: the game's drawing did not match what EMF expects (see emf_core.log). Move and "
          "hide still work.");
    CHECK(r.count_logged("HUD editor: scale is off until the game restarts") == 1);
    CHECK(e.view().elements[2].scale == 1.5f);
}

TEST_CASE("editor: reset_all keeps the layout before as \"Before reset\"") {
    HudRig r;
    HudEditor& e = r.make();
    e.set_offset(0, 5, 5, Save::Now);
    const LayoutResult res = e.reset_all();
    CHECK(res.ok);
    CHECK(res.message ==
          "every part is back where the game draws it; the layout before is saved as \"Before reset\" (hud.layout load "
          "\"Before reset\" brings it back)");
    CHECK(e.view().elements[0].is_default());
    CHECK(e.snapshots().current()->edits[0].x_percent == 0.0f);
    bool found = false;
    for (const auto& f : r.files())
        if (f.slug == "before-reset") {
            found = true;
            REQUIRE(f.layout.has_value());
            CHECK(f.layout->name == "Before reset");
            CHECK(f.layout->elements[0].x == 5.0f);
        }
    CHECK(found);
    const auto again = e.reset_all();
    CHECK(again.ok);
    CHECK(again.message == "the HUD is already the game's own");
    const auto names = e.view().layouts;
    CHECK(names == std::vector<std::string>{"Before reset", "Default"});
}

TEST_CASE("editor: reset_all after hiding everything") {  // Review Focus 2
    HudRig r;
    HudEditor& e = r.make();
    for (size_t i = 0; i < kHudElementCount; ++i) e.set_hidden(i, true, Save::Now);
    e.reset_all();
    for (size_t i = 0; i < kHudElementCount; ++i) {
        CAPTURE(i);
        CHECK_FALSE(e.view().elements[i].hidden);
        CHECK_FALSE(e.snapshots().current()->edits[i].hidden);
    }
    for (const auto& f : r.files())
        if (f.slug == "before-reset") {
            REQUIRE(f.layout.has_value());
            for (size_t i = 0; i < kHudElementCount; ++i) CHECK(f.layout->elements[i].hidden);
        }
}

TEST_CASE("editor: reset_all on the \"Before reset\" layout itself writes no backup") {
    HudRig r;
    HudEditor& e = r.make();
    e.set_offset(0, 5, 5, Save::Now);
    e.reset_all();
    REQUIRE(e.load("before reset").ok);
    CHECK(e.view().elements[0].x == 5.0f);
    const size_t before = r.files().size();
    const LayoutResult res = e.reset_all();
    CHECK(res.message == "every part is back where the game draws it");
    CHECK(r.files().size() == before);
    CHECK(e.view().layout_name == "Before reset");
    CHECK(e.view().elements[0].is_default());
}

TEST_CASE("editor: save_as, load and remove") {
    HudRig r;
    HudEditor& e = r.make();
    e.set_offset(3, 7, 1, Save::Now);
    LayoutResult res = e.save_as("Building 2");
    CHECK(res.ok);
    CHECK(res.message == "saved as \"Building 2\"");
    bool has_file = false;
    for (const auto& f : r.files()) has_file = has_file || f.slug == "building-2";
    CHECK(has_file);
    CHECK(r.store.get(kEmfSettingsId, "hud_editor.active_layout") == "building-2");
    CHECK(e.view().layout_name == "Building 2");
    CHECK(e.view().layouts == std::vector<std::string>{"Building 2", "Default"});
    res = e.load("default");
    CHECK(res.ok);
    CHECK(res.message == "layout \"Default\" is in use");
    CHECK(e.view().layout_name == "Default");
    CHECK(e.view().elements[3].x == 7.0f);  // the Default layout got the first edit
    res = e.load("BUILDING-2");
    CHECK(e.view().layout_name == "Building 2");
    CHECK(r.store.get(kEmfSettingsId, "hud_editor.active_layout") == "building-2");
    res = e.load("nope");
    CHECK_FALSE(res.ok);
    CHECK(res.message == "no layout \"nope\"; layouts: Building 2, Default");
    res = e.remove("Building 2");
    CHECK(res.ok);
    CHECK(res.message == "layout \"Building 2\" deleted; \"Default\" is in use");
    CHECK(e.view().layout_name == "Default");
    CHECK(r.store.get(kEmfSettingsId, "hud_editor.active_layout") == "default");
    for (const auto& f : r.files()) CHECK(f.slug != "building-2");
    res = e.remove("nope");
    CHECK_FALSE(res.ok);
    res = e.save_as("a/b");
    CHECK_FALSE(res.ok);
    CHECK(res.message == profile_name_problem("a/b"));
}

TEST_CASE("editor: remove a layout that is not active keeps the active one") {
    HudRig r;
    HudEditor& e = r.make();
    e.save_as("Other");
    e.load("Default");
    const LayoutResult res = e.remove("other");
    CHECK(res.message == "layout \"Other\" deleted");
    CHECK(e.view().layout_name == "Default");
}

TEST_CASE("editor: save_as and load: names by slug") {  // Review Focus 3
    HudRig r;
    HudEditor& e = r.make();
    e.set_offset(0, 2, 2, Save::Now);
    LayoutResult res = e.save_as("Default ");
    CHECK(res.ok);
    CHECK(res.message == "saved as \"Default\" (replaced the layout of that name)");
    res = e.save_as("CON");
    CHECK(res.ok);
    const std::string slug = profile_slug("CON");
    CHECK(slug == "con-profile");
    bool has_file = false;
    for (const auto& f : r.files()) has_file = has_file || f.slug == "con-profile";
    CHECK(has_file);
    CHECK(e.load("con").ok);
    CHECK(e.view().layout_name == "CON");
}

TEST_CASE("editor: the page key toggles the page, the reset key resets") {
    HudRig r;
    HudEditor& e = r.make();
    r.down = {VK_F9};
    e.tick();
    CHECK(r.toggles == 1);
    e.tick();
    CHECK(r.toggles == 1);
    r.down = {};
    e.tick();
    r.down = {VK_F9};
    e.tick();
    CHECK(r.toggles == 2);
    r.down = {};
    e.tick();
    e.set_offset(0, 5, 5, Save::Now);
    r.down = {VK_PAUSE};
    e.tick();
    CHECK(e.view().elements[0].is_default());
    CHECK(r.logged("the reset key"));
}

TEST_CASE("editor: hotkeys follow their settings") {  // Review Focus 5
    HudRig r;
    HudEditor& e = r.make();
    r.set("key_page", "F8");
    e.tick();
    r.down = {VK_F8};
    e.tick();
    CHECK(r.toggles == 1);
    r.down = {VK_F9};
    e.tick();
    CHECK(r.toggles == 1);
    r.down = {};
    e.tick();
    r.set("key_page", "");
    e.tick();
    r.down = {VK_F8};
    e.tick();
    r.down = {};
    e.tick();
    r.down = {VK_F8, VK_F9};
    e.tick();
    CHECK(r.toggles == 1);
}

TEST_CASE("editor: outlines only while the page is drawn; the selected part blinks") {
    HudRig r;
    HudEditor& e = r.make();
    r.now = 100000;
    e.page_frame(2);
    for (size_t i = 0; i < kHudElementCount; ++i) CHECK(e.snapshots().current()->edits[i].outline);
    r.now += 250;
    e.page_frame(2);
    for (size_t i = 0; i < kHudElementCount; ++i) {
        CAPTURE(i);
        CHECK(e.snapshots().current()->edits[i].outline == (i != 2));
    }
    r.now += 300;
    e.tick();
    for (size_t i = 0; i < kHudElementCount; ++i) CHECK_FALSE(e.snapshots().current()->edits[i].outline);
    r.now = 200000;
    r.set("show_outlines", "0");
    e.page_frame(2);
    for (size_t i = 0; i < kHudElementCount; ++i) CHECK_FALSE(e.snapshots().current()->edits[i].outline);
}

TEST_CASE("editor: page_frame writes a due Later save (the main menu has no ticks)") {
    HudRig r;
    HudEditor& e = r.make();
    e.set_offset(0, 1, 1, Save::Later);
    r.now += 600;
    e.page_frame(0);
    CHECK(r.writes == 1);
}

// ---- the final review's fixes ----

TEST_CASE("editor: the other-HUD-mod check is not made while the editor could not turn on") {
    {
        HudRig r;
        r.set("enabled", "0");
        HudEditor& e = r.make();
        r.now += 20000;
        e.tick();
        e.tick();
        CHECK(r.other_calls == 0);
        CHECK(e.view().gate.state == GateState::Disabled);
        r.set("enabled", "1");  // now it could: asked at the next tick
        e.tick();
        CHECK(r.other_calls == 1);
        CHECK(e.view().gate.state == GateState::On);
    }
    {
        HudRig r;
        r.gate.build_known = false;
        HudEditor& e = r.make();
        r.now += 20000;
        e.tick();
        CHECK(r.other_calls == 0);
        CHECK(e.view().gate.state == GateState::UnknownBuild);
    }
    {
        HudRig r;
        HudEditor& e = r.make();
        CHECK(r.other_calls == 1);
        r.fault = 0xC0000005;
        e.tick();
        r.now += 20000;
        e.tick();
        CHECK(r.other_calls == 1);  // faulted: off until the game restarts, no need to ask
    }
}

TEST_CASE("editor: a layout file that was refused is kept as .bad before the first write over it") {
    HudRig r;
    std::filesystem::create_directories(r.dir);
    const std::string original = "{\"format\":2,\"name\":\"Default\"}";  // a newer EMF's file
    std::ofstream(std::filesystem::path(r.dir) / L"default.json", std::ios::binary) << original;
    HudEditor& e = r.make();
    e.nudge(0, 1, 1, Save::Now);
    e.nudge(0, 1, 1, Save::Now);
    std::ifstream bad(std::filesystem::path(r.dir) / L"default.json.bad", std::ios::binary);
    REQUIRE(bad.good());
    const std::string kept((std::istreambuf_iterator<char>(bad)), std::istreambuf_iterator<char>());
    CHECK(kept == original);
    bool ok = false;
    for (const auto& f : r.files())
        if (f.slug == "default") ok = f.layout.has_value() && f.layout->elements[0].x == 2.0f;
    CHECK(ok);
    CHECK(r.count_logged("default.json.bad") == 1);
}

TEST_CASE("editor: save_as and reset-all onto refused files keep them too") {
    HudRig r;
    std::filesystem::create_directories(r.dir);
    std::ofstream(std::filesystem::path(r.dir) / L"before-reset.json", std::ios::binary) << "{";
    std::ofstream(std::filesystem::path(r.dir) / L"mine.json", std::ios::binary) << "not json";
    HudEditor& e = r.make();
    e.set_offset(0, 5, 5, Save::Now);
    e.reset_all();
    CHECK(std::filesystem::exists(std::filesystem::path(r.dir) / L"before-reset.json.bad"));
    CHECK(e.save_as("Mine").ok);
    CHECK(std::filesystem::exists(std::filesystem::path(r.dir) / L"mine.json.bad"));
}

TEST_CASE("set_aside_layout_file: numbered when .bad exists; a missing file is fine") {
    HudRig r;
    std::filesystem::create_directories(r.dir);
    std::string name;
    CHECK(set_aside_layout_file(r.dir, "none", &name));
    std::ofstream(std::filesystem::path(r.dir) / L"a.json") << "1";
    std::ofstream(std::filesystem::path(r.dir) / L"a.json.bad") << "0";
    CHECK(set_aside_layout_file(r.dir, "a", &name));
    CHECK(name == "a.json.bad2");
    CHECK(std::filesystem::exists(std::filesystem::path(r.dir) / L"a.json.bad2"));
    CHECK_FALSE(std::filesystem::exists(std::filesystem::path(r.dir) / L"a.json"));
}

TEST_CASE("editor: the active layout survives a restart through the ini, whatever its name") {
    for (const char* name : {"PvP; big", "100%", "\xF0\x9F\x99\x82 HUD", "plain"}) {
        CAPTURE(name);
        HudRig r;
        const std::wstring ini = r.root + L"\\emf_core.ini";
        {
            CoreIniBackend backend(ini);
            SettingsStore real(&backend);
            for (SettingDef d : emf_setting_defs({}))
                REQUIRE(real.declare(kEmfSettingsId, std::move(d), nullptr, SettingsStore::Declare::Start));
            HudEditorDeps d = r.deps();
            d.settings = &real;
            HudEditor e(d);
            e.start();
            REQUIRE(e.save_as(name).ok);
        }
        CoreIniBackend backend(ini);
        SettingsStore real(&backend);
        for (SettingDef d : emf_setting_defs({}))
            REQUIRE(real.declare(kEmfSettingsId, std::move(d), nullptr, SettingsStore::Declare::Start));
        HudEditorDeps d = r.deps();
        d.settings = &real;
        HudEditor e(d);
        e.start();
        CHECK(e.view().layout_name == name);
    }
}

TEST_CASE("editor: a pending edit is saved to its own layout when settings switch the layout") {
    HudRig r;
    write_layout(r, "Building", 3, 12.5f);
    HudEditor& e = r.make();
    e.set_offset(0, 7, 7, Save::Later);
    r.set("active_layout", "building");
    e.tick();
    CHECK(e.view().layout_name == "Building");
    bool ok = false;
    for (const auto& f : r.files())
        if (f.slug == "default") ok = f.layout.has_value() && f.layout->elements[0].x == 7.0f;
    CHECK(ok);
    for (const auto& f : r.files())
        if (f.slug == "building") CHECK(f.layout->elements[0].x == 0.0f);  // the new layout got nothing
    r.now += 1000;
    e.tick();
    for (const auto& f : r.files())
        if (f.slug == "building") CHECK(f.layout->elements[0].x == 0.0f);
}

TEST_CASE("editor: a pending edit is saved to the old layout before save_as") {
    HudRig r;
    HudEditor& e = r.make();
    e.set_offset(0, 7, 7, Save::Later);
    REQUIRE(e.save_as("Other").ok);
    bool ok = false;
    for (const auto& f : r.files())
        if (f.slug == "default") ok = f.layout.has_value() && f.layout->elements[0].x == 7.0f;
    CHECK(ok);
}

TEST_CASE("editor: swap_bars and set_order reach the snapshot and the file; an invalid order is ignored") {
    HudRig r;
    HudEditor& e = r.make();
    e.swap_bars(kBarHealth, kBarMana, Save::Now);
    CHECK(e.view().order == BarOrder{1, 0, 2});
    CHECK(e.snapshots().current()->order == BarOrder{1, 0, 2});
    bool in_file = false;
    for (const auto& f : r.files())
        if (f.layout && f.layout->order == BarOrder{1, 0, 2}) in_file = true;
    CHECK(in_file);
    e.set_order(BarOrder{1, 1, 2}, Save::Now);
    CHECK(e.view().order == BarOrder{1, 0, 2});
    e.swap_bars(kBarHealth, kBarStamina, Save::Now);  // health and stamina change places in the order
    CHECK(e.view().order == BarOrder{1, 2, 0});
    e.swap_bars(kBarMana, kBarMana, Save::Now);  // nothing to swap
    e.swap_bars(kBarMana, 3, Save::Now);         // not a bar
    CHECK(e.view().order == BarOrder{1, 2, 0});
    e.set_order(kDefaultOrder, Save::Now);
    CHECK(e.view().order == kDefaultOrder);
    CHECK(e.snapshots().current()->order == kDefaultOrder);
}

TEST_CASE("editor: set_buffs_up reaches the snapshot; reset(buffs) turns it off, reset(vitals) restores the order, reset_all both") {
    HudRig r;
    HudEditor& e = r.make();
    e.set_buffs_up(true, Save::Now);
    CHECK(e.view().buffs_up);
    CHECK(e.snapshots().current()->buffs_up);
    e.reset(1, Save::Now);  // the buffs part
    CHECK_FALSE(e.view().buffs_up);
    CHECK_FALSE(e.snapshots().current()->buffs_up);
    e.swap_bars(kBarHealth, kBarMana, Save::Now);
    e.reset(0, Save::Now);  // the vitals part
    CHECK(e.view().order == kDefaultOrder);
    e.swap_bars(kBarHealth, kBarMana, Save::Now);
    e.set_buffs_up(true, Save::Now);
    CHECK(e.reset_all().ok);
    CHECK(e.view().order == kDefaultOrder);
    CHECK_FALSE(e.view().buffs_up);
    CHECK_FALSE(e.snapshots().current()->buffs_up);
}

TEST_CASE("editor: a bar part has no scale of its own") {
    HudRig r;
    HudEditor& e = r.make();
    const size_t mana = static_cast<size_t>(bar_element(kBarMana));
    e.set_scale(mana, 1.5f, Save::Now);
    CHECK(e.view().elements[mana].scale == 1.0f);
    e.set_offset(mana, 0, 5, Save::Now);
    CHECK(e.view().elements[mana].y == 5.0f);
    CHECK(e.snapshots().current()->edits[mana].y_percent == 5.0f);
}

TEST_CASE("editor: bars_work and its note follow bars_ready") {
    HudRig r;
    r.bars = false;
    HudEditor& e = r.make();
    CHECK_FALSE(e.view().bars_work);
    CHECK(e.view().bars_note == "Moving health, mana and stamina on their own is not available on this game build.");
    r.bars = true;
    CHECK(e.view().bars_work);
    CHECK(e.view().bars_note.empty());
}

TEST_CASE("editor: the bars and grow-upward stop with scale, and say so") {  // review: Important 1
    HudRig r;
    HudEditor& e = r.make();
    CHECK(e.view().bars_work);
    CHECK(e.view().grow_works);
    CHECK(e.view().grow_note.empty());
    r.scale = ScaleState{true, true};  // scale stopped after a mismatch
    CHECK_FALSE(e.view().bars_work);
    CHECK(e.view().bars_note ==
          "Moving health, mana and stamina on their own stopped with scale for this session (see emf_core.log).");
    CHECK_FALSE(e.view().grow_works);
    CHECK(e.view().grow_note == "Status effects growing upward stopped with scale for this session (see emf_core.log).");
    r.scale = ScaleState{false, false};  // no transform functions on this build
    CHECK_FALSE(e.view().bars_work);
    CHECK(e.view().bars_note == "Moving health, mana and stamina on their own is not available on this game build.");
    CHECK_FALSE(e.view().grow_works);
    CHECK(e.view().grow_note == "Status effects growing upward is not available on this game build.");
}

TEST_CASE("editor: set_colour publishes and saves; nullopt brings the game's back") {
    HudRig r;
    HudEditor& e = r.make();
    const Colour red = *parse_colour("#ff0000").colour;
    e.set_colour(kBarMana, BarPart::Fill, red, Save::Now);
    CHECK(e.view().colours[kBarMana].part[0] == red);
    CHECK(e.snapshots().current()->colours[kBarMana].part[0] == red);
    bool in_file = false;
    for (const auto& f : r.files())
        if (f.layout && f.layout->colours[kBarMana].part[0] == red) in_file = true;
    CHECK(in_file);
    e.set_colour(kBarMana, BarPart::Fill, std::nullopt, Save::Now);
    CHECK_FALSE(e.view().colours[kBarMana].any());
    CHECK_FALSE(e.snapshots().current()->colours[kBarMana].any());
    e.set_colour(3, BarPart::Fill, red, Save::Now);  // not a bar
    e.set_colour(-1, BarPart::Fill, red, Save::Now);
    for (const auto& c : e.view().colours) CHECK_FALSE(c.any());
    e.set_colour(kBarHealth, BarPart::Heal, red, Save::Now);
    e.set_colour(kBarHealth, BarPart::Empty, red, Save::Now);
    e.clear_colours(kBarHealth, Save::Now);
    CHECK_FALSE(e.view().colours[kBarHealth].any());
    e.set_panel(red, Save::Now);
    CHECK(e.view().panel == red);
    CHECK(e.snapshots().current()->panel == red);
    e.set_panel(std::nullopt, Save::Now);
    CHECK_FALSE(e.view().panel.set);
}

TEST_CASE("editor: a part's reset keeps its colours and the panel; reset all clears everything") {  // review Minor 4
    HudRig r;
    HudEditor& e = r.make();
    const Colour red = *parse_colour("#ff0000").colour;
    e.set_colour(kBarMana, BarPart::Fill, red, Save::Now);
    e.set_colour(kBarStamina, BarPart::Fill, red, Save::Now);
    e.set_panel(red, Save::Now);
    e.set_offset(static_cast<size_t>(bar_element(kBarMana)), 0, 5, Save::Now);
    e.reset(static_cast<size_t>(bar_element(kBarMana)), Save::Now);
    CHECK(e.view().elements[static_cast<size_t>(bar_element(kBarMana))].is_default());
    CHECK(e.view().colours[kBarMana].any());
    CHECK(e.view().panel.set);
    e.reset(0, Save::Now);  // the vitals part
    CHECK(e.view().panel.set);
    CHECK(e.view().colours[kBarStamina].any());
    CHECK(e.reset_all().ok);
    CHECK_FALSE(e.view().colours[kBarStamina].any());
    CHECK_FALSE(e.view().panel.set);
    CHECK_FALSE(e.snapshots().current()->panel.set);
}

TEST_CASE("editor: colours work without scale; the panel follows its status") {
    HudRig r;
    r.build_panel = false;  // the panel's signature is not usable on this build
    PanelStatus status = PanelStatus::Unknown;
    HudEditorDeps d = r.deps();
    d.panel_status = [&status] { return status; };
    r.editor = std::make_unique<HudEditor>(d);
    r.editor->start();
    HudEditor& e = *r.editor;
    r.scale = ScaleState{true, true};  // scale stopped: moving the bars stops, colours do not
    CHECK(e.view().colours_work);
    CHECK(e.view().colours_note.empty());
    CHECK(e.view().panel_works);
    status = PanelStatus::Available;
    CHECK(e.view().panel_works);
    CHECK(e.view().panel_note.empty());
    status = PanelStatus::Unavailable;
    CHECK_FALSE(e.view().panel_works);
    CHECK(e.view().panel_note == "The panel colour is not available on this game build.");
    HudRig off;
    off.bars = false;  // no bar signature on this build
    HudEditor& f = off.make();
    CHECK_FALSE(f.view().colours_work);
    CHECK(f.view().colours_note == "Bar colours are not available on this game build.");
    CHECK_FALSE(f.view().panel_works);
}

TEST_CASE("editor: before its hooks go in (off at start, another HUD mod) nothing is called not available that the build has") {
    for (int why = 0; why < 2; ++why) {
        HudRig r;
        if (why == 0) r.set("enabled", "0");
        else r.other_mod = "ShroudHud";
        HudEditorDeps d = r.deps();  // this build has every signature
        // what the real hooks say before they go in: no transform functions, no bar detour, the panel not known
        r.scale = ScaleState{false, false};
        r.bars = false;
        d.panel_status = [] { return PanelStatus::Unknown; };
        r.editor = std::make_unique<HudEditor>(d);
        r.editor->start();
        const HudView v = r.editor->view();
        CHECK(v.gate.state == (why == 0 ? GateState::Disabled : GateState::OtherHudMod));
        CHECK(r.installs == 0);
        CHECK(v.scale_works);  // they work once the editor is on: the page keeps them usable, as move and hide
        CHECK(v.bars_work);
        CHECK(v.colours_work);
        CHECK(v.panel_works);
        CHECK(v.grow_works);
        for (const std::string& note : {v.scale_note, v.bars_note, v.colours_note, v.panel_note, v.grow_note})
            CHECK(note.empty());  // the gate's message says why the editor is off
    }
    // a build without them says so before the hooks go in too
    HudRig r;
    r.set("enabled", "0");
    r.scale = ScaleState{false, false};
    r.bars = false;
    HudEditor& e = r.make();
    const HudView v = e.view();
    CHECK(r.installs == 0);
    CHECK_FALSE(v.scale_works);
    CHECK(v.scale_note == "Scale is not available on this game build; move and hide work.");
    CHECK(v.bars_note == "Moving health, mana and stamina on their own is not available on this game build.");
    CHECK(v.colours_note == "Bar colours are not available on this game build.");
    CHECK(v.grow_note == "Status effects growing upward is not available on this game build.");
    HudRig p;
    p.set("enabled", "0");
    p.build_panel = false;
    HudEditor& pe = p.make();
    CHECK_FALSE(pe.view().panel_works);
    CHECK(pe.view().panel_note == "The panel colour is not available on this game build.");
    CHECK(pe.view().colours_work);
}

TEST_CASE("editor: a bar or panel hook that failed where the build has its signature says it could not be hooked") {
    HudRig r;
    PanelStatus status = PanelStatus::Unavailable;
    HudEditorDeps d = r.deps();  // every signature usable
    d.panel_status = [&status] { return status; };
    r.editor = std::make_unique<HudEditor>(d);
    r.editor->start();
    HudEditor& e = *r.editor;
    CHECK_FALSE(e.view().panel_works);
    CHECK(e.view().panel_note == "The panel colour is off: the game's panel call could not be hooked (see emf_core.log).");
    r.bars = false;  // the bar detour did not go in
    const HudView v = e.view();
    CHECK_FALSE(v.bars_work);
    CHECK(v.bars_note ==
          "Moving health, mana and stamina on their own is off: the game's bar code could not be hooked (see "
          "emf_core.log).");
    CHECK_FALSE(v.colours_work);
    CHECK(v.colours_note == "Bar colours are off: the game's bar code could not be hooked (see emf_core.log).");
    CHECK(v.panel_note == v.colours_note);
}

TEST_CASE("editor: the game's colours are logged once per bar") {
    HudRig r;
    StyleBlock b{};
    b[kFrame] = {rgba_to_game(0x10101080), 1.0f};
    b[kRange] = {rgba_to_game(0x202020C0), 1.0f};
    b[kFill] = {rgba_to_game(0x4FD17CFF), 1.0f};
    b[kFillEnd] = {rgba_to_game(0xA0F0B0E0), 1.2f};
    b[kDamage] = {rgba_to_game(0xD04040FF), 1.0f};
    b[kHeal] = {rgba_to_game(0xF0F0A0FF), 1.0f};
    HudEditorDeps d = r.deps();
    d.game_colours = [&b](int bar) -> std::optional<StyleBlock> {
        if (bar == kBarHealth) return b;
        return std::nullopt;
    };
    r.editor = std::make_unique<HudEditor>(d);
    r.editor->start();
    r.editor->tick();
    CHECK(r.count_logged("HUD editor: the game's health colours: frame 101010-80, empty 202020-c0, fill 4fd17c, end "
                         "a0f0b0-e0*1.20, damage d04040, heal f0f0a0") == 1);
    CHECK_FALSE(r.logged("the game's mana colours"));
    r.editor->tick();
    CHECK(r.count_logged("the game's health colours") == 1);
    REQUIRE(r.editor->view().game_colours[kBarHealth].has_value());
    CHECK_FALSE(r.editor->view().game_colours[kBarMana].has_value());
}

TEST_CASE("editor: a panel colour that is not available on this game build is logged once") {
    HudRig r;
    r.build_panel = false;
    PanelStatus status = PanelStatus::Unknown;
    HudEditorDeps d = r.deps();
    d.panel_status = [&status] { return status; };
    r.editor = std::make_unique<HudEditor>(d);
    r.editor->start();
    r.editor->tick();
    status = PanelStatus::Available;
    r.editor->tick();
    CHECK_FALSE(r.logged("panel colour is not available"));
    status = PanelStatus::Unavailable;
    r.editor->tick();
    r.editor->tick();
    CHECK(r.count_logged("HUD editor: the panel colour is not available on this game build (its signature is not "
                         "usable); the panel keeps the game's colour") == 1);
}

TEST_CASE("editor: the panel's 'not available' log line names its cause: a signature, the bars' hook or its own hook") {
    struct Case {
        bool bars_sig, panel_sig, bars_hooked;
        const char* line;
    };
    const Case cases[] = {
        {false, true, false,
         "HUD editor: the panel colour is not available on this game build (the bars' signatures are not usable); the "
         "panel keeps the game's colour"},
        {true, false, true,
         "HUD editor: the panel colour is not available on this game build (its signature is not usable); the panel "
         "keeps the game's colour"},
        {true, true, false,
         "HUD editor: the panel colour is off (the bars' hook could not be installed); the panel keeps the game's colour"},
        {true, true, true,
         "HUD editor: the panel colour is off (its hook could not be installed); the panel keeps the game's colour"},
    };
    for (const Case& c : cases) {
        HudRig r;
        r.bars = c.bars_sig;
        r.build_panel = c.panel_sig;
        HudEditorDeps d = r.deps();
        r.bars = c.bars_hooked;
        d.panel_status = [] { return PanelStatus::Unavailable; };
        r.editor = std::make_unique<HudEditor>(d);
        r.editor->start();
        r.editor->tick();
        r.editor->tick();
        CHECK(r.count_logged(c.line) == 1);
        CHECK(r.count_logged("the panel colour") == 1);
    }
}
