// Nexus Mods' public GraphQL API: request bodies (user text only in variables) and parsers for the replies. The
// replies in tests/fixtures/nexus/ are real ones, trimmed (see the fixture notes in
// test_nexus_classify.cpp); the mod authors' summaries and descriptions in them are replaced with invented text.
#include <doctest.h>

#include <fstream>
#include <sstream>
#include <string>

#include "core/json.h"
#include "nexus/api.h"

using namespace emf::nexus;
using emf::core::json::Value;

namespace {
std::string fixture(const char* name) {
    std::ifstream f(std::string(EMF_SOURCE_DIR) + "/tests/fixtures/nexus/" + name, std::ios::binary);
    REQUIRE_MESSAGE(f.good(), name);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

Value parsed(const std::string& body) {
    auto r = emf::core::json::parse(body);
    REQUIRE_MESSAGE(r.value.has_value(), r.error);
    return *r.value;
}

const Value& at(const Value& v, const char* key) {
    const Value* m = v.get(key);
    REQUIRE_MESSAGE(m != nullptr, key);
    return *m;
}
}  // namespace

TEST_CASE("nexus requests: search puts the text, game, sort and page into variables") {
    SearchQuery q;
    q.text = "loot";
    q.count = 20;
    q.offset = 40;
    const Value body = parsed(search_request(q));
    const std::string query = at(body, "query").s;
    CHECK(query.find("mods(filter: $filter, sort: $sort, count: $count, offset: $offset)") != std::string::npos);
    CHECK(query.find("loot") == std::string::npos);  // the text is not in the query
    for (const char* field : {"modId", "name", "summary", "version", "downloads", "endorsements", "updatedAt",
                              "createdAt", "category", "pictureUrl", "thumbnailUrl", "adultContent", "author",
                              "uploader { name }", "totalCount"})
        CHECK_MESSAGE(query.find(field) != std::string::npos, field);
    const Value& vars = at(body, "variables");
    const Value& filter = at(vars, "filter");
    const Value& game = at(filter, "gameDomainName");
    REQUIRE(game.arr.size() == 1);
    CHECK(at(game.arr[0], "value").s == "enshrouded");
    CHECK(at(game.arr[0], "op").s == "EQUALS");
    const Value& name = at(filter, "name");
    REQUIRE(name.arr.size() == 1);
    CHECK(at(name.arr[0], "value").s == "loot");  // as typed: Nexus finds nothing for "*loot*"
    CHECK(at(name.arr[0], "op").s == "WILDCARD");
    CHECK(filter.get("categoryName") == nullptr);
    const Value& sort = at(vars, "sort");
    REQUIRE(sort.arr.size() == 1);
    CHECK(at(at(sort.arr[0], "downloads"), "direction").s == "DESC");
    CHECK(at(vars, "count").n == 20);
    CHECK(at(vars, "offset").n == 40);
}

TEST_CASE("nexus requests: quotes, backslashes and braces in the search text cannot break the request") {
    SearchQuery q;
    q.text = "a\"b\\c} { mods { name } } \n\t";
    const Value body = parsed(search_request(q));
    const Value& name = at(at(at(body, "variables"), "filter"), "name");
    CHECK(at(name.arr[0], "value").s == "a\"b\\c} { mods { name } }");  // trimmed, otherwise unchanged
    CHECK(at(body, "query").s.find("mods { name }") == std::string::npos);
}

TEST_CASE("nexus requests: no text means no name filter; a category, other sorts and limits") {
    SearchQuery q;
    q.text = "   ";
    q.category = "Visuals";
    q.sort = SortBy::Updated;
    q.descending = false;
    q.count = 5000;
    q.offset = -3;
    const Value vars = at(parsed(search_request(q)), "variables");
    CHECK(at(vars, "filter").get("name") == nullptr);
    CHECK(at(at(at(vars, "filter"), "categoryName").arr[0], "value").s == "Visuals");
    CHECK(at(at(at(vars, "sort").arr[0], "updatedAt"), "direction").s == "ASC");
    CHECK(at(vars, "count").n == 100);
    CHECK(at(vars, "offset").n == 0);

    q.sort = SortBy::Endorsements;
    CHECK(at(parsed(search_request(q)), "variables").get("sort")->arr[0].get("endorsements") != nullptr);
    q.sort = SortBy::Created;
    CHECK(at(parsed(search_request(q)), "variables").get("sort")->arr[0].get("createdAt") != nullptr);
    q.sort = SortBy::Name;
    CHECK(at(parsed(search_request(q)), "variables").get("sort")->arr[0].get("name") != nullptr);
    q.count = 0;
    CHECK(at(at(parsed(search_request(q)), "variables"), "count").n == 1);
}

TEST_CASE("nexus requests: a very long search text is cut (at a character boundary)") {
    SearchQuery q;
    q.text = std::string(199, 'a') + "\xC3\xA9" + std::string(50, 'b');  // a cut at 200 would split the 2-byte 'e'
    const Value vars = at(parsed(search_request(q)), "variables");
    CHECK(at(at(vars, "filter"), "name").arr[0].get("value")->s == std::string(199, 'a'));
}

TEST_CASE("nexus requests: sort names") {
    CHECK(sort_from_name("downloads") == SortBy::Downloads);
    CHECK(sort_from_name("endorsements") == SortBy::Endorsements);
    CHECK(sort_from_name("updated") == SortBy::Updated);
    CHECK(sort_from_name("created") == SortBy::Created);
    CHECK(sort_from_name("name") == SortBy::Name);
    CHECK_FALSE(sort_from_name("Downloads; drop").has_value());
}

TEST_CASE("nexus requests: one mod, its files and an archive's paths, ids as variables") {
    const Value mod = parsed(mod_request(195));
    CHECK(at(mod, "query").s.find("mod(modId: $modId, gameId: $gameId)") != std::string::npos);
    CHECK(at(mod, "query").s.find("description") != std::string::npos);
    CHECK(at(at(mod, "variables"), "modId").n == 195);
    CHECK(at(at(mod, "variables"), "gameId").n == 5811);

    const Value files = parsed(files_request(170));
    const std::string fq = at(files, "query").s;
    CHECK(fq.find("modFiles(modId: $modId, gameId: $gameId)") != std::string::npos);
    for (const char* field : {"fileId", "name", "version", "category", "size", "sizeInBytes", "date", "primary"})
        CHECK_MESSAGE(fq.find(field) != std::string::npos, field);
    CHECK(at(at(files, "variables"), "modId").n == 170);

    const Value c = parsed(contents_request(170, 973, 200, 400));
    CHECK(at(c, "query").s.find("modFileContents(filter: $filter, count: $count, offset: $offset)") !=
          std::string::npos);
    const Value& f = at(at(c, "variables"), "filter");
    CHECK(at(at(f, "gameId").arr[0], "value").n == 5811);  // Int, not a string
    CHECK(at(at(f, "gameId").arr[0], "op").s == "EQUALS");
    CHECK(at(at(f, "modId").arr[0], "value").n == 170);
    CHECK(at(at(f, "fileId").arr[0], "value").n == 973);
    CHECK(at(at(c, "variables"), "count").n == 200);
    CHECK(at(at(c, "variables"), "offset").n == 400);
    CHECK(at(at(parsed(contents_request(1, 2, 99999)), "variables"), "count").n == 1000);
}

TEST_CASE("nexus json_quote: escapes, control characters and invalid UTF-8") {
    CHECK(json_quote("plain") == "\"plain\"");
    CHECK(json_quote("q\"b\\") == "\"q\\\"b\\\\\"");
    CHECK(json_quote("\n\t\x01") == "\"\\n\\t\\u0001\"");
    CHECK(json_quote("caf\xC3\xA9 \xE4\xB8\xAD") == "\"caf\xC3\xA9 \xE4\xB8\xAD\"");  // valid UTF-8 passes
    CHECK(json_quote("a\xFF" "b") == "\"a\xEF\xBF\xBD" "b\"");                       // a stray byte
    CHECK(json_quote("\xC3") == "\"\xEF\xBF\xBD\"");                                  // cut sequence
    CHECK(json_quote("\xED\xA0\x80") == "\"\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\"");  // a surrogate
}

TEST_CASE("nexus parse: a real search reply") {
    const auto r = parse_search(fixture("search_loot.json"));
    REQUIRE_MESSAGE(r.ok(), r.error);
    CHECK(r.value.total == 5);
    REQUIRE(r.value.mods.size() == 4);
    const NexusMod& m = r.value.mods[0];
    CHECK(m.id == 170);
    CHECK(m.name == "Auto Loot Plus");
    CHECK(m.version == "1.1.8");
    CHECK(m.author == "Dellemann");
    CHECK(m.uploader == "Dellemann");
    CHECK(m.downloads == 5235);
    CHECK(m.endorsements == 46);
    CHECK(m.updated_at == "2026-10-23T21:30:04Z");
    CHECK(m.created_at == "2026-10-18T01:23:35Z");
    CHECK(m.category == "Miscellaneous");
    CHECK(m.picture_url == "https://staticdelivery.nexusmods.com/mods/5811/images/170/170-1792285344-301928562.jpg");
    CHECK(m.thumbnail_url.find("/thumbnails/170/") != std::string::npos);
    CHECK_FALSE(m.adult);
    CHECK(m.summary.rfind("Auto loot in a radius", 0) == 0);
    CHECK(m.description.empty());
    CHECK(r.value.mods[3].author == "XiaoHuLi");  // author and uploader differ here
    CHECK(r.value.mods[3].uploader == "XiaoHuLi666");
    CHECK(r.value.mods[3].summary.find("\xE8\x87\xAA") != std::string::npos);  // UTF-8 kept (Chinese text)
}

TEST_CASE("nexus parse: a recorded mod reply with its description") {
    const auto r = parse_mod(fixture("mod_195.json"));
    REQUIRE_MESSAGE(r.ok(), r.error);
    CHECK(r.value.id == 195);
    CHECK(r.value.name == "Quick Deposit - Sort Your Backpack Into Chests");
    const std::string& d = r.value.description;
    CHECK(d.rfind("[center][img]", 0) == 0);
    // all of it, to its last line: past the nested lists, the line breaks and the escaped backslashes
    const std::string last = "github.com/Greeneye0/enshrouded-quick-deposit[/url]";
    CHECK(d.size() > 1000);
    REQUIRE(d.size() >= last.size());
    CHECK(d.compare(d.size() - last.size(), last.size(), last) == 0);
}

TEST_CASE("nexus parse: real files reply; sizeInBytes is a string, primary an Int") {
    const auto r = parse_files(fixture("files_170.json"));
    REQUIRE_MESSAGE(r.ok(), r.error);
    REQUIRE(r.value.size() == 5);
    const NexusFile& f = r.value[0];
    CHECK(f.file_id == 918);
    CHECK(f.name == "AutoLootPlus 1.0.0");
    CHECK(f.version == "1.0.0");
    CHECK(f.category == "ARCHIVED");
    CHECK(f.size_bytes == 394052);
    CHECK(f.date == 1792286403);
    CHECK_FALSE(f.primary);
    CHECK(r.value[2].file_id == 1197);
    CHECK(r.value[2].category == "MAIN");
    CHECK(r.value[2].primary);
}

TEST_CASE("nexus parse: real archive listings; fileSize is a string too") {
    auto r = parse_contents(fixture("contents_147_857.json"));
    REQUIRE_MESSAGE(r.ok(), r.error);
    CHECK(r.value.total == 26);
    REQUIRE(r.value.entries.size() == 26);
    CHECK(r.value.entries[0].path == "dbghelp.dll");
    CHECK(r.value.entries[0].size == 10667520);

    r = parse_contents(fixture("contents_170_1197.json"));  // a new file Nexus has not listed yet
    REQUIRE(r.ok());
    CHECK(r.value.total == 0);
    CHECK(r.value.entries.empty());
}

TEST_CASE("nexus parse: GraphQL errors are an error text, not a crash") {
    auto r = parse_mod(fixture("error_not_found.json"));
    CHECK_FALSE(r.ok());
    CHECK(r.error.find("Mod not found") != std::string::npos);

    auto c = parse_contents(fixture("error_complexity.json"));
    CHECK_FALSE(c.ok());
    CHECK(c.error.find("exceeds max complexity") != std::string::npos);

    // several errors are joined; a 400 reply with GraphQL errors shows those, not just the status
    auto s = parse_search(R"({"errors":[{"message":"one"},{"message":"two"}]})", 400);
    CHECK(s.error.find("one") != std::string::npos);
    CHECK(s.error.find("two") != std::string::npos);
}

TEST_CASE("nexus parse: HTTP failures and replies that are not JSON") {
    auto r = parse_search("<html>Attention Required! | Cloudflare</html>", 403);
    CHECK_FALSE(r.ok());
    CHECK(r.error.find("403") != std::string::npos);
    r = parse_search("", 429);
    CHECK(r.error.find("429") != std::string::npos);
    CHECK(r.error.find("too many") != std::string::npos);
    r = parse_search("<html></html>", 200);
    CHECK(r.error.find("not JSON") != std::string::npos);
    r = parse_search(R"({"data":null})");
    CHECK_FALSE(r.ok());
    r = parse_search(R"({"data":{"mods":{"nodes":"x"}}})");
    CHECK_FALSE(r.ok());
    r = parse_search("[1,2]");
    CHECK_FALSE(r.ok());
}

TEST_CASE("nexus parse: missing fields, nulls and numbers as strings or too big") {
    const char* body = R"({"data":{"mods":{"totalCount":"12","nodes":[
        {"modId":7,"name":null,"author":null,"uploader":null,"adultContent":null,"downloads":"123456789012",
         "endorsements":1e300,"pictureUrl":null},
        {"modId":"8","name":"x","uploader":{"name":null},"adultContent":true,"downloads":-5,"endorsements":"12abc"},
        "not an object",
        {"name":"no id"}
    ]}}})";
    const auto r = parse_search(body);
    REQUIRE_MESSAGE(r.ok(), r.error);
    CHECK(r.value.total == 12);
    REQUIRE(r.value.mods.size() == 2);  // the entries without a usable modId are skipped
    CHECK(r.value.mods[0].id == 7);
    CHECK(r.value.mods[0].name.empty());
    CHECK(r.value.mods[0].downloads == 123456789012);
    CHECK(r.value.mods[0].endorsements == 0);  // out of range: 0, not undefined behaviour
    CHECK_FALSE(r.value.mods[0].adult);
    CHECK(r.value.mods[1].id == 8);
    CHECK(r.value.mods[1].adult);
    CHECK(r.value.mods[1].downloads == 0);     // negative counts are nonsense
    CHECK(r.value.mods[1].endorsements == 0);  // not a number

    const auto f = parse_files(R"({"data":{"modFiles":[
        {"fileId":1,"size":2},
        {"fileId":2,"sizeInBytes":"18446744073709551615","primary":1},
        {"fileId":3,"sizeInBytes":"99999999999999999999999"},
        {"fileId":4,"sizeInBytes":4096,"size":4}
    ]}})");
    REQUIRE_MESSAGE(f.ok(), f.error);
    REQUIRE(f.value.size() == 4);
    CHECK(f.value[0].size_bytes == 2048);  // only size (KiB): its bytes
    CHECK(f.value[1].size_bytes == 18446744073709551615ull);
    CHECK(f.value[1].primary);
    CHECK(f.value[2].size_bytes == 0);  // does not fit
    CHECK(f.value[3].size_bytes == 4096);

    const auto c = parse_contents(R"({"data":{"modFileContents":{"nodes":[{"filePath":"a.dll"},{"fileSize":"3"}]}}})");
    REQUIRE(c.ok());
    REQUIRE(c.value.entries.size() == 1);  // an entry without a path is skipped
    CHECK(c.value.entries[0].size == 0);
    CHECK(c.value.total == 1);  // no totalCount: what arrived
}

