// EMF.exe's bridge commands for mod profiles (app/bridge_profiles.cpp): the state's profile picker data, Save as
// (asking before replacing), Switch (refused while the game runs; the report), Rename, Delete, Export and Import with
// the missing mods. A fake host stands in for the window and the file dialogs; the game folder is a real fake one.
#include <windows.h>

#include <doctest.h>

#include <optional>
#include <string>
#include <vector>

#include "app/bridge.h"
#include "app/game_folder.h"
#include "app/profiles.h"
#include "app/text.h"
#include "app_test_files.h"
#include "core/json.h"
#include "core/profiles.h"

using namespace emf::app;
using app_test::TempGame;
namespace json = emf::core::json;
namespace core = emf::core;

namespace {

std::string utf16(const std::wstring& w) {
    std::string b = "\xFF\xFE";
    b.append(reinterpret_cast<const char*>(w.data()), w.size() * sizeof(wchar_t));
    return b;
}

// EMF, EML as dbghelp.dll, an EMF mod, an EML folder and zip mod, two DLLs in a UTF-16 dinput8-chain.ini.
void every_kind(const TempGame& g) {
    g.write(L"dinput8.dll", "MZ EMF-LOADER-7d3c9f fake");
    g.write(L"emf\\emf_core.dll", std::string("MZ emf_core 0.3.0\0 more", 23));
    g.write(L"emf\\known_tools.txt", "eml | EML (Enshrouded Mod Loader) | text:EML_LOG_FILE_PATH\n");
    g.write(L"dbghelp.dll", "EML_LOG_FILE_PATH");
    g.write(L"emf\\mods\\clock\\mod.json",
            R"({"id":"clock","name":"Clock","version":"1.1.0","settings":[{"key":"size","type":"int","default":10}]})");
    g.write(L"emf\\mods\\clock\\clock.dll", "dll");
    g.write(L"mods\\foo\\mod.json", R"({"id":"foo","name":"Foo","version":"1.0.0","nexus_id":170})");
    g.write(L"mods\\pack.zip", "PK fake zip");
    g.write(L"dinput8-chain.ini", utf16(L"[Chain]\r\ncreative.dll\r\nmöd.dll\r\n"));
    g.write(L"creative.dll", "x");
    g.write(L"möd.dll", "x");
}

bool is_on(const std::wstring& game, const std::string& source, const std::string& id) {
    for (const auto& m : scan_game_folder(game).mods)
        if (m.source == source && m.id == id) return m.enabled;
    FAIL("no such mod");
    return false;
}

struct ProfileHost : BridgeHost {
    std::wstring dir;
    std::string running;
    std::optional<std::wstring> open_file, save_file;  // what the file dialogs return
    std::wstring suggested;                             // the name the save dialog was given
    bool testing = false;
    std::wstring game() const override { return dir; }
    void choose_game(const std::wstring& d) override { dir = d; }
    std::optional<std::wstring> pick_folder() override { return std::nullopt; }
    bool open(const std::wstring&, Open) override { return true; }
    std::string running_reason() override { return running; }
    void rendered(int, const std::vector<std::string>&) override {}
    std::wstring steam_game() override { return {}; }
    bool copy_text(const std::string&) override { return true; }
    std::optional<std::wstring> pick_archive() override { return std::nullopt; }
    // work runs at once and its messages are kept in `async_out`; with `defer` it waits in `queued` until run_queued()
    bool defer = false;
    std::vector<std::function<Done()>> queued;
    std::vector<std::string> async_out;
    void run_async(std::function<Done()> work) override {
        if (defer) queued.push_back(std::move(work));
        else finish(work());
    }
    void finish(const Done& d) {
        if (!d) return;
        for (auto& m : d()) async_out.push_back(m);
    }
    void run_queued() {
        auto q = std::move(queued);
        queued.clear();
        for (auto& w : q) finish(w());
    }
    bool test_mode() const override { return testing; }
    std::optional<std::wstring> pick_profile_file() override { return open_file; }
    std::optional<std::wstring> pick_profile_save(const std::wstring& name) override {
        suggested = name;
        return save_file;
    }
};

json::Value parse(const std::string& text) {
    auto r = json::parse(text);
    REQUIRE_MESSAGE(r.value, text);
    return *r.value;
}

// The first reply of `type` ("result", "state", "profile_report", ...).
std::optional<json::Value> reply_of(const std::vector<std::string>& replies, const std::string& type) {
    for (const auto& r : replies) {
        json::Value v = parse(r);
        if (v.get("type") && v.get("type")->s == type) return v;
    }
    return std::nullopt;
}

std::string g_last;  // the last result's error, shown when a check on it fails

bool ok_of(const std::vector<std::string>& replies) {
    const auto r = reply_of(replies, "result");
    REQUIRE(r);
    g_last = r->get("error")->s;
    return r->get("ok")->b;
}

std::string cmd(const std::string& c, const std::string& rest = {}) {
    return "{\"cmd\":\"" + c + "\"" + (rest.empty() ? "" : "," + rest) + "}";
}

// A message and what its work on the worker sent afterwards (Switch runs there).
std::vector<std::string> handled(Bridge& bridge, ProfileHost& host, const std::string& message) {
    host.async_out.clear();
    std::vector<std::string> out = bridge.handle(message);
    for (auto& m : host.async_out) out.push_back(m);
    host.async_out.clear();
    return out;
}

}  // namespace

