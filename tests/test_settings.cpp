#include <doctest.h>

#include <windows.h>

#include <atomic>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>

#include "core/settings.h"
#include "emf/keys.hpp"
#include "test_scratch.h"

using namespace emf::core;

namespace {
struct FakeIni : IniBackend {
    std::map<std::string, std::string> values;  // "mod.key" -> text
    int writes = 0;
    bool fail = false;
    std::optional<std::string> read(const std::string& mod, const std::string& key) override {
        auto it = values.find(mod + "." + key);
        if (it == values.end()) return std::nullopt;
        return it->second;
    }
    bool write(const std::string& mod, const std::string& key, const std::string& value) override {
        if (fail) return false;
        ++writes;
        values[mod + "." + key] = value;
        return true;
    }
    bool erase(const std::string& mod, const std::string& key) override {
        if (fail) return false;
        values.erase(mod + "." + key);
        return true;
    }
};
// A backend from before erase existed (the default: it cannot remove a line).
struct OldIni : IniBackend {
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

SettingDef def(const char* key, SettingType t, const char* dflt, double mn = 0, double mx = 0, bool has_range = false,
               std::vector<std::string> choices = {}) {
    SettingDef d;
    d.key = key;
    d.type = t;
    d.default_value = dflt;
    if (has_range) {
        d.min = mn;
        d.max = mx;
        d.has_min = d.has_max = true;
    }
    d.choices = std::move(choices);
    return d;
}
}  // namespace

TEST_CASE("settings: defaults show until the ini holds a value; ini values are read when declared") {
    FakeIni ini;
    ini.values["clock.h24"] = "0";
    SettingsStore s(&ini);
    REQUIRE(s.declare("clock", def("place", SettingType::Choice, "1", 0, 0, false, {"tr", "br", "bl"})));
    REQUIRE(s.declare("clock", def("h24", SettingType::Bool, "1")));
    CHECK(s.get("clock", "place") == "1");
    CHECK(s.get("clock", "h24") == "0");
    CHECK(s.get_int("clock", "place", -1) == 1);
    CHECK(s.get_int("clock", "missing", -1) == -1);
    CHECK(s.get("clock", "missing").empty());
    CHECK(s.defs("clock").size() == 2);
    CHECK(s.mods() == std::vector<std::string>{"clock"});
    CHECK(ini.writes == 0);  // declaring never writes
}

TEST_CASE("settings: bad ini values are clamped or fall back to the default") {
    FakeIni ini;
    ini.values["m.radius"] = "abc";
    ini.values["m.big"] = "999";
    ini.values["m.place"] = "7";
    ini.values["m.flag"] = "maybe";
    ini.values["m.speed"] = "-3.5";
    ini.values["m.key"] = "notakey";
    SettingsStore s(&ini);
    s.declare("m", def("radius", SettingType::Int, "10", 2, 50, true));
    s.declare("m", def("big", SettingType::Int, "10", 2, 50, true));
    s.declare("m", def("place", SettingType::Choice, "1", 0, 0, false, {"a", "b", "c"}));
    s.declare("m", def("flag", SettingType::Bool, "1"));
    s.declare("m", def("speed", SettingType::Float, "1.5", 0.5, 4, true));
    s.declare("m", def("key", SettingType::Key, "L"));
    CHECK(s.get("m", "radius") == "10");  // not a number: default
    CHECK(s.get("m", "big") == "50");     // above max: clamped
    CHECK(s.get("m", "place") == "1");    // no such choice: default
    CHECK(s.get("m", "flag") == "1");
    CHECK(s.get("m", "speed") == "0.5");  // below min: clamped
    CHECK(s.get_float("m", "speed", 0) == doctest::Approx(0.5));
    CHECK(s.get("m", "key") == "L");
}

TEST_CASE("settings: set validates, writes through only on a change, and records each change once") {
    FakeIni ini;
    SettingsStore s(&ini);
    s.declare("m", def("radius", SettingType::Int, "10", 2, 50, true));
    s.declare("m", def("flag", SettingType::Bool, "0"));
    s.declare("m", def("place", SettingType::Choice, "0", 0, 0, false, {"top right", "bottom right"}));
    s.declare("m", def("speed", SettingType::Float, "1", 0.5, 4, true));
    s.declare("m", def("key", SettingType::Key, "L"));
    s.declare("m", def("name", SettingType::Text, ""));
    const uint64_t v0 = s.version();
    std::string why;
    CHECK(s.set("m", "radius", "20", &why) == SettingsStore::SetResult::Ok);
    CHECK(ini.values["m.radius"] == "20");
    CHECK(s.version() > v0);
    CHECK(s.set("m", "radius", "20") == SettingsStore::SetResult::Unchanged);
    CHECK(ini.writes == 1);
    CHECK(s.set("m", "radius", "99", &why) == SettingsStore::SetResult::Invalid);
    CHECK(why.find("50") != std::string::npos);
    CHECK(s.set("m", "radius", "2.5") == SettingsStore::SetResult::Invalid);
    CHECK(s.set("m", "flag", "on") == SettingsStore::SetResult::Ok);
    CHECK(s.get("m", "flag") == "1");
    CHECK(s.set("m", "flag", "False") == SettingsStore::SetResult::Ok);
    CHECK(s.get("m", "flag") == "0");
    CHECK(s.set("m", "place", "bottom right") == SettingsStore::SetResult::Ok);  // by name
    CHECK(s.get("m", "place") == "1");
    CHECK(s.set("m", "place", "0") == SettingsStore::SetResult::Ok);  // by index
    CHECK(s.set("m", "place", "5") == SettingsStore::SetResult::Invalid);
    CHECK(s.set("m", "speed", "2.25") == SettingsStore::SetResult::Ok);
    CHECK(s.get("m", "speed") == "2.25");
    CHECK(s.set("m", "key", "f2") == SettingsStore::SetResult::Ok);
    CHECK(s.get("m", "key") == "F2");  // stored by its canonical name
    CHECK(s.set("m", "key", "Mouse1") == SettingsStore::SetResult::Invalid);
    CHECK(s.set("m", "key", "0xBA") == SettingsStore::SetResult::Ok);  // a key without a name
    CHECK(s.get("m", "key") == "0xba");  // stored so it reads back
    CHECK(emf::keys::parse_key(L"0xba") == 0xBA);
    CHECK(s.set("m", "name", "Line\nbreak") == SettingsStore::SetResult::Invalid);
    CHECK(s.set("m", "name", std::string(257, 'x')) == SettingsStore::SetResult::Invalid);
    CHECK(s.set("m", "name", "Bob") == SettingsStore::SetResult::Ok);
    CHECK(s.set("m", "nope", "1") == SettingsStore::SetResult::UnknownSetting);
    CHECK(s.set("other", "radius", "1") == SettingsStore::SetResult::UnknownSetting);

    auto changes = s.take_changes();
    REQUIRE(changes.size() == 9);
    CHECK(changes[0] == std::make_pair(std::string("m"), std::string("radius")));
    CHECK(s.take_changes().empty());

    ini.fail = true;
    CHECK(s.set("m", "radius", "30", &why) == SettingsStore::SetResult::WriteFailed);
    CHECK(s.get("m", "radius") == "20");  // unchanged when it could not be saved
}

TEST_CASE("settings: redeclaring replaces the definition and keeps a value that is still valid") {
    FakeIni ini;
    SettingsStore s(&ini);
    s.declare("m", def("radius", SettingType::Int, "10", 2, 50, true));
    s.set("m", "radius", "40");
    s.declare("m", def("radius", SettingType::Int, "10", 2, 100, true));
    CHECK(s.get("m", "radius") == "40");
    s.declare("m", def("radius", SettingType::Int, "10", 2, 30, true));
    CHECK(s.get("m", "radius") == "30");  // clamped to the new range
    CHECK(s.defs("m").size() == 1);
    std::string why;
    CHECK_FALSE(s.declare("m", def("enabled", SettingType::Bool, "1"), &why));
    CHECK_FALSE(s.declare("Bad Mod", def("x", SettingType::Bool, "1"), &why));
    CHECK_FALSE(s.declare("m", def("k", SettingType::Key, "Mouse1"), &why));  // a key default must be a key
}

TEST_CASE("settings: the on/off switch is the mod's enabled key") {
    FakeIni ini;
    SettingsStore s(&ini);
    CHECK(s.enabled("m", true));
    CHECK_FALSE(s.enabled("m", false));
    ini.values["m.enabled"] = "0";
    CHECK_FALSE(s.enabled("m", true));
    CHECK(s.set_enabled("m", true));
    CHECK(ini.values["m.enabled"] == "1");
    CHECK(s.enabled("m", false));
    auto changes = s.take_changes();
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].second == "enabled");
}

