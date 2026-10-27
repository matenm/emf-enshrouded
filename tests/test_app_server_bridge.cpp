// EMF.exe's page and dedicated servers: the folder switcher (folders.json), adding a server folder found
// through Steam or picked, the Server tab's state (enshrouded_server.json without passwords, EMF's chat settings, the
// process), saving chat settings, start, stop (on a worker, with progress), Stop now, what closing EMF.exe does,
// installing EMF and the logs (passwords stripped). A fake host stands in for the window, Steam and the processes.
#include <doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "app/bridge.h"
#include "app/server_bridge.h"
#include "app/text.h"
#include "app_test_files.h"
#include "core/json.h"

using namespace emf::app;
using app_test::TempGame;
namespace json = emf::core::json;

namespace {

const char* kConfig = R"({"name": "Ember Test Server", "queryPort": 15637, "slotCount": 8, "enableTextChat": true,
  "logDirectory": "./logs",
  "userGroups": [{"name": "Admin", "password": "Adm1nSecretXyz"}, {"name": "Guest", "password": ""}]})";

struct FakeHost : BridgeHost {
    std::wstring dir;
    std::wstring appdata;
    std::optional<std::wstring> picked;
    std::vector<std::wstring> steam;
    std::vector<ProcessInfo> procs;
    bool testing = false;
    // start / stop
    std::vector<std::pair<std::wstring, bool>> starts;
    std::vector<std::pair<std::wstring, bool>> close_flags;
    unsigned long next_pid = 4100;
    StopOutcome stop_answer;
    std::vector<unsigned long> stops, kills;
    EmfPackage package;
    std::vector<std::wstring> installs;
    ScriptRun install_answer;
    std::wstring exe = L"C:\\Elsewhere\\emf\\EMF.exe";

    std::wstring game() const override { return dir; }
    void choose_game(const std::wstring& d) override { dir = d; }
    std::optional<std::wstring> pick_folder() override { return picked; }
    bool open(const std::wstring&, Open) override { return true; }
    std::string running_reason() override { return game_running_reason(dir, procs); }
    void rendered(int, const std::vector<std::string>&) override {}
    std::wstring steam_game() override { return {}; }
    bool copy_text(const std::string&) override { return true; }
    std::optional<std::wstring> pick_archive() override { return std::nullopt; }
    bool defer = false;
    std::vector<std::function<Done()>> queued;
    std::vector<std::string> async_out;
    void run_async(std::function<Done()> work) override {
        if (defer) queued.push_back(std::move(work));
        else finish(work());
    }
    void finish(const Done& d) {
        if (d)
            for (auto& m : d()) async_out.push_back(m);
    }
    void run_queued() {
        auto q = std::move(queued);
        queued.clear();
        for (auto& w : q) finish(w());
    }
    std::function<void(Done)> ui_poster() override {
        return [this](Done d) { finish(d); };
    }
    bool test_mode() const override { return testing; }
    std::wstring app_data() const override { return appdata; }
    std::wstring exe_path() const override { return exe; }

    std::vector<std::wstring> steam_servers() override { return steam; }
    std::vector<ProcessInfo> processes() override { return procs; }
    ServerStart start_server(const std::wstring& folder, bool stop_on_close) override {
        starts.emplace_back(folder, stop_on_close);
        ServerStart s;
        s.ok = true;
        s.pid = next_pid;
        procs.push_back({L"enshrouded_server.exe", folder + L"\\enshrouded_server.exe", next_pid});
        return s;
    }
    bool set_server_stop_on_close(const std::wstring& folder, bool on) override {
        close_flags.emplace_back(folder, on);
        return true;
    }
    bool server_started_here(unsigned long pid) override { return pid == next_pid; }
    bool last_shared_ok = false;
    StopOutcome stop_server(unsigned long pid, const StopOptions& o) override {
        stops.push_back(pid);
        last_shared_ok = o.shared_ok;
        if (o.progress) o.progress("Asking the server to stop");
        if (stop_answer.stopped) procs.clear();
        return stop_answer;
    }
    bool kill_server(unsigned long pid) override {
        kills.push_back(pid);
        procs.clear();
        return true;
    }
    EmfPackage emf_package() override { return package; }
    std::vector<std::pair<std::wstring, std::string>> install_writes;  // what the fake installer puts into the server
    ScriptRun install_emf(const EmfPackage&, const std::wstring& server) override {
        installs.push_back(server);
        for (const auto& [rel, text] : install_writes) {
            const std::filesystem::path p(server + L"\\" + rel);
            std::filesystem::create_directories(p.parent_path());
            std::ofstream(p, std::ios::binary) << text;
        }
        return install_answer;
    }
};