TEST_CASE("app profiles bridge: the state carries the profiles, the current one and whether the mods changed") {
    TempGame g(L"pb_state");
    every_kind(g);
    ProfileHost host;
    host.dir = g.dir;
    Bridge bridge(host);
    auto s = reply_of(bridge.handle(cmd("state")), "state");
    REQUIRE(s);
    const json::Value* p = s->get("profiles");
    REQUIRE(p);
    CHECK(p->get("list")->arr.empty());
    CHECK(p->get("current")->s == "");
    CHECK_FALSE(p->get("changed")->b);
    const auto out = bridge.handle(cmd("profile_save", R"("name":"Vanilla+","settings":true)"));
    REQUIRE_MESSAGE(ok_of(out), g_last);
    CHECK(reply_of(out, "result")->get("text")->s.find("Vanilla+") != std::string::npos);
    s = reply_of(out, "state");
    REQUIRE(s);
    p = s->get("profiles");
    REQUIRE(p->get("list")->arr.size() == 1);
    const json::Value& item = p->get("list")->arr[0];
    CHECK(item.get("name")->s == "Vanilla+");
    CHECK(item.get("slug")->s == "vanilla");
    CHECK(item.get("mods")->n == 5);
    CHECK(item.get("with_settings")->b);
    CHECK(item.get("created")->s.size() == 16);
    CHECK(item.get("error")->s == "");
    CHECK(p->get("current")->s == "vanilla");
    CHECK(p->get("current_name")->s == "Vanilla+");
    CHECK_FALSE(p->get("changed")->b);
    // a switch in the Mods tab: the picker says "(changed)"
    s = reply_of(bridge.handle(cmd("set_enabled", R"("source":"eml","id":"foo","on":false)")), "state");
    REQUIRE(s);
    CHECK(s->get("profiles")->get("changed")->b);
}

TEST_CASE("app profiles bridge: saving over another profile asks first; the page says yes with replace") {
    TempGame g(L"pb_replace");
    every_kind(g);
    ProfileHost host;
    host.dir = g.dir;
    Bridge bridge(host);
    REQUIRE_MESSAGE(ok_of(bridge.handle(cmd("profile_save", R"("name":"Building")"))), g_last);
    REQUIRE(set_mod_enabled(g.dir, "eml", "foo", false).ok);
    auto out = bridge.handle(cmd("profile_save", R"("name":"building!","settings":true)"));
    const auto ask = reply_of(out, "profile_exists");
    REQUIRE(ask);
    CHECK(ask->get("name")->s == "building!");
    CHECK(ask->get("existing_name")->s == "Building");
    CHECK(ask->get("settings")->b);
    CHECK_FALSE(reply_of(out, "result"));  // a question, not an error
    CHECK(core::ProfileStore(profiles_dir(g.dir)).read("building").profile->name == "Building");
    out = bridge.handle(cmd("profile_save", R"("name":"building!","replace":true)"));
    REQUIRE_MESSAGE(ok_of(out), g_last);
    CHECK(core::ProfileStore(profiles_dir(g.dir)).read("building").profile->name == "building!");
    CHECK_FALSE(ok_of(bridge.handle(cmd("profile_save", R"("name":"a/b")"))));
}

