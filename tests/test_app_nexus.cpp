// EMF.exe's Nexus account through the page bridge, with a fake host: setting and checking a key (stored with DPAPI,
// never sent to the page), logging out, downloads from an nxm:// link and from the Browse page (Premium), the link that
// waits for a key, refusals (another game, another account, an expired link, a free account, used-up limits, a second
// download), cancel, the downloaded file flowing into the install plan with its Nexus ids, the nxm:// handler setting
// against a memory registry, and single sign-on with a fake socket. No network.
#include <doctest.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "app/account_store.h"
#include "app/bridge.h"
#include "app/text.h"
#include "app_archive_files.h"
#include "app_test_files.h"
#include "core/json.h"
#include "nexus/nxm.h"

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

std::string header(const emf::nexus::HttpRequest& r, const std::string& name) {
    for (const auto& [k, v] : r.headers)
        if (k == name) return v;
    return "<missing>";
}

const std::string kKey = "aGVsbG8=--d29ybGQ=--a2V5";

struct FakeHost : BridgeHost {
    std::wstring dir, data;
    std::vector<std::pair<std::wstring, Open>> opened;
    std::string running;
    std::vector<std::string> async_out;
    bool defer = false;
    std::vector<std::function<Done()>> queued;
    MemoryRegistry reg;
    bool with_registry = true;
    std::string slug;
    emf::nexus::SsoSocketFactory sockets;
    int64_t now = 1792790000;
    // the fake Nexus
    std::vector<emf::nexus::HttpRequest> api;
    std::vector<emf::nexus::StreamRequest> cdn;
    std::string validate_reply = fixture("v1_validate_premium.json");
    int validate_status = 200;
    std::string file_reply = fixture("v1_file_170_1197.json");
    std::string link_reply = fixture("v1_download_link_170_1197.json");
    int link_status = 200;
    std::string cdn_body = std::string(735506, 'Z');
    emf::nexus::HttpHeaders limits = {{"X-RL-Daily-Remaining", "100"}, {"X-RL-Hourly-Remaining", "50"}};

    std::wstring game() const override { return dir; }
    void choose_game(const std::wstring& d) override { dir = d; }
    std::optional<std::wstring> pick_folder() override { return std::nullopt; }
    bool open(const std::wstring& target, Open how) override {
        opened.emplace_back(target, how);
        return true;
    }
    std::string running_reason() override { return running; }
    void rendered(int, const std::vector<std::string>&) override {}
    std::wstring steam_game() override { return {}; }
    bool copy_text(const std::string&) override { return true; }
    std::optional<std::wstring> pick_archive() override { return std::nullopt; }
    bool testing = false;
    bool test_mode() const override { return testing; }
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
    // Worker threads must post through ui_poster (it outlives the window); run_on_ui is for the UI thread only.
    int run_on_ui_calls = 0;
    int posted = 0;
    void run_on_ui(Done done) override {
        ++run_on_ui_calls;
        finish(done);
    }
    std::function<void(Done)> ui_poster() override {
        return [this](Done done) {
            ++posted;
            finish(done);
        };
    }
    // The tester switch ([nexus] personal_key=1 in app.ini): on for most tests, which use a personal key; the tests of
    // the public build (no slug, no switch) turn it off.
    bool personal = true;
    bool personal_key_allowed() const override { return personal; }
    std::function<void()> mid_stream;  // called once after the first piece of a download
    std::wstring app_data() const override { return data; }
    RegistryLayer* registry() override { return with_registry ? &reg : nullptr; }
    std::wstring exe_path() const override { return L"C:\\Games\\Enshrouded\\emf\\EMF.exe"; }
    std::string sso_slug() const override { return slug; }
    emf::nexus::SsoSocketFactory sso_sockets() override { return sockets; }
    std::vector<emf::nexus::HttpRequest> graphql;  // Browse's and Check for updates' requests (answered 503)
    emf::nexus::Client::Transport nexus_transport() override {
        return [this](const emf::nexus::HttpRequest& r) {
            graphql.push_back(r);
            emf::nexus::HttpResponse resp;
            resp.status = 503;
            return resp;
        };
    }
    int64_t now_unix() const override { return now; }
    emf::nexus::V1Client::Transport nexus_v1_transport() override {
        return [this](const emf::nexus::HttpRequest& r) {
            api.push_back(r);
            emf::nexus::HttpResponse resp;
            resp.headers = limits;
            if (r.url.find("validate.json") != std::string::npos) {
                resp.status = validate_status;
                resp.body = validate_reply;
            } else if (r.url.find("download_link.json") != std::string::npos) {
                resp.status = link_status;
                resp.body = link_reply;
            } else {
                resp.status = 200;
                resp.body = file_reply;
            }
            return resp;
        };
    }
    emf::nexus::Downloader::Fetch nexus_fetch() override {
        return [this](const emf::nexus::StreamRequest& r, const emf::nexus::StreamSink& sink) {
            cdn.push_back(r);
            emf::nexus::StreamResponse out;
            out.status = 200;
            if (sink.start && !sink.start(cdn_body.size(), true)) {
                out.failure = emf::nexus::HttpFailure::Stopped;
                return out;
            }
            for (size_t at = 0; at < cdn_body.size(); at += 65536) {
                if (r.cancel && r.cancel->load()) {
                    out.failure = emf::nexus::HttpFailure::Cancelled;
                    return out;
                }
                const size_t n = (std::min<size_t>)(65536, cdn_body.size() - at);
                out.bytes += n;
                if (sink.write && !sink.write(cdn_body.data() + at, n)) {
                    out.failure = emf::nexus::HttpFailure::Stopped;
                    return out;
                }
                if (at == 0 && mid_stream) mid_stream();
            }
            return out;
        };
    }
};

// A game folder, an app-data folder and a fake host on them.
struct Setup {
    TempGame g{L"nexus_game"};
    TempGame appdata{L"nexus_appdata", false};
    FakeHost host;
    Setup() {
        host.dir = g.dir;
        host.data = appdata.dir;
    }
};