json::Value parse(const std::string& s) {
    auto r = json::parse(s);
    REQUIRE_MESSAGE(r.value.has_value(), s);
    return *r.value;
}

std::string q(const std::wstring& p) {  // a path as a JSON string
    return json_quote(utf8(p));
}

// The last message of a type in `out`.
std::optional<json::Value> last_of(const std::vector<std::string>& out, const std::string& type) {
    for (auto it = out.rbegin(); it != out.rend(); ++it) {
        const json::Value v = parse(*it);
        if (const json::Value* t = v.get("type"); t && t->s == type) return v;
    }
    return std::nullopt;
}

std::string text(const json::Value& v, const char* key) {
    const json::Value* x = v.get(key);
    return x && x->is(json::Value::Type::String) ? x->s : std::string();
}

bool flag(const json::Value& v, const char* key) {
    const json::Value* x = v.get(key);
    return x && x->is(json::Value::Type::Bool) && x->b;
}

std::string all(const std::vector<std::string>& out) {
    std::string s;
    for (const auto& m : out) s += m + "\n";
    return s;
}

struct World {
    TempGame app{L"sb_appdata", false};
    TempGame game{L"sb_game"};
    TempGame server{L"sb_server", false};
    FakeHost host;
    World() {
        server.write(L"enshrouded_server.exe", "fake");
        server.write(L"enshrouded_server.json", kConfig);
        host.dir = game.dir;
        host.appdata = app.dir;
        host.steam = {server.dir};
    }
};

}  // namespace

TEST_CASE("app server bridge: the folder switcher keeps the game and adds a server Steam has") {
    World w;
    Bridge b(w.host);
    auto st = parse(b.handle(R"({"cmd":"state"})")[0]);
    const json::Value* folders = st.get("folders");
    REQUIRE(folders);
    REQUIRE(folders->arr.size() == 1);
    CHECK(text(folders->arr[0], "kind") == "game");
    CHECK(text(folders->arr[0], "label") == "Game");
    CHECK(flag(folders->arr[0], "current"));
    CHECK(st.get("server_tab")->is(json::Value::Type::Null));  // a game folder has no Server tab

    // "Add a server folder...": what Steam has
    auto det = last_of(b.handle(R"({"cmd":"server_detect"})"), "server_detect");
    REQUIRE(det);
    REQUIRE(det->get("found")->arr.size() == 1);
    CHECK(text(det->get("found")->arr[0], "name") == "Ember Test Server");
    CHECK_FALSE(flag(det->get("found")->arr[0], "added"));

    auto out = b.handle(R"({"cmd":"folder_add","path":)" + q(w.server.dir) + "}");
    auto res = last_of(out, "result");
    REQUIRE(res);
    CHECK(flag(*res, "ok"));
    CHECK(w.host.dir == w.server.dir);  // it shows the server now
    auto st2 = last_of(out, "state");
    REQUIRE(st2);
    REQUIRE(st2->get("folders")->arr.size() == 2);
    CHECK(text(st2->get("folders")->arr[1], "label") == "Server: Ember Test Server");
    CHECK(flag(st2->get("folders")->arr[1], "current"));
    CHECK(flag(*st2, "server"));  // the state's own server flag (the folder kind)
    // remembered in folders.json, and found again by a new bridge (EMF.exe started again)
    const FolderList saved = read_folder_list(w.app.at(L"folders.json"));
    REQUIRE(saved.folders.size() == 2);
    CHECK(saved.folders[1].kind == "server");

    // back to the game, and forgetting the server
    b.handle(R"({"cmd":"folder_switch","path":)" + q(w.game.dir) + "}");
    CHECK(w.host.dir == w.game.dir);
    auto f = last_of(b.handle(R"({"cmd":"folder_forget","path":)" + q(w.server.dir) + "}"), "state");
    REQUIRE(f);
    CHECK(f->get("folders")->arr.size() == 1);
    CHECK(read_folder_list(w.app.at(L"folders.json")).folders.size() == 1);
}