TEST_CASE("app profiles bridge: Switch is refused while the game runs; save, switch a mod, switch back: it is back") {
    TempGame g(L"pb_switch");
    every_kind(g);
    ProfileHost host;
    host.dir = g.dir;
    Bridge bridge(host);
    REQUIRE_MESSAGE(ok_of(bridge.handle(cmd("profile_save", R"("name":"Smoke")"))), g_last);
    REQUIRE_MESSAGE(ok_of(bridge.handle(cmd("set_enabled", R"("source":"chain","id":"creative.dll","on":false)"))), g_last);
    CHECK_FALSE(is_on(g.dir, "chain", "creative.dll"));
    host.running = "enshrouded.exe runs from this folder";
    auto out = handled(bridge, host, cmd("profile_switch", R"("slug":"smoke")"));
    auto r = reply_of(out, "result");
    REQUIRE(r);
    CHECK_FALSE(r->get("ok")->b);
    CHECK(r->get("error")->s.find("close the game") != std::string::npos);
    CHECK_FALSE(reply_of(out, "profile_report"));
    CHECK_FALSE(is_on(g.dir, "chain", "creative.dll"));
    host.running.clear();
    out = handled(bridge, host, cmd("profile_switch", R"("slug":"smoke")"));
    REQUIRE_MESSAGE(ok_of(out), g_last);
    CHECK(reply_of(out, "result")->get("text")->s.find("next game start") != std::string::npos);
    const auto rep = reply_of(out, "profile_report");
    REQUIRE(rep);
    CHECK(rep->get("profile")->s == "Smoke");
    CHECK(rep->get("ok")->b);
    REQUIRE(rep->get("switched")->arr.size() == 1);
    CHECK(rep->get("switched")->arr[0].get("id")->s == "creative.dll");
    CHECK(rep->get("switched")->arr[0].get("on")->b);
    CHECK(rep->get("unchanged")->n == 4);
    CHECK(is_on(g.dir, "chain", "creative.dll"));  // the switch is back
    CHECK(g.read(L"dinput8-chain.ini") == utf16(L"[Chain]\r\ncreative.dll\r\nmöd.dll\r\n"));
    const auto s = reply_of(out, "state");
    REQUIRE(s);
    CHECK_FALSE(s->get("profiles")->get("changed")->b);
    CHECK_FALSE(ok_of(handled(bridge, host, cmd("profile_switch", R"("slug":"nope")"))));
}

TEST_CASE("app profiles bridge: a switch report names missing mods with Nexus ids and mods not in the profile") {
    TempGame g(L"pb_report");
    every_kind(g);
    g.write(L"emf\\profiles\\shared.json", R"({"name":"Shared","mods":[
      {"source":"eml","id":"foo","on":false},
      {"source":"eml","id":"auto-loot-plus","on":true,"name":"Auto Loot Plus","nexus_id":"170"}]})");
    ProfileHost host;
    host.dir = g.dir;
    Bridge bridge(host);
    const auto out = handled(bridge, host, cmd("profile_switch", R"("slug":"shared")"));
    const auto rep = reply_of(out, "profile_report");
    REQUIRE(rep);
    REQUIRE(rep->get("missing")->arr.size() == 1);
    CHECK(rep->get("missing")->arr[0].get("name")->s == "Auto Loot Plus");
    CHECK(rep->get("missing")->arr[0].get("nexus_id")->s == "170");
    CHECK(rep->get("not_in_profile")->arr.size() == 4);
    CHECK(rep->get("switched")->arr.size() == 1);
    CHECK(rep->get("failed")->arr.empty());
    CHECK(rep->get("read_only")->arr.empty());
    CHECK(g.has(L"mods\\.foo"));
}

