// The Mods tab: the cursor the console draws, setting descriptions, edits from widgets, and the tab drawn headless.
#include <doctest.h>

#include <map>
#include <string>
#include <vector>

#include "core/manifest.h"
#include "core/overlay/console_model.h"
#include "core/overlay/console_ui.h"
#include "core/overlay/cursor_model.h"
#include "core/overlay/mods_ui.h"
#include "core/overlay/overlay.h"
#include "imgui.h"
#include "imgui_internal.h"

using namespace emf::core;

namespace {
struct MemIni : IniBackend {
    std::map<std::string, std::string> values;
    std::optional<std::string> read(const std::string& mod, const std::string& key) override {
        auto it = values.find(mod + "." + key);
        if (it == values.end()) return std::nullopt;
        return it->second;
    }
    bool write(const std::string& mod, const std::string& key, const std::string& value) override {
        values[mod + "." + key] = value;
        return true;
    }
};

SettingDef def(const char* key, SettingType t, const char* dflt) {
    SettingDef d;
    d.key = key;
    d.label = key;
    d.type = t;
    d.default_value = dflt;
    return d;
}

#include <windows.h>
int guarded_frame(void (*fn)(void*), void* ctx) {
    __try {
        fn(ctx);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}
struct Headless {
    ImGuiContext* ctx;
    Headless() {
        ctx = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1920, 1080);
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    }
    ~Headless() { ImGui::DestroyContext(ctx); }
};
}  // namespace

TEST_CASE("cursor: starts in the middle, follows raw movement scaled by the speed, stays on the screen") {
    CursorModel c;
    c.set_display(ImVec2(1000, 500));
    CHECK(c.pos().x == 500.0f);
    CHECK(c.pos().y == 250.0f);
    c.move(10, -20, 1.0f);
    CHECK(c.pos().x == 510.0f);
    CHECK(c.pos().y == 230.0f);
    c.move(10, 10, 2.0f);
    CHECK(c.pos().x == 530.0f);
    CHECK(c.pos().y == 250.0f);
    c.move(-5000, 9000, 1.0f);
    CHECK(c.pos().x == 0.0f);
    CHECK(c.pos().y == 499.0f);
    c.set_display(ImVec2(300, 200));  // a smaller window: pulled back in
    CHECK(c.pos().y == 199.0f);
    c.set_display(ImVec2(1000, 500));  // a later size change does not re-centre
    CHECK(c.pos().x == 0.0f);
}

TEST_CASE("mods ui: settings are described for players") {
    SettingDef place = def("place", SettingType::Choice, "1");
    place.choices = {"top right", "bottom right"};
    CHECK(describe_setting(place) == "one of: top right, bottom right");
    SettingDef radius = def("radius", SettingType::Int, "10");
    radius.min = 2;
    radius.max = 50;
    radius.has_min = radius.has_max = true;
    CHECK(describe_setting(radius) == "a whole number, 2 to 50");
    SettingDef speed = def("speed", SettingType::Float, "1");
    speed.min = 0.5;
    speed.has_min = true;
    CHECK(describe_setting(speed) == "a number, at least 0.5");
    CHECK(describe_setting(def("k", SettingType::Key, "G")) == "a key: a letter, a digit, F1-F24, Space, PageUp, Numpad0-9...");
    CHECK(describe_setting(def("b", SettingType::Bool, "1")) == "on or off");
    CHECK(describe_setting(def("t", SettingType::Text, "")) == "text");
}

TEST_CASE("mods ui: an edit from a widget is saved, or refused with the reason shown") {
    MemIni ini;
    SettingsStore s(&ini);
    SettingDef radius = def("radius", SettingType::Int, "10");
    radius.min = 2;
    radius.max = 50;
    radius.has_min = radius.has_max = true;
    s.declare("m", radius);
    SettingDef live = def("h24", SettingType::Bool, "1");
    live.live = true;
    s.declare("m", live);
    ModsUiState ui;
    CHECK(apply_setting_edit(s, "m", "radius", "20", ui));
    CHECK(ini.values["m.radius"] == "20");
    CHECK(ui.message == "radius = 20: applies at the next game start");
    CHECK_FALSE(ui.message_error);
    CHECK_FALSE(apply_setting_edit(s, "m", "radius", "99", ui));
    CHECK(ui.message_error);
    CHECK(ui.message.find("between 2 and 50") != std::string::npos);
    CHECK(apply_setting_edit(s, "m", "h24", "0", ui));
    CHECK(ui.message == "h24 = 0");
}