TEST_CASE("keys: names for common keys, case-insensitive; the old forms still work") {
    std::string name;
    CHECK(emf::keys::parse_key(L"f2", &name) == VK_F2);
    CHECK(name == "F2");
    CHECK(emf::keys::parse_key(L"F12") == VK_F12);
    CHECK(emf::keys::parse_key(L"F24") == VK_F24);
    CHECK(emf::keys::parse_key(L"F25") == 0);
    CHECK(emf::keys::parse_key(L"Space", &name) == VK_SPACE);
    CHECK(name == "Space");
    CHECK(emf::keys::parse_key(L"pageup") == VK_PRIOR);
    CHECK(emf::keys::parse_key(L"Numpad7", &name) == VK_NUMPAD7);
    CHECK(name == "Numpad7");
    CHECK(emf::keys::parse_key(L"Shift") == VK_SHIFT);
    CHECK(emf::keys::parse_key(L"Ctrl") == VK_CONTROL);
    CHECK(emf::keys::parse_key(L"Alt") == VK_MENU);
    CHECK(emf::keys::parse_key(L"Tab") == VK_TAB);
    CHECK(emf::keys::parse_key(L"Insert") == VK_INSERT);
    CHECK(emf::keys::parse_key(L"Mouse1") == 0);
    CHECK(emf::keys::parse_key(L"G", &name) == 'G');
    CHECK(name == "G");
    CHECK(emf::keys::parse_key(L"0x71", &name) == VK_F2);
    CHECK(name == "F2");  // a number that has a name shows the name
    CHECK(emf::keys::parse_key(L"0xBA", &name) == 0xBA);
    CHECK(name == "key 0xba");
}

