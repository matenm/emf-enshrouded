// The update check's Nexus side (src/nexus/updates.h, Client::updates, Client::changelogs): the batched request and its
// cap, the recorded reply (a hidden, a removed and a missing mod among them), failed parts of a reply, the changelogs of
// the files to take, version compares, the upload time in a Nexus archive's name, file-name matching and the rules that
// decide between "update", "can't tell", "up to date", "not on Nexus now", "gone" and "failed". No network: recorded
// replies and a fake transport.
#include <doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/json.h"
#include "nexus/client.h"
#include "nexus/updates.h"

using namespace emf::nexus;
namespace json = emf::core::json;

namespace {
std::string fixture(const char* name) {
    std::ifstream f(std::string(EMF_SOURCE_DIR) + "/tests/fixtures/nexus/" + name, std::ios::binary);
    REQUIRE_MESSAGE(f.good(), name);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

const std::vector<int64_t> kAsked = {170, 195, 154, 168, 179, 23, 159, 999999};

std::map<int64_t, ModStatus> recorded() {
    auto r = parse_updates(fixture("updates_reply.json"), kAsked);
    REQUIRE_MESSAGE(r.ok(), r.error);
    return r.value;
}

std::string header(const HttpRequest& r, const std::string& name) {
    for (const auto& [k, v] : r.headers)
        if (k == name) return v;
    return "<missing>";
}

// The ids a request asked for, from its variables.
std::vector<int64_t> asked_ids(const HttpRequest& r) {
    const auto doc = json::parse(r.body);
    REQUIRE(doc.value.has_value());
    std::vector<int64_t> out;
    for (const auto& e : doc.value->get("variables")->get("ids")->arr) out.push_back(static_cast<int64_t>(e.get("modId")->n));
    return out;
}

// A fake Nexus: every asked mod is published at version 2.0 with one MAIN file.
HttpResponse fake_reply(const HttpRequest& r) {
    std::string nodes, files;
    for (int64_t id : asked_ids(r)) {
        const std::string s = std::to_string(id);
        nodes += (nodes.empty() ? "" : ",") + std::string("{\"modId\":") + s + ",\"name\":\"Mod " + s +
                 "\",\"version\":\"2.0\",\"updatedAt\":\"2026-10-26T10:00:00Z\",\"status\":\"published\"}";
        files += ",\"f" + s + "\":[{\"fileId\":" + std::to_string(id * 10) + ",\"uid\":\"" + std::to_string(id * 10 + 7) +
                 "\",\"name\":\"Mod " + s + " 2.0\",\"version\":\"2.0\",\"category\":\"MAIN\",\"date\":1792790000,\"size\":1,"
                 "\"sizeInBytes\":\"1024\"}]";
    }
    return HttpResponse{200, "{\"data\":{\"mods\":{\"nodes\":[" + nodes + "]}" + files + "}}", ""};
}

const NexusFile* file_of(const ModStatus& m, int64_t id) {
    for (const auto& f : m.files)
        if (f.file_id == id) return &f;
    return nullptr;
}

std::vector<int64_t> ids_of(const std::vector<NexusFile>& files) {
    std::vector<int64_t> out;
    for (const auto& f : files) out.push_back(f.file_id);
    return out;
}

ModStatus mod_of(const char* version) {
    ModStatus m;
    m.mod_id = 9;
    m.found = true;
    m.status = "published";
    m.version = version;
    return m;
}

void add(ModStatus& m, int64_t id, const char* name, const char* version, const char* category, int64_t date) {
    NexusFile f;
    f.file_id = id;
    f.name = name;
    f.version = version;
    f.category = category;
    f.date = date;
    m.files.push_back(f);
}
}  // namespace

TEST_CASE("nexus updates: one request asks for up to 50 mods, the ids only as variables") {
    // the exact text sent to Nexus when this was checked against the real API: no changelogs here, the file's uid for them
    CHECK(updates_request({170, 195}) ==
          R"({"query":"query EmfUpdates($ids: [CompositeDomainWithIdInput!]!, $count: Int, $gameId: ID!, $f170: ID!, $f195: ID!) { )"
          R"(mods: legacyModsByDomain(ids: $ids, count: $count) { nodes { modId name version updatedAt status } } )"
          R"(f170: modFiles(modId: $f170, gameId: $gameId) { fileId uid name version category date size sizeInBytes } )"
          R"(f195: modFiles(modId: $f195, gameId: $gameId) { fileId uid name version category date size sizeInBytes } }",)"
          R"("variables":{"ids":[{"gameDomain":"enshrouded","modId":170},{"gameDomain":"enshrouded","modId":195}],)"
          R"("count":2,"gameId":5811,"f170":170,"f195":195}})");