// Stores a Premium (or free) account the way a successful check does.
void store(Setup& s, bool premium = true, int64_t user_id = 12345) {
    StoredAccount a;
    a.key = kKey;
    a.user_id = user_id;
    a.name = "EmberTester";
    a.premium = premium;
    REQUIRE(save_account(s.appdata.at(L"nexus.key"), a));
}

// A real zip (an EML mod) the fake CDN sends, with the file reply naming it and its exact size.
void serve_zip(Setup& s, const std::string& name = "FooMod-170-1-0-1792790875.zip") {
    TempGame src(L"nexus_zip_src", false), keep(L"nexus_zip", false);
    src.write(L"FooMod\\mods\\foo\\mod.json", R"({"id":"foo","name":"Foo Mod","version":"1.0"})");
    src.write(L"FooMod\\mods\\foo\\src\\mod.lua", "-- lua");
    const std::wstring zip = keep.at(L"foo.zip");
    REQUIRE(app_test::make_zip(src.dir, zip));
    std::ifstream f(std::filesystem::path(zip), std::ios::binary);
    s.host.cdn_body.assign(std::istreambuf_iterator<char>(f), {});
    s.host.file_reply = "{\"file_id\":1197,\"name\":\"Foo Mod 1.0\",\"version\":\"1.0\",\"category_name\":\"MAIN\",\"file_name\":\"" + name +
                        "\",\"size_in_bytes\":" + std::to_string(s.host.cdn_body.size()) + "}";
}

bool mentions_key(const std::vector<std::string>& msgs) {
    for (const auto& m : msgs)
        if (m.find(kKey) != std::string::npos || m.find("aGVsbG8") != std::string::npos) return true;
    return false;
}

}  // namespace

TEST_CASE("app nexus: with no key the account says so, and the page never gets a key") {
    Setup s;
    Bridge bridge(s.host);
    const auto a = of_type(bridge.handle(R"({"cmd":"account"})"), "account");
    REQUIRE(a.size() == 1);
    CHECK_FALSE(a[0].get("key_set")->b);
    CHECK(a[0].get("user")->is(json::Value::Type::Null));
    CHECK_FALSE(a[0].get("sso")->get("available")->b);  // no slug yet: no "Log in with Nexus"
    CHECK(a[0].get("nxm")->get("available")->b);
    CHECK(a[0].get("nxm")->get("owner")->s == "nothing");
    CHECK_FALSE(a[0].get("nxm")->get("ours")->b);
    CHECK(a[0].get("downloads")->s == utf8(s.appdata.at(L"downloads")));
    CHECK_FALSE(a[0].get("rate")->get("known")->b);
    s.host.with_registry = false;
    CHECK_FALSE(of_type(bridge.handle(R"({"cmd":"account"})"), "account")[0].get("nxm")->get("available")->b);
}

TEST_CASE("app nexus: a pasted key is checked with Nexus, stored with DPAPI and shown only as the account") {
    Setup s;
    Bridge bridge(s.host);
    const auto first = bridge.handle("{\"cmd\":\"account_key\",\"key\":\"  " + kKey + "\\n\"}");
    REQUIRE(of_type(first, "account").size() == 1);
    REQUIRE(s.host.api.size() == 1);
    CHECK(s.host.api[0].url == "https://api.nexusmods.com/v1/users/validate.json");
    CHECK(header(s.host.api[0], "apikey") == kKey);  // trimmed
    const auto results = of_type(s.host.async_out, "result");
    REQUIRE(results.size() == 1);
    CHECK(results[0].get("ok")->b);
    CHECK(results[0].get("text")->s == "Logged in to Nexus as EmberTester (Premium).");
    const auto acc = of_type(s.host.async_out, "account");
    REQUIRE(acc.size() == 1);
    CHECK(acc[0].get("key_set")->b);
    CHECK(acc[0].get("user")->get("name")->s == "EmberTester");
    CHECK(acc[0].get("user")->get("premium")->b);
    CHECK(acc[0].get("rate")->get("daily")->n == 100);
    CHECK_FALSE(mentions_key(first));
    CHECK_FALSE(mentions_key(s.host.async_out));
    // stored encrypted: readable through DPAPI only
    const auto stored = load_account(s.appdata.at(L"nexus.key"));
    REQUIRE(stored);
    CHECK(stored->key == kKey);
    CHECK(stored->premium);
    CHECK(s.appdata.read(L"nexus.key").find(kKey) == std::string::npos);
    // a new bridge (EMF.exe started again) knows the account without asking Nexus
    s.host.api.clear();
    Bridge again(s.host);
    const auto a = of_type(again.handle(R"({"cmd":"account"})"), "account");
    CHECK(a[0].get("key_set")->b);
    CHECK(a[0].get("user")->get("name")->s == "EmberTester");
    CHECK(s.host.api.empty());
}

TEST_CASE("app nexus: a key Nexus refuses is not stored; something that is not a key is never sent") {
    Setup s;
    s.host.validate_reply = fixture("v1_validate_401_bad_key.json");
    s.host.validate_status = 401;
    Bridge bridge(s.host);
    bridge.handle("{\"cmd\":\"account_key\",\"key\":\"" + kKey + "\"}");
    const auto results = of_type(s.host.async_out, "result");
    REQUIRE(results.size() == 1);
    CHECK_FALSE(results[0].get("ok")->b);
    CHECK(results[0].get("error")->s.find("did not accept the API key") != std::string::npos);
    const auto acc = of_type(s.host.async_out, "account");
    CHECK_FALSE(acc.back().get("key_set")->b);
    CHECK(acc.back().get("error")->s.find("valid API Key") != std::string::npos);
    CHECK_FALSE(s.appdata.has(L"nexus.key"));

    s.host.api.clear();
    const auto bad = of_type(bridge.handle(R"({"cmd":"account_key","key":"two words"})"), "result");
    REQUIRE(bad.size() == 1);
    CHECK_FALSE(bad[0].get("ok")->b);
    CHECK(bad[0].get("error")->s.find("settings/api-keys") != std::string::npos);  // the page the button opens
    CHECK(s.host.api.empty());
}

