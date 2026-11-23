// EMF.exe's page <-> host bridge: JSON commands from the page become changes in the game folder or desktop actions,
// and the state goes back as JSON. A fake host stands in for the window, the browser and the process list.
#include <doctest.h>

#include <share.h>

#include <cstdio>
#include <ctime>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "app/archive.h"
#include "app/bridge.h"
#include "app/browse.h"
#include "app_archive_files.h"
#include "app/text.h"
#include "app_test_files.h"
#include "core/json.h"
#include "loader/play_once.h"
#include "test_scratch.h"

using namespace emf::app;
using app_test::TempGame;
namespace json = emf::core::json;

namespace {

struct FakeHost : BridgeHost {
    std::wstring dir;
    std::wstring chosen;
    std::optional<std::wstring> picked;
    std::vector<std::pair<std::wstring, Open>> opened;
    std::string running;
    int rendered_count = -1;
    std::vector<std::string> rendered_titles;
    std::wstring steam;  // the game folder Steam knows
    std::string copied;

    std::wstring game() const override { return dir; }
    void choose_game(const std::wstring& d) override {
        chosen = d;
        dir = d;
    }
    std::optional<std::wstring> pick_folder() override { return picked; }
    bool open_ok = true;  // false: the browser, Explorer or Steam did not open
    bool open(const std::wstring& target, Open how) override {
        opened.emplace_back(target, how);
        return open_ok;
    }
    std::string running_reason() override { return running; }
    std::wstring steam_game() override { return steam; }
    bool copy_text(const std::string& text) override {
        copied = text;
        return true;
    }
    void rendered(int count, const std::vector<std::string>& titles) override {
        rendered_count = count;
        rendered_titles = titles;
    }
    std::optional<std::wstring> archive;  // what the file dialog returns
    std::optional<std::wstring> pick_archive() override { return archive; }
    // work runs at once; with `defer` it waits in `queued` until run_queued()
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
    void run_queued(bool reverse = false) {
        auto q = std::move(queued);
        queued.clear();
        if (reverse) std::reverse(q.begin(), q.end());
        for (auto& w : q) finish(w());
    }
    emf::nexus::Client::Transport transport;
    emf::nexus::Client::Transport nexus_transport() override { return transport; }
    bool personal_key_allowed() const override { return true; }  // the tester switch: Browse asks (the fake) Nexus
    bool testing = false;
    bool test_mode() const override { return testing; }
    bool wine = false;  // EMF.exe runs under Wine or Proton
    bool under_wine() const override { return wine; }
};

json::Value parse(const std::string& text) {
    auto r = json::parse(text);
    REQUIRE_MESSAGE(r.value, text);
    return *r.value;
}

// The "result" message among the replies.
json::Value result_of(const std::vector<std::string>& replies) {
    for (const auto& r : replies) {
        json::Value v = parse(r);
        if (v.get("type") && v.get("type")->s == "result") return v;
    }
    FAIL("no result message");
    return {};
}

bool has_state(const std::vector<std::string>& replies) {
    for (const auto& r : replies)
        if (parse(r).get("type")->s == "state") return true;
    return false;
}

void fake_game(const TempGame& g) {
    g.write(L"dinput8.dll", "EMF-LOADER-7d3c9f");
    g.write(L"emf\\emf_core.dll", std::string("emf_core 1.2.3\0", 15));
    g.write(L"emf\\mods\\clock\\mod.json",
            R"({"id":"clock","name":"Clock \"quoted\"","version":"1.0.0","nexus_id":77,)"
            R"("settings":[{"key":"size","type":"int","default":3,"min":1,"max":9}]})");
    g.write(L"emf\\mods\\clock\\clock.dll", "dll");
}

}  // namespace

TEST_CASE("app bridge: JSON strings are escaped, and bytes that are not UTF-8 become U+FFFD") {
    CHECK(json_quote("a\"b\\c\n\t\x01") == "\"a\\\"b\\\\c\\n\\t\\u0001\"");
    CHECK(json_quote("caf\xC3\xA9") == "\"caf\xC3\xA9\"");
    CHECK(json_quote("bad\xFFz") == "\"bad\xEF\xBF\xBDz\"");
    CHECK(json_quote("cut\xC3") == "\"cut\xEF\xBF\xBD\"");
}