    // repeated and invalid ids are dropped, the order stays
    CHECK(updates_request_ids({170, 195, 170, 0, -3, 23}) == std::vector<int64_t>{170, 195, 23});
    const auto doc = json::parse(updates_request({170, 195, 170, 0, -3, 23}));
    REQUIRE(doc.value.has_value());
    CHECK(doc.value->get("variables")->get("count")->n == 3);
    CHECK(doc.value->get("variables")->get("ids")->arr.size() == 3);

    // the cap: 60 ids, 50 asked
    std::vector<int64_t> many;
    for (int64_t i = 1; i <= 60; ++i) many.push_back(i);
    const auto capped = updates_request_ids(many);
    REQUIRE(capped.size() == kUpdatesPerRequest);
    CHECK(capped.back() == 50);
    const std::string body = updates_request(many);
    CHECK(body.find("f50: modFiles") != std::string::npos);
    CHECK(body.find("f51") == std::string::npos);
    CHECK(json::parse(body).value->get("variables")->get("count")->n == 50);

    // no ids, no mods: still a valid request (the caller does not send it)
    CHECK(json::parse(updates_request({})).value.has_value());
}

TEST_CASE("nexus updates: the recorded reply, with a hidden, a removed and a missing mod") {
    const auto mods = recorded();
    REQUIRE(mods.size() == kAsked.size());

    const ModStatus& alp = mods.at(170);
    CHECK(alp.found);
    CHECK_FALSE(alp.files_failed);
    CHECK(alp.name == "Auto Loot Plus");
    CHECK(alp.version == "1.1.8");
    CHECK(alp.status == "published");
    CHECK(alp.updated_at == "2026-10-23T21:30:04Z");
    REQUIRE(alp.files.size() == 6);
    const NexusFile* main = file_of(alp, 1197);
    REQUIRE(main);
    CHECK(main->name == "AutoLootPlus 1.1.8");
    CHECK(main->category == "MAIN");
    CHECK(main->size_bytes == 735506);
    CHECK(main->date == 1792790875);
    CHECK(main->uid == "24958054958253");
    CHECK(main->changelog.empty());  // asked for later, for the files to take only

    CHECK(mods.at(23).found);
    CHECK(mods.at(23).status == "hidden");
    CHECK(mods.at(23).files.size() == 2);  // a hidden mod still lists its files
    CHECK(mods.at(159).status == "removed");

    // an id Nexus does not have: left out of legacyModsByDomain, modFiles is []
    CHECK_FALSE(mods.at(999999).found);
    CHECK(mods.at(999999).files.empty());

    // an asked id the reply does not mention at all is "not found" too
    const auto more = parse_updates(fixture("updates_reply.json"), {170, 5});
    REQUIRE(more.ok());
    CHECK_FALSE(more.value.at(5).found);
    CHECK(more.value.at(170).found);
}

TEST_CASE("nexus updates: failed replies are error texts; a failed part marks its mod, never a wrong answer") {
    CHECK(parse_updates("", {1}, 500).error.find("not available") != std::string::npos);
    CHECK(parse_updates("", {1}, 429).error.find("too many requests") != std::string::npos);
    CHECK(parse_updates("<html>", {1}).error.find("not JSON") != std::string::npos);
    const auto complex = parse_updates(fixture("error_complexity.json"), {1});
    CHECK_FALSE(complex.ok());
    CHECK(complex.error.find("complexity") != std::string::npos);
    const auto none = parse_updates(R"({"errors":[{"message":"Mod not found"}],"data":null})", {1});
    CHECK(none.error == "Nexus: Mod not found");
    CHECK_FALSE(parse_updates(R"({"data":{}})", {1}).ok());  // neither the mods nor their files

    // one mod's files failed (errors[].path names its alias): that mod is "failed", the rest of the answer stays
    const auto partial = parse_updates(
        R"({"errors":[{"message":"boom","path":["f2"]}],"data":{"mods":{"nodes":[{"modId":1,"name":"A","version":"1.0","status":"published"},{"modId":2,"name":"B","version":"1.0","status":"published"}]},"f1":[{"fileId":10,"name":"A 1.0","version":"1.0","category":"MAIN","date":5}],"f2":null}})",
        {1, 2});
    REQUIRE_MESSAGE(partial.ok(), partial.error);
    CHECK(partial.value.at(1).files.size() == 1);
    CHECK_FALSE(partial.value.at(1).files_failed);
    CHECK(partial.value.at(2).files_failed);
    Installed in;
    in.version = "0.9";
    const auto failed = decide_update(in, partial.value.at(2));
    CHECK(failed.state == UpdateState::Failed);
    CHECK_FALSE(failed.file);
    CHECK(decide_update(in, partial.value.at(1)).state == UpdateState::Update);

    // files but no word about the mod itself: not checked, not "gone"
    const auto no_node = parse_updates(
        R"({"data":{"mods":{"nodes":[]},"f1":[{"fileId":10,"name":"A 1.0","version":"1.0","category":"MAIN","date":5}]}})", {1});
    REQUIRE(no_node.ok());
    CHECK(decide_update(in, no_node.value.at(1)).state == UpdateState::Failed);

    // the mods' own part failed: files alone would make every mod look gone from Nexus, so it is an error
    const auto no_mods = parse_updates(
        R"({"errors":[{"message":"boom","path":["mods"]}],"data":{"mods":null,"f1":[{"fileId":10,"name":"A 1.0","version":"1.0","category":"MAIN","date":5}]}})",
        {1});
    CHECK_FALSE(no_mods.ok());
    CHECK(no_mods.error.find("no mods") != std::string::npos);
}