TEST_CASE("settings: a key setting can be empty (no key)") {
    FakeIni ini;
    SettingsStore s(&ini);
    REQUIRE(s.declare("m", def("key_stay", SettingType::Key, "")));
    CHECK(s.get("m", "key_stay").empty());
    CHECK(s.set("m", "key_stay", "F3") == SettingsStore::SetResult::Ok);
    CHECK(s.set("m", "key_stay", "") == SettingsStore::SetResult::Ok);  // cleared again
    CHECK(s.get("m", "key_stay").empty());
    CHECK(ini.values["m.key_stay"].empty());
}

TEST_CASE("settings: a mod's new default key reaches only players who never set that key; a chosen key stays") {
    // Defaults are never written into the ini, so a value there is one the player (or a profile they loaded) set.
    FakeIni ini;
    SettingsStore before(&ini);
    REQUIRE(before.declare("companion", def("key", SettingType::Key, "G")));
    CHECK(before.get("companion", "key") == "G");
    CHECK(before.set("companion", "key", "G") == SettingsStore::SetResult::Unchanged);  // picking the default: nothing kept
    CHECK(ini.writes == 0);
    CHECK(ini.values.empty());
    SettingsStore after(&ini);  // the next game start, with the new mod.json
    REQUIRE(after.declare("companion", def("key", SettingType::Key, "F6")));
    CHECK(after.get("companion", "key") == "F6");
    CHECK(ini.values.empty());

    FakeIni chose;  // a player who picked G (or any key) keeps it
    chose.values["companion.key"] = "G";
    SettingsStore mine(&chose);
    REQUIRE(mine.declare("companion", def("key", SettingType::Key, "F6")));
    CHECK(mine.get("companion", "key") == "G");
    CHECK(chose.writes == 0);
    CHECK(chose.values["companion.key"] == "G");
}