TEST_CASE("app bridge: the state carries the folder, EMF, the game's state, the mods and the warnings") {
    TempGame g(L"bridge_state");
    fake_game(g);
    FakeHost host;
    host.dir = g.dir;
    Bridge bridge(host);
    const auto replies = bridge.handle(R"({"cmd":"state"})");
    REQUIRE(replies.size() == 1);
    const json::Value s = parse(replies[0]);
    CHECK(s.get("type")->s == "state");
    CHECK(s.get("game")->s == emf::app::utf8(g.dir));
    CHECK(s.get("emf_installed")->b);
    CHECK(s.get("emf_version")->s == "1.2.3");
    CHECK_FALSE(s.get("game_running")->b);
    REQUIRE(s.get("mods")->arr.size() == 1);
    const json::Value& m = s.get("mods")->arr[0];
    CHECK(m.get("source")->s == "emf");
    CHECK(m.get("id")->s == "clock");
    CHECK(m.get("name")->s == "Clock \"quoted\"");
    CHECK(m.get("nexus_id")->s == "77");
    CHECK(m.get("enabled")->b);
    REQUIRE(m.get("settings")->arr.size() == 1);
    const json::Value& set = m.get("settings")->arr[0];
    CHECK(set.get("key")->s == "size");
    CHECK(set.get("type")->s == "int");
    CHECK(set.get("value")->s == "3");
    CHECK(set.get("min")->n == 1);
    CHECK(set.get("max")->n == 9);
    CHECK(s.get("warnings")->is(json::Value::Type::Array));
}

TEST_CASE("app bridge: a setting carries its control: options, step, slider, max length, group and enabledWhen") {
    TempGame g(L"bridge_controls");
    fake_game(g);
    g.write(L"emf\\mods\\starter\\mod.json", R"({"id":"starter","version":"1.0.0","settings":[
      {"key":"show","type":"bool","default":false,"label":"Show"},
      {"key":"corner","type":"select","group":"Screen","enabledWhen":{"setting":"show","equals":true},
       "options":[{"value":"tr","label":"Top right"},"bl"]},
      {"key":"speed","type":"slider","min":0,"max":2,"step":0.5,"group":"Screen"},
      {"key":"name","type":"text","maxLength":12}]})");
    g.write(L"emf\\mods\\starter\\starter.dll", "dll");
    FakeHost host;
    host.dir = g.dir;
    Bridge bridge(host);
    const json::Value s = parse(bridge.handle(R"({"cmd":"state"})")[0]);
    const json::Value* starter = nullptr;
    for (const auto& m : s.get("mods")->arr)
        if (m.get("id")->s == "starter") starter = &m;
    REQUIRE(starter);
    const auto& set = starter->get("settings")->arr;
    REQUIRE(set.size() == 4);
    CHECK(set[0].get("key")->s == "show");  // ungrouped first
    CHECK(set[0].get("controls")->b);
    CHECK(set[1].get("key")->s == "name");
    CHECK(set[1].get("max_length")->n == 12);
    CHECK(set[2].get("key")->s == "corner");
    CHECK(set[2].get("type")->s == "select");
    CHECK(set[2].get("group")->s == "Screen");
    CHECK_FALSE(set[2].get("active")->b);
    CHECK(set[2].get("when")->s == "only while Show is on");
    REQUIRE(set[2].get("options")->arr.size() == 2);
    CHECK(set[2].get("options")->arr[0].get("value")->s == "tr");
    CHECK(set[2].get("options")->arr[0].get("label")->s == "Top right");
    CHECK(set[2].get("options")->arr[1].get("label")->s == "bl");
    CHECK(set[3].get("control")->s == "slider");
    CHECK(set[3].get("step")->n == 0.5);
    CHECK(set[3].get("type")->s == "float");
    // the default as the in-game Mods tab shows it on hover (the page puts it in the setting's tooltip)
    CHECK(set[0].get("default")->s == "0");
    CHECK(set[0].get("default_shown")->s == "off");
    CHECK(set[1].get("default_shown")->s == "");
    CHECK(set[2].get("default")->s == "tr");
    CHECK(set[2].get("default_shown")->s == "Top right");
    CHECK(set[3].get("default_shown")->s == "0");
}

TEST_CASE("app bridge: the state is sent again only when it changed") {
    TempGame g(L"bridge_changed");
    fake_game(g);
    FakeHost host;
    host.dir = g.dir;
    Bridge bridge(host);
    CHECK(bridge.state_if_changed(false));
    CHECK_FALSE(bridge.state_if_changed(false));
    CHECK(bridge.state_if_changed(true));
    host.running = "enshrouded.exe is running from this folder";
    const auto s = bridge.state_if_changed(false);
    REQUIRE(s);
    CHECK(parse(*s).get("game_running")->b);
}

TEST_CASE("app bridge: switching mods is refused while the game runs; settings are still saved") {
    TempGame g(L"bridge_running");
    fake_game(g);
    FakeHost host;
    host.dir = g.dir;
    host.running = "enshrouded.exe (process 5) is running from this folder";
    Bridge bridge(host);
    json::Value r = result_of(bridge.handle(R"({"cmd":"set_enabled","source":"emf","id":"clock","on":false})"));
    CHECK_FALSE(r.get("ok")->b);
    CHECK(r.get("error")->s.find("close the game first") != std::string::npos);
    CHECK_FALSE(g.has(L"emf\\config\\clock.ini"));
    const auto replies = bridge.handle(R"({"cmd":"set_setting","id":"clock","key":"size","value":"5"})");
    CHECK(result_of(replies).get("ok")->b);
    CHECK(has_state(replies));
    CHECK(g.read(L"emf\\config\\clock.ini").find("size=5") != std::string::npos);
}