TEST_CASE("nexus updates: the changelogs of the files to take, in one more request") {
    // the exact text checked against the real API (modFilesByUid leaves out a uid it does not know)
    CHECK(changelogs_request({"24958054958253", "24958054958360"}) ==
          R"({"query":"query EmfChangelogs($uids: [ID!]!, $count: Int) { files: modFilesByUid(uids: $uids, count: $count) { )"
          R"(nodes { uid fileId modId changelogText } } }","variables":{"uids":["24958054958253","24958054958360"],"count":2}})");
    // only uids (digits), each once, at most 50
    std::vector<std::string> uids = {"1", "1", "x\"y", "", "2"};
    for (int i = 3; i <= 60; ++i) uids.push_back(std::to_string(i));
    CHECK(changelogs_request_uids(uids).size() == kChangelogsPerRequest);
    CHECK(changelogs_request_uids({"1", "1", "x\"y", "", "2"}) == std::vector<std::string>{"1", "2"});

    const auto logs = parse_changelogs(fixture("changelogs_reply.json"));
    REQUIRE_MESSAGE(logs.ok(), logs.error);
    REQUIRE(logs.value.size() == 2);
    REQUIRE(logs.value.at("24958054958253").size() == 1);
    CHECK(logs.value.at("24958054958253")[0].rfind("- Fixed:", 0) == 0);
    CHECK(logs.value.count("24958054958360") == 1);
    CHECK_FALSE(parse_changelogs("", 503).ok());
    CHECK_FALSE(parse_changelogs(R"({"data":{"files":null}})").ok());

    // a long changelog is cut: 30 lines, 300 bytes each, never inside a UTF-8 character
    std::string lines;
    for (int i = 0; i < 40; ++i) lines += std::string(i ? "," : "") + "\"" + (i == 0 ? std::string(299, 'x') + "\xC3\xA9" + "yyy" : "line") + "\"";
    const auto cut = parse_changelogs(R"({"data":{"files":{"nodes":[{"uid":"7","fileId":1,"modId":1,"changelogText":[)" + lines + "]}]}}}");
    REQUIRE(cut.ok());
    const auto& log = cut.value.at("7");
    CHECK(log.size() == kChangelogLines);
    CHECK(log[0] == std::string(299, 'x'));

    // the client: one request, nothing for no uids
    std::vector<HttpRequest> sent;
    Client c("0.3.0", [&](const HttpRequest& r) {
        sent.push_back(r);
        return HttpResponse{200, fixture("changelogs_reply.json"), ""};
    });
    CHECK(c.changelogs({}).ok());
    CHECK(sent.empty());
    const auto got = c.changelogs({"24958054958253", "24958054958360"});
    REQUIRE(got.ok());
    CHECK(sent.size() == 1);
    CHECK(header(sent[0], "Application-Name") == "EMF");
    CHECK(got.value.size() == 2);
}

TEST_CASE("nexus updates: versions compare SemVer-ish, and say so when they cannot") {
    CHECK(compare_versions("1.1.7", "1.1.8") == -1);
    CHECK(compare_versions("1.1.8", "1.1.8") == 0);
    CHECK(compare_versions("v1.2", "1.2.0") == 0);
    CHECK(compare_versions("1", "1.0.0") == 0);
    CHECK(compare_versions("1.10", "1.9") == 1);
    CHECK(compare_versions("V2", "1.9") == 1);
    CHECK(compare_versions("1.2.3.4", "1.2.3.5") == -1);
    CHECK(compare_versions(" 1.2 ", "1.2") == 0);
    CHECK(compare_versions("1.0.0+build5", "1.0.0") == 0);
    // pre-releases: only the known words (alpha, beta, rc, pre, preview, dev), in that word's order
    CHECK(compare_versions("1.0.0-beta", "1.0.0") == -1);
    CHECK(compare_versions("1.0.0-alpha", "1.0.0-beta") == -1);
    CHECK(compare_versions("1.0.0-beta.2", "1.0.0-beta.11") == -1);
    CHECK(compare_versions("1.0-beta2", "1.0-beta10") == -1);
    CHECK(compare_versions("1.0-RC1", "1.0") == -1);
    CHECK(compare_versions("1.0-dev", "1.0-alpha") == -1);
    CHECK(compare_versions("1.0-rc1", "1.0-preview") == 1);
    CHECK(compare_versions("1.0-pre", "1.0-preview") == 0);
    // any other suffix is not a pre-release EMF can order: "1.0-hotfix", "1.2-3"
    CHECK_FALSE(compare_versions("1.0-hotfix", "1.0").has_value());
    CHECK_FALSE(compare_versions("1.2-3", "1.2").has_value());
    CHECK_FALSE(compare_versions("1.0-", "1.0").has_value());
    // what does not parse is never "older" or "newer"
    CHECK_FALSE(compare_versions("1.0b", "1.0").has_value());
    CHECK_FALSE(compare_versions("Beta 2", "1").has_value());
    CHECK_FALSE(compare_versions("", "1").has_value());
    CHECK_FALSE(compare_versions("1..2", "1").has_value());
    CHECK_FALSE(compare_versions("1.0", "E.g.729115").has_value());
    CHECK_FALSE(compare_versions("1.", "1").has_value());
}