TEST_CASE("mods ui: the console's Mods tab draws every kind of setting and many mods without ImGui errors") {
    Headless h;
    MemIni ini;
    SettingsStore store(&ini);
    std::vector<ModRecord> mods;
    for (int i = 0; i < 40; ++i) {
        ModRecord r;
        r.id = "mod" + std::to_string(i);
        r.name = "Mod " + std::to_string(i);
        r.version = "1.0." + std::to_string(i);
        r.status = static_cast<ModStatus>(i % 5);
        r.reason = i % 5 ? "some reason" : "";
        r.has_manifest = true;
        r.source = r.id + "\\";
        mods.push_back(r);
    }
    SettingDef place = def("place", SettingType::Choice, "1");
    place.choices = {"top right", "bottom right", "bottom left"};
    place.live = true;
    place.help = "Where the clock sits.";
    SettingDef radius = def("radius", SettingType::Int, "10");
    radius.min = 2;
    radius.max = 50;
    radius.has_min = radius.has_max = true;
    SettingDef count = def("count", SettingType::Int, "3");  // no range: an input box
    SettingDef speed = def("speed", SettingType::Float, "1.5");
    speed.min = 0.5;
    speed.max = 4;
    speed.has_min = speed.has_max = true;
    for (const auto& d : {place, radius, count, speed, def("h24", SettingType::Bool, "1"),
                          def("key", SettingType::Key, "G"), def("name", SettingType::Text, "Bob")})
        store.declare("mod0", d);

    ConsoleModel model;
    ConsoleUiState ui;
    ui.open_fraction = 1.0f;
    ModsUiState mods_ui;
    CompletionSources src;
    const auto mods_tab = [&] { draw_mods_tab(mods, &store, mods_ui); };
    struct FrameCtx {
        ConsoleModel* model;
        ConsoleUiState* ui;
        CompletionSources* src;
        const std::function<void()>* tab;
    } fc{&model, &ui, &src, nullptr};
    std::function<void()> tab_fn = mods_tab;
    fc.tab = &tab_fn;
    auto frame = [&] {
        const int crashed = guarded_frame([](void* p) {
            auto* c = static_cast<FrameCtx*>(p);
            ImGui::NewFrame();
            draw_console(*c->model, *c->ui, *c->src, ImVec2(1920, 1080), [](const std::string&) {}, *c->tab);
            ImGui::Render();
        }, &fc);
        REQUIRE_MESSAGE(crashed == 0, std::string(emf_imgui_last_assert()));
    };
    for (int i = 0; i < 3; ++i) frame();
    CHECK(ui.tab == 0);

    // click the "Mods" tab
    ImGuiTabBar* bar = nullptr;
    for (int i = 0; i < GImGui->TabBars.GetMapSize(); ++i)
        if (ImGuiTabBar* b = GImGui->TabBars.TryGetMapData(i)) bar = b;
    REQUIRE(bar != nullptr);
    REQUIRE(bar->Tabs.Size == 2);
    const ImGuiTabItem& tab = bar->Tabs[1];
    const ImVec2 at(bar->BarRect.Min.x + tab.Offset + tab.Width * 0.5f, (bar->BarRect.Min.y + bar->BarRect.Max.y) * 0.5f);
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(at.x, at.y);
    frame();
    io.AddMouseButtonEvent(0, true);
    frame();
    io.AddMouseButtonEvent(0, false);
    for (int i = 0; i < 3; ++i) frame();
    CHECK(ui.tab == 1);

    mods_ui.selected = "mod0\\";  // every widget type
    for (int i = 0; i < 3; ++i) frame();
    mods_ui.selected = "mod3\\";  // a mod without settings
    for (int i = 0; i < 3; ++i) frame();
    CHECK(emf_imgui_last_assert()[0] == '\0');
}

