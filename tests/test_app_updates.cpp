// "Check for updates" in EMF.exe (app/updates.h and the bridge): the links the player sets by hand
// (emf\config\nexus_links.json), which installed mods are checked and how, the page's message from a recorded Nexus
// reply, and the flows through the bridge: the check only on a click, one batched request, a Premium update downloaded
// and planned as an update of the same install (also for an install the player linked by hand), and a Mod Manager
// Download link that comes back for it. A fake host and a fake Nexus; no network.
#include <doctest.h>

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "app/account_store.h"
#include "app/bridge.h"
#include "app/browse.h"
#include "app/text.h"
#include "app/updates.h"
#include "app_archive_files.h"
#include "app_test_files.h"
#include "core/json.h"

using namespace emf::app;
using app_test::TempGame;
namespace json = emf::core::json;

namespace {

std::string fixture(const char* name) {
    std::ifstream f(std::string(EMF_SOURCE_DIR) + "/tests/fixtures/nexus/" + name, std::ios::binary);
    REQUIRE_MESSAGE(f.good(), name);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

json::Value parse(const std::string& text) {
    auto r = json::parse(text);
    REQUIRE_MESSAGE(r.value, text);
    return *r.value;
}

std::vector<json::Value> of_type(const std::vector<std::string>& msgs, const std::string& type) {
    std::vector<json::Value> out;
    for (const auto& m : msgs) {
        json::Value v = parse(m);
        if (v.get("type") && v.get("type")->s == type) out.push_back(v);
    }
    return out;
}

const json::Value* item_for(const json::Value& reply, int64_t nexus_id, const std::string& install_key = "") {
    for (const auto& it : reply.get("items")->arr)
        if (it.get("nexus_id")->n == static_cast<double>(nexus_id) && (install_key.empty() || it.get("install_key")->s == install_key))
            return &it;
    return nullptr;
}

std::vector<std::string> strings(const json::Value* v) {
    std::vector<std::string> out;
    if (v)
        for (const auto& x : v->arr) out.push_back(x.s);
    return out;
}

// An install record as EMF writes it (only the fields the check reads, and the mod folder it installed).
void record(const TempGame& g, const std::string& key, const std::string& nexus_id, const std::string& file_id,
            const std::string& version, const std::string& archive, const std::string& root, const std::string& chain = "") {
    std::string roots = root.empty() ? "[]" : "[\"" + root + "\"]";
    for (size_t at = 0; (at = roots.find('\\', at)) != std::string::npos; at += 2) roots.insert(at, "\\");
    g.write(L"emf\\installed_mods\\" + wide(key) + L".json",
            "{\"key\":\"" + key + "\",\"name\":\"" + key + "\",\"version\":\"" + version + "\",\"nexus_id\":\"" + nexus_id +
                "\",\"nexus_file_id\":\"" + file_id + "\",\"archive\":\"" + archive + "\",\"installed_at\":\"2026-10-20 10:00:00\"," +
                "\"roots\":" + roots + ",\"chain\":" + (chain.empty() ? "[]" : "[\"" + chain + "\"]") + ",\"files\":[]}\n");
}

// The test folder: EMF, an install of Auto Loot Plus (file 1131, archived on Nexus now) with its EML part and its DLL,
// Quick Deposit with nexus_id in its mod.json, a DLL started by EMF and an EML mod EMF installed without a Nexus id
// (both linked by hand), and an EMF mod without any id.
void make_folder(const TempGame& g) {
    g.write(L"dinput8.dll", "EMF-LOADER-7d3c9f fake");
    g.write(L"emf\\emf_core.dll", std::string("emf_core 0.2.0") + '\0');
    g.write(L"mods\\auto-loot-plus\\mod.json", R"({"id":"auto-loot-plus","name":"Auto Loot Plus","version":"1.1.7"})");
    g.write(L"autolootplus.dll", "MZ stand-in");
    g.write(L"dinput8-chain.ini", "[Chain]\r\nautolootplus.dll\r\ncreative.dll\r\n");
    g.write(L"creative.dll", "MZ creative");
    record(g, "nexus-170", "170", "1131", "1.1.7", "AutoLootPlus 1.1.7-170-1-1-7-1792707900.zip", "mods\\auto-loot-plus",
           "autolootplus.dll");
    g.write(L"mods\\quick-deposit\\mod.json", R"({"id":"quick-deposit","name":"Quick Deposit","version":"1.3.0","nexus_id":195})");
    g.write(L"mods\\shroud-dig\\mod.json", R"({"id":"shroud-dig","name":"Shroud Dig","version":"1.0"})");
    record(g, "shroud-dig", "", "", "1.0", "ShroudDig - Standalone version.zip", "mods\\shroud-dig");
    g.write(L"emf\\mods\\clock\\mod.json", R"({"id":"clock","name":"Clock","version":"1.0.0","emf":1,"dll":"clock.dll"})");
    g.write(L"emf\\mods\\clock\\clock.dll", "fake");
}

struct FakeHost : BridgeHost {
    std::wstring dir, data;
    std::vector<std::pair<std::wstring, Open>> opened;
    std::vector<std::string> async_out;
    bool defer = false;       // work waits for run_queued
    bool defer_done = false;  // work runs at once, its answer (Done) waits for run_dones: a race on the UI thread
    std::vector<std::function<Done()>> queued;
    std::vector<Done> dones;
    int64_t emf_id = 0;
    int64_t now = 1792790000;
    std::vector<emf::nexus::HttpRequest> graphql;  // the update check's requests
    std::string graphql_reply = fixture("updates_reply.json");
    std::string changelogs_reply = fixture("changelogs_reply.json");
    std::vector<emf::nexus::HttpRequest> api;
    std::string file_reply, cdn_body;

    std::wstring game() const override { return dir; }
    void choose_game(const std::wstring& d) override { dir = d; }
    std::optional<std::wstring> picked;  // what the folder picker returns (nothing: cancelled)
    std::optional<std::wstring> pick_folder() override { return picked; }
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
        if (defer) queued.push_back(std::move(work));
        else if (defer_done) dones.push_back(work());
        else finish(work());
    }
    void run_dones() {
        auto d = std::move(dones);
        dones.clear();
        for (auto& x : d) finish(x);
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
    void run_on_ui(Done done) override { finish(done); }
    std::function<void(Done)> ui_poster() override {
        return [this](Done done) { finish(done); };
    }
    std::wstring app_data() const override { return data; }
    int64_t now_unix() const override { return now; }
    int64_t emf_nexus_id() const override { return emf_id; }
    bool personal_key_allowed() const override { return true; }  // the tester switch: the check asks (the fake) Nexus
    emf::nexus::Client::Transport nexus_transport() override {
        return [this](const emf::nexus::HttpRequest& r) {
            graphql.push_back(r);
            const bool logs = r.body.find("EmfChangelogs") != std::string::npos;
            return emf::nexus::HttpResponse{200, logs ? changelogs_reply : graphql_reply, ""};
        };
    }
    emf::nexus::V1Client::Transport nexus_v1_transport() override {
        return [this](const emf::nexus::HttpRequest& r) {
            api.push_back(r);
            emf::nexus::HttpResponse resp;
            resp.headers = {{"X-RL-Daily-Remaining", "100"}, {"X-RL-Hourly-Remaining", "50"}};
            resp.status = 200;
            resp.body = r.url.find("download_link.json") != std::string::npos
                            ? R"([{"name":"Nexus CDN","short_name":"Nexus CDN","URI":"https://cf-files.nexusmods.com/cdn/5811/x/file.zip"}])"
                            : file_reply;
            return resp;
        };
    }
    emf::nexus::Downloader::Fetch nexus_fetch() override {
        return [this](const emf::nexus::StreamRequest&, const emf::nexus::StreamSink& sink) {
            emf::nexus::StreamResponse out;
            out.status = 200;
            if (sink.start && !sink.start(cdn_body.size(), true)) {
                out.failure = emf::nexus::HttpFailure::Stopped;
                return out;
            }
            out.bytes = cdn_body.size();
            if (sink.write && !sink.write(cdn_body.data(), cdn_body.size())) out.failure = emf::nexus::HttpFailure::Stopped;
            return out;
        };
    }
};

struct Setup {
    TempGame g{L"updates_game"};
    TempGame appdata{L"updates_appdata", false};
    FakeHost host;
    Setup() {
        make_folder(g);
        host.dir = g.dir;
        host.data = appdata.dir;
    }
    void account(bool premium) {
        StoredAccount a;
        a.key = "aGVsbG8=--d29ybGQ=--a2V5";
        a.user_id = 12345;
        a.name = "EmberTester";
        a.premium = premium;
        REQUIRE(save_account(appdata.at(L"nexus.key"), a));
    }
    // A real zip of an EML mod the fake CDN sends, with the file reply naming it.
    void serve_zip(const std::string& folder, const std::string& version, const std::string& name, int64_t file_id) {
        TempGame src(L"updates_zip_src", false), keep(L"updates_zip", false);
        src.write(wide("mods\\" + folder + "\\mod.json"), "{\"id\":\"" + folder + "\",\"name\":\"" + folder + "\",\"version\":\"" + version + "\"}");
        src.write(wide("mods\\" + folder + "\\src\\mod.lua"), "-- lua " + version);
        const std::wstring zip = keep.at(L"m.zip");
        REQUIRE(app_test::make_zip(src.dir, zip));
        std::ifstream f(std::filesystem::path(zip), std::ios::binary);
        host.cdn_body.assign(std::istreambuf_iterator<char>(f), {});
        host.file_reply = "{\"file_id\":" + std::to_string(file_id) + ",\"name\":\"" + folder + "\",\"version\":\"" + version +
                          "\",\"category_name\":\"MAIN\",\"file_name\":\"" + name + "\",\"size_in_bytes\":" +
                          std::to_string(host.cdn_body.size()) + "}";
    }
};

json::Value check(Bridge& bridge, Setup& s) {
    s.host.async_out.clear();
    const auto now = bridge.handle(R"({"cmd":"updates_check","seq":7})");
    std::vector<std::string> all = now;
    all.insert(all.end(), s.host.async_out.begin(), s.host.async_out.end());
    const auto replies = of_type(all, "updates");
    REQUIRE(replies.size() == 1);
    return replies[0];
}

}  // namespace

TEST_CASE("app updates: the Nexus links the player sets are kept in emf\\config\\nexus_links.json") {
    TempGame g(L"updates_links");
    CHECK(read_nexus_links(g.dir).empty());
    CHECK(nexus_links_path(g.dir) == g.at(L"emf\\config\\nexus_links.json"));
    REQUIRE(write_nexus_link(g.dir, "chain:creative.dll", 182).ok);
    REQUIRE(write_nexus_link(g.dir, "eml:foo", 195).ok);
    auto links = read_nexus_links(g.dir);
    REQUIRE(links.size() == 2);
    CHECK(links["chain:creative.dll"] == 182);
    CHECK(links["eml:foo"] == 195);
    const auto doc = parse(g.read(L"emf\\config\\nexus_links.json"));
    CHECK(doc.get("links")->get("eml:foo")->n == 195);
    // changed, then removed
    REQUIRE(write_nexus_link(g.dir, "eml:foo", 23).ok);
    CHECK(read_nexus_links(g.dir)["eml:foo"] == 23);
    REQUIRE(write_nexus_link(g.dir, "eml:foo", 0).ok);
    links = read_nexus_links(g.dir);
    CHECK(links.size() == 1);
    CHECK(links.count("eml:foo") == 0);
    // what a hand-edited file may hold that is not a link is left out
    g.write(L"emf\\config\\nexus_links.json",
            R"({"links":{"eml:ok":170,"eml:text":"195","nope:x":5,"eml:":5,"emf:neg":-1,"emf:big":1e13,"chain:zero":0,"emf:list":[1]}})");
    links = read_nexus_links(g.dir);
    CHECK(links.size() == 2);
    CHECK(links["eml:ok"] == 170);
    CHECK(links["eml:text"] == 195);
    g.write(L"emf\\config\\nexus_links.json", "not json");
    CHECK(read_nexus_links(g.dir).empty());
    CHECK_FALSE(write_nexus_link(g.dir, "bad", 1).ok);
    // the row names
    CHECK(link_row("eml", "foo") == "eml:foo");
    CHECK(link_row("chain", "Creative.dll") == "chain:Creative.dll");
    CHECK(link_row("web", "x").empty());
    CHECK(link_row("emf", "").empty());
    CHECK(link_row("emf", std::string(300, 'a')).empty());
}

TEST_CASE("app updates: a Nexus mod is given by its id or its page on nexusmods.com") {
    CHECK(parse_nexus_mod_link("170").mod_id == 170);
    CHECK(parse_nexus_mod_link("  170 ").mod_id == 170);
    CHECK(parse_nexus_mod_link("https://www.nexusmods.com/enshrouded/mods/170").mod_id == 170);
    CHECK(parse_nexus_mod_link("https://www.nexusmods.com/enshrouded/mods/170?tab=files&file_id=1197").mod_id == 170);
    CHECK(parse_nexus_mod_link("www.nexusmods.com/enshrouded/mods/170/").mod_id == 170);
    CHECK(parse_nexus_mod_link("nexusmods.com/Enshrouded/mods/195#description").mod_id == 195);
    CHECK(parse_nexus_mod_link("http://www.nexusmods.com/enshrouded/mods/23").mod_id == 23);
    const auto other = parse_nexus_mod_link("https://www.nexusmods.com/skyrimspecialedition/mods/266");
    CHECK(other.mod_id == 0);
    CHECK(other.error.find("skyrimspecialedition") != std::string::npos);
    for (const char* bad : {"", "abc", "0", "-5", "1234567890123", "https://example.com/enshrouded/mods/170",
                            "https://www.nexusmods.com.evil.example/enshrouded/mods/170", "https://www.nexusmods.com/enshrouded/mods/",
                            "https://www.nexusmods.com/enshrouded/collections/abc", "https://user@www.nexusmods.com/enshrouded/mods/1"}) {
        const auto r = parse_nexus_mod_link(bad);
        CHECK_MESSAGE(r.mod_id == 0, bad);
        CHECK_MESSAGE(!r.error.empty(), bad);
    }
}

TEST_CASE("app updates: which installed mods are checked, and with what") {
    Setup s;
    REQUIRE(write_nexus_link(s.g.dir, "chain:creative.dll", 182).ok);
    REQUIRE(write_nexus_link(s.g.dir, "eml:shroud-dig", 179).ok);
    REQUIRE(write_nexus_link(s.g.dir, "eml:quick-deposit", 999).ok);  // mod.json has an id: the player's link wins
    REQUIRE(write_nexus_link(s.g.dir, "eml:auto-loot-plus", 5).ok);   // the record has one: the record wins
    const FolderState st = scan_game_folder(s.g.dir);
    const auto targets = update_targets(st, read_install_records(s.g.dir), read_nexus_links(s.g.dir));
    REQUIRE(targets.size() == 4);  // the clock (no id) is not checked
    const auto find = [&](int64_t id) {
        const auto it = std::find_if(targets.begin(), targets.end(), [&](const UpdateTarget& t) { return t.nexus_id == id; });
        REQUIRE(it != targets.end());
        return *it;
    };
    const UpdateTarget alp = find(170);
    CHECK(alp.via == "record");
    CHECK(alp.install_key == "nexus-170");
    CHECK(alp.installed.file_id == 1131);
    CHECK(alp.installed.version == "1.1.7");
    CHECK(alp.installed.archive == "AutoLootPlus 1.1.7-170-1-1-7-1792707900.zip");
    std::vector<std::string> rows = alp.rows;
    std::sort(rows.begin(), rows.end());
    CHECK(rows == std::vector<std::string>{"chain:autolootplus.dll", "eml:auto-loot-plus"});
    const UpdateTarget qd = find(999);
    CHECK(qd.via == "link");
    CHECK(qd.installed.version == "1.3.0");
    CHECK(qd.rows == std::vector<std::string>{"eml:quick-deposit"});
    const UpdateTarget dig = find(179);
    CHECK(dig.via == "link");
    CHECK(dig.install_key == "shroud-dig");  // EMF installed it, without knowing its Nexus mod
    CHECK(dig.installed.archive == "ShroudDig - Standalone version.zip");
    const UpdateTarget creative = find(182);
    CHECK(creative.via == "link");
    CHECK(creative.install_key.empty());
    CHECK(creative.rows == std::vector<std::string>{"chain:creative.dll"});

    // without the link, mod.json's id
    REQUIRE(write_nexus_link(s.g.dir, "eml:quick-deposit", 0).ok);
    const auto again = update_targets(st, read_install_records(s.g.dir), read_nexus_links(s.g.dir));
    const auto q2 = std::find_if(again.begin(), again.end(), [](const UpdateTarget& t) { return t.nexus_id == 195; });
    REQUIRE(q2 != again.end());
    CHECK(q2->via == "mod.json");

    // an update of a linked install keeps that install's key; the usual nexus-<id> needs nothing
    const auto keys = update_keys(targets, read_install_records(s.g.dir));
    CHECK(keys.size() == 1);
    CHECK(keys.at("179").key == "shroud-dig");
    CHECK(keys.at("179").why == "you linked Shroud Dig to this Nexus mod");
}

TEST_CASE("app updates: a record whose files changed by hand since is compared by version, not by its file") {
    Setup s;
    s.g.write(L"mods\\auto-loot-plus\\mod.json", R"({"id":"auto-loot-plus","name":"Auto Loot Plus","version":"1.1.8"})");
    const FolderState st = scan_game_folder(s.g.dir);
    const auto targets = update_targets(st, read_install_records(s.g.dir), {});
    const auto it = std::find_if(targets.begin(), targets.end(), [](const UpdateTarget& t) { return t.nexus_id == 170; });
    REQUIRE(it != targets.end());
    CHECK(it->installed.file_id == 0);
    CHECK(it->installed.version == "1.1.8");
    CHECK(it->note.find("1.1.7") != std::string::npos);

    // the same version written another way is the same version: the file stays known
    s.g.write(L"mods\\auto-loot-plus\\mod.json", R"({"id":"auto-loot-plus","name":"Auto Loot Plus","version":"v1.1.7"})");
    const auto same = update_targets(scan_game_folder(s.g.dir), read_install_records(s.g.dir), {});
    const auto alp = std::find_if(same.begin(), same.end(), [](const UpdateTarget& t) { return t.nexus_id == 170; });
    REQUIRE(alp != same.end());
    CHECK(alp->installed.file_id == 1131);
    CHECK(alp->note.empty());
}

TEST_CASE("app updates: Check for updates asks Nexus once, only when clicked, and says what it found") {
    Setup s;
    REQUIRE(write_nexus_link(s.g.dir, "eml:shroud-dig", 179).ok);
    REQUIRE(write_nexus_link(s.g.dir, "chain:creative.dll", 23).ok);  // hidden on Nexus
    Bridge bridge(s.host);
    bridge.handle(R"({"cmd":"state"})");
    bridge.handle(R"({"cmd":"account"})");
    CHECK(s.host.graphql.empty());  // nothing at start-up

    const json::Value reply = check(bridge, s);
    REQUIRE(s.host.graphql.size() == 2);  // one request for every mod, one for the changelogs of the files to take
    const auto body = parse(s.host.graphql[0].body);
    std::vector<int64_t> asked;
    for (const auto& e : body.get("variables")->get("ids")->arr) asked.push_back(static_cast<int64_t>(e.get("modId")->n));
    std::sort(asked.begin(), asked.end());
    CHECK(asked == std::vector<int64_t>{23, 170, 179, 195});
    CHECK(reply.get("ok")->b);
    CHECK(reply.get("seq")->n == 7);
    CHECK(reply.get("requests")->n == 2);
    CHECK(s.host.graphql[1].body.find("EmfChangelogs") != std::string::npos);
    CHECK(s.host.graphql[1].body.find("24958054958253") != std::string::npos);  // Auto Loot Plus 1.1.8, the file to take

    const json::Value* alp = item_for(reply, 170);
    REQUIRE(alp);
    CHECK(alp->get("state")->s == "update");
    CHECK(alp->get("file")->get("file_id")->n == 1197);
    CHECK(alp->get("file")->get("name")->s == "AutoLootPlus 1.1.8");
    CHECK(alp->get("file")->get("size")->n == 735506);
    CHECK(alp->get("file")->get("page")->s == "https://www.nexusmods.com/enshrouded/mods/170?tab=files&file_id=1197");
    CHECK_FALSE(alp->get("file")->get("changelog")->arr.empty());
    CHECK(alp->get("installed")->get("file_name")->s == "AutoLootPlus 1.1.7");
    CHECK(alp->get("installed")->get("file_id")->n == 1131);
    CHECK(alp->get("nexus")->get("version")->s == "1.1.8");
    CHECK(alp->get("nexus")->get("files_url")->s == "https://www.nexusmods.com/enshrouded/mods/170?tab=files");
    CHECK(alp->get("install_key")->s == "nexus-170");
    CHECK(strings(alp->get("rows")).size() == 2);

    const json::Value* qd = item_for(reply, 195);
    REQUIRE(qd);
    CHECK(qd->get("state")->s == "update");
    CHECK(qd->get("via")->s == "mod.json");
    CHECK(qd->get("file")->get("file_id")->n == 1304);

    const json::Value* dig = item_for(reply, 179);
    REQUIRE(dig);
    CHECK(dig->get("state")->s == "update");  // 1.0 installed, Nexus has 1.1
    REQUIRE(dig->get("file")->is(json::Value::Type::Object));
    CHECK(dig->get("file")->get("file_id")->n == 1049);  // the archive's name says Standalone

    const json::Value* hidden = item_for(reply, 23);
    REQUIRE(hidden);
    CHECK(hidden->get("state")->s == "unavailable");  // hidden is "not on Nexus now", not gone
    CHECK(hidden->get("reason")->s.find("hidden") != std::string::npos);
    CHECK(hidden->get("file")->is(json::Value::Type::Null));

    const json::Value* sum = reply.get("summary");
    CHECK(sum->get("update")->n == 3);
    CHECK(sum->get("unavailable")->n == 1);
    CHECK(sum->get("gone")->n == 0);
    CHECK(reply.get("retry_after")->n == 0);
    CHECK(alp->get("installs_as")->s == "nexus-170");
    CHECK(alp->get("separate")->s.empty());
    CHECK(sum->get("maybe")->n == 0);
    CHECK(sum->get("current")->n == 0);
    CHECK(reply.get("emf")->is(json::Value::Type::Null));  // EMF's own Nexus id is not set yet
}

TEST_CASE("app updates: --nexus-fixture answers the update check from its updates.json") {
    const emf::nexus::Client c("0.2.0", fixture_transport(wide(std::string(EMF_SOURCE_DIR) + "\\tests\\fixtures\\nexus_app")));
    const auto r = c.updates({170, 195, 23, 999999});
    REQUIRE_MESSAGE(r.ok(), r.error);
    CHECK(r.value.requests == 1);
    CHECK(r.value.mods.at(170).found);
    CHECK(r.value.mods.at(195).version == "1.3.12");
    CHECK(r.value.mods.at(23).status == "hidden");
    CHECK_FALSE(r.value.mods.at(999999).found);
}

TEST_CASE("app updates: files to choose from are listed (without their changelogs) when EMF can't pick one") {
    Setup s;
    REQUIRE(write_nexus_link(s.g.dir, "chain:creative.dll", 179).ok);  // no version, no archive: nothing to match
    Bridge bridge(s.host);
    bridge.handle(R"({"cmd":"state"})");
    const json::Value reply = check(bridge, s);
    const json::Value* it = item_for(reply, 179, "");
    REQUIRE(it);
    CHECK(it->get("state")->s == "maybe");  // EMF doesn't know which version a DLL has
    CHECK(it->get("file")->is(json::Value::Type::Null));
    REQUIRE(it->get("choices")->arr.size() == 2);
    for (const auto& c : it->get("choices")->arr) {
        CHECK(c.get("changelog")->arr.empty());
        CHECK(c.get("page")->s.find("file_id=") != std::string::npos);
    }
}

TEST_CASE("app updates: a failed check says why, and nothing to check says that") {
    Setup s;
    s.host.graphql_reply = R"({"errors":[{"message":"Query has complexity of 18000"}]})";
    Bridge bridge(s.host);
    bridge.handle(R"({"cmd":"state"})");
    json::Value reply = check(bridge, s);
    CHECK_FALSE(reply.get("ok")->b);
    CHECK(reply.get("error")->s.find("complexity") != std::string::npos);
    const json::Value* alp = item_for(reply, 170);
    REQUIRE(alp);
    CHECK(alp->get("state")->s == "failed");
    CHECK(reply.get("summary")->get("failed")->n == 2);
    CHECK(reply.get("retry_after")->n == 60);

    // Nexus failed: EMF asks it again only after a minute, whatever the page sends
    s.host.graphql.clear();
    const auto refused = of_type(bridge.handle(R"({"cmd":"updates_check","seq":8})"), "result");
    REQUIRE(refused.size() == 1);
    CHECK_FALSE(refused[0].get("ok")->b);
    CHECK(refused[0].get("error")->s.find("60 s") != std::string::npos);
    CHECK(s.host.graphql.empty());
    s.host.now += 61;

    TempGame empty(L"updates_empty");
    s.host.dir = empty.dir;
    bridge.handle(R"({"cmd":"state"})");
    s.host.graphql.clear();
    reply = check(bridge, s);
    CHECK(reply.get("ok")->b);
    CHECK(reply.get("items")->arr.empty());
    CHECK(reply.get("note")->s.find("Nexus id") != std::string::npos);
    CHECK(s.host.graphql.empty());
}

TEST_CASE("app updates: a newer check replaces one still running") {
    Setup s;
    Bridge bridge(s.host);
    bridge.handle(R"({"cmd":"state"})");
    s.host.defer = true;
    bridge.handle(R"({"cmd":"updates_check","seq":1})");
    bridge.handle(R"({"cmd":"updates_check","seq":2})");
    s.host.run_queued();
    const auto replies = of_type(s.host.async_out, "updates");
    REQUIRE(replies.size() == 1);
    CHECK(replies[0].get("seq")->n == 2);
}

TEST_CASE("app updates: EMF's own page, once its id is set, says when a newer EMF is out") {
    Setup s;
    s.host.emf_id = 195;  // stands in for EMF's page: Nexus has 1.3.12, this EMF is 0.2.0
    Bridge bridge(s.host);
    bridge.handle(R"({"cmd":"state"})");
    const json::Value reply = check(bridge, s);
    const json::Value* emf = reply.get("emf");
    REQUIRE(emf->is(json::Value::Type::Object));
    CHECK(emf->get("newer")->b);
    CHECK(emf->get("nexus")->s == "1.3.12");
    CHECK(emf->get("installed")->s == "0.2.0");
    CHECK(emf->get("url")->s == "https://www.nexusmods.com/enshrouded/mods/195");
}

TEST_CASE("app updates: links through the page") {
    Setup s;
    Bridge bridge(s.host);
    bridge.handle(R"({"cmd":"state"})");
    auto out = bridge.handle(R"({"cmd":"nexus_link","source":"chain","id":"creative.dll","link":"https://www.nexusmods.com/enshrouded/mods/182"})");
    auto res = of_type(out, "result");
    REQUIRE(res.size() == 1);
    CHECK(res[0].get("ok")->b);
    auto links = of_type(out, "nexus_links");
    REQUIRE(links.size() == 1);
    CHECK(links[0].get("links")->get("chain:creative.dll")->n == 182);
    CHECK(read_nexus_links(s.g.dir).at("chain:creative.dll") == 182);

    out = bridge.handle(R"({"cmd":"nexus_link","source":"chain","id":"creative.dll","link":"https://www.nexusmods.com/skyrim/mods/1"})");
    CHECK_FALSE(of_type(out, "result")[0].get("ok")->b);
    CHECK(read_nexus_links(s.g.dir).at("chain:creative.dll") == 182);  // unchanged
    out = bridge.handle(R"({"cmd":"nexus_link","source":"chain","id":"nothere.dll","link":"170"})");
    CHECK_FALSE(of_type(out, "result")[0].get("ok")->b);  // no such mod in the folder
    out = bridge.handle(R"({"cmd":"nexus_link","source":"chain","id":"creative.dll","link":""})");
    CHECK(of_type(out, "result")[0].get("ok")->b);
    CHECK(read_nexus_links(s.g.dir).empty());
    links = of_type(bridge.handle(R"({"cmd":"nexus_links"})"), "nexus_links");
    REQUIRE(links.size() == 1);
    CHECK(links[0].get("links")->obj.empty());
    CHECK(links[0].get("game")->s == utf8(s.g.dir));
}

TEST_CASE("app updates: Update with EMF (Premium) downloads the file and plans it as an update of the same install") {
    Setup s;
    s.account(true);
    s.serve_zip("auto-loot-plus", "1.1.8", "AutoLootPlus 1.1.8-170-1-1-8-1792790875.zip", 1197);
    Bridge bridge(s.host);
    bridge.handle(R"({"cmd":"state"})");
    check(bridge, s);
    s.host.async_out.clear();
    // the page's button sends the file the check named
    bridge.handle(R"({"cmd":"nexus_download","mod_id":170,"file_id":1197,"name":"Auto Loot Plus"})");
    auto plans = of_type(s.host.async_out, "install_plan");
    REQUIRE(plans.size() == 1);
    REQUIRE_MESSAGE(plans[0].get("ok")->b, plans[0].get("error")->s);
    const json::Value* plan = plans[0].get("plan");
    CHECK(plan->get("key")->s == "nexus-170");
    CHECK(plan->get("update")->b);
    CHECK(plan->get("nexus_file_id")->s == "1197");
    // nothing is installed until Install
    CHECK(s.g.read(L"mods\\auto-loot-plus\\mod.json").find("1.1.7") != std::string::npos);
    s.host.async_out.clear();
    bridge.handle("{\"cmd\":\"install_apply\",\"plan_id\":\"" + plan->get("id")->s + "\"}");
    const auto done = of_type(s.host.async_out, "install_done");
    REQUIRE(done.size() == 1);
    REQUIRE_MESSAGE(done[0].get("ok")->b, done[0].get("error")->s);
    CHECK(s.g.read(L"mods\\auto-loot-plus\\mod.json").find("1.1.8") != std::string::npos);
    const auto rec = read_install_record(s.g.dir, "nexus-170");
    REQUIRE(rec);
    CHECK(rec->nexus_file_id == "1197");
    CHECK_FALSE(rec->backups.empty());  // the old files went to emf\backup
}

TEST_CASE("app updates: an install the player linked by hand is updated under its own key") {
    Setup s;
    s.account(true);
    REQUIRE(write_nexus_link(s.g.dir, "eml:shroud-dig", 179).ok);
    s.serve_zip("shroud-dig", "1.2", "ShroudDig - Standalone version (GV 1.0.0.3)-179-1-2-1792585369.zip", 1049);
    Bridge bridge(s.host);
    bridge.handle(R"({"cmd":"state"})");
    s.host.async_out.clear();
    bridge.handle(R"({"cmd":"nexus_download","mod_id":179,"file_id":1049,"name":"ShroudDig"})");
    auto plans = of_type(s.host.async_out, "install_plan");
    REQUIRE(plans.size() == 1);
    REQUIRE_MESSAGE(plans[0].get("ok")->b, plans[0].get("error")->s);
    const json::Value* plan = plans[0].get("plan");
    CHECK(plan->get("key")->s == "shroud-dig");  // not nexus-179: the same install
    CHECK(plan->get("update")->b);
    CHECK(plan->get("nexus_id")->s == "179");
    s.host.async_out.clear();
    bridge.handle("{\"cmd\":\"install_apply\",\"plan_id\":\"" + plan->get("id")->s + "\"}");
    REQUIRE(of_type(s.host.async_out, "install_done").at(0).get("ok")->b);
    const auto rec = read_install_record(s.g.dir, "shroud-dig");
    REQUIRE(rec);
    CHECK(rec->nexus_id == "179");  // the record knows its Nexus mod now
    CHECK(rec->nexus_file_id == "1049");
    CHECK_FALSE(read_install_record(s.g.dir, "nexus-179"));
}

TEST_CASE("app updates: a free account's Mod Manager Download link for it comes back as the same update") {
    Setup s;
    s.account(false);
    REQUIRE(write_nexus_link(s.g.dir, "eml:shroud-dig", 179).ok);
    s.serve_zip("shroud-dig", "1.2", "ShroudDig - Standalone version (GV 1.0.0.3)-179-1-2-1792585369.zip", 1049);
    Bridge bridge(s.host);
    bridge.handle(R"({"cmd":"state"})");
    // a free account cannot download through the API: the check's button opens the file's page instead
    auto dl = of_type(bridge.handle(R"({"cmd":"nexus_download","mod_id":179,"file_id":1049})"), "download");
    REQUIRE(dl.size() == 1);
    CHECK(dl[0].get("state")->s == "failed");
    CHECK(dl[0].get("page")->s == "https://www.nexusmods.com/enshrouded/mods/179?tab=files&file_id=1049");
    // the website's Mod Manager Download button sends the link back
    s.host.async_out.clear();
    bridge.handle_nxm("nxm://enshrouded/mods/179/files/1049?key=abc&expires=1792799999&user_id=12345");
    auto plans = of_type(s.host.async_out, "install_plan");
    REQUIRE(plans.size() == 1);
    REQUIRE_MESSAGE(plans[0].get("ok")->b, plans[0].get("error")->s);
    CHECK(plans[0].get("plan")->get("key")->s == "shroud-dig");
    CHECK(plans[0].get("plan")->get("update")->b);
}

// ---- the same install, failed and stale answers, the cooldown ----

namespace {
// An install EMF made without knowing its Nexus mod, with two rows: an EML folder and a DLL started by EMF.
void multi_install(const TempGame& g) {
    g.write(L"mods\\multi\\mod.json", R"({"id":"multi","name":"Multi","version":"1.0"})");
    g.write(L"multi.dll", "MZ multi");
    g.write(L"dinput8-chain.ini", "[Chain]\r\nautolootplus.dll\r\ncreative.dll\r\nmulti.dll\r\n");
    record(g, "multi", "", "", "1.0", "Multi.zip", "mods\\multi", "multi.dll");
}
}  // namespace

TEST_CASE("app updates: a link on one row of a multi-row install never makes an update replace the whole install") {
    Setup s;
    s.account(true);
    multi_install(s.g);
    REQUIRE(write_nexus_link(s.g.dir, "eml:multi", 179).ok);  // only the EML part
    Bridge bridge(s.host);
    bridge.handle(R"({"cmd":"state"})");

    // the check says Update with EMF installs it on its own, and why
    const json::Value reply = check(bridge, s);
    const json::Value* it = item_for(reply, 179, "multi");
    REQUIRE(it);
    CHECK(it->get("installs_as")->s == "nexus-179");
    CHECK(it->get("separate")->s.find("only part") != std::string::npos);
    const auto records = read_install_records(s.g.dir);
    CHECK(update_keys(update_targets(scan_game_folder(s.g.dir), records, read_nexus_links(s.g.dir)), records).count("179") == 0);

    // a plan for that Nexus mod is its own install: the other part (multi.dll) and its record stay
    s.serve_zip("multi", "1.1", "Multi 1.1-179-1-1-1792800000.zip", 1049);
    s.host.async_out.clear();
    bridge.handle(R"({"cmd":"nexus_download","mod_id":179,"file_id":1049,"name":"Multi"})");
    auto plans = of_type(s.host.async_out, "install_plan");
    REQUIRE(plans.size() == 1);
    REQUIRE_MESSAGE(plans[0].get("ok")->b, plans[0].get("error")->s);
    CHECK(plans[0].get("plan")->get("key")->s == "nexus-179");
    s.host.async_out.clear();
    bridge.handle("{\"cmd\":\"install_apply\",\"plan_id\":\"" + plans[0].get("plan")->get("id")->s + "\"}");
    REQUIRE(of_type(s.host.async_out, "install_done").at(0).get("ok")->b);
    CHECK(s.g.has(L"multi.dll"));
    CHECK(read_install_record(s.g.dir, "multi"));
}

TEST_CASE("app updates: a link on every row of an install updates it under its key, with a backup and the reason") {
    Setup s;
    s.account(true);
    multi_install(s.g);
    REQUIRE(write_nexus_link(s.g.dir, "eml:multi", 179).ok);
    REQUIRE(write_nexus_link(s.g.dir, "chain:multi.dll", 179).ok);
    const auto records = read_install_records(s.g.dir);
    const auto keys = update_keys(update_targets(scan_game_folder(s.g.dir), records, read_nexus_links(s.g.dir)), records);
    REQUIRE(keys.count("179") == 1);
    CHECK(keys.at("179").key == "multi");

    s.serve_zip("multi", "1.1", "Multi 1.1-179-1-1-1792800000.zip", 1049);
    Bridge bridge(s.host);
    bridge.handle(R"({"cmd":"state"})");
    s.host.async_out.clear();
    bridge.handle(R"({"cmd":"nexus_download","mod_id":179,"file_id":1049,"name":"Multi"})");
    auto plans = of_type(s.host.async_out, "install_plan");
    REQUIRE(plans.size() == 1);
    REQUIRE_MESSAGE(plans[0].get("ok")->b, plans[0].get("error")->s);
    const json::Value* plan = plans[0].get("plan");
    CHECK(plan->get("key")->s == "multi");
    CHECK(plan->get("update")->b);
    const auto notes = strings(plan->get("notes"));
    CHECK(std::any_of(notes.begin(), notes.end(), [](const std::string& n) { return n.find("because you linked Multi") != std::string::npos; }));
    s.host.async_out.clear();
    bridge.handle("{\"cmd\":\"install_apply\",\"plan_id\":\"" + plan->get("id")->s + "\"}");
    REQUIRE(of_type(s.host.async_out, "install_done").at(0).get("ok")->b);
    const auto rec = read_install_record(s.g.dir, "multi");
    REQUIRE(rec);
    CHECK(rec->nexus_id == "179");
    REQUIRE_FALSE(rec->backups.empty());  // the old version is kept in emf\backup
    CHECK(s.g.read(wide(rec->backups[0]) + L"\\mods\\multi\\mod.json").find("1.0") != std::string::npos);
}

TEST_CASE("app updates: update keys - a record named nexus-<id> wins, two installs leave it out, a bad key is never used") {
    TempGame g(L"updates_keys");
    const auto target = [](int64_t id, const char* key, const char* via) {
        UpdateTarget t;
        t.nexus_id = id;
        t.install_key = key;
        t.via = via;
        t.name = key;
        return t;
    };
    InstallRecord named;
    named.key = "nexus-6";
    // linked to two installs: neither
    CHECK(update_keys({target(5, "a", "link"), target(5, "b", "link")}, {}).empty());
    // the usual key is there already
    CHECK(update_keys({target(6, "c", "link")}, {named}).empty());
    // never a key EMF would not write
    CHECK(update_keys({target(7, "Bad Key!", "link")}, {}).empty());
    // a record that learned its Nexus id keeps its key
    const auto rec = update_keys({target(8, "d", "record")}, {});
    REQUIRE(rec.count("8") == 1);
    CHECK(rec.at("8").key == "d");
    CHECK(rec.at("8").why == "its install record names this Nexus mod");

    // build_plan uses a valid key only, and says why
    TempGame unpack(L"updates_keys_unpack", false);
    unpack.write(L"mods\\foo\\mod.json", R"({"id":"foo","name":"Foo","version":"1.1"})");
    unpack.write(L"mods\\foo\\src\\mod.lua", "-- lua");
    PlanInput in;
    in.game = g.dir;
    in.folder = unpack.dir;
    in.files = {"mods/foo/mod.json", "mods/foo/src/mod.lua"};
    in.archive = "Foo 1.1-170-1-1-1792800000.zip";
    in.nexus_id = "170";
    in.nexus_certain = true;
    in.update_keys["170"] = UpdateKey{"Bad Key!", "you linked Foo to this Nexus mod"};
    CHECK(build_plan(in).key == "nexus-170");
    in.update_keys["170"] = UpdateKey{"my-foo", "you linked Foo to this Nexus mod"};
    const InstallPlan plan = build_plan(in);
    CHECK(plan.key == "my-foo");
    CHECK(std::any_of(plan.notes.begin(), plan.notes.end(), [](const std::string& n) { return n.find("because you linked Foo") != std::string::npos; }));
}

TEST_CASE("app updates: a Nexus mod tied to two installs is installed on its own, and the check says so") {
    Setup s;
    s.g.write(L"mods\\dig-two\\mod.json", R"({"id":"dig-two","name":"Dig Two","version":"1.0"})");
    record(s.g, "dig-two", "", "", "1.0", "DigTwo.zip", "mods\\dig-two");
    REQUIRE(write_nexus_link(s.g.dir, "eml:shroud-dig", 179).ok);
    REQUIRE(write_nexus_link(s.g.dir, "eml:dig-two", 179).ok);
    Bridge bridge(s.host);
    bridge.handle(R"({"cmd":"state"})");
    const json::Value reply = check(bridge, s);
    for (const char* key : {"shroud-dig", "dig-two"}) {
        const json::Value* it = item_for(reply, 179, key);
        REQUIRE(it);
        CHECK(it->get("installs_as")->s == "nexus-179");
        CHECK(it->get("separate")->s.find("more than one install") != std::string::npos);
    }
}

TEST_CASE("app updates: an install with several rows counts as unchanged while any of them says the installed version") {
    Setup s;
    s.g.write(L"mods\\pack-a\\mod.json", R"({"id":"pack-a","name":"Pack A","version":"1.0"})");
    s.g.write(L"mods\\pack-b\\mod.json", R"({"id":"pack-b","name":"Pack B","version":"2.3"})");  // a bundled part's own version
    s.g.write(L"emf\\installed_mods\\nexus-200.json",
              R"({"key":"nexus-200","name":"Pack","version":"1.0","nexus_id":"200","nexus_file_id":"5","archive":"Pack-200-1-0-1792700000.zip",)"
              R"("roots":["mods\\pack-a","mods\\pack-b"],"chain":[],"files":[]})");
    auto find200 = [&] {
        const auto targets = update_targets(scan_game_folder(s.g.dir), read_install_records(s.g.dir), {});
        const auto it = std::find_if(targets.begin(), targets.end(), [](const UpdateTarget& t) { return t.nexus_id == 200; });
        REQUIRE(it != targets.end());
        return *it;
    };
    UpdateTarget t = find200();
    CHECK(t.rows.size() == 2);
    CHECK(t.installed.file_id == 5);
    CHECK(t.installed.archive_names_file);
    s.g.write(L"mods\\pack-a\\mod.json", R"({"id":"pack-a","name":"Pack A","version":"1.1"})");  // now none says 1.0
    t = find200();
    CHECK(t.installed.file_id == 0);
    CHECK_FALSE(t.installed.archive_names_file);
}

TEST_CASE("app updates: a check's answer that comes after a newer check started is dropped (the race on the UI thread)") {
    Setup s;
    Bridge bridge(s.host);
    bridge.handle(R"({"cmd":"state"})");
    s.host.defer_done = true;
    bridge.handle(R"({"cmd":"updates_check","seq":1})");  // its worker has finished: an answer, not cancelled
    bridge.handle(R"({"cmd":"updates_check","seq":2})");
    REQUIRE(s.host.dones.size() == 2);
    s.host.run_dones();  // the first answer arrives after the second check started
    const auto replies = of_type(s.host.async_out, "updates");
    REQUIRE(replies.size() == 1);
    CHECK(replies[0].get("seq")->n == 2);
}

TEST_CASE("app updates: an install or uninstall while a check runs drops its answer and says why") {
    Setup s;
    Bridge bridge(s.host);
    bridge.handle(R"({"cmd":"state"})");
    s.host.defer_done = true;
    bridge.handle(R"({"cmd":"updates_check","seq":3})");
    bridge.handle(R"({"cmd":"uninstall_mod","key":"nexus-170"})");
    REQUIRE(s.host.dones.size() == 2);
    auto dones = std::move(s.host.dones);
    s.host.dones.clear();
    s.host.finish(dones[1]);  // the uninstall finishes first
    s.host.finish(dones[0]);  // then the check's answer: about a folder that changed since
    const auto replies = of_type(s.host.async_out, "updates");
    REQUIRE(replies.size() == 1);
    CHECK(replies[0].get("seq")->n == 3);
    CHECK_FALSE(replies[0].get("ok")->b);
    CHECK(replies[0].get("error")->s.find("uninstalled a mod while EMF was checking") != std::string::npos);
    CHECK(replies[0].get("retry_after")->n == 0);  // not Nexus's fault: no waiting
    CHECK(replies[0].get("items")->arr.empty());
}

TEST_CASE("app updates: EMF's own Nexus id is asked first, so the cap never leaves it out") {
    Setup s;
    s.host.emf_id = 23;
    Bridge bridge(s.host);
    bridge.handle(R"({"cmd":"state"})");
    check(bridge, s);
    REQUIRE_FALSE(s.host.graphql.empty());
    const auto body = parse(s.host.graphql[0].body);
    CHECK(body.get("variables")->get("ids")->arr.at(0).get("modId")->n == 23);
}

TEST_CASE("app updates: mods past the cap are reported as not checked, without false advice") {
    UpdateTarget a, b;
    a.nexus_id = 1;
    a.via = "mod.json";
    a.installed.version = "1.0";
    b.nexus_id = 2;
    b.via = "mod.json";
    emf::nexus::Result<emf::nexus::UpdatesCheck> r;
    emf::nexus::ModStatus m;
    m.mod_id = 1;
    m.found = true;
    m.status = "published";
    m.version = "1.0";
    r.value.mods[1] = m;
    r.value.skipped = {2};
    r.value.requests = 4;
    const json::Value reply = parse(updates_reply(5, {a, b}, r, EmfSelf{}));
    const json::Value* past = item_for(reply, 2);
    REQUIRE(past);
    CHECK(past->get("state")->s == "failed");
    CHECK(past->get("reason")->s.find("past") != std::string::npos);
    CHECK(past->get("reason")->s.find("again") == std::string::npos);
    CHECK(reply.get("note")->s.find("1 of 2") != std::string::npos);
    CHECK(reply.get("summary")->get("failed")->n == 1);
}

TEST_CASE("app updates: choosing another game folder drops a check of the old one") {
    Setup s;
    TempGame other(L"updates_other");
    Bridge bridge(s.host);
    bridge.handle(R"({"cmd":"state"})");
    s.host.defer_done = true;
    bridge.handle(R"({"cmd":"updates_check","seq":4})");
    s.host.picked = other.dir;
    s.host.defer_done = false;
    bridge.handle(R"({"cmd":"pick_folder"})");
    CHECK(s.host.dir == other.dir);
    s.host.run_dones();  // the old folder's answer arrives after the change
    CHECK(of_type(s.host.async_out, "updates").empty());
}

TEST_CASE("app updates: switching to a server folder (the folder list) drops a check of the old one") {
    Setup s;
    TempGame server(L"updates_server", false);
    server.write(L"enshrouded_server.exe", "fake");
    Bridge bridge(s.host);
    bridge.handle(R"({"cmd":"state"})");
    s.host.defer_done = true;
    bridge.handle(R"({"cmd":"updates_check","seq":4})");
    s.host.picked = server.dir;
    s.host.defer_done = false;
    bridge.handle(R"({"cmd":"folder_pick_server"})");
    CHECK(s.host.dir == server.dir);
    s.host.run_dones();
    CHECK(of_type(s.host.async_out, "updates").empty());
}