TEST_CASE("app server bridge: a folder the page names is added only when Steam has it (or picked in the dialog)") {
    World w;
    TempGame other(L"sb_other", false);
    other.write(L"enshrouded_server.exe", "fake");
    Bridge b(w.host);
    b.handle(R"({"cmd":"state"})");
    auto res = last_of(b.handle(R"({"cmd":"folder_add","path":)" + q(other.dir) + "}"), "result");
    REQUIRE(res);
    CHECK_FALSE(flag(*res, "ok"));
    CHECK(w.host.dir == w.game.dir);
    // switching to a folder that is not in the list: refused
    res = last_of(b.handle(R"({"cmd":"folder_switch","path":)" + q(other.dir) + "}"), "result");
    CHECK_FALSE(flag(*res, "ok"));
    // the dialog: a game folder is not a server; a server folder is added and shown
    w.host.picked = w.game.dir;
    res = last_of(b.handle(R"({"cmd":"folder_pick_server"})"), "result");
    CHECK_FALSE(flag(*res, "ok"));
    CHECK(text(*res, "error").find("enshrouded_server.exe") != std::string::npos);
    w.host.picked = other.dir;
    b.handle(R"({"cmd":"folder_pick_server"})");
    CHECK(w.host.dir == other.dir);
    w.host.picked = std::nullopt;  // cancelled: nothing changes, no message
    CHECK(b.handle(R"({"cmd":"folder_pick_server"})").size() == 1);
}

TEST_CASE("app server bridge: the Server tab shows the server's config without passwords, and EMF's chat settings") {
    World w;
    w.server.write(L"emf\\emf_core.ini", "[chat]\r\nadmins=0x0123456789abcdef\r\neveryone=0\r\n");
    w.server.write(L"emf\\emf_core.log",
                   "[11:06:10.000 t2] chat command refused from account 0x1111222233334444: help (to allow it, ...)\r\n"
                   "[11:06:11.000 t2] chat command refused from account 0x0123456789abcdef: help (an admin by now)\r\n");
    w.host.dir = w.server.dir;
    Bridge b(w.host);
    const std::vector<std::string> out = b.handle(R"({"cmd":"state"})");
    CHECK(all(out).find("Adm1nSecretXyz") == std::string::npos);
    auto st = parse(out[0]);
    const json::Value* srv = st.get("server_tab");
    REQUIRE(srv);
    REQUIRE(srv->is(json::Value::Type::Object));
    CHECK(text(*srv->get("config"), "name") == "Ember Test Server");
    CHECK(srv->get("config")->get("slots")->n == 8);
    CHECK(flag(srv->get("config")->get("groups")->arr[0], "password_set"));
    CHECK_FALSE(flag(srv->get("config")->get("groups")->arr[1], "password_set"));
    const json::Value* chat = srv->get("chat");
    REQUIRE(chat);
    CHECK(chat->get("admins")->arr.size() == 1);
    CHECK_FALSE(flag(*chat, "everyone"));
    // accounts the server refused, without the ones that are admins by now
    REQUIRE(srv->get("refused")->arr.size() == 1);
    CHECK(text(srv->get("refused")->arr[0], "account") == "0x1111222233334444");
    CHECK(text(srv->get("refused")->arr[0], "time") == "11:06:10");
    CHECK_FALSE(flag(*srv->get("process"), "running"));
    // the folder was shown at start: it is in the list, as a server
    REQUIRE(st.get("folders")->arr.size() == 1);
    CHECK(text(st.get("folders")->arr[0], "kind") == "server");
}

TEST_CASE("app server bridge: chat settings are checked and saved; a refused account is added in one click") {
    World w;
    w.host.dir = w.server.dir;
    Bridge b(w.host);
    b.handle(R"({"cmd":"state"})");
    auto res = last_of(b.handle(R"({"cmd":"server_chat_save","everyone":false,"admins":"0x0123456789abcdef\n76561198012345678901"})"), "result");
    REQUIRE(res);
    CHECK_FALSE(flag(*res, "ok"));
    CHECK(text(*res, "error").find("76561198012345678901") != std::string::npos);
    CHECK_FALSE(w.server.has(L"emf\\emf_core.ini"));  // nothing saved

    auto out = b.handle(R"({"cmd":"server_chat_save","everyone":true,"admins":"0x0123456789abcdef\r\n0xfedcba9876543210\n"})");
    res = last_of(out, "result");
    CHECK(flag(*res, "ok"));
    CHECK(text(*res, "text").find("next start") != std::string::npos);
    const ChatSettings saved = read_chat_settings(w.server.dir);
    CHECK(saved.everyone);
    CHECK(saved.admins == std::vector<std::string>{"0x0123456789abcdef", "0xfedcba9876543210"});

    res = last_of(b.handle(R"({"cmd":"server_admin_add","account":"0x1111222233334444"})"), "result");
    CHECK(flag(*res, "ok"));
    CHECK(read_chat_settings(w.server.dir).admins.size() == 3);
    res = last_of(b.handle(R"({"cmd":"server_admin_add","account":"not-an-account"})"), "result");
    CHECK_FALSE(flag(*res, "ok"));
    // only for a server folder (the game, picked with Change)
    w.host.dir = w.game.dir;
    b.handle(R"({"cmd":"state"})");
    res = last_of(b.handle(R"({"cmd":"server_chat_save","everyone":false,"admins":""})"), "result");
    CHECK_FALSE(flag(*res, "ok"));
}