TEST_CASE("app bridge: set_enabled switches the mod and sends the new state") {
    TempGame g(L"bridge_toggle");
    fake_game(g);
    FakeHost host;
    host.dir = g.dir;
    Bridge bridge(host);
    const auto replies = bridge.handle(R"({"cmd":"set_enabled","source":"emf","id":"clock","on":false})");
    CHECK(result_of(replies).get("ok")->b);
    REQUIRE(has_state(replies));
    CHECK(g.read(L"emf\\config\\clock.ini").find("enabled=0") != std::string::npos);
    const json::Value bad = result_of(bridge.handle(R"({"cmd":"set_setting","id":"clock","key":"size","value":"99"})"));
    CHECK_FALSE(bad.get("ok")->b);
    CHECK(bad.get("error")->s.find("between 1 and 9") != std::string::npos);
}

TEST_CASE("app bridge: reset_setting needs its key (\"\" for every setting): a message without one changes nothing") {
    TempGame g(L"bridge_reset");
    fake_game(g);
    g.write(L"emf\\config\\clock.ini", "[clock]\r\nsize=5\r\n");
    FakeHost host;
    host.dir = g.dir;
    Bridge bridge(host);
    CHECK_FALSE(result_of(bridge.handle(R"({"cmd":"reset_setting","id":"clock"})")).get("ok")->b);
    CHECK_FALSE(result_of(bridge.handle(R"({"cmd":"reset_setting","id":"clock","key":5})")).get("ok")->b);
    CHECK(g.read(L"emf\\config\\clock.ini").find("size=5") != std::string::npos);
    CHECK(result_of(bridge.handle(R"({"cmd":"reset_setting","id":"clock","key":"size"})")).get("ok")->b);
    CHECK(g.read(L"emf\\config\\clock.ini").find("size=") == std::string::npos);
}

TEST_CASE("app bridge: play starts the game through Steam") {
    TempGame g(L"bridge_play");
    FakeHost host;
    host.dir = g.dir;
    Bridge bridge(host);
    CHECK(result_of(bridge.handle(R"({"cmd":"play"})")).get("ok")->b);
    REQUIRE(host.opened.size() == 1);
    CHECK(host.opened[0].first == L"steam://rungameid/1203620");
    CHECK(host.opened[0].second == BridgeHost::Open::Url);
}