TEST_CASE("app nexus: logging out while a key is being checked keeps it out") {
    Setup s;
    s.host.defer = true;
    Bridge bridge(s.host);
    bridge.handle("{\"cmd\":\"account_key\",\"key\":\"" + kKey + "\"}");
    REQUIRE(s.host.queued.size() == 1);
    bridge.handle(R"({"cmd":"account_logout"})");
    s.host.run_queued();  // Nexus answers after the player logged out
    CHECK_FALSE(s.appdata.has(L"nexus.key"));
    CHECK_FALSE(of_type(bridge.handle(R"({"cmd":"account"})"), "account")[0].get("key_set")->b);
}

TEST_CASE("app nexus: log out removes the key from the PC") {
    Setup s;
    store(s);
    Bridge bridge(s.host);
    CHECK(of_type(bridge.handle(R"({"cmd":"account"})"), "account")[0].get("key_set")->b);
    const auto out = bridge.handle(R"({"cmd":"account_logout"})");
    CHECK(of_type(out, "result")[0].get("ok")->b);
    CHECK_FALSE(of_type(out, "account")[0].get("key_set")->b);
    CHECK_FALSE(s.appdata.has(L"nexus.key"));
}

TEST_CASE("app nexus: a Premium member's Download with EMF: progress, the file, then the plan with the Nexus ids") {
    Setup s;
    store(s);
    serve_zip(s);
    Bridge bridge(s.host);
    bridge.handle(R"({"cmd":"state"})");
    const auto first = of_type(bridge.handle(R"({"cmd":"nexus_download","mod_id":170,"file_id":1197,"name":"Foo Mod"})"), "download");
    REQUIRE(first.size() == 1);
    CHECK(first[0].get("state")->s == "running");
    // the API: the file's details and a link without parameters; the CDN: no key
    REQUIRE(s.host.api.size() == 2);
    CHECK(s.host.api[1].url.find("download_link.json") != std::string::npos);
    CHECK(s.host.api[1].url.find('?') == std::string::npos);
    REQUIRE(s.host.cdn.size() == 1);
    for (const auto& [k, v] : s.host.cdn[0].headers) CHECK(v.find(kKey) == std::string::npos);
    const auto dl = of_type(s.host.async_out, "download");
    REQUIRE_FALSE(dl.empty());
    CHECK(dl.front().get("state")->s == "running");  // progress
    CHECK(dl.back().get("state")->s == "done");
    CHECK(dl.back().get("file")->s == "FooMod-170-1-0-1792790875.zip");
    CHECK(dl.back().get("bytes")->n == static_cast<double>(s.host.cdn_body.size()));
    CHECK(s.appdata.has(L"downloads\\FooMod-170-1-0-1792790875.zip"));
    // the downloaded archive goes through the plan, which knows the mod and the file
    const auto plans = of_type(s.host.async_out, "install_plan");
    REQUIRE(plans.size() == 1);
    REQUIRE_MESSAGE(plans[0].get("ok")->b, plans[0].get("error")->s);
    const json::Value* plan = plans[0].get("plan");
    CHECK(plan->get("nexus_id")->s == "170");
    CHECK(plan->get("nexus_file_id")->s == "1197");
    CHECK(plan->get("key")->s == "nexus-170");
    CHECK(plan->get("installable")->b);
    CHECK_FALSE(mentions_key(s.host.async_out));

    // Install records the file id
    s.host.async_out.clear();
    bridge.handle("{\"cmd\":\"install_apply\",\"plan_id\":\"" + plan->get("id")->s + "\"}");
    const auto done = of_type(s.host.async_out, "install_done");
    REQUIRE(done.size() == 1);
    REQUIRE_MESSAGE(done[0].get("ok")->b, done[0].get("error")->s);
    const auto rec = read_install_record(s.g.dir, "nexus-170");
    REQUIRE(rec);
    CHECK(rec->nexus_file_id == "1197");
}

TEST_CASE("app nexus: a free account's Download with EMF opens the file's page instead") {
    Setup s;
    store(s, false);
    Bridge bridge(s.host);
    const auto dl = of_type(bridge.handle(R"({"cmd":"nexus_download","mod_id":170,"file_id":1197})"), "download");
    REQUIRE(dl.size() == 1);
    CHECK(dl[0].get("state")->s == "failed");
    CHECK(dl[0].get("error")->s.find("Premium members only") != std::string::npos);
    CHECK(dl[0].get("page")->s == "https://www.nexusmods.com/enshrouded/mods/170?tab=files&file_id=1197");
    CHECK(s.host.api.empty());
    CHECK(s.host.cdn.empty());
}

TEST_CASE("app nexus: a Mod Manager Download link downloads with its key and expiry") {
    Setup s;
    store(s, false);  // a free account
    serve_zip(s);
    Bridge bridge(s.host);
    bridge.handle(R"({"cmd":"state"})");
    bridge.handle_nxm("nxm://enshrouded/mods/170/files/1197?key=one+time&expires=1792799999&user_id=12345");
    REQUIRE(s.host.api.size() == 2);
    CHECK(s.host.api[1].url.find("download_link.json?key=one%2Btime&expires=1792799999") != std::string::npos);
    const auto dl = of_type(s.host.async_out, "download");
    REQUIRE_FALSE(dl.empty());
    CHECK(dl.back().get("state")->s == "done");
    CHECK(of_type(s.host.async_out, "install_plan").size() == 1);
}