TEST_CASE("app server bridge: start, then stop on a worker with progress; a stop that fails offers Stop now") {
    World w;
    w.host.dir = w.server.dir;
    Bridge b(w.host);
    b.handle(R"({"cmd":"state"})");
    auto out = b.handle(R"({"cmd":"server_start"})");
    REQUIRE(w.host.starts.size() == 1);
    CHECK(w.host.starts[0].first == w.server.dir);
    CHECK_FALSE(w.host.starts[0].second);  // closing EMF.exe leaves it running unless chosen
    auto st = last_of(out, "state");
    REQUIRE(st);
    const json::Value* proc = st->get("server_tab")->get("process");
    CHECK(flag(*proc, "running"));
    CHECK(proc->get("pid")->n == 4100);
    CHECK(flag(*proc, "started_here"));
    CHECK(flag(*st, "game_running"));  // the switches lock while it runs
    // a second start: refused
    auto res = last_of(b.handle(R"({"cmd":"server_start"})"), "result");
    CHECK_FALSE(flag(*res, "ok"));
    CHECK(w.host.starts.size() == 1);

    // a stop that does not work: the page hears the progress, then the failure, and Stop now becomes possible
    w.host.defer = true;
    w.host.stop_answer = {false, "not_stopped", "It wrote no autosave line."};
    out = b.handle(R"({"cmd":"server_stop"})");
    auto stopping = last_of(out, "state");
    REQUIRE(stopping);
    CHECK(flag(*stopping->get("server_tab"), "stopping"));
    res = last_of(b.handle(R"({"cmd":"server_stop"})"), "result");  // one stop at a time
    CHECK_FALSE(flag(*res, "ok"));
    w.host.run_queued();
    auto prog = last_of(w.host.async_out, "server_stop");
    REQUIRE(prog);
    CHECK(text(*prog, "state") == "failed");
    CHECK(text(*prog, "text") == "It wrote no autosave line.");
    CHECK(all(w.host.async_out).find("Asking the server to stop") != std::string::npos);
    st = last_of(w.host.async_out, "state");
    REQUIRE(st);
    CHECK(flag(*st->get("server_tab"), "stop_failed"));
    CHECK_FALSE(flag(*st->get("server_tab"), "stopping"));
    // Stop now: ends it
    w.host.async_out.clear();
    out = b.handle(R"({"cmd":"server_kill"})");
    CHECK(w.host.kills == std::vector<unsigned long>{4100});
    st = last_of(out, "state");
    CHECK_FALSE(flag(*st->get("server_tab")->get("process"), "running"));

    // started again and stopped cleanly
    w.host.defer = false;
    b.handle(R"({"cmd":"server_start"})");
    w.host.stop_answer = {true, "clean", "The server stopped cleanly."};
    b.handle(R"({"cmd":"server_stop"})");
    prog = last_of(w.host.async_out, "server_stop");
    REQUIRE(prog);
    CHECK(text(*prog, "state") == "done");
    st = last_of(w.host.async_out, "state");
    CHECK_FALSE(flag(*st->get("server_tab"), "stop_failed"));
    CHECK_FALSE(flag(*st->get("server_tab")->get("process"), "running"));
    // Stop now without a failed stop first: refused
    res = last_of(b.handle(R"({"cmd":"server_kill"})"), "result");
    CHECK_FALSE(flag(*res, "ok"));
}

TEST_CASE("app server bridge: what closing EMF.exe does is a choice per server, kept in folders.json") {
    World w;
    w.host.dir = w.server.dir;
    Bridge b(w.host);
    b.handle(R"({"cmd":"state"})");
    auto out = b.handle(R"({"cmd":"server_on_close","stop":true})");
    CHECK(flag(*last_of(out, "result"), "ok"));
    REQUIRE(w.host.close_flags.size() == 1);
    CHECK(w.host.close_flags[0].second);
    CHECK(flag(*last_of(out, "state")->get("server_tab"), "stop_on_close"));
    const FolderEntry* e = find_folder(read_folder_list(w.app.at(L"folders.json")), w.server.dir);
    REQUIRE(e);
    CHECK(e->stop_on_close);
    b.handle(R"({"cmd":"server_start"})");
    CHECK(w.host.starts.back().second);  // a start uses the choice
}