TEST_CASE("app profiles bridge: rename and delete touch only the profile's file") {
    TempGame g(L"pb_rename");
    every_kind(g);
    ProfileHost host;
    host.dir = g.dir;
    Bridge bridge(host);
    REQUIRE_MESSAGE(ok_of(bridge.handle(cmd("profile_save", R"("name":"Testing")"))), g_last);
    REQUIRE_MESSAGE(ok_of(bridge.handle(cmd("profile_save", R"("name":"Building")"))), g_last);
    REQUIRE_MESSAGE(ok_of(bridge.handle(cmd("profile_rename", R"("slug":"testing","name":"Try outs")"))), g_last);
    CHECK(g.has(L"emf\\profiles\\try-outs.json"));
    CHECK_FALSE(g.has(L"emf\\profiles\\testing.json"));
    CHECK_FALSE(ok_of(bridge.handle(cmd("profile_rename", R"("slug":"try-outs","name":"Building")"))));
    const auto out = bridge.handle(cmd("profile_delete", R"("slug":"try-outs")"));
    REQUIRE_MESSAGE(ok_of(out), g_last);
    CHECK_FALSE(g.has(L"emf\\profiles\\try-outs.json"));
    CHECK(g.has(L"mods\\foo\\mod.json"));  // never a mod
    CHECK(g.has(L"emf\\mods\\clock\\mod.json"));
    const auto s = reply_of(out, "state");
    REQUIRE(s);
    CHECK(s->get("profiles")->get("list")->arr.size() == 1);
    CHECK_FALSE(ok_of(bridge.handle(cmd("profile_delete", R"("slug":"..\\..\\enshrouded")"))));
    CHECK(g.has(L"enshrouded.exe"));
}

TEST_CASE("app profiles bridge: Export writes the profile where the player chose; cancelled writes nothing") {
    TempGame g(L"pb_export");
    every_kind(g);
    ProfileHost host;
    host.dir = g.dir;
    Bridge bridge(host);
    REQUIRE_MESSAGE(ok_of(bridge.handle(cmd("profile_save", R"("name":"Vanilla+")"))), g_last);
    auto out = bridge.handle(cmd("profile_export", R"("slug":"vanilla")"));
    CHECK(host.suggested == L"Vanilla+.json");
    CHECK(out.empty());  // cancelled: nothing to say
    g.mkdir(L"shared");
    host.save_file = g.dir + L"\\shared\\my set.json";
    out = bridge.handle(cmd("profile_export", R"("slug":"vanilla")"));
    REQUIRE_MESSAGE(ok_of(out), g_last);
    CHECK(g.read(L"shared\\my set.json") == g.read(L"emf\\profiles\\vanilla.json"));
    CHECK_FALSE(ok_of(bridge.handle(cmd("profile_export", R"("slug":"nope")"))));
}

TEST_CASE("app profiles bridge: Import shows the missing mods with their Nexus ids, then saves it (asking before replacing)") {
    TempGame g(L"pb_import");
    every_kind(g);
    g.write(L"shared\\friend.json", R"({"emf_profile":1,"name":"Friend","mods":[
      {"source":"emf","id":"clock","on":false},
      {"source":"eml","id":"auto-loot-plus","on":true,"name":"Auto Loot Plus","nexus_id":"170"}]})");
    ProfileHost host;
    host.dir = g.dir;
    Bridge bridge(host);
    auto out = bridge.handle(cmd("profile_import"));
    CHECK(out.empty());  // the dialog was cancelled
    host.open_file = g.dir + L"\\shared\\friend.json";
    out = bridge.handle(cmd("profile_import"));
    auto imp = reply_of(out, "profile_import");
    REQUIRE(imp);
    CHECK(imp->get("ok")->b);
    CHECK(imp->get("name")->s == "Friend");
    CHECK(imp->get("mods")->n == 2);
    CHECK_FALSE(imp->get("exists")->b);
    REQUIRE(imp->get("missing")->arr.size() == 1);
    CHECK(imp->get("missing")->arr[0].get("name")->s == "Auto Loot Plus");
    CHECK(imp->get("missing")->arr[0].get("nexus_id")->s == "170");
    CHECK_FALSE(g.has(L"emf\\profiles\\friend.json"));  // nothing saved before the player says so
    REQUIRE_MESSAGE(ok_of(bridge.handle(cmd("profile_import_save"))), g_last);
    CHECK(g.has(L"emf\\profiles\\friend.json"));
    CHECK(is_on(g.dir, "emf", "clock"));  // importing never switches anything
    // the same file again: its name is taken; saving needs the player's yes (or another name)
    imp = reply_of(bridge.handle(cmd("profile_import")), "profile_import");
    REQUIRE(imp);
    CHECK(imp->get("exists")->b);
    CHECK(imp->get("existing_name")->s == "Friend");
    CHECK_FALSE(ok_of(bridge.handle(cmd("profile_import_save"))));
    REQUIRE_MESSAGE(ok_of(bridge.handle(cmd("profile_import_save", R"("name":"Friend 2")"))), g_last);
    CHECK(core::ProfileStore(profiles_dir(g.dir)).read("friend-2").profile->name == "Friend 2");
    CHECK_FALSE(ok_of(bridge.handle(cmd("profile_import_save", R"("replace":true)"))));  // nothing waits any more
    // a bad file says why
    g.write(L"shared\\bad.json", "{\"name\":\"x\"}");
    host.open_file = g.dir + L"\\shared\\bad.json";
    imp = reply_of(bridge.handle(cmd("profile_import")), "profile_import");
    REQUIRE(imp);
    CHECK_FALSE(imp->get("ok")->b);
    CHECK(imp->get("error")->s.find("mods") != std::string::npos);
    // the smoke test's stand-in for the dialog works only in test mode
    CHECK_FALSE(ok_of(bridge.handle(cmd("profile_import_path", R"("path":"x")"))));
    host.testing = true;
    imp = reply_of(bridge.handle(cmd("profile_import_path", "\"path\":" + json_quote(utf8(g.dir + L"\\shared\\friend.json")))),
                   "profile_import");
    REQUIRE(imp);
    CHECK(imp->get("ok")->b);
}