TEST_CASE("mods ui: rows are told apart by folder, so two records with one id never share an ImGui id") {
    ModRecord a;
    a.id = "clock";
    a.name = "Clock";
    a.source = "clock\\";
    ModRecord b = a;
    b.source = "clock - Copy\\";
    CHECK(mod_row_label(a) != mod_row_label(b));
    CHECK(mod_row_label(a).find("Clock") == 0);
}

TEST_CASE("mods ui: the switch is read from the ini once, again after a change or a second later, not every frame") {
    struct CountingIni : MemIni {
        int reads = 0;
        std::optional<std::string> read(const std::string& mod, const std::string& key) override {
            ++reads;
            return MemIni::read(mod, key);
        }
    } ini;
    SettingsStore s(&ini);
    ModsUiState ui;
    ModRecord a;
    a.id = "a";
    ModRecord b;
    b.id = "b";
    b.enabled_default = false;
    CHECK(mod_switch(s, a, ui, 1000));
    CHECK(ini.reads == 1);
    for (uint64_t t = 1001; t < 1500; t += 16) CHECK(mod_switch(s, a, ui, t));  // frames
    CHECK(ini.reads == 1);
    ini.values["a.enabled"] = "0";  // edited outside the game
    CHECK(mod_switch(s, a, ui, 1900));  // still the value read a moment ago
    CHECK_FALSE(mod_switch(s, a, ui, 2000));  // a second later: read again
    CHECK(ini.reads == 2);
    REQUIRE(s.set_enabled("a", true));  // the checkbox
    CHECK(mod_switch(s, a, ui, 2010));  // a change: read again at once
    CHECK(ini.reads == 3);
    CHECK_FALSE(mod_switch(s, b, ui, 2020));  // another mod selected
    CHECK(ini.reads == 4);
}

TEST_CASE("mods ui: an edit in progress is dropped once its widget was not drawn in the frame before") {
    ModsUiState ui;
    set_pending_edit(ui, "m.radius", "20", 10);
    forget_stale_edits(ui, 11);
    REQUIRE(pending_edit(ui, "m.radius", 11));  // drawn again: still being dragged
    CHECK(*pending_edit(ui, "m.radius", 11) == "20");
    forget_stale_edits(ui, 12);
    CHECK(pending_edit(ui, "m.radius", 12));
    set_pending_edit(ui, "m.speed", "2.5", 12);
    // frames 13 and 14: the tab is not drawn (another tab, the console closed)
    forget_stale_edits(ui, 15);
    CHECK(pending_edit(ui, "m.radius", 15) == nullptr);  // the saved value shows again
    CHECK(pending_edit(ui, "m.speed", 15) == nullptr);
    CHECK(ui.pending.empty());
}

TEST_CASE("mods ui: the new controls are described for players") {
    auto r = parse_manifest(R"({"id": "m", "version": "1", "settings": [
        {"key": "corner", "type": "select", "options": [{"value": "tr", "label": "Top right"}, "bl"]},
        {"key": "tint", "type": "color"},
        {"key": "speed", "type": "slider", "min": 0.5, "max": 3, "step": 0.25},
        {"key": "name", "type": "text", "maxLength": 12}]})");
    REQUIRE_MESSAGE(r.manifest, r.error);
    const auto& d = r.manifest->settings;
    CHECK(describe_setting(d[0]) == "one of: Top right, bl");
    CHECK(describe_setting(d[1]) == "a colour (#rrggbb)");
    CHECK(describe_setting(d[2]) == "a number, 0.5 to 3, in steps of 0.25");
    CHECK(describe_setting(d[3]) == "text, at most 12 characters");
}