TEST_CASE("app nexus: a link that arrives before a key waits for it, then downloads") {
    Setup s;
    serve_zip(s);
    Bridge bridge(s.host);
    bridge.handle(R"({"cmd":"state"})");
    const auto out = bridge.handle_nxm("nxm://enshrouded/mods/170/files/1197?key=k1&expires=1792799999");
    const auto dl = of_type(out, "download");
    REQUIRE(dl.size() == 1);
    CHECK(dl[0].get("state")->s == "need_key");
    const auto acc = of_type(out, "account");
    REQUIRE(acc.size() == 1);
    CHECK(acc[0].get("waiting")->get("file_id")->n == 1197);
    CHECK(s.host.api.empty());

    bridge.handle("{\"cmd\":\"account_key\",\"key\":\"" + kKey + "\"}");
    REQUIRE(s.host.api.size() == 3);  // validate, then the file's details and its link
    CHECK(s.host.api[2].url.find("key=k1") != std::string::npos);
    // (the fake host runs work at once, so the finished download's messages come before the check's own)
    const auto after = of_type(s.host.async_out, "download");
    CHECK(std::any_of(after.begin(), after.end(), [](const json::Value& d) { return d.get("state")->s == "done"; }));
    CHECK(of_type(s.host.async_out, "account").back().get("waiting")->is(json::Value::Type::Null));
}

TEST_CASE("app nexus: links for another game, another account or past their expiry are refused with the reason") {
    Setup s;
    store(s);
    Bridge bridge(s.host);
    auto dl = of_type(bridge.handle_nxm("nxm://skyrimspecialedition/mods/1/files/2?key=k&expires=1792799999"), "download");
    REQUIRE(dl.size() == 1);
    CHECK(dl[0].get("state")->s == "failed");
    CHECK(dl[0].get("error")->s.find("skyrimspecialedition") != std::string::npos);
    // after EMF took the links from Vortex, the message says how to give them back
    s.host.reg.set(L"Software\\Classes\\nxm\\shell\\open\\command", L"", L"\"C:\\Vortex\\Vortex.exe\" -d \"%1\"");
    bridge.handle(R"({"cmd":"nxm_take"})");
    dl = of_type(bridge.handle_nxm("nxm://skyrimspecialedition/mods/1/files/2"), "download");
    CHECK(dl[0].get("error")->s.find("Vortex") != std::string::npos);
    CHECK(dl[0].get("error")->s.find("then click Mod Manager Download again") != std::string::npos);

    dl = of_type(bridge.handle_nxm("nxm://enshrouded/mods/170/files/1197?key=k&expires=1792799999&user_id=999"), "download");
    CHECK(dl[0].get("error")->s.find("another Nexus account") != std::string::npos);
    dl = of_type(bridge.handle_nxm("nxm://enshrouded/mods/170/files/1197?key=k&expires=1792780000&user_id=12345"), "download");
    CHECK(dl[0].get("error")->s.find("expired") != std::string::npos);
    dl = of_type(bridge.handle_nxm("not a link"), "download");
    CHECK(dl[0].get("state")->s == "failed");
    CHECK(s.host.api.empty());
}

TEST_CASE("app nexus: one download at a time; cancel stops it and leaves no file") {
    Setup s;
    store(s);
    serve_zip(s);
    s.host.defer = true;
    Bridge bridge(s.host);
    CHECK(of_type(bridge.handle(R"({"cmd":"nexus_download","mod_id":170,"file_id":1197})"), "download")[0].get("state")->s == "running");
    const auto second = of_type(bridge.handle(R"({"cmd":"nexus_download","mod_id":170,"file_id":1198})"), "download");
    REQUIRE(second.size() == 1);
    CHECK(second[0].get("state")->s == "failed");
    CHECK(second[0].get("error")->s.find("downloading") != std::string::npos);
    bridge.handle(R"({"cmd":"download_cancel"})");
    s.host.run_queued();
    const auto dl = of_type(s.host.async_out, "download");
    REQUIRE_FALSE(dl.empty());
    CHECK(dl.back().get("state")->s == "cancelled");
    CHECK_FALSE(s.appdata.has(L"downloads\\FooMod-170-1-0-1792790875.zip"));
    CHECK(of_type(s.host.async_out, "install_plan").empty());
    // and the next one may start
    s.host.defer = false;
    s.host.async_out.clear();
    bridge.handle(R"({"cmd":"nexus_download","mod_id":170,"file_id":1197})");
    CHECK(of_type(s.host.async_out, "download").back().get("state")->s == "done");
}

TEST_CASE("app nexus: when Nexus's request allowances are used up, EMF asks nothing more and says so") {
    Setup s;
    store(s);
    serve_zip(s);
    s.host.limits = {{"X-RL-Daily-Remaining", "0"}, {"X-RL-Hourly-Remaining", "0"}, {"X-RL-Hourly-Reset", "2026-10-26T20:00:00+00:00"}};
    Bridge bridge(s.host);
    bridge.handle(R"({"cmd":"nexus_download","mod_id":170,"file_id":1197})");
    auto dl = of_type(s.host.async_out, "download");
    CHECK(dl.back().get("state")->s == "failed");
    CHECK(dl.back().get("error")->s.find("used up") != std::string::npos);
    const auto acc = of_type(s.host.async_out, "account");
    REQUIRE_FALSE(acc.empty());
    CHECK(acc.back().get("rate")->get("exhausted")->b);
    CHECK(acc.back().get("rate")->get("message")->s.find("opens again at " + emf::nexus::local_clock(1793044800)) != std::string::npos);
    s.host.api.clear();
    dl = of_type(bridge.handle(R"({"cmd":"nexus_download","mod_id":170,"file_id":1197})"), "download");
    CHECK(dl[0].get("error")->s.find("used up") != std::string::npos);
    CHECK(s.host.api.empty());
    CHECK_FALSE(of_type(bridge.handle(R"({"cmd":"account_check"})"), "result")[0].get("ok")->b);
    CHECK(s.host.api.empty());

    // after the hourly reset EMF may ask again
    s.host.now = 1793044800 + 1;  // 2026-10-26T20:00:01Z
    s.host.limits = {{"X-RL-Daily-Remaining", "0"}, {"X-RL-Hourly-Remaining", "499"}};
    s.host.async_out.clear();
    bridge.handle(R"({"cmd":"nexus_download","mod_id":170,"file_id":1197})");
    CHECK(s.host.api.size() == 2);
    CHECK(of_type(s.host.async_out, "download").back().get("state")->s == "done");
}