TEST_CASE("settings: a corrected ini value is written back, so mods reading their ini see what the Mods tab shows") {
    FakeIni ini;
    ini.values["m.radius"] = "999";
    ini.values["m.bad"] = "abc";
    ini.values["m.sci"] = "1e1";
    ini.values["m.half"] = "12.6";
    ini.values["m.flag"] = "yes";
    ini.values["m.fine"] = "7";
    SettingsStore s(&ini);
    s.declare("m", def("radius", SettingType::Int, "10", 1, 50, true));
    s.declare("m", def("bad", SettingType::Int, "10", 1, 50, true));
    s.declare("m", def("sci", SettingType::Int, "5", 1, 50, true));
    s.declare("m", def("half", SettingType::Int, "5", 1, 50, true));
    s.declare("m", def("flag", SettingType::Bool, "0"));
    s.declare("m", def("fine", SettingType::Int, "5", 1, 50, true));
    s.declare("m", def("absent", SettingType::Int, "5", 1, 50, true));
    CHECK(ini.values["m.radius"] == "50");
    CHECK(ini.values["m.bad"] == "10");
    CHECK(ini.values["m.sci"] == "10");
    CHECK(ini.values["m.half"] == "13");
    CHECK(ini.values["m.flag"] == "1");
    CHECK(ini.values["m.fine"] == "7");
    CHECK(ini.writes == 5);                       // only the corrected ones
    CHECK(ini.values.count("m.absent") == 0);     // a missing value stays missing (the default applies)
}

TEST_CASE("settings: a hand-edited ini value that had to change (range, step, not a number) is named for the log") {
    FakeIni ini;
    ini.values["m.every"] = "1";      // a slider in steps of 5 from 0: becomes 0, which is "off"
    ini.values["m.radius"] = "999";   // above the maximum
    ini.values["m.bad"] = "abc";      // not a number: the default
    ini.values["m.flag"] = "yes";     // the same meaning, written as 1: nothing to say
    ini.values["m.sci"] = "1e1";      // the same number: nothing to say
    SettingsStore s(&ini);
    SettingDef every = def("every", SettingType::Int, "30", 0, 120, true);
    every.step = 5;
    std::string why;
    REQUIRE(s.declare("m", every, &why));
    CHECK(why.find("\"every\"") != std::string::npos);
    CHECK(why.find("1 in the ini became 0") != std::string::npos);
    CHECK(why.find("steps of 5") != std::string::npos);
    REQUIRE(s.declare("m", def("radius", SettingType::Int, "10", 1, 50, true), &why));
    CHECK(why.find("999 in the ini became 50") != std::string::npos);
    REQUIRE(s.declare("m", def("bad", SettingType::Int, "10", 1, 50, true), &why));
    CHECK(why.find("abc in the ini became 10") != std::string::npos);
    REQUIRE(s.declare("m", def("flag", SettingType::Bool, "0"), &why));
    CHECK(why.empty());
    REQUIRE(s.declare("m", def("sci", SettingType::Int, "5", 1, 50, true), &why));
    CHECK(why.empty());
}

TEST_CASE("settings: an enabled value other than 1/0 is written back as 1/0 (mods read it with GetPrivateProfileInt)") {
    FakeIni ini;
    ini.values["m.enabled"] = "true";
    SettingsStore s(&ini);
    CHECK(s.enabled("m", false));
    CHECK(ini.values["m.enabled"] == "1");
    ini.values["n.enabled"] = "off";
    CHECK_FALSE(s.enabled("n", true));
    CHECK(ini.values["n.enabled"] == "0");
    ini.values["o.enabled"] = "maybe";  // not a bool: the default, file untouched
    CHECK(s.enabled("o", true));
    CHECK(ini.values["o.enabled"] == "maybe");
}