TEST_CASE("nexus updates: the upload time in a Nexus download's name") {
    CHECK(archive_upload_time("AutoLootPlus 1.1.8-170-1-1-8-1792790875.zip") == 1792790875);
    CHECK(archive_upload_time("ShroudDig - Standalone version-179-1-1-1792428967.7z") == 1792428967);
    CHECK(archive_upload_time("Foo-12-1792790875.RAR") == 1792790875);
    CHECK(archive_upload_time("AutoLootPlus 1.1.8.zip") == 0);
    CHECK(archive_upload_time("Foo-1792790875.zip") == 0);       // no mod id before it
    CHECK(archive_upload_time("Foo-170-1-0-5.zip") == 0);        // not a time
    CHECK(archive_upload_time("Foo-170-1-0-1792790875") == 0);   // not an archive's name
    CHECK(archive_upload_time("") == 0);
}

TEST_CASE("nexus updates: file names are compared by their words, without numbers") {
    CHECK(file_name_key("ShroudHud 1.7 - Standalone (GV 1.0.0.3)") == "shroud hud standalone gv");
    CHECK(file_name_key("AutoLootPlus 1.1.8-170-1-1-8-1792790875.zip") == "auto loot plus");
    CHECK(file_name_key("Auto Loot Plus") == "auto loot plus");
    CHECK(file_name_key("EnshroudedMultiDLLInstaller") == "enshrouded multi dll installer");
    CHECK(file_name_key("Quick Deposit v1.3.12") == "quick deposit");
    CHECK(file_name_key("M182 Server EN 2.5") == "m182 server en");
    CHECK(file_name_key("") == "");
    CHECK(file_name_key("AutoLootPlus 1.1.7") == file_name_key("AutoLootPlus 1.1.8"));

    // picking among several main files by an archive's name: a file's own words must all be in it
    ModStatus m = mod_of("2.0");
    add(m, 1, "Bar", "2.0", "MAIN", 100);
    add(m, 2, "Bar B", "2.0", "MAIN", 200);
    Installed in;
    in.version = "1.0";
    in.archive = "Foo.zip";  // shares no word with either: the player chooses
    auto d = decide_update(in, m);
    CHECK(d.state == UpdateState::Update);
    CHECK_FALSE(d.file);
    CHECK(d.choices.size() == 2);
    in.archive = "Bar 1.0.zip";  // "B" is not in it: the plain one
    d = decide_update(in, m);
    REQUIRE(d.file);
    CHECK(d.file->file_id == 1);
    in.archive = "Bar B 1.0.zip";  // both fit; the one with more of its own words
    d = decide_update(in, m);
    REQUIRE(d.file);
    CHECK(d.file->file_id == 2);
    add(m, 3, "Bar 2.0 again", "2.0", "MAIN", 300);  // "again" is its own word: not the plain one any more
    add(m, 4, "Bar 2.0", "2.0", "MAIN", 400);        // two uploads of "Bar": the newest
    in.archive = "Bar 1.0.zip";
    d = decide_update(in, m);
    REQUIRE(d.file);
    CHECK(d.file->file_id == 4);
    in.archive = "Foo.zip";  // the choices list each file once, its newest upload
    d = decide_update(in, m);
    CHECK_FALSE(d.file);
    CHECK(ids_of(d.choices) == std::vector<int64_t>{4, 3, 2});

    // the mod's own name only counts when it has a file's own word: "Better Map" is the name of the Compact variant too
    ModStatus map = mod_of("1.1");
    add(map, 1, "Better Map 1.1", "1.1", "MAIN", 1000);
    add(map, 2, "Better Map Compact 1.1", "1.1", "MAIN", 1010);
    Installed hand;
    hand.version = "1.0";
    hand.name = "Better Map";
    d = decide_update(hand, map);
    CHECK(d.state == UpdateState::Update);
    CHECK_FALSE(d.file);
    CHECK(ids_of(d.choices) == std::vector<int64_t>{2, 1});
    hand.name = "Better Map Compact";
    d = decide_update(hand, map);
    REQUIRE(d.file);
    CHECK(d.file->file_id == 2);

    // two builds named alike where the newest upload is not the highest version (a legacy build uploaded last): choose
    ModStatus builds = mod_of("2.0");
    add(builds, 1, "MyMod 2.0 (GV 1.0.0.3)", "2.0", "MAIN", 1000);
    add(builds, 2, "MyMod 1.9 (GV 0.9.9)", "1.9", "MAIN", 5000);
    Installed legacy;
    legacy.version = "1.5";
    legacy.archive = "MyMod 1.5 (GV 0.9.9)-9-1-5-500.zip";
    d = decide_update(legacy, builds);
    CHECK(d.state == UpdateState::Update);
    CHECK_FALSE(d.file);
    CHECK(ids_of(d.choices) == std::vector<int64_t>{2, 1});
}