TEST_CASE("app nexus: the startup link is handled when the page first asks for the state") {
    Setup s;
    Bridge bridge(s.host);
    bridge.queue_nxm("nxm://enshrouded/mods/170/files/1197?key=k&expires=1792799999");
    const auto out = bridge.handle(R"({"cmd":"state"})");
    CHECK(of_type(out, "state").size() == 1);
    CHECK(of_type(out, "download")[0].get("state")->s == "need_key");
    CHECK(of_type(bridge.handle(R"({"cmd":"state"})"), "download").empty());  // only once
}

TEST_CASE("app nexus: the nxm:// setting through the page: take over from Vortex, then give back") {
    Setup s;
    s.host.reg.set(L"Software\\Classes\\nxm\\shell\\open\\command", L"", L"\"C:\\Vortex\\Vortex.exe\" -d \"%1\"");
    Bridge bridge(s.host);
    auto acc = of_type(bridge.handle(R"({"cmd":"account"})"), "account")[0];
    CHECK(acc.get("nxm")->get("owner")->s == "Vortex");
    auto out = bridge.handle(R"({"cmd":"nxm_take"})");
    CHECK(of_type(out, "result")[0].get("ok")->b);
    acc = of_type(out, "account")[0];
    CHECK(acc.get("nxm")->get("ours")->b);
    CHECK(acc.get("nxm")->get("previous")->s == "Vortex");
    out = bridge.handle(R"({"cmd":"nxm_give_back"})");
    CHECK(of_type(out, "result")[0].get("text")->s.find("Vortex") != std::string::npos);
    acc = of_type(out, "account")[0];
    CHECK(acc.get("nxm")->get("owner")->s == "Vortex");
    CHECK(acc.get("nxm")->get("previous")->s.empty());
}

namespace {
class ScriptSocket : public emf::nexus::SsoSocket {
public:
    explicit ScriptSocket(std::vector<std::string> in) : in_(std::move(in)) {}
    bool connect(std::string&) override { return true; }
    bool send_text(const std::string&) override { return true; }
    std::optional<std::string> receive_text() override {
        if (at_ < in_.size()) return in_[at_++];
        return std::nullopt;
    }
    std::string error() const override { return "closed"; }
    void abort() override {}

private:
    std::vector<std::string> in_;
    size_t at_ = 0;
};
}  // namespace

TEST_CASE("app nexus: Log in with Nexus is refused without a slug; with one, the key from Nexus is checked and stored") {
    Setup s;
    Bridge bridge(s.host);
    CHECK_FALSE(of_type(bridge.handle(R"({"cmd":"sso_start"})"), "result")[0].get("ok")->b);

    s.host.slug = "emf";
    s.host.sockets = [] {
        return std::make_unique<ScriptSocket>(std::vector<std::string>{
            R"({"success":true,"data":{"connection_token":"tok123"},"error":null})",
            "{\"success\":true,\"data\":{\"api_key\":\"" + kKey + "\"},\"error\":null}"});
    };
    Bridge with_slug(s.host);
    CHECK(of_type(with_slug.handle(R"({"cmd":"account"})"), "account")[0].get("sso")->get("available")->b);
    with_slug.handle(R"({"cmd":"sso_start"})");
    REQUIRE(s.host.opened.size() == 1);
    CHECK(utf8(s.host.opened[0].first).rfind("https://www.nexusmods.com/sso?id=", 0) == 0);
    CHECK(utf8(s.host.opened[0].first).find("&application=emf") != std::string::npos);
    REQUIRE(s.host.api.size() == 1);  // the key from Nexus is checked like a pasted one
    CHECK(header(s.host.api[0], "apikey") == kKey);
    CHECK(of_type(s.host.async_out, "account").back().get("key_set")->b);
    CHECK(load_account(s.appdata.at(L"nexus.key")));
    CHECK_FALSE(mentions_key(s.host.async_out));
}

// ---- edge cases ----

TEST_CASE("app nexus: worker threads post through the poster, never through the host's run_on_ui") {
    Setup s;
    store(s);
    Bridge bridge(s.host);
    bridge.handle(R"({"cmd":"nexus_download","mod_id":170,"file_id":1197})");
    CHECK(of_type(s.host.async_out, "download").back().get("state")->s == "done");
    CHECK(s.host.posted > 0);  // the progress messages
    CHECK(s.host.run_on_ui_calls == 0);
}

TEST_CASE("app nexus: cancel in the middle of a transfer stops it and leaves no file") {
    Setup s;
    store(s);
    Bridge bridge(s.host);
    s.host.mid_stream = [&] { bridge.handle(R"({"cmd":"download_cancel"})"); };
    bridge.handle(R"({"cmd":"nexus_download","mod_id":170,"file_id":1197})");
    REQUIRE(s.host.cdn.size() == 1);  // it had started
    CHECK(of_type(s.host.async_out, "download").back().get("state")->s == "cancelled");
    CHECK_FALSE(s.appdata.has(L"downloads\\AutoLootPlus 1.1.8-170-1-1-8-1792790875.zip"));
    CHECK_FALSE(s.appdata.has(L"downloads\\AutoLootPlus 1.1.8-170-1-1-8-1792790875.zip.part"));
}

TEST_CASE("app nexus: log out cancels a download that uses the key") {
    Setup s;
    store(s);
    s.host.defer = true;
    Bridge bridge(s.host);
    bridge.handle(R"({"cmd":"nexus_download","mod_id":170,"file_id":1197})");
    bridge.handle(R"({"cmd":"account_logout"})");
    s.host.run_queued();
    CHECK(of_type(s.host.async_out, "download").back().get("state")->s == "cancelled");
    CHECK(s.host.api.empty());  // the worker saw the cancel before asking Nexus
}