TEST_CASE("settings: text that the ini cannot keep (quotes or spaces around it) is refused") {
    FakeIni ini;
    SettingsStore s(&ini);
    s.declare("m", def("name", SettingType::Text, ""));
    std::string why;
    CHECK(s.set("m", "name", "\"quoted\"", &why) == SettingsStore::SetResult::Invalid);
    CHECK(why.find("quote") != std::string::npos);
    CHECK(s.set("m", "name", " padded", &why) == SettingsStore::SetResult::Invalid);
    CHECK(s.set("m", "name", "has \"inner\" quotes and spaces", &why) == SettingsStore::SetResult::Ok);
}

TEST_CASE("settings: the file backend keeps non-ASCII text (the ini becomes UTF-16)") {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"emf_settings_test_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    DeleteFileW((dir + L"\\m.ini").c_str());
    {
        FileIniBackend b(dir);
        REQUIRE(b.write("m", "plain", "abc"));                    // creates an ordinary file
        REQUIRE(b.write("m", "name", "Gr\xC3\xBC\xC3\x9F" "e \xE6\x97\xA5\xE6\x9C\xAC"));  // "Grüße 日本"
    }
    FileIniBackend again(dir);
    CHECK(again.read("m", "name").value_or("") == "Gr\xC3\xBC\xC3\x9F" "e \xE6\x97\xA5\xE6\x9C\xAC");
    CHECK(again.read("m", "plain").value_or("") == "abc");  // what was there before survives the conversion
    wchar_t back[64] = {};
    GetPrivateProfileStringW(L"m", L"plain", L"", back, 64, (dir + L"\\m.ini").c_str());
    CHECK(std::wstring(back) == L"abc");  // mods' own reads still work
    DeleteFileW((dir + L"\\m.ini").c_str());
    RemoveDirectoryW(dir.c_str());
}

namespace {
// A backend whose first write checks that another thread can read the store meanwhile (the file write is slow).
struct ReaderDuringWriteIni : IniBackend {
    SettingsStore* store = nullptr;
    std::atomic<bool> read_done{false};
    bool read_during_write = false;
    std::thread reader;
    std::map<std::string, std::string> values;
    std::optional<std::string> read(const std::string&, const std::string&) override { return std::nullopt; }
    bool write(const std::string& mod, const std::string& key, const std::string& value) override {
        if (store && !reader.joinable()) {
            reader = std::thread([this] {
                (void)store->get("m", "radius");
                read_done = true;
            });
            for (int i = 0; i < 200 && !read_done; ++i) Sleep(5);  // up to a second
            read_during_write = read_done;
        }
        values[mod + "." + key] = value;
        return true;
    }
};
}  // namespace

TEST_CASE("settings: set writes the file without holding the store's lock (the Mods tab and mods read on)") {
    ReaderDuringWriteIni ini;
    SettingsStore s(&ini);
    REQUIRE(s.declare("m", def("radius", SettingType::Int, "10", 2, 50, true)));
    ini.store = &s;
    CHECK(s.set("m", "radius", "20") == SettingsStore::SetResult::Ok);
    if (ini.reader.joinable()) ini.reader.join();
    CHECK(ini.read_during_write);
    CHECK(s.get("m", "radius") == "20");
}

TEST_CASE("settings: changes from two threads keep their order: the file, the value and the change list agree") {
    struct LockedIni : IniBackend {
        std::map<std::string, std::string> values;
        std::optional<std::string> read(const std::string&, const std::string&) override { return std::nullopt; }
        bool write(const std::string& mod, const std::string& key, const std::string& value) override {
            values[mod + "." + key] = value;  // the store calls writes one at a time
            Sleep(0);
            return true;
        }
    } ini;
    SettingsStore s(&ini);
    REQUIRE(s.declare("m", def("radius", SettingType::Int, "10", 0, 1000, true)));
    std::atomic<int> ok{0};
    auto work = [&](int base) {
        for (int i = 0; i < 200; ++i)
            if (s.set("m", "radius", std::to_string(base + (i % 2))) == SettingsStore::SetResult::Ok) ++ok;
    };
    std::thread a(work, 100), b(work, 500);
    a.join();
    b.join();
    CHECK(ini.values["m.radius"] == s.get("m", "radius"));  // the last write is the value kept
    CHECK(static_cast<int>(s.take_changes().size()) == ok.load());
}