TEST_CASE("mods ui: a key bind takes the next key press, named as key settings read it; Esc cancels") {
    CHECK(key_event_name(KeyEvent{0x41, VK_F7, true, false}) == "F7");
    CHECK(key_event_name(KeyEvent{0x22, 'G', true, false}) == "G");
    CHECK(key_event_name(KeyEvent{0x2A, VK_LSHIFT, true, false}) == "Shift");  // left/right as the generic key
    CHECK(key_event_name(KeyEvent{0x1D, VK_RCONTROL, true, true}) == "Ctrl");
    CHECK(key_event_name(KeyEvent{0x4C, VK_NUMPAD5, true, false}) == "Numpad5");
    CHECK(key_event_name(KeyEvent{0x27, VK_OEM_1, true, false}) == "0xba");  // no name: its number
    CHECK(key_event_name(KeyEvent{0x01, VK_ESCAPE, true, false}).empty());
    CHECK(key_event_name(KeyEvent{0x39, 0, true, false}) == "Space");  // scan code only

    ModsUiState ui;
    CHECK_FALSE(capture_key_event(ui, KeyEvent{0x22, 'G', true, false}, 5));  // nothing waits: ImGui gets it
    ui.capturing = "m.toggle";
    ui.capture_frame = 5;
    CHECK_FALSE(capture_key_event(ui, KeyEvent{0x22, 'G', false, false}, 5));  // a release goes on to ImGui, binds nothing
    CHECK(ui.captured.empty());
    CHECK(capture_key_event(ui, KeyEvent{0x41, VK_F7, true, false}, 5));
    CHECK(ui.captured == "F7");
    CHECK(capture_key_event(ui, KeyEvent{0x22, 'G', true, false}, 6));  // a second key does not replace the first
    CHECK(ui.captured == "F7");
    ui.captured.clear();
    CHECK(capture_key_event(ui, KeyEvent{0x01, VK_ESCAPE, true, false}, 6));
    CHECK(ui.capturing.empty());  // cancelled

    // the player clicked the bind button, then went to the Console tab (or closed the console): the widget is no longer
    // drawn, so keys go to the console again instead of waiting forever
    ui.capturing = "m.toggle";
    ui.capture_frame = 20;
    CHECK(capture_key_event(ui, KeyEvent{0x22, 'G', true, false}, 21));  // drawn in the frame before: still waiting
    ui.captured.clear();
    CHECK_FALSE(capture_key_event(ui, KeyEvent{0x1C, VK_RETURN, true, false}, 22));
    CHECK(ui.capturing.empty());

    ui.capturing = "m.toggle";  // a capture whose widget is no longer drawn is dropped
    ui.capture_frame = 10;
    forget_stale_edits(ui, 11);
    CHECK(ui.capturing == "m.toggle");
    forget_stale_edits(ui, 12);
    CHECK(ui.capturing.empty());
}

TEST_CASE("mods ui: colours go to the colour picker's floats and back") {
    float rgb[3] = {0, 0, 0};
    REQUIRE(color_to_floats("#ff8000", rgb));
    CHECK(rgb[0] == doctest::Approx(1.0f));
    CHECK(rgb[1] == doctest::Approx(128.0f / 255.0f));
    CHECK(rgb[2] == doctest::Approx(0.0f));
    CHECK(floats_to_color(rgb) == "#ff8000");
    CHECK_FALSE(color_to_floats("ff8000", rgb));
    CHECK_FALSE(color_to_floats("#ff80zz", rgb));
    const float out_of_range[3] = {1.5f, -0.2f, 0.5f};
    CHECK(floats_to_color(out_of_range) == "#ff0080");
}

