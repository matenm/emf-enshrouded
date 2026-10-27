// EMF.exe's Saves tab through the page bridge (app/bridge_saves.cpp) against a fake PC: the list with sizes, the starter
// backup at the first start, Back up now, Restore (a single Local file; refused while a game runs), Delete, the
// automatic backup settings, and why saves are busy.
#include <doctest.h>

#include <string>
#include <vector>

#include "app/bridge.h"
#include "app_test_files.h"
#include "core/json.h"
#include "save_test_files.h"
#include "saves/backup.h"

using namespace emf::app;
using app_test::TempGame;
using save_test::FakePc;
namespace json = emf::core::json;

namespace {

struct SavesHost : BridgeHost {
    std::wstring dir;
    emf::saves::SaveRoots roots;
    std::vector<ProcessInfo> procs;
    std::vector<std::pair<std::wstring, Open>> opened;
    bool defer = false;
    std::vector<std::function<Done()>> queued;
    std::vector<std::string> async_out;

    std::wstring game() const override { return dir; }
    void choose_game(const std::wstring& d) override { dir = d; }
    std::optional<std::wstring> pick_folder() override { return std::nullopt; }
    bool open(const std::wstring& target, Open how) override {
        opened.emplace_back(target, how);
        return true;
    }
    std::string running_reason() override { return {}; }
    void rendered(int, const std::vector<std::string>&) override {}
    std::wstring steam_game() override { return {}; }
    bool copy_text(const std::string&) override { return true; }
    std::optional<std::wstring> pick_archive() override { return std::nullopt; }
    void run_async(std::function<Done()> work) override {
        if (defer) {
            queued.push_back(std::move(work));
            return;
        }
        if (Done d = work())
            for (auto& m : d()) async_out.push_back(m);
    }
    void run_queued() {
        auto q = std::move(queued);
        queued.clear();
        for (auto& w : q)
            if (Done d = w())
                for (auto& m : d()) async_out.push_back(m);
    }
    std::vector<ProcessInfo> processes() override { return procs; }
    emf::saves::SaveRoots save_roots() override { return roots; }
    // stands in for EMF.exe's own rule (a smoke run counts only a game running from the folder shown)
    std::optional<std::string> busy;
    std::string saves_busy_for(const std::wstring& g) override { return busy ? *busy : BridgeHost::saves_busy_for(g); }
};

json::Value parse(const std::string& text) {
    auto r = json::parse(text);
    REQUIRE_MESSAGE(r.value, text);
    return *r.value;
}

std::optional<json::Value> of_type(const std::vector<std::string>& replies, const char* type) {
    for (const auto& r : replies) {
        json::Value v = parse(r);
        if (v.get("type") && v.get("type")->s == type) return v;
    }
    return std::nullopt;
}

void emf_game(const TempGame& g) {
    g.write(L"dinput8.dll", "EMF-LOADER-7d3c9f");
    g.write(L"emf\\emf_core.dll", std::string("emf_core 1.2.3\0", 15));
}

}  // namespace

TEST_CASE("app saves: why saves are busy") {
    const std::wstring game = L"C:\\Games\\Enshrouded";
    CHECK(saves_busy_reason({}, game).empty());
    CHECK(saves_busy_reason({{L"enshrouded.exe", L"D:\\Other\\enshrouded.exe", 12}}, game).find("Enshrouded is running") !=
          std::string::npos);  // any folder: the saves are the player's
    CHECK(saves_busy_reason({{L"enshrouded.exe", L"", 12}}, game).find("process 12") != std::string::npos);
    CHECK(saves_busy_reason({{L"enshrouded_server.exe", L"C:\\Games\\Enshrouded\\enshrouded_server.exe", 7}}, game)
              .find("dedicated server") != std::string::npos);
    CHECK(saves_busy_reason({{L"enshrouded_server.exe", L"D:\\Server\\enshrouded_server.exe", 7}}, game).empty());
}