TEST_CASE("app server bridge: Install EMF runs the package's installer for the server, never while it runs") {
    World w;
    w.host.dir = w.server.dir;
    Bridge b(w.host);
    b.handle(R"({"cmd":"state"})");
    // no package: the reason
    w.host.package.error = "This EMF.exe has no EMF package with it.";
    auto res = last_of(b.handle(R"({"cmd":"server_install_emf"})"), "result");
    CHECK_FALSE(flag(*res, "ok"));
    CHECK(text(*res, "error").find("no EMF package") != std::string::npos);

    w.host.package = EmfPackage{true, "", L"C:\\pkg\\setup", L"C:\\pkg\\game", L"", "0.3.0"};
    auto st = parse(b.handle(R"({"cmd":"state"})")[0]);
    CHECK(text(*st.get("server_tab")->get("package"), "version") == "0.3.0");
    w.host.install_answer.ok = true;
    w.host.install_answer.lines = {"EMF installed into the server (12 files copied)."};
    b.handle(R"({"cmd":"server_install_emf"})");
    REQUIRE(w.host.installs.size() == 1);
    CHECK(w.host.installs[0] == w.server.dir);
    auto done = last_of(w.host.async_out, "server_install");
    REQUIRE(done);
    CHECK(flag(*done, "ok"));
    CHECK(done->get("lines")->arr[0].s.find("12 files") != std::string::npos);

    // while the server runs: refused
    b.handle(R"({"cmd":"server_start"})");
    res = last_of(b.handle(R"({"cmd":"server_install_emf"})"), "result");
    CHECK_FALSE(flag(*res, "ok"));
    CHECK(w.host.installs.size() == 1);
    w.host.procs.clear();
    // this EMF.exe runs from the server folder itself: its installer would refuse; EMF says why first
    w.host.exe = w.server.dir + L"\\emf\\EMF.exe";
    res = last_of(b.handle(R"({"cmd":"server_install_emf"})"), "result");
    CHECK_FALSE(flag(*res, "ok"));
    CHECK(text(*res, "error").find("install.cmd") != std::string::npos);
}

TEST_CASE("app server bridge: the logs are the server's own and EMF's, with passwords stripped") {
    World w;
    w.server.write(L"logs\\enshrouded_server.log",
                   "[I 00:00:00,007] Config Parsed\r\n[I 00:00:01,000] someone pasted Adm1nSecretXyz\r\n[I 00:00:02,000] \"password\": \"x9y8z7\"\r\n");
    w.server.write(L"emf\\emf_core.log", "[11:05:40.860 t1] emf_core 0.3.0 starting\r\n");
    w.host.dir = w.server.dir;
    Bridge b(w.host);
    b.handle(R"({"cmd":"state"})");
    b.handle(R"({"cmd":"server_logs"})");  // read on a worker
    auto logs = last_of(w.host.async_out, "server_logs");
    REQUIRE(logs);
    const std::string server = text(*logs, "server");
    CHECK(server.find("Config Parsed") != std::string::npos);
    CHECK(server.find("Adm1nSecretXyz") == std::string::npos);
    CHECK(server.find("x9y8z7") == std::string::npos);
    CHECK(text(*logs, "core").find("emf_core 0.3.0 starting") != std::string::npos);
}

TEST_CASE("app server bridge: a server folder's mods carry where they run") {
    World w;
    w.server.write(L"emf\\mods\\clock\\mod.json", R"({"id":"clock","name":"Clock","version":"1.1.0","side":"client"})");
    w.server.write(L"emf\\mods\\clock\\clock.dll", "dll");
    w.host.dir = w.server.dir;
    Bridge b(w.host);
    auto st = parse(b.handle(R"({"cmd":"state"})")[0]);
    const json::Value* mods = st.get("mods");
    REQUIRE(mods);
    const auto it = std::find_if(mods->arr.begin(), mods->arr.end(), [](const json::Value& m) { return text(m, "id") == "clock"; });
    REQUIRE(it != mods->arr.end());
    CHECK(text(*it, "side") == "client");
    CHECK(text(*it, "server_note") == "Its mod.json says it does nothing on a server.");
}

TEST_CASE("app server bridge: a server taken off the list is no longer stopped with EMF.exe (its job's flag cleared)") {
    World w;
    w.host.dir = w.game.dir;
    Bridge b(w.host);
    b.handle(R"({"cmd":"state"})");
    b.handle(R"({"cmd":"folder_add","path":)" + q(w.server.dir) + "}");
    b.handle(R"({"cmd":"server_on_close","stop":true})");
    b.handle(R"({"cmd":"server_start"})");
    CHECK(w.host.starts.back().second);  // started set to stop with EMF.exe
    w.host.close_flags.clear();
    b.handle(R"({"cmd":"folder_forget","path":)" + q(w.server.dir) + "}");
    REQUIRE(w.host.close_flags.size() == 1);
    CHECK_FALSE(w.host.close_flags[0].second);
    CHECK(w.host.dir == w.game.dir);  // it showed the server: the game is shown again
}