TEST_CASE("app nexus: a link's expiry allows a few minutes for the PC's clock; beyond that Nexus decides") {
    Setup s;
    store(s);
    Bridge bridge(s.host);
    // two minutes past the link's expiry by this PC's clock: still asked (Nexus's own reply decides)
    bridge.handle_nxm("nxm://enshrouded/mods/170/files/1197?key=k&expires=" + std::to_string(s.host.now - 120));
    CHECK(s.host.api.size() == 2);
    s.host.api.clear();
    const auto dl = of_type(bridge.handle_nxm("nxm://enshrouded/mods/170/files/1197?key=k&expires=" +
                                              std::to_string(s.host.now - 600)), "download");
    CHECK(dl[0].get("error")->s.find("expired") != std::string::npos);
    CHECK(s.host.api.empty());
}

TEST_CASE("app nexus: two links before the page listens are both answered, none lost") {
    Setup s;
    Bridge bridge(s.host);
    bridge.queue_nxm("nxm://enshrouded/mods/170/files/1197?key=k1&expires=1792799999");
    bridge.queue_nxm("nxm://enshrouded/mods/170/files/1198?key=k2&expires=1792799999");
    const auto dl = of_type(bridge.handle(R"({"cmd":"state"})"), "download");
    REQUIRE(dl.size() == 2);
    CHECK(dl[0].get("state")->s == "need_key");
    CHECK(dl[0].get("file_id")->n == 1197);
    CHECK(dl[1].get("state")->s == "failed");  // the second is refused with the reason, not dropped
    CHECK(dl[1].get("file_id")->n == 1198);
    CHECK(dl[1].get("error")->s.find("already waiting") != std::string::npos);
}

TEST_CASE("app nexus: a download that finishes while a plan is open waits instead of replacing it") {
    Setup s;
    store(s);
    serve_zip(s);
    TempGame src(L"nexus_open_plan_src", false), keep(L"nexus_open_plan_zip", false);
    src.write(L"mods\\bar\\mod.json", R"({"id":"bar","name":"Bar"})");
    src.write(L"mods\\bar\\src\\mod.lua", "-- lua");
    REQUIRE(app_test::make_zip(src.dir, keep.at(L"bar.zip")));
    Bridge bridge(s.host);
    bridge.handle(R"({"cmd":"state"})");
    s.host.testing = true;
    bridge.handle("{\"cmd\":\"install_path\",\"path\":" + json_quote(utf8(keep.at(L"bar.zip"))) + "}");
    auto plans = of_type(s.host.async_out, "install_plan");
    REQUIRE(plans.size() == 1);
    const std::string open_id = plans[0].get("plan")->get("id")->s;
    CHECK_FALSE(plans[0].get("from_download")->b);
    s.host.async_out.clear();

    bridge.handle(R"({"cmd":"nexus_download","mod_id":170,"file_id":1197,"name":"Foo Mod"})");
    CHECK(of_type(s.host.async_out, "install_plan").empty());  // the open plan stays
    const auto ready = of_type(s.host.async_out, "download_ready");
    REQUIRE(ready.size() == 1);
    CHECK(ready[0].get("count")->n == 1);
    const std::string ready_id = std::to_string(static_cast<int>(ready[0].get("id")->n));
    // the open plan still installs
    s.host.async_out.clear();
    bridge.handle("{\"cmd\":\"install_cancel\",\"plan_id\":\"" + open_id + "\"}");
    // then the waiting download's plan, marked as coming from a download
    bridge.handle("{\"cmd\":\"download_plan\",\"id\":" + ready_id + "}");
    plans = of_type(s.host.async_out, "install_plan");
    REQUIRE(plans.size() == 1);
    REQUIRE_MESSAGE(plans[0].get("ok")->b, plans[0].get("error")->s);
    CHECK(plans[0].get("from_download")->b);
    CHECK(plans[0].get("plan")->get("nexus_file_id")->s == "1197");
}

TEST_CASE("app nexus: with an application slug only Log in with Nexus shows, unless personal keys are allowed") {
    Setup s;
    {
        Bridge bridge(s.host);  // no slug (today), the tester switch on: the key field shows
        const auto a = of_type(bridge.handle(R"({"cmd":"account"})"), "account")[0];
        CHECK(a.get("personal_key")->b);
        CHECK(a.get("mode")->s == "personal_key");
    }
    s.host.slug = "emf";
    s.host.personal = false;
    {
        Bridge bridge(s.host);
        const auto a = of_type(bridge.handle(R"({"cmd":"account"})"), "account")[0];
        CHECK(a.get("mode")->s == "sso");
        CHECK_FALSE(a.get("personal_key")->b);
        CHECK(a.get("sso")->get("available")->b);
        CHECK(a.get("nxm")->get("available")->b);  // registered: the nxm:// setting is offered
        const auto r = of_type(bridge.handle("{\"cmd\":\"account_key\",\"key\":\"" + kKey + "\"}"), "result");
        REQUIRE(r.size() == 1);
        CHECK_FALSE(r[0].get("ok")->b);
        CHECK(s.host.api.empty());
    }
    s.host.personal = true;  // the tester switch: developers keep the key field next to Log in with Nexus
    {
        Bridge bridge(s.host);
        const auto a = of_type(bridge.handle(R"({"cmd":"account"})"), "account")[0];
        CHECK(a.get("mode")->s == "sso");
        CHECK(a.get("personal_key")->b);
        CHECK(a.get("sso")->get("available")->b);
        bridge.handle("{\"cmd\":\"account_key\",\"key\":\"" + kKey + "\"}");
        CHECK(s.host.api.size() == 1);
    }
}