TEST_CASE("nexus updates: the file id is known") {
    const auto mods = recorded();
    Installed in;

    SUBCASE("the installed file is archived: the main file is named") {
        in.file_id = 1131;  // AutoLootPlus 1.1.7
        const auto d = decide_update(in, mods.at(170));
        CHECK(d.state == UpdateState::Update);
        REQUIRE(d.file);
        CHECK(d.file->file_id == 1197);
        REQUIRE(d.installed_file);
        CHECK(d.installed_file->name == "AutoLootPlus 1.1.7");
        CHECK(d.reason.find("archived") != std::string::npos);
        CHECK(d.nexus_version == "1.1.8");
    }
    SUBCASE("an optional variant stays that variant (Lite)") {
        in.file_id = 1132;  // AutoLootPlus Lite 1.1.7, archived
        const auto d = decide_update(in, mods.at(170));
        CHECK(d.state == UpdateState::Update);
        REQUIRE(d.file);
        CHECK(d.file->file_id == 1198);  // AutoLootPlus Lite 1.1.8, not the main file
    }
    SUBCASE("the installed file is the current main file") {
        in.file_id = 1197;
        const auto d = decide_update(in, mods.at(170));
        CHECK(d.state == UpdateState::Current);
        CHECK_FALSE(d.file);
    }
    SUBCASE("several main files: the one most like the installed file") {
        in.file_id = 932;  // ShroudHud 1.6 - For ModLoader (archived)
        auto d = decide_update(in, mods.at(154));
        CHECK(d.state == UpdateState::Update);
        REQUIRE(d.file);
        CHECK(d.file->file_id == 1051);  // ShroudHud 1.7 - ModLoader (GV 1.0.0.3)
        in.file_id = 950;                // ShroudHud 1.7 - Standalone (an old version now)
        d = decide_update(in, mods.at(154));
        CHECK(d.state == UpdateState::Update);
        REQUIRE(d.file);
        CHECK(d.file->file_id == 1050);
        CHECK(d.reason.find("old version") != std::string::npos);
    }
    SUBCASE("a retired file whose main file is older is not an update (ShroudHud 1.8 Beta, archived; mains are 1.7)") {
        in.file_id = 1054;
        const auto d = decide_update(in, mods.at(154));
        CHECK(d.state == UpdateState::Maybe);
        CHECK(d.reason.find("older") != std::string::npos);
        CHECK(d.reason.find("1.7") != std::string::npos);
    }
    SUBCASE("a rollback (1.2 archived, 1.1 the main file again) is not an update") {
        ModStatus m = mod_of("1.1");
        add(m, 1, "MyMod 1.2", "1.2", "ARCHIVED", 100000);
        add(m, 2, "MyMod 1.1", "1.1", "MAIN", 900000);
        in.file_id = 1;
        const auto d = decide_update(in, m);
        CHECK(d.state == UpdateState::Maybe);
        CHECK(d.reason.find("older") != std::string::npos);
    }
    SUBCASE("a removed file counts as retired") {
        ModStatus m = mod_of("2.0");
        add(m, 1, "MyMod 1.0", "1.0", "REMOVED", 1000);
        add(m, 2, "MyMod 2.0", "2.0", "MAIN", 9000);
        in.file_id = 1;
        const auto d = decide_update(in, m);
        CHECK(d.state == UpdateState::Update);
        REQUIRE(d.file);
        CHECK(d.file->file_id == 2);
        CHECK(d.reason.find("removed") != std::string::npos);
    }
    SUBCASE("a re-upload of the same version replaces a retired file (the recorded mod 23's shape)") {
        ModStatus m = mod_of("1.1");
        add(m, 1, "All Materials (Mostly)", "1.1", "OLD_VERSION", 1000);
        add(m, 2, "All Materials (Mostly)", "1.1", "MAIN", 1700);
        in.file_id = 1;
        const auto d = decide_update(in, m);
        CHECK(d.state == UpdateState::Update);
        REQUIRE(d.file);
        CHECK(d.file->file_id == 2);
    }
    SUBCASE("a variant uploaded with the installed one is not an update") {
        in.file_id = 1050;  // ShroudHud Standalone; the ModLoader variant came 36 seconds later, the same version
        CHECK(decide_update(in, mods.at(154)).state == UpdateState::Current);
    }
    SUBCASE("a hotfix within the hour under another name is not taken for a variant") {
        ModStatus m = mod_of("1.2.1");
        add(m, 1, "MyMod 1.2", "1.2", "MAIN", 100000);
        add(m, 2, "MyMod 1.2.1 Hotfix", "1.2.1", "MAIN", 100000 + 1200);
        in.file_id = 1;
        const auto d = decide_update(in, m);
        CHECK(d.state == UpdateState::Maybe);
        CHECK(ids_of(d.choices) == std::vector<int64_t>{2});
    }
    SUBCASE("a newer update file and, for an optional file, newer files under other names are not ignored") {
        ModStatus patch = mod_of("1.3");
        add(patch, 1, "MyMod 1.2", "1.2", "MAIN", 100000);
        add(patch, 2, "MyMod 1.3 patch", "1.3", "UPDATE", 900000);
        in.file_id = 1;
        auto d = decide_update(in, patch);
        CHECK(d.state == UpdateState::Maybe);
        CHECK(ids_of(d.choices) == std::vector<int64_t>{2});
        ModStatus lite = mod_of("2.0");
        add(lite, 1, "Foo Lite 1.0", "1.0", "OPTIONAL", 1000);
        add(lite, 2, "Foo 2.0", "2.0", "MAIN", 900000);
        d = decide_update(in, lite);
        CHECK(d.state == UpdateState::Maybe);
        CHECK(ids_of(d.choices) == std::vector<int64_t>{2});
        // an older build uploaded later under the same name is not an update either
        ModStatus old = mod_of("2.0");
        add(old, 1, "Foo 2.0", "2.0", "MAIN", 1000);
        add(old, 2, "Foo 1.9", "1.9", "MAIN", 9000);
        CHECK(decide_update(in, old).state == UpdateState::Current);
    }
    SUBCASE("newer main files with other names: EMF can't tell, and lists them") {
        in.file_id = 1265;  // M182 Server EN 2.5, still a main file; the 3.0 files are named differently
        const auto d = decide_update(in, mods.at(168));
        CHECK(d.state == UpdateState::Maybe);
        CHECK_FALSE(d.file);
        CHECK(ids_of(d.choices) == std::vector<int64_t>{1311, 1310});  // not 1266, uploaded with it at the same version
    }
    SUBCASE("a same-named file is an update only with a newer version") {
        ModStatus m = mod_of("1.0");
        add(m, 10, "Foo Pack Part 1", "1.0", "MAIN", 1000);
        add(m, 11, "Foo Pack Part 2", "1.0", "MAIN", 9000);  // the same words without numbers, but not a newer version
        in.file_id = 10;
        auto d = decide_update(in, m);
        CHECK(d.state == UpdateState::Maybe);
        CHECK_FALSE(d.file);
        CHECK(ids_of(d.choices) == std::vector<int64_t>{11});
        add(m, 12, "Foo Pack Part 1", "1.1", "MAIN", 9500);  // a newer upload of the installed file
        d = decide_update(in, m);
        CHECK(d.state == UpdateState::Update);
        REQUIRE(d.file);
        CHECK(d.file->file_id == 12);
    }
    SUBCASE("several main files, none more alike: the player chooses") {
        in.file_id = 1;  // not on Nexus any more, and nothing to match
        const auto d = decide_update(in, mods.at(179));
        CHECK(d.state == UpdateState::Update);
        CHECK_FALSE(d.file);
        CHECK(ids_of(d.choices) == std::vector<int64_t>{1049, 1048});
        CHECK(d.reason.find("no longer on Nexus") != std::string::npos);
    }
    SUBCASE("the installed file is gone from Nexus: matched by the archive's name") {
        in.file_id = 1000;
        in.archive = "ShroudDig - Standalone version-179-1-0-1792416638.zip";
        const auto d = decide_update(in, mods.at(179));
        CHECK(d.state == UpdateState::Update);
        REQUIRE(d.file);
        CHECK(d.file->file_id == 1049);
    }
    SUBCASE("a Nexus archive's upload time names the installed file (a file picked by hand)") {
        in.archive = "ShroudDig - Standalone version-179-1-1-1792428967.zip";  // file 974, an old version now
        in.version = "1.1";
        auto d = decide_update(in, mods.at(179));
        REQUIRE(d.installed_file);
        CHECK(d.installed_file->file_id == 974);
        CHECK(d.state == UpdateState::Update);
        REQUIRE(d.file);
        CHECK(d.file->file_id == 1049);
        in.archive_names_file = false;  // the files changed since: the archive says nothing about them
        d = decide_update(in, mods.at(179));
        CHECK_FALSE(d.installed_file);
    }
}