TEST_CASE("app saves: the list, the starter backup at the first start, Back up now, Delete") {
    TempGame g(L"saves_tab");
    emf_game(g);
    FakePc pc(L"app");
    pc.usual();
    SavesHost host;
    host.dir = g.dir;
    host.roots = pc.roots;
    Bridge bridge(host);

    // the first state: the starter backup is made (on the worker) and announced once
    auto out = bridge.handle(R"({"cmd":"state"})");
    auto state = of_type(out, "state");
    REQUIRE(state);
    CHECK(state->get("saves_busy")->s.empty());
    auto note = of_type(host.async_out, "saves_note");
    REQUIRE(note);
    CHECK(note->get("ok")->b);
    CHECK(note->get("text")->s.find("starter backup") != std::string::npos);
    host.async_out.clear();
    bridge.handle(R"({"cmd":"state"})");
    CHECK_FALSE(of_type(host.async_out, "saves_note"));

    out = bridge.handle(R"({"cmd":"saves_list"})");
    auto saves = of_type(out, "saves");
    REQUIRE(saves);
    CHECK(saves->get("available")->b);
    REQUIRE(saves->get("list")->arr.size() == 1);
    const auto& starter = saves->get("list")->arr[0];
    CHECK(starter.get("kind")->s == "starter");
    CHECK(starter.get("label")->s == "Before EMF");
    CHECK(starter.get("made_by")->s == "EMF.exe");
    CHECK(starter.get("files")->arr.size() == 8);
    CHECK(starter.get("summary")->s == "Steam: character slot 1, world slot 1 \xC2\xB7 Local: character slot 1, character slot 2, world slot 1");
    CHECK(starter.get("bytes")->n > 0);
    CHECK(saves->get("total_bytes")->n == starter.get("bytes")->n);
    CHECK(saves->get("count")->n == 1);
    CHECK(saves->get("now_files")->n == 8);
    CHECK(saves->get("auto")->get("available")->b);
    CHECK(saves->get("auto")->get("on")->b);
    CHECK(saves->get("auto")->get("keep")->n == 10);

    // Back up now, with a label
    out = bridge.handle(R"({"cmd":"saves_backup","label":"  before the boss\n"})");
    auto r = of_type(host.async_out, "result");
    REQUIRE(r);
    CHECK(r->get("ok")->b);
    CHECK(r->get("text")->s.find("Backed up 8 save files") != std::string::npos);
    saves = of_type(host.async_out, "saves");
    REQUIRE(saves);
    REQUIRE(saves->get("list")->arr.size() == 2);
    CHECK(saves->get("list")->arr[0].get("kind")->s == "manual");
    CHECK(saves->get("list")->arr[0].get("label")->s == "before the boss");
    host.async_out.clear();

    // refused while the game runs, from any folder
    host.procs = {{L"enshrouded.exe", L"Z:\\elsewhere\\enshrouded.exe", 42}};
    out = bridge.handle(R"({"cmd":"saves_backup","label":"x"})");
    r = of_type(out, "result");
    REQUIRE(r);
    CHECK_FALSE(r->get("ok")->b);
    CHECK(r->get("error")->s.find("Enshrouded is running") != std::string::npos);
    CHECK(emf::saves::list_backups(pc.roots.store).size() == 2);
    state = of_type(bridge.handle(R"({"cmd":"state"})"), "state");
    CHECK(state->get("saves_busy")->s.find("Enshrouded is running") != std::string::npos);
    host.procs.clear();

    // Delete (on the worker: the fake host runs it at once, into async_out)
    const std::string id = emf::saves::list_backups(pc.roots.store)[0].id;
    host.async_out.clear();
    bridge.handle(R"({"cmd":"saves_delete","id":")" + id + "\"}");
    r = of_type(host.async_out, "result");
    REQUIRE(r);
    CHECK(r->get("ok")->b);
    CHECK(emf::saves::list_backups(pc.roots.store).size() == 1);
    out = bridge.handle(R"({"cmd":"saves_delete","id":"..\\.."})");
    CHECK_FALSE(of_type(out, "result")->get("ok")->b);

    // Open folder: the store, or one backup's folder
    bridge.handle(R"({"cmd":"saves_open"})");
    REQUIRE(host.opened.size() == 1);
    CHECK(host.opened[0].first == pc.roots.store);
    bridge.handle(R"({"cmd":"saves_open","id":")" + emf::saves::list_backups(pc.roots.store)[0].id + "\"}");
    REQUIRE(host.opened.size() == 2);
    CHECK(host.opened[1].first.find(L"_starter") != std::wstring::npos);
}