TEST_CASE("app nexus: with a slug and no switch, the nxm:// links can be taken and a stored key downloads") {
    Setup s;
    s.host.slug = "emf";
    s.host.personal = false;
    store(s);  // the key Log in with Nexus stored
    serve_zip(s);
    s.host.reg.set(L"Software\\Classes\\nxm\\shell\\open\\command", L"", L"\"C:\\Vortex\\Vortex.exe\" -d \"%1\"");
    Bridge bridge(s.host);
    CHECK(of_type(bridge.handle(R"({"cmd":"account"})"), "account")[0].get("key_set")->b);
    CHECK(of_type(bridge.handle(R"({"cmd":"nxm_take"})"), "result")[0].get("ok")->b);
    bridge.handle_nxm("nxm://enshrouded/mods/170/files/1197?key=k&expires=1792799999&user_id=12345");
    CHECK(of_type(s.host.async_out, "download").back().get("state")->s == "done");
}

// ---- the release build: Nexus off (no slug, no tester switch) ----

TEST_CASE("app nexus: the Nexus mode: no slug and no switch is off, the switch a personal key, a slug SSO") {
    CHECK(nexus_mode("", false) == NexusMode::Off);
    CHECK(nexus_mode("", true) == NexusMode::PersonalKey);
    CHECK(nexus_mode("emf", false) == NexusMode::Sso);
    CHECK(nexus_mode("emf", true) == NexusMode::Sso);  // the switch then only adds the key field
    CHECK(std::string(nexus_mode_name(NexusMode::Off)) == "off");
    CHECK(std::string(nexus_mode_name(NexusMode::PersonalKey)) == "personal_key");
    CHECK(std::string(nexus_mode_name(NexusMode::Sso)) == "sso");
}

namespace {
// The public build: no slug (Setup has none) and the tester switch off.
struct OffSetup : Setup {
    OffSetup() { host.personal = false; }
};
bool refused(const std::vector<std::string>& out) {
    const auto r = of_type(out, "result");
    return r.size() == 1 && !r[0].get("ok")->b && r[0].get("error")->s.find("doesn't connect to Nexus Mods") != std::string::npos;
}
}  // namespace

TEST_CASE("app nexus: the state tells the page the Nexus mode") {
    Setup s;
    const auto nexus_of = [&]() {
        Bridge bridge(s.host);
        return parse(*bridge.state_if_changed(true)).get("nexus")->s;
    };
    CHECK(nexus_of() == "personal_key");
    s.host.personal = false;
    CHECK(nexus_of() == "off");
    s.host.slug = "emf";
    CHECK(nexus_of() == "sso");
}

TEST_CASE("app nexus: off, Browse and Check for updates are refused by the host and ask Nexus nothing") {
    OffSetup s;
    s.g.write(L"mods\\auto-loot-plus\\mod.json", R"({"id":"auto-loot-plus","name":"Auto Loot Plus","nexus_id":170})");
    Bridge bridge(s.host);
    bridge.handle(R"({"cmd":"state"})");
    CHECK(refused(bridge.handle(R"({"cmd":"nexus_search","text":"loot","seq":1})")));
    CHECK(refused(bridge.handle(R"({"cmd":"nexus_mod","id":170,"seq":2})")));
    CHECK(refused(bridge.handle(R"({"cmd":"nexus_file","mod_id":170,"file_id":1197,"seq":3})")));
    CHECK(refused(bridge.handle(R"({"cmd":"updates_check","seq":4})")));
    CHECK(s.host.async_out.empty());  // nothing went to a worker
    CHECK(s.host.graphql.empty());
    CHECK(s.host.api.empty());
    CHECK(s.host.cdn.empty());
    // with the switch, the same commands do reach (the fake) Nexus
    s.host.personal = true;
    Bridge on(s.host);
    on.handle(R"({"cmd":"state"})");
    on.handle(R"({"cmd":"nexus_search","text":"loot","seq":1})");
    on.handle(R"({"cmd":"updates_check","seq":2})");
    CHECK(s.host.graphql.size() >= 2);
}

TEST_CASE("app nexus: off, everything local stays: installing a downloaded file, the hand links to Nexus mods") {
    OffSetup s;
    s.host.testing = true;  // install_path: the test's stand-in for the file dialog
    TempGame src(L"nexus_off_src", false), keep(L"nexus_off_zip", false);
    src.write(L"mods\\bar\\mod.json", R"({"id":"bar","name":"Bar"})");
    src.write(L"mods\\bar\\src\\mod.lua", "-- lua");
    REQUIRE(app_test::make_zip(src.dir, keep.at(L"bar.zip")));
    Bridge bridge(s.host);
    bridge.handle(R"({"cmd":"state"})");
    bridge.handle("{\"cmd\":\"install_path\",\"path\":" + json_quote(utf8(keep.at(L"bar.zip"))) + "}");
    const auto plans = of_type(s.host.async_out, "install_plan");
    REQUIRE(plans.size() == 1);
    REQUIRE_MESSAGE(plans[0].get("ok")->b, plans[0].get("error")->s);
    s.host.async_out.clear();
    bridge.handle("{\"cmd\":\"install_apply\",\"plan_id\":\"" + plans[0].get("plan")->get("id")->s + "\"}");
    const auto done = of_type(s.host.async_out, "install_done");
    REQUIRE(done.size() == 1);
    CHECK(done[0].get("ok")->b);
    CHECK(s.g.has(L"mods\\bar\\src\\mod.lua"));
    // nexus_links reads a file in the game folder: no request
    CHECK(of_type(bridge.handle(R"({"cmd":"nexus_links"})"), "nexus_links").size() == 1);
    CHECK(s.host.graphql.empty());
    CHECK(s.host.api.empty());
}

TEST_CASE("app nexus: off, the account says so and a key stored by a test build is neither shown nor read") {
    OffSetup s;
    store(s);  // the user's key from a testing build
    Bridge bridge(s.host);
    const auto a = of_type(bridge.handle(R"({"cmd":"account"})"), "account")[0];
    CHECK(a.get("mode")->s == "off");
    CHECK_FALSE(a.get("key_set")->b);
    CHECK(a.get("user")->is(json::Value::Type::Null));
    CHECK_FALSE(a.get("personal_key")->b);
    CHECK_FALSE(a.get("sso")->get("available")->b);
    CHECK_FALSE(a.get("nxm")->get("available")->b);  // nothing to take: EMF could not download what the links bring
    CHECK(a.get("note")->s.empty());
    CHECK(load_account(s.appdata.at(L"nexus.key")));
}