TEST_CASE("mods ui: the Mods tab draws groups, sliders with steps, lists, colours, key binds and greyed-out settings") {
    Headless h;
    MemIni ini;
    SettingsStore store(&ini);
    auto r = parse_manifest(R"({"id": "mod0", "version": "1", "settings": [
        {"key": "show", "type": "bool", "default": false, "label": "Show"},
        {"key": "text", "type": "text", "maxLength": 10, "default": "hi", "group": "Display",
         "enabledWhen": {"setting": "show", "equals": true}},
        {"key": "corner", "type": "select", "group": "Display", "options": [{"value": "tr", "label": "Top right"}, "bl"]},
        {"key": "tint", "type": "color", "default": "#ffd070", "group": "Display"},
        {"key": "speed", "type": "slider", "min": 0.5, "max": 3, "step": 0.25, "default": 1, "group": "Display"},
        {"key": "evens", "type": "slider", "min": 0, "max": 10, "step": 2, "default": 4},
        {"key": "count", "type": "int", "step": 5, "default": 10},
        {"key": "toggle", "type": "keybind", "default": "F7", "group": "Keys", "live": true}]})");
    REQUIRE_MESSAGE(r.manifest, r.error);
    for (const auto& d : r.manifest->settings) REQUIRE(store.declare("mod0", d));
    std::vector<ModRecord> mods(1);
    mods[0].id = "mod0";
    mods[0].name = "Mod 0";
    mods[0].source = "mod0\\";
    mods[0].has_manifest = true;
    ModsUiState ui;
    struct Ctx {
        std::vector<ModRecord>* mods;
        SettingsStore* store;
        ModsUiState* ui;
    } c{&mods, &store, &ui};
    auto frame = [&] {
        const int crashed = guarded_frame([](void* p) {
            auto* x = static_cast<Ctx*>(p);
            ImGui::NewFrame();
            ImGui::SetNextWindowSize(ImVec2(1200, 900));
            ImGui::Begin("mods");
            draw_mods_tab(*x->mods, x->store, *x->ui);
            ImGui::End();
            ImGui::Render();
        }, &c);
        REQUIRE_MESSAGE(crashed == 0, std::string(emf_imgui_last_assert()));
    };
    for (int i = 0; i < 3; ++i) frame();

    // a key bind: the widget waits, the key arrives, the setting is saved
    ui.capturing = "mod0.toggle";  // as a click on the button leaves it
    ui.capture_frame = ImGui::GetFrameCount();
    frame();
    CHECK(ui.capturing == "mod0.toggle");  // still waiting, drawn as such
    CHECK(capture_key_event(ui, KeyEvent{0x22, 'G', true, false}, ImGui::GetFrameCount()));
    frame();
    CHECK(ui.capturing.empty());
    CHECK(store.get("mod0", "toggle") == "G");
    CHECK(ini.values["mod0.toggle"] == "G");
    CHECK(ui.message == "toggle = G");

    // a colour edit in progress is saved once nothing is active
    set_pending_edit(ui, "mod0.tint", "#00ff80", ImGui::GetFrameCount());
    frame();
    CHECK(store.get("mod0", "tint") == "#00ff80");

    store.set("mod0", "show", "1");
    for (int i = 0; i < 3; ++i) frame();
    CHECK(emf_imgui_last_assert()[0] == '\0');
}

TEST_CASE("mods ui: which widget a number gets, and its numbers exactly (no 1.23457e+06)") {
    auto r = parse_manifest(R"({"id": "m", "version": "1", "settings": [
        {"key": "a", "type": "int", "min": 0, "max": 100},
        {"key": "b", "type": "int", "min": 0, "max": 10000000},
        {"key": "c", "type": "int", "min": 0, "max": 1000000000000, "control": "slider"},
        {"key": "d", "type": "float", "min": 0.5, "max": 3},
        {"key": "e", "type": "float"},
        {"key": "f", "type": "int", "step": 5},
        {"key": "g", "type": "slider", "min": 1, "max": 1234567, "step": 1}]})");
    REQUIRE_MESSAGE(r.manifest, r.error);
    const auto& d = r.manifest->settings;
    CHECK(number_widget(d[0]) == NumberWidget::SliderInt);
    CHECK(number_widget(d[1]) == NumberWidget::InputInt);   // too wide for a useful slider
    CHECK(number_widget(d[2]) == NumberWidget::SliderInt);  // asked for: a 64-bit slider, no overflow
    CHECK(number_widget(d[3]) == NumberWidget::SliderDouble);
    CHECK(number_widget(d[4]) == NumberWidget::InputDouble);
    CHECK(number_widget(d[5]) == NumberWidget::InputInt);
    CHECK(describe_setting(d[6]) == "a whole number, 1 to 1234567, in steps of 1");
    CHECK(describe_setting(d[2]) == "a whole number, 0 to 1000000000000");
    CHECK(setting_number_text(1234567.0) == "1234567");
    CHECK(setting_number_text(999999999999999.0) == "999999999999999");
    CHECK(setting_number_text(1234567.5) == "1234567.5");
    CHECK(setting_number_text(0.1) == "0.1");
}