TEST_CASE("nexus updates: only the version is known") {
    const auto mods = recorded();
    Installed in;
    in.name = "Quick Deposit";

    in.version = "1.3.0";
    auto d = decide_update(in, mods.at(195));
    CHECK(d.state == UpdateState::Update);
    REQUIRE(d.file);
    CHECK(d.file->file_id == 1304);
    CHECK(d.reason.find("1.3.0") != std::string::npos);
    CHECK(d.reason.find("1.3.12") != std::string::npos);

    for (const char* same : {"1.3.12", "v1.3.12", "1.3.13"}) {
        in.version = same;
        CHECK(decide_update(in, mods.at(195)).state == UpdateState::Current);
    }

    in.version = "1.3b";  // does not parse: "installed X, Nexus has Y", not an update
    d = decide_update(in, mods.at(195));
    CHECK(d.state == UpdateState::Maybe);
    CHECK(d.reason.find("1.3b") != std::string::npos);
    CHECK(d.reason.find("1.3.12") != std::string::npos);

    in.version = "";
    d = decide_update(in, mods.at(195));
    CHECK(d.state == UpdateState::Maybe);
    CHECK(d.reason.find("doesn't know which version") != std::string::npos);

    // several main files, nothing to tell them apart: an update, and the player chooses
    in = Installed{};
    in.name = "ShrouDig - A Pickaxe and Rake companion";
    in.version = "1.0";
    d = decide_update(in, mods.at(179));
    CHECK(d.state == UpdateState::Update);
    CHECK_FALSE(d.file);
    CHECK(ids_of(d.choices) == std::vector<int64_t>{1049, 1048});
    // with the archive's name, the matching one
    in.archive = "ShroudDig - ModLoader version-179-1-0-1792416603.zip";
    d = decide_update(in, mods.at(179));
    REQUIRE(d.file);
    CHECK(d.file->file_id == 1048);

    // the page's version lags its files: the page says 1.1, the main files are 1.2 (the recorded ShroudDig)
    in = Installed{};
    in.name = "ShrouDig - A Pickaxe and Rake companion";
    in.version = "1.1";
    d = decide_update(in, mods.at(179));
    CHECK(d.state == UpdateState::Update);
    CHECK(d.nexus_version == "1.2");
    CHECK(ids_of(d.choices) == std::vector<int64_t>{1049, 1048});
    // and the same when the page lags one upload of a single file line
    ModStatus lag = mod_of("1.0");
    add(lag, 1, "Foo 1.0", "1.0", "MAIN", 1000);
    add(lag, 2, "Foo 1.1", "1.1", "MAIN", 5000);
    Installed foo;
    foo.version = "1.0";
    d = decide_update(foo, lag);
    CHECK(d.state == UpdateState::Update);
    REQUIRE(d.file);
    CHECK(d.file->file_id == 2);

    // the page says 1.8 but its main files are 1.7, the version installed: can't tell
    in = Installed{};
    in.version = "1.7";
    in.archive = "ShroudHud 1.7 - Standalone (GV 1.0.0.3).zip";
    d = decide_update(in, mods.at(154));
    CHECK(d.state == UpdateState::Maybe);
    CHECK(d.reason.find("1.8") != std::string::npos);
    CHECK(d.reason.find("1.7") != std::string::npos);
    // with Nexus's own archive name (its upload time), EMF knows the very file: Nexus's current one
    in.archive = "ShroudHud 1.7 - Standalone (GV 1.0.0.3)-154-1-7-1792587097.zip";
    CHECK(decide_update(in, mods.at(154)).state == UpdateState::Current);

    // a dash suffix that is not a known pre-release: can't tell
    ModStatus hot = mod_of("1.0-hotfix");
    add(hot, 2, "MyMod 1.0-hotfix", "1.0-hotfix", "MAIN", 900000);
    foo.version = "1.0";
    CHECK(decide_update(foo, hot).state == UpdateState::Maybe);

    // only optional files and the mod's name says nothing: choose
    ModStatus opt = mod_of("1.1");
    add(opt, 1, "Foo 1.1", "1.1", "OPTIONAL", 1000);
    add(opt, 2, "Foo Source 1.1", "1.1", "OPTIONAL", 1010);
    foo.name = "Foo";
    d = decide_update(foo, opt);
    CHECK(d.state == UpdateState::Update);
    CHECK(d.choices.size() == 2);
}