// ---- edge cases ----

TEST_CASE("app profiles bridge: Switch runs on a worker: the page hears it is busy, and nothing else switches meanwhile") {
    TempGame g(L"pb_busy");
    every_kind(g);
    ProfileHost host;
    host.dir = g.dir;
    Bridge bridge(host);
    REQUIRE_MESSAGE(ok_of(bridge.handle(cmd("profile_save", R"("name":"Smoke")"))), g_last);
    REQUIRE(set_mod_enabled(g.dir, "eml", "foo", false).ok);
    host.defer = true;
    auto out = bridge.handle(cmd("profile_switch", R"("slug":"smoke")"));
    const auto busy = reply_of(out, "profile_busy");
    REQUIRE(busy);
    CHECK(busy->get("busy")->b);
    CHECK_FALSE(reply_of(out, "profile_report"));
    CHECK(host.queued.size() == 1);
    CHECK_FALSE(g.has(L"mods\\foo"));  // nothing switched on the UI thread
    CHECK(bridge.changing());           // the window does not close in the middle
    // a second Switch, and an install, wait for it
    CHECK_FALSE(ok_of(bridge.handle(cmd("profile_switch", R"("slug":"smoke")"))));
    CHECK(g_last.find("busy") != std::string::npos);
    CHECK_FALSE(ok_of(bridge.handle(cmd("profile_save", R"("name":"Half")"))));  // never a half-switched folder
    CHECK_FALSE(g.has(L"emf\\profiles\\half.json"));
    host.run_queued();
    out = host.async_out;
    REQUIRE(reply_of(out, "profile_report"));
    REQUIRE(ok_of(out));
    const auto done = reply_of(out, "profile_busy");
    REQUIRE(done);
    CHECK_FALSE(done->get("busy")->b);
    CHECK(g.has(L"mods\\foo"));
    CHECK_FALSE(bridge.changing());
}

TEST_CASE("app profiles bridge: Import previews what Switch would flip and which settings it would rewrite") {
    TempGame g(L"pb_preview");
    every_kind(g);
    g.write(L"shared\\friend.json", R"({"name":"Friend","mods":[
      {"source":"emf","id":"clock","on":false},
      {"source":"eml","id":"foo","on":true},
      {"source":"chain","id":"creative.dll","on":false}],
      "settings":{"clock":{"size":"25"}}})");
    ProfileHost host;
    host.dir = g.dir;
    host.open_file = g.dir + L"\\shared\\friend.json";
    Bridge bridge(host);
    const auto imp = reply_of(bridge.handle(cmd("profile_import")), "profile_import");
    REQUIRE(imp);
    const json::Value* would = imp->get("would_switch");
    REQUIRE(would);
    REQUIRE(would->arr.size() == 2);
    CHECK(would->arr[0].get("id")->s == "clock");
    CHECK_FALSE(would->arr[0].get("on")->b);
    CHECK(would->arr[1].get("id")->s == "creative.dll");
    const json::Value* set = imp->get("would_set");
    REQUIRE(set);
    REQUIRE(set->arr.size() == 1);
    CHECK(set->arr[0].get("id")->s == "clock.size");
    CHECK(set->arr[0].get("value")->s == "25");
    CHECK(is_on(g.dir, "emf", "clock"));  // a preview switches nothing
}