TEST_CASE("mods ui: a large whole number survives being drawn and edited in its box") {
    Headless h;
    MemIni ini;
    ini.values["m.big"] = "123456789012";
    SettingsStore store(&ini);
    auto r = parse_manifest(R"({"id": "m", "version": "1", "settings": [{"key": "big", "type": "int"},
        {"key": "wide", "type": "int", "min": 0, "max": 1000000000000, "control": "slider", "default": 999999999999}]})");
    REQUIRE_MESSAGE(r.manifest, r.error);
    for (const auto& d : r.manifest->settings) REQUIRE(store.declare("m", d));
    std::vector<ModRecord> mods(1);
    mods[0].id = "m";
    mods[0].name = "M";
    mods[0].source = "m\\";
    mods[0].has_manifest = true;
    ModsUiState ui;
    struct Ctx {
        std::vector<ModRecord>* mods;
        SettingsStore* store;
        ModsUiState* ui;
    } c{&mods, &store, &ui};
    for (int i = 0; i < 3; ++i) {
        const int crashed = guarded_frame([](void* p) {
            auto* x = static_cast<Ctx*>(p);
            ImGui::NewFrame();
            ImGui::SetNextWindowSize(ImVec2(1200, 900));
            ImGui::Begin("mods");
            draw_mods_tab(*x->mods, x->store, *x->ui);
            ImGui::End();
            ImGui::Render();
        }, &c);
        REQUIRE_MESSAGE(crashed == 0, std::string(emf_imgui_last_assert()));
    }
    CHECK(store.get("m", "big") == "123456789012");
    CHECK(store.get("m", "wide") == "999999999999");
    CHECK(ini.values["m.big"] == "123456789012");
    CHECK(emf_imgui_last_assert()[0] == '\0');
}

TEST_CASE("mods ui: a key bind takes Shift, Ctrl or Alt only when released alone; Shift+G binds G; the IME key never binds") {
    ModsUiState ui;
    const auto wait = [&] {
        ui.capturing = "m.toggle";
        ui.captured.clear();
        ui.capture_modifier.clear();
        ui.capture_frame = 5;
    };
    wait();
    CHECK(capture_key_event(ui, KeyEvent{0x2A, VK_LSHIFT, true, false}, 5));  // Shift down: taken, nothing bound yet
    CHECK(ui.captured.empty());
    CHECK(capture_key_event(ui, KeyEvent{0x22, 'G', true, false}, 5));       // G while Shift is held
    CHECK(ui.captured == "G");

    wait();
    CHECK(capture_key_event(ui, KeyEvent{0x1D, VK_CONTROL, true, false}, 5));
    CHECK(ui.captured.empty());
    CHECK_FALSE(capture_key_event(ui, KeyEvent{0x1D, VK_CONTROL, false, false}, 5));  // released alone: Ctrl (the release goes on)
    CHECK(ui.captured == "Ctrl");

    wait();
    CHECK(capture_key_event(ui, KeyEvent{0x38, VK_RMENU, true, true}, 5));
    CHECK_FALSE(capture_key_event(ui, KeyEvent{0x38, VK_LMENU, false, false}, 5));  // the other Alt's release: still Alt
    CHECK(ui.captured == "Alt");

    wait();
    CHECK(key_event_name(KeyEvent{0x00, VK_PROCESSKEY, true, false}).empty());  // the IME is composing
    CHECK(capture_key_event(ui, KeyEvent{0x00, VK_PROCESSKEY, true, false}, 5));
    CHECK(ui.captured.empty());
    CHECK(ui.capturing == "m.toggle");  // still waiting for a real key
}

TEST_CASE("mods ui: the overlay hands a key to a waiting key bind, never to ImGui; other keys go to ImGui") {
    Headless h;
    ImGuiIO& io = ImGui::GetIO();
    ModsUiState ui;
    const int frame = ImGui::GetFrameCount();
    const int queued = GImGui->InputEventsQueue.Size;
    feed_key_event(ui, KeyEvent{0x22, 'G', true, false}, io, frame);  // nothing waits: ImGui gets G
    CHECK(GImGui->InputEventsQueue.Size == queued + 1);
    ui.capturing = "m.toggle";
    ui.capture_frame = frame;
    feed_key_event(ui, KeyEvent{0x23, 'H', true, false}, io, frame);  // a bind waits: the press is the bind's
    CHECK(GImGui->InputEventsQueue.Size == queued + 1);
    CHECK(ui.captured == "H");
    // releases still reach ImGui while a bind waits: G, pressed before, is not left held there
    feed_key_event(ui, KeyEvent{0x22, 'G', false, false}, io, frame);
    CHECK(GImGui->InputEventsQueue.Size == queued + 2);
}