TEST_CASE("app server bridge: Start is refused while Windows hides the folder of a running server") {
    World w;
    w.host.dir = w.server.dir;
    w.host.procs = {{L"enshrouded_server.exe", L"", 77}};  // runs as administrator: no path
    Bridge b(w.host);
    b.handle(R"({"cmd":"state"})");
    auto res = last_of(b.handle(R"({"cmd":"server_start"})"), "result");
    REQUIRE(res);
    CHECK_FALSE(flag(*res, "ok"));
    CHECK(w.host.starts.empty());
}

TEST_CASE("app server bridge: the EMF tab's logs of a server folder have its passwords stripped too") {
    World w;
    w.server.write(L"emf\\emf_core.log", "[11:05:40.860 t1] a line with Adm1nSecretXyz in it\r\n");
    w.host.dir = w.server.dir;
    Bridge b(w.host);
    b.handle(R"({"cmd":"state"})");
    auto logs = last_of(b.handle(R"({"cmd":"logs"})"), "logs");
    REQUIRE(logs);
    CHECK(text(*logs, "core").find("a line with") != std::string::npos);
    CHECK(text(*logs, "core").find("Adm1nSecretXyz") == std::string::npos);
}

TEST_CASE("app server bridge: a failed stop belongs to its server: another server's tab offers no Stop now") {
    World w;
    TempGame second(L"sb_second", false);
    second.write(L"enshrouded_server.exe", "fake");
    second.write(L"enshrouded_server.json", R"({"name": "Second"})");
    w.host.steam = {w.server.dir, second.dir};
    Bridge b(w.host);
    b.handle(R"({"cmd":"state"})");
    b.handle(R"({"cmd":"folder_add","path":)" + q(second.dir) + "}");
    b.handle(R"({"cmd":"server_start"})");  // pid 4100 in the second folder
    w.host.next_pid = 4200;
    b.handle(R"({"cmd":"folder_add","path":)" + q(w.server.dir) + "}");
    b.handle(R"({"cmd":"server_start"})");  // pid 4200 in the first
    w.host.stop_answer = {false, "not_stopped", "It wrote no autosave line."};
    b.handle(R"({"cmd":"server_stop"})");  // fails for 4200
    auto st = last_of(b.handle(R"({"cmd":"state"})"), "state");
    CHECK(flag(*st->get("server_tab"), "stop_failed"));
    b.handle(R"({"cmd":"folder_switch","path":)" + q(second.dir) + "}");
    st = last_of(b.handle(R"({"cmd":"state"})"), "state");
    CHECK_FALSE(flag(*st->get("server_tab"), "stop_failed"));
    CHECK(text(*st->get("server_tab"), "stop_text").empty());
    auto res = last_of(b.handle(R"({"cmd":"server_kill"})"), "result");
    CHECK_FALSE(flag(*res, "ok"));
    CHECK(w.host.kills.empty());
}

TEST_CASE("app server bridge: no Start while EMF is being installed into the server") {
    World w;
    w.host.dir = w.server.dir;
    w.host.package = EmfPackage{true, "", L"C:\\pkg\\setup", L"C:\\pkg\\game", L"", "0.3.0"};
    w.host.install_answer.ok = true;
    Bridge b(w.host);
    b.handle(R"({"cmd":"state"})");
    w.host.defer = true;
    auto st = last_of(b.handle(R"({"cmd":"server_install_emf"})"), "state");
    REQUIRE(st);
    CHECK_FALSE(flag(*st->get("server_tab"), "can_start"));
    auto res = last_of(b.handle(R"({"cmd":"server_start"})"), "result");
    CHECK_FALSE(flag(*res, "ok"));
    CHECK(w.host.starts.empty());
    w.host.run_queued();
    st = last_of(w.host.async_out, "state");
    CHECK(flag(*st->get("server_tab"), "can_start"));
}

TEST_CASE("app server bridge: the server shown cannot be taken off a list without a game to show instead") {
    World w;
    w.host.dir = w.server.dir;  // no game in the list
    Bridge b(w.host);
    b.handle(R"({"cmd":"state"})");
    auto res = last_of(b.handle(R"({"cmd":"folder_forget","path":)" + q(w.server.dir) + "}"), "result");
    CHECK_FALSE(flag(*res, "ok"));
    CHECK(read_folder_list(w.app.at(L"folders.json")).folders.size() == 1);
}

// ---- edge cases ----