TEST_CASE("nexus updates: a mod gone from Nexus, or not on it now, says so") {
    const auto mods = recorded();
    Installed in;
    in.version = "1.0";
    auto d = decide_update(in, mods.at(23));
    CHECK(d.state == UpdateState::Unavailable);
    CHECK(d.reason.find("hidden") != std::string::npos);
    d = decide_update(in, mods.at(159));
    CHECK(d.state == UpdateState::Gone);
    CHECK(d.reason.find("removed") != std::string::npos);
    d = decide_update(in, mods.at(999999));
    CHECK(d.state == UpdateState::Gone);
    CHECK(d.reason.find("no mod") != std::string::npos);

    ModStatus m = mod_of("2.0");
    m.status = "wastebinned";
    CHECK(decide_update(in, m).state == UpdateState::Gone);
    for (const char* status : {"not_published", "under_moderation"}) {
        m.status = status;
        CHECK(decide_update(in, m).state == UpdateState::Unavailable);
    }
    m.status = "published";  // published, but no files to download
    d = decide_update(in, m);
    CHECK(d.state == UpdateState::Maybe);
    CHECK(d.reason.find("no files") != std::string::npos);
    CHECK(std::string(update_state_name(UpdateState::Update)) == "update");
    CHECK(std::string(update_state_name(UpdateState::Maybe)) == "maybe");
    CHECK(std::string(update_state_name(UpdateState::Unavailable)) == "unavailable");
    CHECK(std::string(update_state_name(UpdateState::Failed)) == "failed");
}