TEST_CASE("app nexus: off, the host refuses the account commands a page script could send") {
    OffSetup s;
    store(s);
    Bridge bridge(s.host);
    CHECK(refused(bridge.handle("{\"cmd\":\"account_key\",\"key\":\"" + kKey + "\"}")));
    CHECK(refused(bridge.handle(R"({"cmd":"account_check"})")));
    CHECK(refused(bridge.handle(R"({"cmd":"sso_start"})")));
    // log out would delete the key: the user's key from a testing build stays, untouched
    CHECK(refused(bridge.handle(R"({"cmd":"account_logout"})")));
    const auto kept = load_account(s.appdata.at(L"nexus.key"));
    REQUIRE(kept);
    CHECK(kept->key == kKey);
    CHECK(kept->name == "EmberTester");
    CHECK(s.host.api.empty());
    CHECK(s.host.async_out.empty());
    CHECK(s.host.opened.empty());
}

TEST_CASE("app nexus: off, the host refuses to take the nxm:// links and to download, also with a stored key") {
    OffSetup s;
    store(s);  // Premium: with the switch this would download
    serve_zip(s);
    s.host.reg.set(L"Software\\Classes\\nxm\\shell\\open\\command", L"", L"\"C:\\Vortex\\Vortex.exe\" -d \"%1\"");
    Bridge bridge(s.host);
    CHECK(refused(bridge.handle(R"({"cmd":"nxm_take"})")));
    CHECK(nxm_owner(s.host.reg, s.host.exe_path()).label == "Vortex");
    CHECK(nxm_previous_label(s.host.reg).empty());  // no backup made either
    const auto dl = of_type(bridge.handle(R"({"cmd":"nexus_download","mod_id":170,"file_id":1197,"name":"Foo"})"), "download");
    REQUIRE(dl.size() == 1);
    CHECK(dl[0].get("state")->s == "failed");
    CHECK(dl[0].get("error")->s.find("doesn't connect to Nexus Mods") != std::string::npos);
    CHECK(s.host.api.empty());
    CHECK(s.host.cdn.empty());
    CHECK_FALSE(s.appdata.has(L"downloads\\FooMod-170-1-0-1792790875.zip"));
    CHECK_FALSE(mentions_key(bridge.handle(R"({"cmd":"account"})")));
}

TEST_CASE("app nexus: off, a Mod Manager Download link says plainly that this EMF does not download from Nexus") {
    OffSetup s;
    store(s);
    Bridge bridge(s.host);
    const auto out = bridge.handle_nxm("nxm://enshrouded/mods/170/files/1197?key=k&expires=1792799999&user_id=12345");
    const auto dl = of_type(out, "download");
    REQUIRE(dl.size() == 1);
    CHECK(dl[0].get("state")->s == "failed");  // not "need_key": there is no key to wait for
    const std::string why = dl[0].get("error")->s;
    CHECK(why.find("doesn't download from Nexus") != std::string::npos);
    CHECK(why.find("Manual Download") != std::string::npos);
    CHECK(why.find("Install a downloaded file") != std::string::npos);
    CHECK(dl[0].get("page")->s == emf::nexus::nexus_file_page(170, 1197));  // the page offers the file on Nexus
    CHECK(of_type(out, "account").empty());
    CHECK(of_type(bridge.handle(R"({"cmd":"account"})"), "account")[0].get("waiting")->is(json::Value::Type::Null));
    CHECK(s.host.api.empty());
    CHECK(s.host.cdn.empty());
    // a link for another game keeps its own reason
    const auto other = of_type(bridge.handle_nxm("nxm://skyrimspecialedition/mods/1/files/2?key=k&expires=1792799999"), "download");
    CHECK(other[0].get("error")->s.find("skyrimspecialedition") != std::string::npos);
    // and the links that came before the page listened get the same answer
    bridge.queue_nxm("nxm://enshrouded/mods/170/files/1197?key=k&expires=1792799999");
    const auto queued = of_type(bridge.handle(R"({"cmd":"state"})"), "download");
    REQUIRE(queued.size() == 1);
    CHECK(queued[0].get("error")->s.find("doesn't download from Nexus") != std::string::npos);
}

TEST_CASE("app nexus: off, links EMF took in a testing build can still be given back") {
    OffSetup s;
    s.host.reg.set(L"Software\\Classes\\nxm\\shell\\open\\command", L"", L"\"C:\\Vortex\\Vortex.exe\" -d \"%1\"");
    REQUIRE(nxm_take(s.host.reg, s.host.exe_path()).ok);  // what the testing build did
    Bridge bridge(s.host);
    const auto a = of_type(bridge.handle(R"({"cmd":"account"})"), "account")[0];
    CHECK(a.get("nxm")->get("available")->b);  // shown so the player can give them back
    CHECK(a.get("nxm")->get("ours")->b);
    CHECK(a.get("nxm")->get("previous")->s == "Vortex");
    // a link says how to make the button open Vortex again
    const auto dl = of_type(bridge.handle_nxm("nxm://enshrouded/mods/170/files/1197?key=k&expires=1792799999"), "download");
    CHECK(dl[0].get("error")->s.find("doesn't download from Nexus") != std::string::npos);
    CHECK(dl[0].get("error")->s.find("give them back to Vortex") != std::string::npos);
    const auto out = bridge.handle(R"({"cmd":"nxm_give_back"})");
    CHECK(of_type(out, "result")[0].get("ok")->b);
    CHECK(nxm_owner(s.host.reg, s.host.exe_path()).label == "Vortex");
    CHECK_FALSE(of_type(out, "account")[0].get("nxm")->get("available")->b);  // given back: the card goes away
}