TEST_CASE("app server bridge: Install EMF and a mod install never run on the folder at once (one busy gate both ways)") {
    World w;
    w.host.dir = w.server.dir;
    w.host.testing = true;  // install_path names the archive
    w.host.package = EmfPackage{true, "", L"C:\\pkg\\setup", L"C:\\pkg\\game", L"", "0.3.0"};
    w.host.install_answer.ok = true;
    Bridge b(w.host);
    b.handle(R"({"cmd":"state"})");
    w.host.defer = true;
    b.handle(R"({"cmd":"server_install_emf"})");  // EMF is being installed
    auto res = last_of(b.handle(R"({"cmd":"install_path","path":"C:\\Downloads\\mod.zip"})"), "result");
    REQUIRE(res);
    CHECK_FALSE(flag(*res, "ok"));
    CHECK(text(*res, "error").find("Wait") != std::string::npos);
    w.host.run_queued();
    w.host.async_out.clear();
    // the other way: a mod archive being unpacked (its plan on a worker), then Install EMF
    TempGame zipdir(L"sb_zip", false);
    zipdir.write(L"mod.zip", "PK not really");
    b.handle(R"({"cmd":"install_path","path":)" + q(zipdir.at(L"mod.zip")) + "}");
    res = last_of(b.handle(R"({"cmd":"server_install_emf"})"), "result");
    REQUIRE(res);
    CHECK_FALSE(flag(*res, "ok"));
    CHECK(w.host.installs.size() == 1);
    w.host.run_queued();
}

TEST_CASE("app server bridge: passwords seen this session stay stripped from the logs after the config changes") {
    World w;
    w.server.write(L"enshrouded_server.json", R"({"name": "S", "userGroups": [{"name": "A", "password": "OldSecretOne"}]})");
    w.server.write(L"logs\\enshrouded_server.log", "[I 00:00:01] typed OldSecretOne\r\n");
    w.host.dir = w.server.dir;
    Bridge b(w.host);
    b.handle(R"({"cmd":"state"})");
    b.handle(R"({"cmd":"server_logs"})");
    w.server.write(L"enshrouded_server.json", R"({"name": "S", "userGroups": [{"name": "A", "password": "NewSecretTwo"}]})");
    w.server.write(L"logs\\enshrouded_server.log",
                   "[I 00:00:01] typed OldSecretOne\r\n[I 00:00:02] typed NewSecretTwo\r\n[I 00:00:03] typed Gl\xFC" "ckspiel24\r\n");
    w.server.write(L"enshrouded_server.json",
                   "{\"name\": \"S\", \"userGroups\": [{\"name\": \"A\", \"password\": \"NewSecretTwo\"}, "
                   "{\"name\": \"B\", \"password\": \"Gl\xC3\xBC" "ckspiel24\"}]}");
    b.handle(R"({"cmd":"server_logs"})");  // read on a worker
    auto logs = last_of(w.host.async_out, "server_logs");
    REQUIRE(logs);
    const std::string server = text(*logs, "server");
    CHECK(server.find("OldSecretOne") == std::string::npos);  // no longer in the file, still stripped
    CHECK(server.find("NewSecretTwo") == std::string::npos);
    CHECK(server.find("ckspiel24") == std::string::npos);  // written in the ANSI code page
    CHECK(server.find("typed ******") != std::string::npos);
}

TEST_CASE("app server bridge: Add as admin drops entries the core skips and refuses a list longer than it reads") {
    World w;
    std::string admins;
    for (int i = 0; i < 102; ++i) admins += std::string(i ? ", " : "") + "0x0123456789ab" + (i < 10 ? "000" : i < 100 ? "00" : "0") + std::to_string(i);
    REQUIRE(admins.size() == 2038);  // 102 accounts: one more does not fit the 2047 characters the core reads
    w.server.write(L"emf\\emf_core.ini", "[chat]\r\nadmins=76561198012345678901, " + admins + "\r\n");
    w.host.dir = w.server.dir;
    Bridge b(w.host);
    b.handle(R"({"cmd":"state"})");
    auto res = last_of(b.handle(R"({"cmd":"server_admin_add","account":"0xfedcba9876543210"})"), "result");
    REQUIRE(res);
    CHECK_FALSE(flag(*res, "ok"));
    CHECK(text(*res, "error").find("2047") != std::string::npos);
    // a short list: the invalid entry goes, the new one is added
    w.server.write(L"emf\\emf_core.ini", "[chat]\r\nadmins=76561198012345678901, 0x1\r\n");
    res = last_of(b.handle(R"({"cmd":"server_admin_add","account":"0xfedcba9876543210"})"), "result");
    CHECK(flag(*res, "ok"));
    CHECK(read_chat_settings(w.server.dir).admins == std::vector<std::string>{"0x1", "0xfedcba9876543210"});
}