TEST_CASE("app saves: restoring one Local file, and the refusal while a game runs") {
    TempGame g(L"saves_restore");
    emf_game(g);
    FakePc pc(L"app-restore");
    pc.usual();
    SavesHost host;
    host.dir = g.dir;
    host.roots = pc.roots;
    Bridge bridge(host);
    REQUIRE(bridge.handle(R"({"cmd":"saves_backup"})").empty());
    const std::string id = emf::saves::list_backups(pc.roots.store)[0].id;
    FakePc::write(pc.local_default() + L"\\world0.ksd", "ruined");
    host.async_out.clear();

    host.procs = {{L"enshrouded.exe", L"", 9}};
    auto out = bridge.handle(R"({"cmd":"saves_restore","id":")" + id + R"(","paths":["local/default/world0.ksd"]})");
    auto done = of_type(out, "saves_restored");
    REQUIRE(done);
    CHECK_FALSE(done->get("ok")->b);
    CHECK(done->get("error")->s.find("closed") != std::string::npos);
    CHECK(FakePc::read(pc.local_default() + L"\\world0.ksd") == "ruined");

    host.procs.clear();
    out = bridge.handle(R"({"cmd":"saves_restore","id":")" + id + R"(","paths":["local/default/world0.ksd"]})");
    done = of_type(host.async_out, "saves_restored");
    REQUIRE(done);
    CHECK_MESSAGE(done->get("ok")->b, done->get("error")->s);
    CHECK_FALSE(done->get("steam")->b);
    CHECK(done->get("restored")->arr.size() == 1);
    CHECK_FALSE(done->get("before_id")->s.empty());
    CHECK(FakePc::read(pc.local_default() + L"\\world0.ksd") == "local world 0");
    const auto list = of_type(host.async_out, "saves");
    REQUIRE(list);
    CHECK(list->get("list")->arr.size() == 2);  // the backup and the before-restore one
    CHECK(list->get("list")->arr[0].get("kind")->s == "before-restore");
}

TEST_CASE("app saves: the restore checks again with the host's own rule, on its worker") {
    TempGame g(L"saves_rule");
    emf_game(g);
    FakePc pc(L"app-rule");
    pc.usual();
    SavesHost host;
    host.dir = g.dir;
    host.roots = pc.roots;
    Bridge bridge(host);
    bridge.handle(R"({"cmd":"saves_backup"})");
    const std::string id = emf::saves::list_backups(pc.roots.store)[0].id;
    FakePc::write(pc.local_default() + L"\\world0.ksd", "later");
    host.async_out.clear();
    // a game runs elsewhere, but the host's rule says the saves are free (as a smoke run says for the player's game)
    host.procs = {{L"enshrouded.exe", L"Z:\\Elsewhere\\enshrouded.exe", 5}};
    host.busy = std::string();
    bridge.handle(R"({"cmd":"saves_restore","id":")" + id + R"(","paths":["local/default/world0.ksd"]})");
    auto done = of_type(host.async_out, "saves_restored");
    REQUIRE(done);
    CHECK_MESSAGE(done->get("ok")->b, done->get("error")->s);
    // and the other way round: the rule says busy, so the worker refuses even when the first check let it through
    FakePc::write(pc.local_default() + L"\\world0.ksd", "later again");
    host.async_out.clear();
    host.defer = true;
    bridge.handle(R"({"cmd":"saves_restore","id":")" + id + R"(","paths":["local/default/world0.ksd"]})");
    host.busy = std::string("the game started meanwhile");
    host.run_queued();
    done = of_type(host.async_out, "saves_restored");
    REQUIRE(done);
    CHECK_FALSE(done->get("ok")->b);
    CHECK(done->get("error")->s.find("started meanwhile") != std::string::npos);
    CHECK(FakePc::read(pc.local_default() + L"\\world0.ksd") == "later again");
}