TEST_CASE("nexus client: the update check batches 50 mods a request, 4 requests at most") {
    std::vector<HttpRequest> sent;
    Client c("0.2.0", [&](const HttpRequest& r) {
        sent.push_back(r);
        return fake_reply(r);
    });
    std::vector<int64_t> ids;
    for (int64_t i = 1; i <= 120; ++i) ids.push_back(i);
    const auto r = c.updates(ids);
    REQUIRE_MESSAGE(r.ok(), r.error);
    REQUIRE(sent.size() == 3);
    CHECK(asked_ids(sent[0]).size() == 50);
    CHECK(asked_ids(sent[1]).front() == 51);
    CHECK(asked_ids(sent[2]).size() == 20);
    CHECK(r.value.requests == 3);
    CHECK(r.value.mods.size() == 120);
    CHECK(r.value.mods.at(120).files.at(0).file_id == 1200);
    CHECK(r.value.skipped.empty());
    for (const auto& s : sent) {
        CHECK(s.url == "https://api.nexusmods.com/v2/graphql");
        CHECK(header(s, "Application-Name") == "EMF");
        CHECK(header(s, "Application-Version") == "0.2.0");
    }

    // past the cap: 200 asked, the rest skipped (and said)
    sent.clear();
    ids.clear();
    for (int64_t i = 1; i <= 260; ++i) ids.push_back(i);
    const auto big = c.updates(ids);
    REQUIRE(big.ok());
    CHECK(sent.size() == kUpdateRequestsMax);
    CHECK(big.value.mods.size() == 200);
    CHECK(big.value.skipped.size() == 60);
    CHECK(big.value.skipped.front() == 201);

    // nothing to ask: no request
    sent.clear();
    CHECK(c.updates({}).ok());
    CHECK(sent.empty());
}

TEST_CASE("nexus client: the update check stops on cancel, the deadline and a failed request") {
    int calls = 0;
    std::atomic<bool> cancel{false};
    Client c("0.2.0", [&](const HttpRequest& r) {
        ++calls;
        CHECK(r.cancel == &cancel);  // the transport can stop a request that hangs
        if (calls == 2) return HttpResponse{0, "", "send: timed out (12002)"};
        return fake_reply(r);
    });
    std::vector<int64_t> ids;
    for (int64_t i = 1; i <= 120; ++i) ids.push_back(i);
    const auto r = c.updates(ids, &cancel);
    CHECK(r.error == "could not reach Nexus: send: timed out (12002)");
    CHECK(r.value.mods.size() == 50);  // the first request's answer stays
    CHECK(r.value.requests == 2);

    calls = 0;
    cancel = true;
    CHECK(c.updates(ids, &cancel).error == "cancelled");
    CHECK(c.changelogs({"1"}, &cancel).error == "cancelled");
    CHECK(calls == 0);

    cancel = false;
    CHECK(c.updates(ids, &cancel, std::chrono::milliseconds(0)).error == "timed out");
    CHECK(calls == 0);

    Client busy("0.2.0", [](const HttpRequest&) { return HttpResponse{503, "", ""}; });
    CHECK(busy.updates({1}).error.find("not available") != std::string::npos);
}