TEST_CASE("nexus parse: a lone UTF-16 surrogate in Nexus's JSON becomes U+FFFD, not a failed reply or bad UTF-8") {
    const auto r = parse_search("{\"data\":{\"mods\":{\"totalCount\":1,\"nodes\":[\n"
        "        {\"modId\":7,\"name\":\"Broken \\ud83d name\",\"summary\":\"cut \\udc00 here\"}]}}}");
    REQUIRE_MESSAGE(r.ok(), r.error);
    REQUIRE(r.value.mods.size() == 1u);
    CHECK(r.value.mods[0].name == "Broken \xEF\xBF\xBD name");
    CHECK(r.value.mods[0].summary == "cut \xEF\xBF\xBD here");
    CHECK(json_quote(r.value.mods[0].name) == "\"Broken \xEF\xBF\xBD name\"");  // valid UTF-8 when it goes on
    const auto m = parse_mod("{\"data\":{\"mod\":{\"modId\":9,\"name\":\"x\\ud800\"}}}");
    REQUIRE_MESSAGE(m.ok(), m.error);
    CHECK(m.value.name == "x\xEF\xBF\xBD");
}

TEST_CASE("nexus parse: lists keep what arrived next to GraphQL errors; one mod and its files stay strict") {
    const char* search = R"({"errors":[{"message":"partly broken"}],"data":{"mods":{"totalCount":1,"nodes":[{"modId":5,"name":"x"}]}}})";
    auto s = parse_search(search);
    REQUIRE_MESSAGE(s.ok(), s.error);
    REQUIRE(s.value.mods.size() == 1);
    CHECK(s.value.mods[0].id == 5);
    const char* contents = R"({"errors":[{"message":"partly"}],"data":{"modFileContents":{"totalCount":1,"nodes":[{"filePath":"a.dll","fileSize":"1"}]}}})";
    auto c = parse_contents(contents);
    REQUIRE_MESSAGE(c.ok(), c.error);
    CHECK(c.value.entries.size() == 1);
    // no data: still an error
    CHECK_FALSE(parse_search(R"({"errors":[{"message":"x"}],"data":{"mods":null}})").ok());

    CHECK_FALSE(parse_mod(R"({"errors":[{"message":"x"}],"data":{"mod":{"modId":5,"name":"x"}}})").ok());
    CHECK_FALSE(parse_files(R"({"errors":[{"message":"x"}],"data":{"modFiles":[{"fileId":1}]}})").ok());
}
