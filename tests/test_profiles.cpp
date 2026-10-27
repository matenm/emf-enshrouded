// Mod profiles, the shared part (src/core/profiles.h): names and slugs, the file, the store, and the rules for
// capturing, comparing and applying a profile over a folder (a fake switcher here; test_app_profiles.cpp applies to a
// real fake game folder).
#include <windows.h>

#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "core/profiles.h"

using namespace emf::core;

namespace {

struct TempDir {
    std::wstring dir;
    explicit TempDir(const wchar_t* name) {
        wchar_t base[MAX_PATH];
        GetTempPathW(MAX_PATH, base);
        dir = std::wstring(base) + L"emf_profiles_test_" + name + L"_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
              std::to_wstring(GetTickCount64());
        std::filesystem::remove_all(dir);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    void write(const std::wstring& rel, const std::string& bytes) const {
        const std::filesystem::path p(dir + L"\\" + rel);
        std::filesystem::create_directories(p.parent_path());
        std::ofstream f(p, std::ios::binary);
        f << bytes;
    }
    std::string read(const std::wstring& rel) const {
        std::ifstream f(std::filesystem::path(dir + L"\\" + rel), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(f), {});
    }
    bool has(const std::wstring& rel) const { return std::filesystem::exists(std::filesystem::path(dir + L"\\" + rel)); }
};

FolderMod fm(const char* source, const char* id, bool on, bool can = true) {
    FolderMod m;
    m.source = source;
    m.id = id;
    m.name = std::string("Name of ") + id;
    m.on = on;
    m.can_switch = can;
    if (!can) {
        m.why_not = "its own loader switches it";
        m.read_only = true;  // its loader manages it
    }
    return m;
}

// Records what applying asked for; switches listed in `fail` fail, settings in `same` answer "unchanged".
struct FakeSwitcher : ProfileSwitcher {
    std::vector<std::string> calls;
    std::map<std::string, std::string> fail;  // "source:id" or "mod.key" -> error
    std::vector<std::string> same;            // "mod.key" the store finds unchanged
    std::vector<FolderMod>* folder = nullptr;  // switched for real when set
    Result set_enabled(const std::string& source, const std::string& id, bool on) override {
        calls.push_back(source + ":" + id + "=" + (on ? "1" : "0"));
        if (auto it = fail.find(source + ":" + id); it != fail.end()) return {false, it->second};
        if (folder)
            for (auto& m : *folder)
                if (m.source == source && m.id == id) m.on = on;
        return {true, {}};
    }
    Result set_setting(const std::string& mod, const std::string& key, const std::string& value) override {
        calls.push_back(mod + "." + key + "=" + value);
        if (auto it = fail.find(mod + "." + key); it != fail.end()) return {false, it->second};
        for (const auto& s : same)
            if (s == mod + "." + key) return {true, {}, true};
        if (folder)
            for (auto& m : *folder)
                if (m.source == "emf" && m.id == mod)
                    for (auto& s : m.settings)
                        if (s.key == key) s.value = value;
        return {true, {}};
    }
};

std::vector<std::string> ids(const std::vector<ApplyItem>& items) {
    std::vector<std::string> out;
    for (const auto& i : items) out.push_back(i.source + ":" + i.id);
    return out;
}

bool has_line(const std::vector<std::string>& lines, const std::string& part) {
    for (const auto& l : lines)
        if (l.find(part) != std::string::npos) return true;
    return false;
}

}  // namespace

// ---- names ----

TEST_CASE("profiles: a name is 1-40 characters that are safe in a file name") {
    CHECK(profile_name_problem("Vanilla+") == "");
    CHECK(profile_name_problem("Building 2") == "");
    CHECK(profile_name_problem("  Testing  ") == "");  // spaces at the ends are trimmed
    CHECK(trim_profile_name("  Testing  ") == "Testing");
    CHECK(profile_name_problem("Caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC") == "");  // UTF-8 letters count as characters
    CHECK(profile_name_problem(std::string(40, 'a')) == "");
    CHECK(profile_name_problem(std::string(41, 'a')) != "");
    std::string forty_cjk;
    for (int i = 0; i < 40; ++i) forty_cjk += "\xE6\x97\xA5";  // 40 characters, 120 bytes
    CHECK(profile_name_problem(forty_cjk) == "");
    CHECK(profile_name_problem(forty_cjk + "\xE6\x97\xA5") != "");
    CHECK(profile_name_problem("") != "");
    CHECK(profile_name_problem("   ") != "");
    for (const char* bad : {"a/b", "a\\b", "a:b", "a*b", "a?b", "a\"b", "a<b", "a>b", "a|b", "tab\there", "nl\nx"})
        CHECK_MESSAGE(profile_name_problem(bad) != "", bad);
    CHECK(profile_name_problem("..") != "");
    CHECK(profile_name_problem("bad \xFF utf8") != "");
}

TEST_CASE("profiles: the file name is a slug of the name; names without letters or digits still get one") {
    CHECK(profile_slug("Vanilla+") == "vanilla");
    CHECK(profile_slug("Building 2") == "building-2");
    CHECK(profile_slug("  Big -- Base!! ") == "big-base");
    // a name with other letters keeps its ASCII part and gets a hash of the whole name, so names that differ only in
    // those letters stay two profiles ("Строительство 1" and "Выживание 1" were both "1")
    const std::string cafe = profile_slug("Caf\xC3\xA9");
    CHECK(cafe.rfind("caf-", 0) == 0);
    CHECK(cafe.size() == 12);
    CHECK(cafe != profile_slug("Caf\xC3\xA8"));
    const std::string build = profile_slug("\xD0\xA1\xD1\x82\xD1\x80\xD0\xBE\xD0\xB8\xD1\x82\xD0\xB5\xD0\xBB\xD1\x8C\xD1\x81\xD1\x82\xD0\xB2\xD0\xBE 1");
    const std::string survive = profile_slug("\xD0\x92\xD1\x8B\xD0\xB6\xD0\xB8\xD0\xB2\xD0\xB0\xD0\xBD\xD0\xB8\xD0\xB5 1");
    CHECK(build.rfind("1-", 0) == 0);
    CHECK(build != survive);
    CHECK(valid_profile_slug(build));
    CHECK(profile_slug("Vanilla+") == profile_slug("vanilla+"));  // plain ASCII names: case and signs still ignored
    CHECK(profile_slug("con") == "con-profile");
    CHECK(profile_slug("NUL") == "nul-profile");
    CHECK(profile_slug("com1") == "com1-profile");
    CHECK(profile_slug("lpt9") == "lpt9-profile");
    CHECK(profile_slug("console") == "console");
    const std::string jp = profile_slug("\xE6\x97\xA5\xE6\x9C\xAC");
    CHECK(jp.rfind("profile-", 0) == 0);
    CHECK(jp.size() == 16);
    CHECK(jp != profile_slug("\xE6\x97\xA5"));  // two such names are two profiles
    CHECK(valid_profile_slug(jp));
    CHECK(valid_profile_slug("vanilla"));
    CHECK_FALSE(valid_profile_slug(""));
    CHECK_FALSE(valid_profile_slug("..\\x"));
    CHECK_FALSE(valid_profile_slug("Vanilla"));
    CHECK_FALSE(valid_profile_slug("a b"));
    CHECK(valid_profile_slug(profile_slug(std::string(40, 'z'))));
}

// ---- the file ----

TEST_CASE("profiles: a profile survives its file: name, created, version, mods and settings") {
    Profile p;
    p.name = "Vanilla+ caf\xC3\xA9 (v2)";
    p.created = "2026-10-26 20:00";
    p.emf_version = "0.3.0";
    p.mods = {{"emf", "clock", true, "Clock \"the\" one", "1234", false},
              {"eml", "Auto Loot Plus", false, "Auto Loot Plus", "", false},
              {"chain", "creative.dll", true, "", "", false},
              {"other", "greeneye:x", true, "X", "", true}};
    p.with_settings = true;
    p.settings["clock"] = {{"place", "1"}, {"size", "10"}, {"text", "a \"b\"\nc\\d"}};
    const std::string text = profile_to_json(p);
    const ProfileRead r = profile_from_json(text);
    REQUIRE_MESSAGE(r.profile, r.error);
    const Profile& q = *r.profile;
    CHECK(q.name == p.name);
    CHECK(q.created == p.created);
    CHECK(q.emf_version == "0.3.0");
    REQUIRE(q.mods.size() == 4);
    CHECK(q.mods[0].source == "emf");
    CHECK(q.mods[0].id == "clock");
    CHECK(q.mods[0].on);
    CHECK(q.mods[0].name == "Clock \"the\" one");
    CHECK(q.mods[0].nexus_id == "1234");
    CHECK_FALSE(q.mods[1].on);
    CHECK(q.mods[1].id == "Auto Loot Plus");
    CHECK(q.mods[3].read_only);
    CHECK_FALSE(q.mods[2].read_only);
    CHECK(q.with_settings);
    REQUIRE(q.settings.count("clock"));
    REQUIRE(q.settings.at("clock").size() == 3);
    CHECK(q.settings.at("clock")[1].key == "size");
    CHECK(q.settings.at("clock")[2].value == "a \"b\"\nc\\d");
    CHECK(text.find("\"emf_profile\": 1") != std::string::npos);  // readable: one mod per line
    CHECK(text.find("\n") != std::string::npos);
}

TEST_CASE("profiles: a profile without settings says so, and a minimal hand-made file reads") {
    Profile p;
    p.name = "Plain";
    p.mods = {{"emf", "clock", false, "", "", false}};
    const ProfileRead r = profile_from_json(profile_to_json(p));
    REQUIRE(r.profile);
    CHECK_FALSE(r.profile->with_settings);
    CHECK(r.profile->settings.empty());
    const ProfileRead m = profile_from_json(R"({"name":"Hand","mods":[{"source":"eml","id":"foo","on":true}]})");
    REQUIRE_MESSAGE(m.profile, m.error);
    CHECK(m.profile->name == "Hand");
    CHECK(m.profile->mods.size() == 1);
    // a Nexus id given as a number is read as its digits
    const ProfileRead n = profile_from_json(R"({"name":"N","mods":[{"source":"eml","id":"foo","on":true,"nexus_id":170}]})");
    REQUIRE(n.profile);
    CHECK(n.profile->mods[0].nexus_id == "170");
}

TEST_CASE("profiles: bad files are refused with the reason") {
    const auto why = [](const std::string& text) {
        const ProfileRead r = profile_from_json(text);
        CHECK_FALSE(r.profile);
        return r.error;
    };
    CHECK(why("not json").find("JSON") != std::string::npos);
    CHECK(why("[1,2]").find("object") != std::string::npos);
    CHECK(why(R"({"mods":[]})").find("name") != std::string::npos);
    CHECK(why(R"({"name":"a/b","mods":[]})").find("name") != std::string::npos);
    CHECK(why(R"({"name":"A"})").find("mods") != std::string::npos);
    CHECK(why(R"({"name":"A","mods":{}})").find("mods") != std::string::npos);
    CHECK(why(R"({"name":"A","mods":[1]})").find("mod 1") != std::string::npos);
    CHECK(why(R"({"name":"A","mods":[{"source":"eml","on":true}]})").find("id") != std::string::npos);
    CHECK(why(R"({"name":"A","mods":[{"source":"eml","id":"x"}]})").find("on") != std::string::npos);
    CHECK(why(R"({"name":"A","mods":[{"source":"EML!","id":"x","on":true}]})").find("source") != std::string::npos);
    CHECK(why(R"({"name":"A","mods":[{"source":"eml","id":"x","on":true},{"source":"eml","id":"X","on":false}]})")
              .find("twice") != std::string::npos);
    CHECK(why(R"({"name":"A","mods":[{"source":"eml","id":"bad\u0001id","on":true}]})").find("id") != std::string::npos);
    CHECK(why(R"({"emf_profile":2,"name":"A","mods":[]})").find("newer") != std::string::npos);
    CHECK(why(R"({"name":"A","mods":[],"settings":{"clock":{"size":10}}})").find("settings") != std::string::npos);
    CHECK(why(R"({"name":"A","mods":[],"settings":{"bad id!":{"size":"10"}}})").find("settings") != std::string::npos);
    CHECK(why(std::string(kProfileFileMax + 1, ' ')).find("1 MB") != std::string::npos);
}

// ---- capture, compare, apply ----

TEST_CASE("profiles: capturing lists every mod, the ones EMF cannot switch as read-only; settings only when asked") {
    std::vector<FolderMod> folder = {fm("emf", "clock", true), fm("eml", "foo", false), fm("chain", "creative.dll", true),
                                     fm("eml", "sf-package", true, false), fm("other", "greeneye:x", true, false)};
    folder[0].nexus_id = "1234";
    folder[0].settings = {{"place", "1"}, {"size", "10"}};
    const Profile p = capture_profile("Test", folder, false);
    CHECK(p.name == "Test");
    REQUIRE(p.mods.size() == 5);
    CHECK(p.mods[0].source == "emf");
    CHECK(p.mods[0].nexus_id == "1234");
    CHECK(p.mods[0].name == "Name of clock");
    CHECK_FALSE(p.mods[1].on);
    CHECK_FALSE(p.mods[1].read_only);
    CHECK(p.mods[3].read_only);
    CHECK(p.mods[4].read_only);
    CHECK_FALSE(p.with_settings);
    CHECK(p.settings.empty());
    const Profile s = capture_profile("Test", folder, true);
    CHECK(s.with_settings);
    REQUIRE(s.settings.count("clock"));
    CHECK(s.settings.at("clock").size() == 2);
    CHECK(s.settings.at("clock")[1].value == "10");
}

TEST_CASE("profiles: the folder differs from a profile when a switch or a saved setting it holds moved") {
    std::vector<FolderMod> folder = {fm("emf", "clock", true), fm("eml", "foo", false), fm("other", "x", true, false)};
    folder[0].settings = {{"size", "10"}};
    const Profile p = capture_profile("P", folder, false);
    CHECK_FALSE(profile_differs(p, folder));
    folder[1].on = true;
    CHECK(profile_differs(p, folder));
    folder[1].on = false;
    folder[2].on = false;  // read-only: does not count
    CHECK_FALSE(profile_differs(p, folder));
    folder[0].settings[0].value = "12";  // the profile holds no settings
    CHECK_FALSE(profile_differs(p, folder));
    const Profile ps = capture_profile("PS", folder, true);
    CHECK_FALSE(profile_differs(ps, folder));
    folder[0].settings[0].value = "13";
    CHECK(profile_differs(ps, folder));
    folder[0].settings[0].value = "12";
    // installed after the profile was saved: the profile has no say on it (a switch to it leaves it and says so), so
    // the picker does not call the profile changed right after switching to it
    folder.push_back(fm("chain", "new.dll", true));
    CHECK_FALSE(profile_differs(p, folder));
    folder.back().on = false;
    CHECK_FALSE(profile_differs(p, folder));
    folder.pop_back();
    Profile missing = p;
    missing.mods.push_back({"eml", "gone", true, "Gone", "", false});  // in the profile, not here: no difference
    CHECK_FALSE(profile_differs(missing, folder));
    folder[1].id = "FOO";  // ids are matched with case ignored (folder and DLL names)
    CHECK_FALSE(profile_differs(p, folder));
}

TEST_CASE("profiles: applying switches each mod to its state and reports the rest") {
    std::vector<FolderMod> folder = {fm("emf", "clock", false), fm("eml", "foo", true), fm("chain", "creative.dll", true),
                                     fm("eml", "sf", false, false), fm("emf", "extra", true), fm("other", "y", true, false)};
    Profile p;
    p.name = "Building";
    p.mods = {{"emf", "clock", true, "Clock", "", false},
              {"eml", "FOO", false, "Foo", "", false},  // case ignored
              {"chain", "creative.dll", true, "", "", false},
              {"eml", "sf", true, "SF package", "", false},
              {"eml", "gone", true, "Gone mod", "170", false}};
    FakeSwitcher sw;
    sw.folder = &folder;
    const ApplyReport r = apply_profile(p, folder, sw);
    CHECK(sw.calls == std::vector<std::string>{"emf:clock=1", "eml:foo=0"});  // the folder's own id
    CHECK(ids(r.switched) == std::vector<std::string>{"emf:clock", "eml:foo"});
    CHECK(ids(r.unchanged) == std::vector<std::string>{"chain:creative.dll"});
    CHECK(ids(r.missing) == std::vector<std::string>{"eml:gone"});
    CHECK(r.missing[0].nexus_id == "170");
    CHECK(r.missing[0].name == "Gone mod");
    CHECK(ids(r.read_only) == std::vector<std::string>{"eml:sf"});
    CHECK(r.read_only[0].why == "its own loader switches it");
    CHECK(ids(r.not_in_profile) == std::vector<std::string>{"emf:extra"});  // the read-only "y" is not listed
    CHECK(r.ok());
    CHECK(r.profile == "Building");
    const auto lines = apply_report_lines(r);
    CHECK(has_line(lines, "2 mods switched"));
    CHECK(has_line(lines, "1 already so"));
    CHECK(has_line(lines, "Gone mod"));
    CHECK(has_line(lines, "Nexus 170"));
    CHECK(has_line(lines, "not in this profile"));
    CHECK(has_line(lines, "Name of extra"));
    CHECK(has_line(lines, "read-only"));
}

TEST_CASE("profiles: one switch that fails is reported and the others still apply (nothing is rolled back)") {
    std::vector<FolderMod> folder = {fm("eml", "a", true), fm("eml", "locked", true), fm("chain", "c.dll", true)};
    Profile p;
    p.name = "Off";
    p.mods = {{"eml", "a", false, "", "", false}, {"eml", "locked", false, "", "", false}, {"chain", "c.dll", false, "", "", false}};
    FakeSwitcher sw;
    sw.folder = &folder;
    sw.fail["eml:locked"] = "Could not rename mods\\locked: it is in use";
    const ApplyReport r = apply_profile(p, folder, sw);
    CHECK(sw.calls.size() == 3);
    CHECK(ids(r.switched) == std::vector<std::string>{"eml:a", "chain:c.dll"});
    REQUIRE(r.failed.size() == 1);
    CHECK(r.failed[0].id == "locked");
    CHECK(r.failed[0].why.find("in use") != std::string::npos);
    CHECK_FALSE(r.ok());
    CHECK_FALSE(folder[0].on);
    CHECK(folder[1].on);  // the failed one is as it was
    CHECK_FALSE(folder[2].on);
    CHECK(has_line(apply_report_lines(r), "in use"));
}

TEST_CASE("profiles: a profile's settings are set after the switches; missing mods and settings are skipped") {
    std::vector<FolderMod> folder = {fm("emf", "clock", true)};
    folder[0].settings = {{"place", "1"}, {"size", "10"}, {"mode", "a"}};
    Profile p;
    p.name = "S";
    p.with_settings = true;
    p.mods = {{"emf", "clock", false, "", "", false}};
    p.settings["clock"] = {{"place", "1"}, {"size", "12"}, {"gone", "x"}, {"mode", "zz"}};
    p.settings["absent"] = {{"k", "v"}};
    FakeSwitcher sw;
    sw.folder = &folder;
    sw.fail["clock.mode"] = "not one of: a, b";
    const ApplyReport r = apply_profile(p, folder, sw);
    CHECK(sw.calls == std::vector<std::string>{"emf:clock=0", "clock.size=12", "clock.mode=zz"});
    CHECK(r.settings_set == 1);
    CHECK(r.settings_unchanged == 1);
    REQUIRE(r.settings_failed.size() == 1);
    CHECK(r.settings_failed[0].id == "clock.mode");
    CHECK(r.settings_failed[0].why == "not one of: a, b");
    REQUIRE(r.settings_skipped.size() == 2);
    CHECK(r.settings_skipped[0].id == "absent.k");  // by mod id
    CHECK(r.settings_skipped[0].why == "the mod is not in this folder");
    CHECK(r.settings_skipped[1].id == "clock.gone");
    CHECK_FALSE(r.ok());
    CHECK(folder[0].settings[1].value == "12");
    const auto lines = apply_report_lines(r);
    CHECK(has_line(lines, "1 setting set"));
    CHECK(has_line(lines, "clock.mode"));
    CHECK(has_line(lines, "clock.gone"));
}

// ---- the store ----

TEST_CASE("profiles store: save, list, read, find; another profile is never replaced without asking") {
    TempDir t(L"store");
    ProfileStore store(t.dir + L"\\emf\\profiles");
    CHECK(store.list().empty());  // no folder yet
    CHECK(store.current() == "");
    Profile p;
    p.name = "Vanilla+";
    p.created = "2026-10-26 20:00";
    p.emf_version = "0.3.0";
    p.mods = {{"emf", "clock", true, "Clock", "", false}, {"eml", "foo", false, "", "", false}};
    ProfileSave s = store.save(p, false);
    REQUIRE_MESSAGE(s.ok, s.error);
    CHECK(s.slug == "vanilla");
    CHECK(t.has(L"emf\\profiles\\vanilla.json"));
    CHECK_FALSE(t.has(L"emf\\profiles\\vanilla.json.emf-new"));
    // "Vanilla" has the same slug: it would replace "Vanilla+", so nothing is written without replace
    Profile q = p;
    q.name = "Vanilla";
    q.mods.pop_back();
    s = store.save(q, false);
    CHECK_FALSE(s.ok);
    CHECK(s.exists);
    CHECK(s.existing_name == "Vanilla+");
    CHECK(store.read("vanilla").profile->mods.size() == 2);
    s = store.save(q, true);
    CHECK(s.ok);
    CHECK(store.read("vanilla").profile->name == "Vanilla");
    CHECK(store.read("vanilla").profile->mods.size() == 1);
    p.name = "building";
    REQUIRE(store.save(p, false).ok);
    t.write(L"emf\\profiles\\broken.json", "{not json");
    t.write(L"emf\\profiles\\notes.txt", "not a profile");
    const auto list = store.list();
    REQUIRE(list.size() == 3);
    CHECK(list[0].slug == "broken");
    CHECK_FALSE(list[0].error.empty());
    CHECK(list[1].name == "building");
    CHECK(list[1].mods == 2);
    CHECK(list[1].created == "2026-10-26 20:00");
    CHECK(list[2].name == "Vanilla");
    CHECK(store.find("VANILLA") == std::optional<std::string>("vanilla"));
    CHECK(store.find("Building") == std::optional<std::string>("building"));
    CHECK(store.find("vanilla") == std::optional<std::string>("vanilla"));
    CHECK_FALSE(store.find("nope"));
    CHECK_FALSE(store.read("..\\..\\x").profile);  // only slugs are opened
    CHECK_FALSE(store.read("nope").profile);
    p.name = "bad/name";
    CHECK_FALSE(store.save(p, true).ok);
}

TEST_CASE("profiles store: rename, delete and the current profile") {
    TempDir t(L"rename");
    ProfileStore store(t.dir);
    Profile p;
    p.name = "Testing";
    p.mods = {{"eml", "foo", true, "", "", false}};
    REQUIRE(store.save(p, false).ok);
    p.name = "Building";
    REQUIRE(store.save(p, false).ok);
    CHECK(store.set_current("testing"));
    CHECK(store.current() == "testing");
    // a new display name with the same slug only changes the name inside
    ProfileSave r = store.rename("testing", "TESTING!");
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(r.slug == "testing");
    CHECK(store.read("testing").profile->name == "TESTING!");
    // another slug moves the file, and the current mark follows it
    r = store.rename("testing", "Try outs");
    REQUIRE(r.ok);
    CHECK(r.slug == "try-outs");
    CHECK_FALSE(t.has(L"testing.json"));
    CHECK(t.has(L"try-outs.json"));
    CHECK(store.current() == "try-outs");
    // onto another profile: refused
    r = store.rename("try-outs", "building");
    CHECK_FALSE(r.ok);
    CHECK(r.exists);
    CHECK(t.has(L"try-outs.json"));
    CHECK_FALSE(store.rename("try-outs", "a|b").ok);
    CHECK_FALSE(store.rename("nope", "x").ok);
    // delete: only the profile file goes, and the current mark with it
    std::string why;
    CHECK(store.remove("try-outs", &why));
    CHECK_FALSE(t.has(L"try-outs.json"));
    CHECK(store.current() == "");
    CHECK_FALSE(store.remove("try-outs", &why));
    CHECK_FALSE(why.empty());
    CHECK(t.has(L"building.json"));
    // a current mark whose file is gone counts as none
    CHECK(store.set_current("building"));
    std::filesystem::remove(std::filesystem::path(t.dir + L"\\building.json"));
    CHECK(store.current() == "");
}

// ---- edge cases ----

TEST_CASE("profiles: entries recorded read-only are never switched and never count as changed, even once EMF can switch them") {
    // saved where ShroudForge ran the package (read-only); now EML runs it and EMF could switch it
    std::vector<FolderMod> folder = {fm("eml", "pkg", false), fm("eml", "plain", false)};
    Profile p;
    p.name = "Under SF";
    p.mods = {{"eml", "pkg", true, "Package", "", true}, {"eml", "plain", false, "Plain", "", false}};
    CHECK_FALSE(profile_differs(p, folder));
    FakeSwitcher sw;
    sw.folder = &folder;
    const ApplyReport r = apply_profile(p, folder, sw);
    CHECK(sw.calls.empty());
    CHECK(r.switched.empty());
    REQUIRE(ids(r.recorded_read_only) == std::vector<std::string>{"eml:pkg"});
    CHECK(r.unchanged.size() == 1);
    CHECK(r.ok());
    CHECK_FALSE(folder[0].on);
    CHECK(has_line(apply_report_lines(r), "recorded read-only, not switched"));
    // the same entry with the same state is simply already so
    folder[0].on = true;
    const ApplyReport same = apply_profile(p, folder, sw);
    CHECK(same.recorded_read_only.empty());
    CHECK(same.unchanged.size() == 2);
}

TEST_CASE("profiles: a mod whose state is forced (a broken mod.json) is not recorded, and is never switched") {
    std::vector<FolderMod> folder = {fm("emf", "broken", false), fm("emf", "clock", true)};
    folder[0].can_switch = false;
    folder[0].forced = true;
    folder[0].why_not = "EMF does not load it until mod.json is fixed.";
    const Profile p = capture_profile("P", folder, false);
    REQUIRE(p.mods.size() == 1);
    CHECK(p.mods[0].id == "clock");
    // a profile from elsewhere that wants it on: reported, not switched
    Profile q = p;
    q.mods.push_back({"emf", "broken", true, "", "", false});
    FakeSwitcher sw;
    const ApplyReport r = apply_profile(q, folder, sw);
    CHECK(sw.calls.empty());
    REQUIRE(ids(r.read_only) == std::vector<std::string>{"emf:broken"});
    CHECK(r.read_only[0].why.find("mod.json") != std::string::npos);
    CHECK_FALSE(profile_differs(q, folder));
}

TEST_CASE("profiles: settings in another form than EMF stores them count as the same value (no lasting \"(changed)\")") {
    SettingDef size;
    size.key = "size";
    size.type = SettingType::Int;
    size.default_value = "10";
    SettingDef flag;
    flag.key = "flag";
    flag.type = SettingType::Bool;
    flag.default_value = "0";
    SettingDef corner;
    corner.key = "corner";
    corner.type = SettingType::Choice;
    corner.choices = {"top right", "bottom right"};
    corner.default_value = "0";
    std::vector<FolderMod> folder = {fm("emf", "clock", true)};
    folder[0].settings = {{"size", "15", size}, {"flag", "1", flag}, {"corner", "1", corner}};
    Profile p;
    p.name = "Hand";
    p.with_settings = true;
    p.mods = {{"emf", "clock", true, "", "", false}};
    p.settings["clock"] = {{"size", "15.0"}, {"flag", "true"}, {"corner", "1"}};
    CHECK_FALSE(profile_differs(p, folder));
    FakeSwitcher sw;
    ApplyReport r = apply_profile(p, folder, sw);
    CHECK(sw.calls.empty());  // nothing to write
    CHECK(r.settings_set == 0);
    CHECK(r.settings_unchanged == 3);
    // a value that differs is set; the store saying "unchanged" counts as unchanged, not as set
    folder[0].settings[0].value = "12";
    CHECK(profile_differs(p, folder));
    sw.same = {"clock.size"};
    r = apply_profile(p, folder, sw);
    CHECK(sw.calls == std::vector<std::string>{"clock.size=15.0"});
    CHECK(r.settings_set == 0);
    CHECK(r.settings_unchanged == 3);
    // a value the mod never accepts cannot be applied: it does not keep the picker on "(changed)"
    Profile bad = p;
    bad.settings["clock"] = {{"size", "big"}};
    folder[0].settings[0].value = "15";
    CHECK_FALSE(profile_differs(bad, folder));
}

TEST_CASE("profiles: a big file reads fast; more than 2000 mods, or huge settings, are refused") {
    std::string t = "{\"name\":\"Big\",\"mods\":[";
    for (int i = 0; i < static_cast<int>(kProfileModsMax); ++i)
        t += std::string(i ? "," : "") + "{\"source\":\"emf\",\"id\":\"m" + std::to_string(100000 + i) + "\",\"on\":true}";
    const std::string big = t + "]}";
    const auto a = GetTickCount64();
    const ProfileRead r = profile_from_json(big);
    const auto ms = GetTickCount64() - a;
    REQUIRE_MESSAGE(r.profile, r.error);
    CHECK(r.profile->mods.size() == kProfileModsMax);
    CHECK_MESSAGE(ms < 100, ms);
    // and applying it over a folder of the same size is as quick
    std::vector<FolderMod> folder;
    for (const auto& m : r.profile->mods) folder.push_back(fm("emf", m.id.c_str(), true));
    FakeSwitcher sw;
    const auto b = GetTickCount64();
    const ApplyReport rep = apply_profile(*r.profile, folder, sw);
    CHECK(profile_differs(*r.profile, folder) == false);
    const auto apply_ms = GetTickCount64() - b;
    CHECK_MESSAGE(apply_ms < 100, apply_ms);
    CHECK(rep.unchanged.size() == kProfileModsMax);
    const ProfileRead over = profile_from_json(t + ",{\"source\":\"emf\",\"id\":\"one_more\",\"on\":true}]}");
    CHECK_FALSE(over.profile);
    CHECK(over.error.find("2000") != std::string::npos);
    std::string s = "{\"name\":\"S\",\"mods\":[],\"settings\":{\"clock\":{";
    for (int i = 0; i <= static_cast<int>(kProfileSettingsMax); ++i) s += std::string(i ? "," : "") + "\"k" + std::to_string(i) + "\":\"1\"";
    CHECK(profile_from_json(s + "}}}").error.find("settings") != std::string::npos);
    CHECK(profile_from_json("{\"name\":\"S\",\"mods\":[],\"settings\":{\"clock\":{\"k\":\"" + std::string(5000, 'x') + "\"}}}")
              .error.find("long") != std::string::npos);
}

TEST_CASE("profiles: text from a file is checked: lengths, control characters, the format number") {
    const auto why = [](const std::string& text) {
        const ProfileRead r = profile_from_json(text);
        CHECK_FALSE(r.profile);
        return r.error;
    };
    CHECK(why(R"({"name":"A","created":")" + std::string(100, '1') + R"(","mods":[]})").find("created") != std::string::npos);
    CHECK(why(R"({"name":"A","emf_version":"0.3\u0007","mods":[]})").find("emf_version") != std::string::npos);
    CHECK(why(R"({"name":"A","mods":[{"source":"eml","id":"x","on":true,"name":")" + std::string(300, 'n') + R"("}]})")
              .find("name") != std::string::npos);
    CHECK(why(R"({"name":"A","mods":[{"source":"eml","id":"x","on":true,"name":"bad\u0001"}]})").find("name") != std::string::npos);
    CHECK(why(R"({"emf_profile":1e300,"name":"A","mods":[]})").find("format") != std::string::npos);
    CHECK(why(R"({"emf_profile":-1,"name":"A","mods":[]})").find("format") != std::string::npos);
    CHECK(profile_from_json(R"({"emf_profile":1,"name":"A","created":"2026-10-26 20:00","emf_version":"0.3.0","mods":[]})").profile);
}

TEST_CASE("profiles store: file names in other case are found; any listed .json can be deleted; reads are cached") {
    TempDir t(L"store2");
    ProfileStore store(t.dir);
    t.write(L"Vanilla.json", R"({"name":"Vanilla","mods":[]})");  // named by hand
    t.write(L"my set (old).json", "not json");                     // not a name EMF makes
    auto list = store.list();
    REQUIRE(list.size() == 2);
    CHECK(list[0].slug == "my set (old)");
    CHECK_FALSE(list[0].error.empty());
    CHECK(list[1].slug == "vanilla");
    CHECK(list[1].error.empty());
    CHECK(store.read("vanilla").profile);
    std::string why;
    CHECK(store.remove("my set (old)", &why));
    CHECK_FALSE(t.has(L"my set (old).json"));
    CHECK_FALSE(store.remove("..\\Vanilla", &why));
    CHECK_FALSE(store.remove("sub/x", &why));
    CHECK(t.has(L"Vanilla.json"));
    // a second listing reads no file that did not change
    const size_t before = profile_files_read();
    list = store.list();
    CHECK(profile_files_read() == before);
    Sleep(20);
    t.write(L"Vanilla.json", R"({"name":"Vanilla 2","mods":[]})");
    list = store.list();
    CHECK(profile_files_read() == before + 1);
    CHECK(list[0].name == "Vanilla 2");
}