TEST_CASE("settings: a whole number beyond what the ini and the API keep exactly is refused (or clamped from the ini)") {
    FakeIni ini;
    ini.values["m.huge"] = "1e300";
    SettingsStore s(&ini);
    REQUIRE(s.declare("m", def("count", SettingType::Int, "3")));
    REQUIRE(s.declare("m", def("huge", SettingType::Int, "3")));
    std::string why;
    CHECK(s.set("m", "count", "1e300", &why) == SettingsStore::SetResult::Invalid);
    CHECK(why.find("999999999999999") != std::string::npos);
    CHECK(s.set("m", "count", "999999999999999") == SettingsStore::SetResult::Ok);
    CHECK(s.get_int("m", "count", 0) == 999999999999999LL);
    CHECK(s.get("m", "huge") == "999999999999999");  // from the ini: clamped
}

// ---- EMF's own settings in emf_core.ini (in-game look phase 2) ----
namespace {
std::string file_bytes(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
void put_bytes(const std::wstring& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
}
SettingDef int_def(const char* key, const char* dflt, double mn, double mx) {
    SettingDef d;
    d.key = key;
    d.type = SettingType::Int;
    d.default_value = dflt;
    d.min = mn;
    d.max = mx;
    d.has_min = d.has_max = true;
    return d;
}
}  // namespace

TEST_CASE("emf_core.ini: hud.size reads and writes [hud] size; a ; comment and spaces are not part of the value") {
    const std::wstring ini = test_scratch::temp_path(L"emf_core_test_a", L".ini");
    put_bytes(ini, "[hud]\r\nsize = 140 ; bigger\r\n");
    CoreIniBackend b(ini);
    CHECK(b.read(kEmfSettingsId, "hud.size").value_or("") == "140");
    CHECK_FALSE(b.read(kEmfSettingsId, "hud.missing").has_value());
    REQUIRE(b.write(kEmfSettingsId, "hud.size", "120"));
    wchar_t back[32] = {};
    GetPrivateProfileStringW(L"hud", L"size", L"", back, 32, ini.c_str());
    CHECK(std::wstring(back) == L"120");
    CHECK_FALSE(b.read(kEmfSettingsId, "nodot").has_value());  // a key is <section>.<name>
    CHECK_FALSE(b.write(kEmfSettingsId, "nodot", "1"));
    DeleteFileW(ini.c_str());
}

TEST_CASE("emf_core.ini: a change touches only its own line") {
    const std::wstring ini = test_scratch::temp_path(L"emf_core_test_b", L".ini");
    const std::string before =
        "; my notes\r\n[hud]\r\nstyle=boxed\r\nsize=140\r\n\r\n[chat]\r\nadmins=0x1\r\n[weird]\r\nx = y\r\n";
    put_bytes(ini, before);
    CoreIniBackend core(ini);
    FakeIni mods;
    RoutingIniBackend route(&core, &mods);
    SettingsStore s(&route);
    REQUIRE(s.declare(kEmfSettingsId, int_def("hud.size", "100", 60, 200), nullptr, SettingsStore::Declare::Keep));
    CHECK(s.get(kEmfSettingsId, "hud.size") == "140");
    REQUIRE(s.set(kEmfSettingsId, "hud.size", "120") == SettingsStore::SetResult::Ok);
    std::string expected = before;
    expected.replace(expected.find("size=140"), 8, "size=120");
    CHECK(file_bytes(ini) == expected);  // every other byte as the player left it, and no UTF-16 BOM
    CHECK(mods.writes == 0);
    DeleteFileW(ini.c_str());
}

TEST_CASE("routing: mod emf goes to emf_core.ini, other mods to their own ini") {
    FakeIni core, mods;
    RoutingIniBackend route(&core, &mods);
    REQUIRE(route.write(kEmfSettingsId, "hud.size", "120"));
    REQUIRE(route.write("clock", "corner", "top_right"));
    CHECK(core.values.count("emf.hud.size") == 1);
    CHECK(core.values.count("clock.corner") == 0);
    CHECK(mods.values.count("clock.corner") == 1);
    CHECK(route.read(kEmfSettingsId, "hud.size").value_or("") == "120");
    CHECK(route.read("clock", "corner").value_or("") == "top_right");
}

TEST_CASE("Declare::Keep: a value outside the range is used clamped and never written back") {
    FakeIni ini;
    ini.values["emf.hud.size"] = "500";
    SettingsStore s(&ini);
    REQUIRE(s.declare(kEmfSettingsId, int_def("hud.size", "100", 60, 200), nullptr, SettingsStore::Declare::Keep));
    CHECK(s.get(kEmfSettingsId, "hud.size") == "200");
    CHECK(ini.writes == 0);
    CHECK(ini.values["emf.hud.size"] == "500");  // the player's text stays in the file
    FakeIni mod_ini;                              // mods keep today's behaviour: the clamped value is written back
    mod_ini.values["m.size"] = "500";
    SettingsStore m(&mod_ini);
    REQUIRE(m.declare("m", int_def("size", "100", 60, 200)));
    CHECK(mod_ini.values["m.size"] == "200");
}

TEST_CASE("emf_core.ini missing: the defaults, and the first change creates the file") {
    const std::wstring ini = test_scratch::temp_path(L"emf_core_test_c", L".ini");
    DeleteFileW(ini.c_str());
    CoreIniBackend core(ini);
    FakeIni mods;
    RoutingIniBackend route(&core, &mods);
    SettingsStore s(&route);
    REQUIRE(s.declare(kEmfSettingsId, int_def("hud.size", "100", 60, 200), nullptr, SettingsStore::Declare::Keep));
    CHECK(s.get(kEmfSettingsId, "hud.size") == "100");
    CHECK(GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES);  // declaring wrote nothing
    REQUIRE(s.set(kEmfSettingsId, "hud.size", "150") == SettingsStore::SetResult::Ok);
    wchar_t back[32] = {};
    GetPrivateProfileStringW(L"hud", L"size", L"", back, 32, ini.c_str());
    CHECK(std::wstring(back) == L"150");
    DeleteFileW(ini.c_str());
}

TEST_CASE("settings: stored() tells a value from the ini or a change from a default nobody set") {
    FakeIni ini;
    ini.values["emf.hud.style"] = "boxed";
    SettingsStore s(&ini);
    REQUIRE(s.declare(kEmfSettingsId, int_def("hud.size", "100", 60, 200), nullptr, SettingsStore::Declare::Keep));
    SettingDef style;
    style.key = "hud.style";
    style.type = SettingType::Text;
    style.default_value = "game";
    REQUIRE(s.declare(kEmfSettingsId, style, nullptr, SettingsStore::Declare::Keep));
    CHECK(s.stored(kEmfSettingsId, "hud.style"));
    CHECK_FALSE(s.stored(kEmfSettingsId, "hud.size"));  // the default: not in the file
    REQUIRE(s.set(kEmfSettingsId, "hud.size", "120") == SettingsStore::SetResult::Ok);
    CHECK(s.stored(kEmfSettingsId, "hud.size"));
    CHECK_FALSE(s.stored(kEmfSettingsId, "nope"));
}

TEST_CASE("emf_core.ini: a size typed as 150% reads as 150") {
    const std::wstring ini = test_scratch::temp_path(L"emf_core_test_d", L".ini");
    put_bytes(ini, "[hud]\r\nsize = 150% ; bigger\r\n");
    CoreIniBackend b(ini);
    CHECK(b.read(kEmfSettingsId, "hud.size").value_or("") == "150");
    DeleteFileW(ini.c_str());
}

TEST_CASE("settings: reset puts a setting back to its default and removes its line (GitHub #54)") {
    FakeIni ini;
    SettingsStore store(&ini);
    REQUIRE(store.declare("clock", def("radius", SettingType::Int, "10", 2, 50, true)));
    REQUIRE(store.set("clock", "radius", "30") == SettingsStore::SetResult::Ok);
    store.take_changes();
    CHECK(store.stored("clock", "radius"));
    CHECK(store.reset("clock", "radius") == SettingsStore::SetResult::Ok);
    CHECK(store.get("clock", "radius") == "10");
    CHECK_FALSE(store.stored("clock", "radius"));
    CHECK(ini.values.count("clock.radius") == 0);  // the line is gone: a later default follows the mod
    CHECK(store.take_changes() == std::vector<std::pair<std::string, std::string>>{{"clock", "radius"}});
    CHECK(store.reset("clock", "radius") == SettingsStore::SetResult::Unchanged);  // nobody set it
    std::string why;
    CHECK(store.reset("clock", "nope", &why) == SettingsStore::SetResult::UnknownSetting);
    CHECK(why.find("nope") != std::string::npos);
    // the default written in the file by hand counts as set: reset removes its line
    ini.values["clock.radius"] = "10";
    SettingsStore fresh(&ini);
    REQUIRE(fresh.declare("clock", def("radius", SettingType::Int, "10", 2, 50, true)));  // read from the file
    CHECK(fresh.stored("clock", "radius"));
    CHECK(fresh.reset("clock", "radius") == SettingsStore::SetResult::Ok);
    CHECK(ini.values.count("clock.radius") == 0);
}

TEST_CASE("settings: a reset that cannot remove the line changes nothing") {
    OldIni old;  // a backend that cannot remove a line
    SettingsStore store(&old);
    REQUIRE(store.declare("clock", def("radius", SettingType::Int, "10", 2, 50, true)));
    REQUIRE(store.set("clock", "radius", "30") == SettingsStore::SetResult::Ok);
    store.take_changes();
    std::string why;
    CHECK(store.reset("clock", "radius", &why) == SettingsStore::SetResult::WriteFailed);
    CHECK(store.get("clock", "radius") == "30");
    CHECK(store.stored("clock", "radius"));
    CHECK(store.take_changes().empty());
    CHECK_FALSE(why.empty());
}


TEST_CASE("settings: the file backends remove a key's line (and only it); no file or no line is fine") {
    const std::wstring ini = test_scratch::temp_path(L"emf_core_test_erase", L".ini");
    put_bytes(ini, "[hud]\r\nsize = 140 ; bigger\r\nstyle=boxed\r\n");
    CoreIniBackend core(ini);
    CHECK(core.erase(kEmfSettingsId, "hud.size"));
    CHECK_FALSE(core.read(kEmfSettingsId, "hud.size").has_value());
    CHECK(core.read(kEmfSettingsId, "hud.style").value_or("") == "boxed");
    CHECK(core.erase(kEmfSettingsId, "hud.size"));  // no line: fine
    CHECK_FALSE(core.erase(kEmfSettingsId, "nodot"));
    put_bytes(ini, "[hud]\r\nsize=140\r\nsize=150\r\nstyle=boxed\r\n");  // a hand-edited file with the key twice
    CHECK(core.erase(kEmfSettingsId, "hud.size"));
    CHECK_FALSE(core.read(kEmfSettingsId, "hud.size").has_value());
    CHECK(core.read(kEmfSettingsId, "hud.style").value_or("") == "boxed");
    DeleteFileW(ini.c_str());
    CHECK(core.erase(kEmfSettingsId, "hud.style"));  // no file: fine, and none is made
    CHECK(GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES);

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"emf_settings_erase_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    FileIniBackend files(dir);
    CHECK(files.erase("m", "radius"));  // no file
    CHECK(GetFileAttributesW((dir + L"\\m.ini").c_str()) == INVALID_FILE_ATTRIBUTES);
    REQUIRE(files.write("m", "radius", "30"));
    REQUIRE(files.write("m", "enabled", "1"));
    CHECK(files.erase("m", "radius"));
    CHECK_FALSE(files.read("m", "radius").has_value());
    CHECK(files.read("m", "enabled").value_or("") == "1");
    DeleteFileW((dir + L"\\m.ini").c_str());
    CHECK(RemoveDirectoryW(dir.c_str()));  // nothing left in it
}