TEST_CASE("app server bridge: when other programs share the server's console, Stop asks before it sends Ctrl+C") {
    World w;
    w.host.dir = w.server.dir;
    Bridge b(w.host);
    b.handle(R"({"cmd":"state"})");
    b.handle(R"({"cmd":"server_start"})");
    w.host.stop_answer = {false, "shared_console", "Another program shares this server's console window. Send it anyway?"};
    b.handle(R"({"cmd":"server_stop"})");
    auto ask = last_of(w.host.async_out, "server_stop");
    REQUIRE(ask);
    CHECK(text(*ask, "state") == "ask");
    CHECK_FALSE(w.host.last_shared_ok);
    auto st = last_of(w.host.async_out, "state");
    CHECK_FALSE(flag(*st->get("server_tab"), "stop_failed"));  // nothing was tried: no Stop now
    w.host.stop_answer = {true, "clean", "The server stopped cleanly."};
    b.handle(R"({"cmd":"server_stop","shared_ok":true})");
    CHECK(w.host.last_shared_ok);
    CHECK(text(*last_of(w.host.async_out, "server_stop"), "state") == "done");
}

TEST_CASE("app server bridge: Install EMF lists the mods it brings and switches a client mod off on a fresh server") {
    World w;
    w.host.dir = w.server.dir;
    w.host.package = EmfPackage{true, "", L"C:\\pkg\\setup", L"C:\\pkg\\game", L"", "0.3.0",
                                {{"clock", "Clock", "client", true}, {"loot", "Loot", "both", false}}};
    w.host.install_answer.ok = true;
    w.host.install_writes = {{L"emf\\mods\\clock\\mod.json", R"({"id":"clock","name":"Clock","version":"1.1.0","side":"client"})"},
                             {L"emf\\mods\\clock\\clock.dll", "dll"}};
    Bridge b(w.host);
    auto st = parse(b.handle(R"({"cmd":"state"})")[0]);
    const json::Value* mods = st.get("server_tab")->get("package")->get("mods");
    REQUIRE(mods);
    REQUIRE(mods->arr.size() == 2);
    CHECK(text(mods->arr[0], "side") == "client");
    CHECK(flag(mods->arr[0], "on"));
    b.handle(R"({"cmd":"server_install_emf"})");
    auto done = last_of(w.host.async_out, "server_install");
    REQUIRE(done);
    std::string lines;
    for (const auto& l : done->get("lines")->arr) lines += l.s + "\n";
    CHECK(lines.find("Clock") != std::string::npos);
    CHECK(w.server.read(L"emf\\config\\clock.ini").find("enabled=0") != std::string::npos);
    // a server whose owner set the mod already: left as it is
    w.server.write(L"emf\\config\\clock.ini", "[clock]\r\nenabled=1\r\n");
    b.handle(R"({"cmd":"server_install_emf"})");
    CHECK(w.server.read(L"emf\\config\\clock.ini").find("enabled=1") != std::string::npos);
}

TEST_CASE("app server bridge: the Server tab's files are read on a worker after the first look (a slow drive never holds the window)") {
    World w;
    w.host.dir = w.server.dir;
    Bridge b(w.host);
    auto st = parse(b.handle(R"({"cmd":"state"})")[0]);
    CHECK(text(*st.get("server_tab")->get("config"), "name") == "Ember Test Server");  // the first look reads at once
    w.server.write(L"enshrouded_server.json", R"({"name": "Renamed Server"})");
    w.host.defer = true;
    b.snapshot_age_for_test(10000);  // as if the last read were 10 s old
    st = parse(b.handle(R"({"cmd":"state"})")[0]);
    CHECK(text(*st.get("server_tab")->get("config"), "name") == "Ember Test Server");  // what was read before
    CHECK(w.host.queued.size() == 1);  // the read waits on a worker
    w.host.run_queued();
    st = parse(b.handle(R"({"cmd":"state"})")[0]);
    CHECK(text(*st.get("server_tab")->get("config"), "name") == "Renamed Server");
}

TEST_CASE("app server bridge: a profile switch waits for Install EMF too (the same busy gate)") {
    World w;
    w.host.dir = w.server.dir;
    w.host.package = EmfPackage{true, "", L"C:/pkg/setup", L"C:/pkg/game", L"", "0.3.0"};
    w.host.install_answer.ok = true;
    Bridge b(w.host);
    b.handle(R"({"cmd":"state"})");
    w.host.defer = true;
    b.handle(R"({"cmd":"server_install_emf"})");
    auto res = last_of(b.handle(R"({"cmd":"profile_switch","slug":"anything"})"), "result");
    REQUIRE(res);
    CHECK_FALSE(flag(*res, "ok"));
    CHECK(text(*res, "error").find("busy") != std::string::npos);
    w.host.run_queued();
}