TEST_CASE("app bridge: open_url opens Nexus links in the browser and nothing else") {
    FakeHost host;
    Bridge bridge(host);
    CHECK(result_of(bridge.handle(R"({"cmd":"open_url","url":"https://www.nexusmods.com/enshrouded/mods/77"})")).get("ok")->b);
    CHECK_FALSE(result_of(bridge.handle(R"({"cmd":"open_url","url":"https://example.com/"})")).get("ok")->b);
    CHECK_FALSE(result_of(bridge.handle(R"({"cmd":"open_url","url":"C:\\Windows\\System32\\calc.exe"})")).get("ok")->b);
    REQUIRE(host.opened.size() == 1);
    CHECK(host.opened[0].first == L"https://www.nexusmods.com/enshrouded/mods/77");
}

TEST_CASE("app bridge: open_folder opens folders inside the game folder only; a file is selected in Explorer") {
    TempGame g(L"bridge_folder");
    fake_game(g);
    FakeHost host;
    host.dir = g.dir;
    Bridge bridge(host);
    CHECK(result_of(bridge.handle("{\"cmd\":\"open_folder\",\"path\":" + json_quote(utf8(g.dir)) + "}")).get("ok")->b);
    CHECK(result_of(bridge.handle("{\"cmd\":\"open_folder\",\"path\":" + json_quote(utf8(g.at(L"emf\\mods\\clock\\clock.dll"))) + "}")).get("ok")->b);
    CHECK_FALSE(result_of(bridge.handle(R"({"cmd":"open_folder","path":"C:\\Windows"})")).get("ok")->b);
    CHECK_FALSE(result_of(bridge.handle("{\"cmd\":\"open_folder\",\"path\":" + json_quote(utf8(g.dir + L"\\..")) + "}")).get("ok")->b);
    REQUIRE(host.opened.size() == 2);
    CHECK(host.opened[0].second == BridgeHost::Open::Folder);
    CHECK(host.opened[1].second == BridgeHost::Open::SelectFile);
}

TEST_CASE("app bridge: pick_folder takes a folder with the game or a server, and refuses others") {
    TempGame game(L"bridge_pick", false);
    game.write(L"enshrouded_server.exe", "fake");
    TempGame other(L"bridge_pick_other", false);
    FakeHost host;
    Bridge bridge(host);
    host.picked = other.dir;
    json::Value r = result_of(bridge.handle(R"({"cmd":"pick_folder"})"));
    CHECK_FALSE(r.get("ok")->b);
    CHECK(r.get("error")->s.find("enshrouded.exe") != std::string::npos);
    CHECK(host.chosen.empty());
    host.picked = game.dir;
    const auto replies = bridge.handle(R"({"cmd":"pick_folder"})");
    CHECK(result_of(replies).get("ok")->b);
    CHECK(host.chosen == game.dir);
    CHECK(has_state(replies));
    host.picked.reset();  // cancelled: nothing changes, no error shown
    CHECK(result_of(bridge.handle(R"({"cmd":"pick_folder"})")).get("ok")->b);
}

TEST_CASE("app bridge: the page's first render of the mod list reaches the host (smoke test)") {
    FakeHost host;
    Bridge bridge(host);
    CHECK(bridge.handle(R"({"cmd":"rendered","count":3,"titles":["Clock","Foo","creative.dll"]})").empty());
    CHECK(host.rendered_count == 3);
    CHECK(host.rendered_titles == std::vector<std::string>{"Clock", "Foo", "creative.dll"});
}

TEST_CASE("app bridge: a result carries the page's ref back, so the page knows which setting it was about") {
    TempGame g(L"bridge_ref");
    fake_game(g);
    FakeHost host;
    host.dir = g.dir;
    Bridge bridge(host);
    const json::Value r = result_of(bridge.handle(R"({"cmd":"set_setting","id":"clock","key":"size","value":"99","ref":"emf:clock:size"})"));
    CHECK_FALSE(r.get("ok")->b);
    REQUIRE(r.get("ref"));
    CHECK(r.get("ref")->s == "emf:clock:size");
}

TEST_CASE("app bridge: a state that did not reach the page is sent again") {
    TempGame g(L"bridge_forget");
    fake_game(g);
    FakeHost host;
    host.dir = g.dir;
    Bridge bridge(host);
    CHECK(bridge.state_if_changed(false));
    CHECK_FALSE(bridge.state_if_changed(false));
    bridge.forget_state();  // the post failed
    CHECK(bridge.state_if_changed(false));
}

TEST_CASE("app bridge: logs, unknown commands and bad JSON") {
    TempGame g(L"bridge_logs");
    g.write(L"emf\\emf_core.log", "line 1\r\nline 2\r\n");
    FakeHost host;
    host.dir = g.dir;
    Bridge bridge(host);
    const auto logs = bridge.handle(R"({"cmd":"logs"})");
    REQUIRE(logs.size() == 1);
    const json::Value l = parse(logs[0]);
    CHECK(l.get("type")->s == "logs");
    CHECK(l.get("core")->s.find("line 2") != std::string::npos);
    CHECK(l.get("loader")->s.empty());
    CHECK_FALSE(result_of(bridge.handle(R"({"cmd":"format_disk"})")).get("ok")->b);
    CHECK_FALSE(result_of(bridge.handle("not json")).get("ok")->b);
}

TEST_CASE("app bridge: logs are read from their end while the game holds them open") {
    TempGame g(L"bridge_biglog");
    std::string log = "first line\r\n" + std::string(200 * 1024, 'x') + "\r\nlast line\r\n";
    g.write(L"emf\\emf_core.log", log);
    FILE* held = _wfsopen(g.at(L"emf\\emf_core.log").c_str(), L"ab", _SH_DENYWR);  // as EMF's core keeps it
    REQUIRE(held);
    FakeHost host;
    host.dir = g.dir;
    Bridge bridge(host);
    const auto replies = bridge.handle(R"({"cmd":"logs"})");
    fclose(held);
    REQUIRE(replies.size() == 1);
    const std::string core = parse(replies[0]).get("core")->s;
    CHECK(core.find("last line") != std::string::npos);
    CHECK(core.find("first line") == std::string::npos);
}

TEST_CASE("app bridge: Play warns when Steam's game folder is not the one shown") {
    TempGame g(L"bridge_play_other");
    TempGame steam_game(L"bridge_play_steam");
    FakeHost host;
    host.dir = g.dir;
    host.steam = steam_game.dir;
    Bridge bridge(host);
    json::Value r = result_of(bridge.handle(R"({"cmd":"play"})"));
    CHECK(r.get("ok")->b);
    CHECK(r.get("text")->s.find("Play starts the game in " + utf8(steam_game.dir)) != std::string::npos);
    host.steam = g.dir;
    r = result_of(bridge.handle(R"({"cmd":"play"})"));
    CHECK(r.get("text")->s.find("Play starts the game in") == std::string::npos);
}

TEST_CASE("app bridge: under Wine or Proton Play still asks Steam and says to start the game from Steam if nothing starts") {
    TempGame g(L"bridge_play_wine");
    fake_game(g);
    FakeHost host;
    host.dir = g.dir;
    host.wine = true;
    Bridge bridge(host);
    json::Value r = result_of(bridge.handle(R"({"cmd":"play"})"));
    CHECK(r.get("ok")->b);
    REQUIRE(host.opened.size() == 1);  // a Windows Steam in the same prefix (Lutris, Bottles) takes the link
    CHECK(r.get("text")->s.find("Wine or Proton") != std::string::npos);
    CHECK(r.get("text")->s.find("If nothing starts, start Enshrouded from Steam") != std::string::npos);
    CHECK(r.get("text")->s.find("EMF still loads") != std::string::npos);
    host.open_ok = false;  // no program for steam:// links in the prefix: the hint is all there is to do
    r = result_of(bridge.handle(R"({"cmd":"play"})"));
    CHECK_FALSE(r.get("ok")->b);
    CHECK(r.get("error")->s.find("Start Enshrouded from Steam") != std::string::npos);
    host.open_ok = true;
    const json::Value s = parse(bridge.handle(R"({"cmd":"state"})")[0]);
    REQUIRE(s.get("wine"));
    CHECK(s.get("wine")->b);  // the page says so on the Play button
    host.wine = false;
    r = result_of(bridge.handle(R"({"cmd":"play"})"));
    CHECK(r.get("ok")->b);
    CHECK(r.get("text")->s.find("Wine") == std::string::npos);
}

TEST_CASE("app bridge: the Play flag has a path only inside a game folder that is there") {
    TempGame g(L"bridge_play_path");
    CHECK(play_once_path(g.dir) == g.dir + L"\\emf\\play_once.txt");
    CHECK_FALSE(play_once_path(L"").has_value());  // never "\\emf\\play_once.txt" on the drive's root
    CHECK_FALSE(play_once_path(g.dir + L"\\not here").has_value());
    FakeHost host;  // no folder chosen: Play still asks Steam, and touches no file
    Bridge bridge(host);
    CHECK(result_of(bridge.handle(R"({"cmd":"play"})")).get("ok")->b);
    CHECK_FALSE(result_of(bridge.handle(R"({"cmd":"play","mode":"no-emf"})")).get("ok")->b);
}

TEST_CASE("app bridge: Play without EMF mods writes the loader's one-shot flag, then starts the game; Play takes it back") {
    using emf::loader::PlayOnce;
    TempGame g(L"bridge_play_once");
    fake_game(g);
    g.write(L"dinput8.dll", "EMF-LOADER-7d3c9f EMF-PLAY-ONCE-1");  // a loader that takes the flag
    g.write(L"eml.dll", "EML_LOG_FILE_PATH");
    FakeHost host;
    host.dir = g.dir;
    host.steam = g.dir;
    Bridge bridge(host);
    const json::Value s = parse(bridge.handle(R"({"cmd":"state"})")[0]);
    REQUIRE(s.get("play_once"));
    CHECK(s.get("play_once")->b);  // the page offers the menu
    const auto flag = [&] { return g.has(L"emf\\play_once.txt") ? g.read(L"emf\\play_once.txt") : std::string(); };
    const auto now = [] { return static_cast<int64_t>(std::time(nullptr)); };

    json::Value r = result_of(bridge.handle(R"({"cmd":"play","mode":"no-emf"})"));
    REQUIRE_MESSAGE(r.get("ok")->b, r.get("error")->s);
    CHECK(r.get("text")->s.find("without EMF mods") != std::string::npos);
    CHECK(r.get("text")->s.find("next start") != std::string::npos);
    REQUIRE(host.opened.size() == 1);
    CHECK(host.opened[0].first == L"steam://rungameid/1203620");
    CHECK(emf::loader::parse_play_once(flag(), now()).mode == PlayOnce::NoEmf);

    r = result_of(bridge.handle(R"({"cmd":"play","mode":"no-mods"})"));
    CHECK(r.get("ok")->b);
    CHECK(r.get("text")->s.find("without any mods") != std::string::npos);
    CHECK(emf::loader::parse_play_once(flag(), now()).mode == PlayOnce::NoMods);

    // plain Play: a start without mods asked for earlier that did not happen is taken back
    r = result_of(bridge.handle(R"({"cmd":"play"})"));
    CHECK(r.get("ok")->b);
    CHECK_FALSE(g.has(L"emf\\play_once.txt"));
    CHECK(host.opened.size() == 3);

    // Steam did not start: no flag left behind for a later start
    host.open_ok = false;
    CHECK_FALSE(result_of(bridge.handle(R"({"cmd":"play","mode":"no-emf"})")).get("ok")->b);
    CHECK_FALSE(g.has(L"emf\\play_once.txt"));
    host.open_ok = true;

    // under Wine or Proton: the flag and the link, and if nothing starts the player starts the game from Steam;
    // when the link reaches nothing, the flag stays for that start
    host.wine = true;
    const size_t opened = host.opened.size();
    r = result_of(bridge.handle(R"({"cmd":"play","mode":"no-emf"})"));
    CHECK(r.get("ok")->b);
    CHECK(r.get("text")->s.find("start Enshrouded from Steam within 10 minutes") != std::string::npos);
    CHECK(host.opened.size() == opened + 1);
    CHECK(emf::loader::parse_play_once(flag(), now()).mode == PlayOnce::NoEmf);
    host.open_ok = false;
    r = result_of(bridge.handle(R"({"cmd":"play","mode":"no-mods"})"));
    CHECK(r.get("ok")->b);
    CHECK(r.get("text")->s.find("Start Enshrouded from Steam within 10 minutes") != std::string::npos);
    CHECK(emf::loader::parse_play_once(flag(), now()).mode == PlayOnce::NoMods);
    host.open_ok = true;
    host.wine = false;
    std::filesystem::remove(g.at(L"emf\\play_once.txt"));

    // refused, and no flag written: an unknown way, the game running, another game folder in Steam, no EMF, an old loader
    CHECK_FALSE(result_of(bridge.handle(R"({"cmd":"play","mode":"sideways"})")).get("ok")->b);
    host.running = "Enshrouded is running from this folder";
    r = result_of(bridge.handle(R"({"cmd":"play","mode":"no-emf"})"));
    CHECK_FALSE(r.get("ok")->b);
    CHECK(r.get("error")->s.find("close it first") != std::string::npos);
    host.running.clear();
    TempGame other(L"bridge_play_once_steam");
    host.steam = other.dir;
    r = result_of(bridge.handle(R"({"cmd":"play","mode":"no-emf"})"));
    CHECK_FALSE(r.get("ok")->b);
    CHECK(r.get("error")->s.find(utf8(other.dir)) != std::string::npos);
    host.steam = g.dir;
    g.write(L"dinput8.dll", "EMF-LOADER-7d3c9f");  // EMF 0.3.2's loader: it would not take the flag
    bridge.handle(R"({"cmd":"state"})");
    r = result_of(bridge.handle(R"({"cmd":"play","mode":"no-mods"})"));
    CHECK_FALSE(r.get("ok")->b);
    CHECK(r.get("error")->s.find("Update EMF") != std::string::npos);
    std::filesystem::remove(g.at(L"emf\\emf_core.dll"));
    bridge.handle(R"({"cmd":"state"})");
    r = result_of(bridge.handle(R"({"cmd":"play","mode":"no-emf"})"));
    CHECK_FALSE(r.get("ok")->b);
    CHECK(r.get("error")->s.find("not installed") != std::string::npos);
    CHECK_FALSE(g.has(L"emf\\play_once.txt"));
}

TEST_CASE("app bridge: copy puts a text on the clipboard (logs and errors for a bug report)") {
    FakeHost host;
    Bridge bridge(host);
    CHECK(result_of(bridge.handle(R"({"cmd":"copy","text":"line 1\nline 2"})")).get("ok")->b);
    CHECK(host.copied == "line 1\nline 2");
}

// ---- Browse and install ----

namespace {

std::wstring nexus_fixture() { return wide(std::string(EMF_SOURCE_DIR) + "/tests/fixtures/nexus_app"); }

std::vector<json::Value> of_type(const std::vector<std::string>& out, const std::string& type) {
    std::vector<json::Value> v;
    for (const auto& m : out) {
        json::Value x = parse(m);
        if (x.get("type") && x.get("type")->s == type) v.push_back(x);
    }
    return v;
}

}  // namespace

TEST_CASE("app bridge: a Nexus search runs away from the UI thread and its answer reaches the page") {
    TempGame g(L"bridge_search");
    FakeHost host;
    host.dir = g.dir;
    host.transport = fixture_transport(nexus_fixture());
    Bridge bridge(host);
    CHECK(bridge.handle(R"({"cmd":"nexus_search","text":"loot","sort":"endorsements","offset":0,"seq":3})").empty());
    const auto r = of_type(host.async_out, "nexus_search");
    REQUIRE(r.size() == 1);
    CHECK(r[0].get("ok")->b);
    CHECK(r[0].get("seq")->n == 3);
    REQUIRE(r[0].get("mods")->arr.size() == 4);
    const json::Value& m = r[0].get("mods")->arr[0];
    CHECK(m.get("name")->s == "Auto Loot Plus");
    CHECK(m.get("author")->s == "Dellemann");
    CHECK(m.get("thumbnail")->s.rfind("https://staticdelivery.nexusmods.com/", 0) == 0);
}

TEST_CASE("app bridge: a newer search cancels the older one, whose answer is dropped") {
    TempGame g(L"bridge_search_old");
    FakeHost host;
    host.dir = g.dir;
    int requests = 0;
    const auto fixture = fixture_transport(nexus_fixture());
    host.transport = [&](const emf::nexus::HttpRequest& r) {
        ++requests;
        return fixture(r);
    };
    host.defer = true;
    Bridge bridge(host);
    bridge.handle(R"({"cmd":"nexus_search","text":"a","seq":1})");
    bridge.handle(R"({"cmd":"nexus_search","text":"ab","seq":2})");
    host.run_queued(true);  // the newer one finishes first, then the older one
    const auto r = of_type(host.async_out, "nexus_search");
    REQUIRE(r.size() == 1);
    CHECK(r[0].get("seq")->n == 2);
    CHECK(requests == 1);  // the cancelled one never asked Nexus
}

TEST_CASE("app bridge: a mod's page has its description, its files in order and what each file is") {
    TempGame g(L"bridge_mod");
    g.write(L"dinput8.dll", "EMF-LOADER-7d3c9f");
    g.write(L"emf\\emf_core.dll", std::string("emf_core 1.2.3\0", 15));
    FakeHost host;
    host.dir = g.dir;
    host.transport = fixture_transport(nexus_fixture());
    Bridge bridge(host);
    bridge.handle(R"({"cmd":"state"})");
    bridge.handle(R"({"cmd":"nexus_mod","id":170,"seq":7})");
    const auto r = of_type(host.async_out, "nexus_mod");
    REQUIRE(r.size() == 1);
    REQUIRE(r[0].get("ok")->b);
    const json::Value* mod = r[0].get("mod");
    CHECK(mod->get("name")->s == "Auto Loot Plus");
    CHECK(mod->get("description_html")->s.find("<b>Auto Loot Plus</b>") != std::string::npos);
    CHECK(mod->get("url")->s == "https://www.nexusmods.com/enshrouded/mods/170");
    const auto& files = r[0].get("files")->arr;
    REQUIRE(files.size() == 5);
    CHECK(files[0].get("category")->s == "MAIN");
    CHECK(files[1].get("category")->s == "OPTIONAL");
    CHECK(files[4].get("category")->s == "ARCHIVED");
    CHECK(files[0].get("checked")->b);
    CHECK_FALSE(files[0].get("listed")->b);   // Nexus has no listing for the newest file
    CHECK_FALSE(files[4].get("checked")->b);  // archived: looked at only when asked
    bridge.handle(R"({"cmd":"nexus_file","mod_id":170,"file_id":929,"seq":8})");
    const auto f = of_type(host.async_out, "nexus_file");
    REQUIRE(f.size() == 1);
    const json::Value* file = f[0].get("file");
    CHECK(file->get("listed")->b);
    CHECK(file->get("kind")->s == "dll-dinput8");
    REQUIRE(file->get("also")->arr.size() == 1);
    CHECK(file->get("also")->arr[0].s == "EML mod");
}

TEST_CASE("app bridge: description links open any https address; the app's own buttons only Nexus") {
    FakeHost host;
    Bridge bridge(host);
    CHECK(result_of(bridge.handle(R"({"cmd":"open_url","url":"https://github.com/x/y","external":true})")).get("ok")->b);
    CHECK_FALSE(result_of(bridge.handle(R"({"cmd":"open_url","url":"https://github.com/x/y"})")).get("ok")->b);
    CHECK(result_of(bridge.handle(R"({"cmd":"open_url","url":"http://github.com/","external":true})")).get("ok")->b);  // upgraded to https
    CHECK_FALSE(result_of(bridge.handle(R"({"cmd":"open_url","url":"https://user:pw@github.com/","external":true})")).get("ok")->b);
    CHECK_FALSE(result_of(bridge.handle(R"({"cmd":"open_url","url":"https://github.com:8443/","external":true})")).get("ok")->b);
    CHECK_FALSE(result_of(bridge.handle(R"({"cmd":"open_url","url":"file:///C:/Windows/","external":true})")).get("ok")->b);
    REQUIRE(host.opened.size() == 2);
    CHECK(host.opened[0].first == L"https://github.com/x/y");
    CHECK(host.opened[1].first == L"https://github.com/");  // the http link opened as https
}

TEST_CASE("app bridge: install a downloaded file: plan, Install, the state lists it, Uninstall") {
    TempGame g(L"bridge_install");
    TempGame src(L"bridge_install_src", false), keep(L"bridge_install_zip", false);
    src.write(L"FooMod\\mods\\foo\\mod.json", R"({"id":"foo","name":"Foo Mod","version":"1.0"})");
    src.write(L"FooMod\\mods\\foo\\src\\mod.lua", "-- lua");
    const std::wstring zip = keep.at(L"FooMod-12-1-0.zip");
    REQUIRE(app_test::make_zip(src.dir, zip));
    FakeHost host;
    host.dir = g.dir;
    Bridge bridge(host);
    bridge.handle(R"({"cmd":"state"})");
    const std::string ask = "{\"cmd\":\"install_path\",\"path\":" + json_quote(utf8(zip)) + ",\"nexus_id\":12}";
    CHECK_FALSE(result_of(bridge.handle(ask)).get("ok")->b);  // only in test mode
    host.testing = true;
    const auto first = bridge.handle(ask);
    REQUIRE(of_type(first, "install_busy").size() == 1);
    auto plans = of_type(host.async_out, "install_plan");
    REQUIRE(plans.size() == 1);
    REQUIRE_MESSAGE(plans[0].get("ok")->b, plans[0].get("error")->s);
    const json::Value* plan = plans[0].get("plan");
    CHECK(plan->get("key")->s == "nexus-12");
    CHECK(plan->get("installable")->b);
    REQUIRE(plan->get("parts")->arr.size() == 1);
    CHECK(plan->get("parts")->arr[0].get("target")->s == "mods\\foo\\");
    CHECK_FALSE(g.has(L"mods\\foo"));  // nothing installed before Install
    const std::string id = plan->get("id")->s;

    host.running = "enshrouded.exe (process 9) is running from this folder";
    auto refused = of_type(bridge.handle("{\"cmd\":\"install_apply\",\"plan_id\":\"" + id + "\"}"), "install_done");
    REQUIRE(refused.size() == 1);
    CHECK_FALSE(refused[0].get("ok")->b);
    CHECK(refused[0].get("error")->s.find("close the game first") != std::string::npos);
    host.running.clear();

    host.async_out.clear();
    CHECK(bridge.handle("{\"cmd\":\"install_apply\",\"plan_id\":\"" + id + "\"}").empty());
    auto done = of_type(host.async_out, "install_done");
    REQUIRE(done.size() == 1);
    REQUIRE_MESSAGE(done[0].get("ok")->b, done[0].get("error")->s);
    CHECK(g.read(L"mods\\foo\\src\\mod.lua") == "-- lua");
    auto states = of_type(host.async_out, "state");
    REQUIRE(states.size() == 1);
    REQUIRE(states[0].get("installed")->arr.size() == 1);
    CHECK(states[0].get("installed")->arr[0].get("nexus_id")->s == "12");
    bool linked = false;
    for (const auto& m : states[0].get("mods")->arr) linked = linked || m.get("install_key")->s == "nexus-12";
    CHECK(linked);
    // the plan is used up
    CHECK_FALSE(of_type(bridge.handle("{\"cmd\":\"install_apply\",\"plan_id\":\"" + id + "\"}"), "install_done")[0].get("ok")->b);

    host.async_out.clear();
    bridge.handle(R"({"cmd":"uninstall_mod","key":"nexus-12"})");
    done = of_type(host.async_out, "uninstall_done");
    REQUIRE(done.size() == 1);
    CHECK(done[0].get("ok")->b);
    CHECK_FALSE(g.has(L"mods\\foo"));
    CHECK(of_type(host.async_out, "state")[0].get("installed")->arr.empty());
}

TEST_CASE("app bridge: a cancelled plan's unpack folder is removed; a file that is not an archive is refused") {
    TempGame g(L"bridge_install_cancel");
    TempGame src(L"bridge_cancel_src", false), keep(L"bridge_cancel_zip", false);
    src.write(L"mods\\foo\\mod.json", R"({"id":"foo"})");
    src.write(L"mods\\foo\\src\\mod.lua", "-- lua");
    REQUIRE(app_test::make_zip(src.dir, keep.at(L"foo.zip")));
    // %TEMP%\emf-install is shared with every other EMF.exe and emf_tests on the PC, whose plans come and go while this
    // test counts: here %TEMP% is a folder of this run's own, so only this plan's unpack folder is in it
    const test_scratch::PrivateTemp tmp(L"emf_bridge_cancel_tmp");
    FakeHost host;
    host.dir = g.dir;
    Bridge bridge(host);
    bridge.handle(R"({"cmd":"state"})");
    host.archive = keep.at(L"foo.zip");
    bridge.handle(R"({"cmd":"install_pick"})");
    auto plans = of_type(host.async_out, "install_plan");
    REQUIRE(plans.size() == 1);
    REQUIRE(plans[0].get("ok")->b);
    const std::wstring folder = tmp.dir() + L"\\emf-install";
    REQUIRE(std::filesystem::is_directory(folder));
    const auto count = [&] {
        size_t n = 0;
        for (const auto& e : std::filesystem::directory_iterator(folder)) n += e.is_directory() ? 1 : 0;
        return n;
    };
    const size_t before = count();
    CHECK(before == 1);  // the plan's
    bridge.handle("{\"cmd\":\"install_cancel\",\"plan_id\":\"" + plans[0].get("plan")->get("id")->s + "\"}");
    size_t after = count();  // the unpack folder is deleted on a background thread
    for (int i = 0; i < 500 && after >= before; ++i) {  // up to 10 s on a busy PC; usually the first look
        Sleep(20);
        after = count();
    }
    CHECK(after == before - 1);
    host.archive = keep.at(L"notes.txt");
    CHECK_FALSE(result_of(bridge.handle(R"({"cmd":"install_pick"})")).get("ok")->b);
    host.archive.reset();  // the dialog was cancelled: nothing happens
    CHECK(bridge.handle(R"({"cmd":"install_pick"})").empty());
}
