#include <doctest.h>

#include <windows.h>

#include <atomic>
#include <map>
#include <string>
#include <thread>

#include "core/settings.h"
#include "emf/keys.hpp"

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
