// API v13: settings (declare, read, change, change callbacks) and mod info.
#include <doctest.h>

#include <cstring>
#include <stdexcept>
#include <map>
#include <string>
#include <vector>

#include "core/api.h"
#include "core/api_sizes.h"
#include "emf/emf.hpp"

using namespace emf::core;

namespace {
struct FakeIni : IniBackend {
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

std::vector<std::string> g_log;

struct Fixture {
    CoreState state;
    FakeIni ini;
    Fixture() {
        state.log_line = [](const std::string& s) { g_log.push_back(s); };
        state.now_ms = [] { return uint64_t{1000}; };
        state.settings = std::make_unique<SettingsStore>(&ini);
        set_core_state(&state);
        g_log.clear();
    }
    ~Fixture() { set_core_state(nullptr); }
};

EmfSettingDef int_def(const char* key, const char* dflt, double mn, double mx) {
    EmfSettingDef d{};
    d.size = sizeof(EmfSettingDef);
    d.key = key;
    d.type = EMF_SETTING_INT;
    d.label = "Radius";
    d.help = "How far";
    d.default_value = dflt;
    d.min = mn;
    d.max = mx;
    d.flags = EMF_SETTING_HAS_MIN | EMF_SETTING_HAS_MAX | EMF_SETTING_LIVE;
    return d;
}

EmfWorld* const kWorld = reinterpret_cast<EmfWorld*>(0x7000);
struct Seen {
    EmfWorld* world;
    std::string mod, key;
};
std::vector<Seen> g_seen;
void on_change(EmfWorld* w, const char* mod, const char* key, void*) { g_seen.push_back({w, mod, key}); }
int g_faults = 0;
void crashing_change(EmfWorld*, const char*, const char*, void*) {
    ++g_faults;
    *static_cast<volatile int*>(nullptr) = 1;
}
bool any_log(const char* needle) {
    for (const auto& l : g_log)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}
}  // namespace

TEST_CASE("api v13: the table has the settings and mod info functions; has_v13 checks the size") {
    const EmfApi* a = api_table();
    CHECK(a->version >= 13);
    CHECK(a->setting_declare);
    CHECK(a->setting_int);
    CHECK(a->setting_float);
    CHECK(a->setting_text);
    CHECK(a->setting_set);
    CHECK(a->on_setting_changed);
    CHECK(a->mod_info);
    CHECK(a->mod_info_at);
    emf::api_ref() = a;
    CHECK(emf::has_v13());
    EmfApi old = *a;
    old.size = static_cast<uint32_t>(offsetof(EmfApi, setting_declare));
    emf::api_ref() = &old;
    CHECK_FALSE(emf::has_v13());
    emf::api_ref() = nullptr;
}

TEST_CASE("api v13: declare, read and change settings") {
    Fixture f;
    const EmfApi* a = api_table();
    EmfSettingDef d = int_def("radius", "10", 2, 50);
    REQUIRE(a->setting_declare("mymod", &d));
    CHECK(a->setting_int("mymod", "radius", -1) == 10);
    CHECK(a->setting_float("mymod", "radius", -1.0) == 10.0);
    CHECK(a->setting_int("mymod", "nope", -1) == -1);
    CHECK(a->setting_int("othermod", "radius", -1) == -1);
    const auto defs = f.state.settings->defs("mymod");
    REQUIRE(defs.size() == 1);
    CHECK(defs[0].label == "Radius");
    CHECK(defs[0].help == "How far");
    CHECK(defs[0].live);
    CHECK(defs[0].has_min);

    CHECK(a->setting_set("mymod", "radius", "25"));
    CHECK(a->setting_int("mymod", "radius", -1) == 25);
    CHECK(f.ini.values["mymod.radius"] == "25");
    CHECK_FALSE(a->setting_set("mymod", "radius", "99"));  // out of range: refused and logged
    CHECK(any_log("radius"));
    CHECK_FALSE(a->setting_set("mymod", "nope", "1"));

    EmfSettingDef c{};
    c.size = sizeof c;
    c.key = "place";
    c.type = EMF_SETTING_CHOICE;
    c.choices = "top right|bottom right|bottom left";
    c.default_value = "bottom right";
    REQUIRE(a->setting_declare("mymod", &c));
    CHECK(a->setting_int("mymod", "place", -1) == 1);
    CHECK(f.state.settings->defs("mymod")[1].choices.size() == 3);

    EmfSettingDef t{};
    t.size = sizeof t;
    t.key = "greeting";
    t.type = EMF_SETTING_TEXT;
    t.default_value = "hello there";
    REQUIRE(a->setting_declare("mymod", &t));
    char buf[6];
    CHECK(a->setting_text("mymod", "greeting", buf, sizeof buf) == 11);  // the full length
    CHECK(std::string(buf) == "hello");                                  // cut and NUL-terminated
    CHECK(a->setting_text("mymod", "nope", buf, sizeof buf) == 0);
    CHECK(buf[0] == '\0');

    EmfSettingDef bad = int_def("enabled", "1", 0, 1);
    CHECK_FALSE(a->setting_declare("mymod", &bad));  // reserved key
    EmfSettingDef short_def = int_def("x", "1", 0, 9);
    short_def.size = 8;
    CHECK_FALSE(a->setting_declare("mymod", &short_def));  // size field too small
    CHECK_FALSE(a->setting_declare("Bad Id", &d));
}

TEST_CASE("api v13: change callbacks run once per change after a world step; others' mods are not told") {
    Fixture f;
    const EmfApi* a = api_table();
    EmfSettingDef d = int_def("radius", "10", 2, 50);
    a->setting_declare("mymod", &d);
    a->setting_declare("other", &d);
    g_seen.clear();
    a->on_setting_changed("mymod", on_change, nullptr);
    a->on_setting_changed(nullptr, on_change, nullptr);  // every mod
    a->setting_set("mymod", "radius", "20");
    a->setting_set("other", "radius", "30");
    CHECK(g_seen.empty());  // nothing until a world steps
    pump_setting_changes(kWorld);
    REQUIRE(g_seen.size() == 3);  // mymod: both watchers; other: only the every-mod watcher
    CHECK(g_seen[0].world == kWorld);
    CHECK(g_seen[0].mod == "mymod");
    CHECK(g_seen[0].key == "radius");
    CHECK(g_seen[2].mod == "other");
    pump_setting_changes(kWorld);
    CHECK(g_seen.size() == 3);  // each change once
}

TEST_CASE("api v13: a faulting change callback is switched off and logged") {
    Fixture f;
    const EmfApi* a = api_table();
    EmfSettingDef d = int_def("radius", "10", 2, 50);
    a->setting_declare("mymod", &d);
    g_faults = 0;
    a->on_setting_changed("mymod", crashing_change, nullptr);
    a->setting_set("mymod", "radius", "20");
    pump_setting_changes(kWorld);
    CHECK(g_faults == 1);
    CHECK(any_log("on_setting_changed"));
    a->setting_set("mymod", "radius", "21");
    pump_setting_changes(kWorld);
    CHECK(g_faults == 1);  // not called again
}

TEST_CASE("api v13: mod_info and mod_info_at read the registry") {
    Fixture f;
    ModRecord r;
    r.id = "clock";
    r.name = "Clock";
    r.version = "1.1.0";
    r.source = "clock\\";
    r.status = ModStatus::Loaded;
    ModRecord s;
    s.id = "luamod";
    s.name = "Lua Mod";
    s.version = "0.3";
    s.source = "luamod\\";
    s.main = "main.lua";
    s.status = ModStatus::Script;
    f.state.mods.set({r, s});
    const EmfApi* a = api_table();
    EmfModInfo info{};
    info.size = sizeof info;
    REQUIRE(a->mod_info("clock", &info));
    CHECK(std::string(info.id) == "clock");
    CHECK(std::string(info.name) == "Clock");
    CHECK(std::string(info.version) == "1.1.0");
    CHECK(info.status == EMF_MOD_LOADED);
    CHECK_FALSE(a->mod_info("ghost", &info));
    REQUIRE(a->mod_info_at(1, &info));
    CHECK(std::string(info.id) == "luamod");
    CHECK(info.status == EMF_MOD_SCRIPT);
    CHECK(std::string(info.source) == "luamod\\");
    CHECK(std::string(info.main) == "main.lua");
    CHECK_FALSE(a->mod_info_at(2, &info));
    EmfModInfo tiny{};
    tiny.size = 4;
    CHECK_FALSE(a->mod_info("clock", &tiny));
}

TEST_CASE("api sizes: setting_declare and mod_info take the v13 size (not sizeof); mod_info writes nothing past it") {
    Fixture f;
    const EmfApi* a = api_table();
    EmfSettingDef d = int_def("radius", "10", 2, 50);
    d.size = kSettingDefV13Size;
    CHECK(a->setting_declare("mymod", &d));
    EmfSettingDef d2 = int_def("other", "1", 0, 9);
    d2.size = kSettingDefV13Size - 1;
    CHECK_FALSE(a->setting_declare("mymod", &d2));

    ModRecord r;
    r.id = "luamod";
    r.main = "main.lua";
    r.status = ModStatus::Script;
    f.state.mods.set({r});
    alignas(8) uint8_t buf[sizeof(EmfModInfo) + 8];
    std::memset(buf, 0xAB, sizeof buf);
    auto* info = reinterpret_cast<EmfModInfo*>(buf);
    info->size = kModInfoV13Size;  // main is the last field; sizeof adds padding (and grows with later fields)
    REQUIRE(a->mod_info("luamod", info));
    CHECK(std::string(info->main) == "main.lua");
    for (size_t i = kModInfoV13Size; i < sizeof buf; ++i) CHECK(buf[i] == 0xAB);
    info->size = kModInfoV13Size - 1;
    CHECK_FALSE(a->mod_info("luamod", info));
    CHECK_FALSE(a->mod_info_at(0, info));
}

TEST_CASE("emf.hpp v13: setting helpers and change callbacks with lambdas") {
    Fixture f;
    REQUIRE(emf::init(api_table()));
    EmfSettingDef d = int_def("radius", "10", 2, 50);
    api_table()->setting_declare("mymod", &d);
    CHECK(emf::setting_int("mymod", "radius", 0) == 10);
    CHECK(emf::setting_float("mymod", "radius", 0.0) == 10.0);
    CHECK(emf::setting_text("mymod", "radius") == "10");
    CHECK_FALSE(emf::setting_bool("mymod", "nope", false));
    CHECK(emf::set_setting("mymod", "radius", "12"));
    pump_setting_changes(kWorld);  // delivered before the watcher below exists
    std::vector<std::string> keys;
    emf::on_setting_changed("mymod", [&keys](emf::World w, const std::string& key) {
        CHECK(w.raw() == kWorld);
        keys.push_back(key);
    });
    emf::set_setting("mymod", "radius", "13");
    pump_setting_changes(kWorld);
    CHECK(keys == std::vector<std::string>{"radius"});
    auto info = emf::mod_info("nobody");
    CHECK_FALSE(info.has_value());
    emf::api_ref() = nullptr;
}

TEST_CASE("api v13: setting_text tells an empty value from an undeclared setting (buf[1] is 0 only for a value)") {
    Fixture f;
    const EmfApi* a = api_table();
    EmfSettingDef k{};
    k.size = sizeof k;
    k.key = "key";
    k.type = EMF_SETTING_KEY;
    k.default_value = "";
    REQUIRE(a->setting_declare("mymod", &k));
    char buf[4];
    std::memset(buf, 1, sizeof buf);
    CHECK(a->setting_text("mymod", "key", buf, sizeof buf) == 0);
    CHECK(buf[0] == '\0');
    CHECK(buf[1] == '\0');  // declared, empty
    std::memset(buf, 1, sizeof buf);
    CHECK(a->setting_text("mymod", "nope", buf, sizeof buf) == 0);
    CHECK(buf[0] == '\0');
    CHECK(buf[1] == 1);  // not declared: left alone
}

TEST_CASE("emf.hpp: setting_text gives an empty value as \"\"; the fallback only when the setting is not declared") {
    Fixture f;
    REQUIRE(emf::init(api_table()));
    EmfSettingDef k{};
    k.size = sizeof k;
    k.key = "key";
    k.type = EMF_SETTING_KEY;
    k.default_value = "J";
    REQUIRE(api_table()->setting_declare("mymod", &k));
    CHECK(emf::setting_text("mymod", "key", "X") == "J");
    REQUIRE(emf::set_setting("mymod", "key", ""));          // the player cleared it: no key
    CHECK(emf::setting_text("mymod", "key", "X").empty());  // not the fallback
    CHECK(emf::setting_text("mymod", "nope", "X") == "X");  // not declared
    emf::api_ref() = nullptr;
}

TEST_CASE("emf.hpp: a C++ exception in an on_setting_changed functor is caught, logged for the mod, and it stays on") {
    Fixture f;
    REQUIRE(emf::init(api_table()));
    EmfSettingDef d = int_def("radius", "10", 2, 50);
    api_table()->setting_declare("mymod", &d);
    pump_setting_changes(kWorld);
    int calls = 0;
    emf::on_setting_changed("mymod", [&calls](emf::World, const std::string&) {
        ++calls;
        throw std::runtime_error("bad value");
    });
    emf::set_setting("mymod", "radius", "13");
    pump_setting_changes(kWorld);
    CHECK(calls == 1);
    bool logged = false;
    for (const auto& l : g_log) logged |= l.find("mymod") != std::string::npos && l.find("bad value") != std::string::npos;
    CHECK_MESSAGE(logged, "the log names the mod and the exception");
    emf::set_setting("mymod", "radius", "14");
    pump_setting_changes(kWorld);
    CHECK(calls == 2);  // not switched off: an exception is not a fault
    emf::api_ref() = nullptr;
}

TEST_CASE("api v13: a faulting change callback's log names the module that holds it, not only the changed mod") {
    Fixture f;
    const EmfApi* a = api_table();
    EmfSettingDef d = int_def("radius", "10", 2, 50);
    a->setting_declare("other", &d);
    g_faults = 0;
    a->on_setting_changed(nullptr, crashing_change, nullptr);  // watches every mod
    a->setting_set("other", "radius", "20");
    pump_setting_changes(kWorld);
    CHECK(g_faults == 1);
    CHECK(any_log("emf_tests"));  // the callback lives in this exe
}

TEST_CASE("emf.hpp: setting_rgba reads a color setting as 0xRRGGBBAA; the fallback when undeclared or not a colour") {
    Fixture f;
    REQUIRE(emf::init(api_table()));
    SettingDef c;
    c.key = "tint";
    c.type = SettingType::Color;
    c.default_value = "#FFD070";
    REQUIRE(f.state.settings->declare("mymod", c));  // as the core declares a mod.json "color" setting
    CHECK(emf::setting_rgba("mymod", "tint", 0) == 0xFFD070FFu);
    REQUIRE(emf::set_setting("mymod", "tint", "#08f"));
    CHECK(emf::setting_rgba("mymod", "tint", 0) == 0x0088FFFFu);
    CHECK(emf::setting_rgba("mymod", "nope", 0x12345678u) == 0x12345678u);
    EmfSettingDef t{};
    t.size = sizeof t;
    t.key = "word";
    t.type = EMF_SETTING_TEXT;
    t.default_value = "green";
    REQUIRE(api_table()->setting_declare("mymod", &t));
    CHECK(emf::setting_rgba("mymod", "word", 7u) == 7u);
    emf::api_ref() = nullptr;
}