TEST_CASE("app saves: deleting the starter needs the dialog's confirmation, and runs on the worker") {
    TempGame g(L"saves_delete");
    emf_game(g);
    FakePc pc(L"app-delete");
    pc.usual();
    SavesHost host;
    host.dir = g.dir;
    host.roots = pc.roots;
    Bridge bridge(host);
    bridge.handle(R"({"cmd":"state"})");  // the starter, at the first start
    REQUIRE(emf::saves::list_backups(pc.roots.store).size() == 1);
    const std::string id = emf::saves::list_backups(pc.roots.store)[0].id;
    auto r = of_type(bridge.handle(R"({"cmd":"saves_delete","id":")" + id + "\"}"), "result");
    REQUIRE(r);
    CHECK_FALSE(r->get("ok")->b);
    CHECK(r->get("error")->s.find("before EMF") != std::string::npos);
    CHECK(emf::saves::list_backups(pc.roots.store).size() == 1);
    host.defer = true;
    host.async_out.clear();
    auto out = bridge.handle(R"({"cmd":"saves_delete","id":")" + id + R"(","starter":true})");
    CHECK_FALSE(of_type(out, "result"));  // nothing yet: the delete runs on the worker
    CHECK(bridge.changing());
    host.run_queued();
    r = of_type(host.async_out, "result");
    REQUIRE(r);
    CHECK(r->get("ok")->b);
    CHECK(emf::saves::list_backups(pc.roots.store).empty());
}

TEST_CASE("app saves: a backup under way keeps the window open and refuses a second one") {
    TempGame g(L"saves_busy");
    emf_game(g);
    FakePc pc(L"app-busy");
    pc.usual();
    SavesHost host;
    host.dir = g.dir;
    host.roots = pc.roots;
    host.defer = true;
    Bridge bridge(host);
    bridge.handle(R"({"cmd":"saves_backup"})");
    CHECK(bridge.changing());
    auto r = of_type(bridge.handle(R"({"cmd":"saves_backup"})"), "result");
    REQUIRE(r);
    CHECK(r->get("error")->s.find("busy") != std::string::npos);
    host.run_queued();
    CHECK_FALSE(bridge.changing());
    CHECK(emf::saves::list_backups(pc.roots.store).size() == 1);
}

TEST_CASE("app saves: the automatic backup settings are written to emf_core.ini") {
    TempGame g(L"saves_auto");
    emf_game(g);
    FakePc pc(L"app-auto");
    SavesHost host;
    host.dir = g.dir;
    host.roots = pc.roots;
    Bridge bridge(host);
    bridge.handle(R"({"cmd":"state"})");
    auto out = bridge.handle(R"({"cmd":"saves_auto","on":false,"keep":4})");
    CHECK(of_type(out, "result")->get("ok")->b);
    const auto a = emf::saves::read_auto_settings(g.at(L"emf\\emf_core.ini"));
    CHECK_FALSE(a.on);
    CHECK(a.keep == 4);
    const auto saves = of_type(out, "saves");
    REQUIRE(saves);
    CHECK_FALSE(saves->get("auto")->get("on")->b);
    // no starter without saves, and nothing announced
    CHECK_FALSE(of_type(host.async_out, "saves_note"));
    // a folder without EMF: refused
    TempGame bare(L"saves_bare");
    host.dir = bare.dir;
    bridge.handle(R"({"cmd":"state"})");
    CHECK_FALSE(of_type(bridge.handle(R"({"cmd":"saves_auto","on":true})"), "result")->get("ok")->b);
}

TEST_CASE("app saves: no store, no backups, a plain answer") {
    TempGame g(L"saves_none");
    SavesHost host;
    host.dir = g.dir;
    Bridge bridge(host);
    bridge.handle(R"({"cmd":"state"})");
    auto saves = of_type(bridge.handle(R"({"cmd":"saves_list"})"), "saves");
    REQUIRE(saves);
    CHECK_FALSE(saves->get("available")->b);
    CHECK(saves->get("list")->arr.empty());
    CHECK_FALSE(of_type(bridge.handle(R"({"cmd":"saves_backup"})"), "result")->get("ok")->b);
}
