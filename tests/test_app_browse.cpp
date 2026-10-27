// EMF.exe's Browse page data (app/browse.h): which picture addresses are allowed (the same list as the page's CSP and
// the host's request filter), file order, Nexus replies as the page's JSON, and the recorded replies of --nexus-fixture.
#include <doctest.h>

#include <string>

#include "app/browse.h"
#include "app/locate.h"
#include "app/text.h"
#include "core/json.h"

using namespace emf::app;
namespace json = emf::core::json;

namespace {

std::wstring fixture() { return wide(std::string(EMF_SOURCE_DIR) + "/tests/fixtures/nexus_app"); }

json::Value parse(const std::string& text) {
    auto r = json::parse(text);
    REQUIRE_MESSAGE(r.value, text);
    return *r.value;
}

emf::nexus::NexusFile file(int64_t id, const char* category, int64_t date) {
    emf::nexus::NexusFile f;
    f.file_id = id;
    f.category = category;
    f.date = date;
    return f;
}

}  // namespace

TEST_CASE("app browse: pictures only over https from the listed hosts") {
    CHECK(image_url_allowed("https://staticdelivery.nexusmods.com/mods/5811/images/thumbnails/170/170-1.jpg"));
    CHECK(image_url_allowed("https://i.imgur.com/abc.png"));
    CHECK(image_url_allowed("https://raw.githubusercontent.com/u/r/main/x.png"));
    CHECK(image_url_allowed("https://user-images.githubusercontent.com/1/2.png"));
    CHECK(image_url_allowed("https://STATICDELIVERY.nexusmods.com/x.png"));
    CHECK_FALSE(image_url_allowed("http://staticdelivery.nexusmods.com/x.png"));
    CHECK_FALSE(image_url_allowed("https://staticdelivery.nexusmods.com:443/x.png"));
    CHECK_FALSE(image_url_allowed("https://staticdelivery.nexusmods.com@example.com/x.png"));
    CHECK_FALSE(image_url_allowed("https://staticdelivery.nexusmods.com.example.com/x.png"));
    CHECK_FALSE(image_url_allowed("https://example.com/x.png"));
    CHECK_FALSE(image_url_allowed("https://staticdelivery.nexusmods.com"));
    CHECK_FALSE(image_url_allowed("https://staticdelivery.nexusmods.com/a b.png"));
    CHECK(image_hosts().size() == 4);
}

TEST_CASE("app browse: any https link from a description, without credentials, ports or odd characters") {
    CHECK(web_url_allowed("https://github.com/owner/repo"));
    CHECK(web_url_allowed("https://discord.gg/abc?x=1#y"));
    CHECK_FALSE(web_url_allowed("http://github.com/"));
    CHECK_FALSE(web_url_allowed("https://user@github.com/"));
    CHECK_FALSE(web_url_allowed("https://github.com:8080/"));
    CHECK_FALSE(web_url_allowed("https://github.com\\@evil/"));
    CHECK_FALSE(web_url_allowed("https://localhost/"));
    CHECK_FALSE(web_url_allowed("https://[::1]/"));
    CHECK_FALSE(web_url_allowed("javascript:alert(1)"));
    CHECK_FALSE(web_url_allowed("https://a..b/"));
}

TEST_CASE("app browse: files are listed MAIN and OPTIONAL first, ARCHIVED last, newest first in each") {
    const auto f = order_files({file(1, "ARCHIVED", 5), file(2, "OPTIONAL", 1), file(3, "MAIN", 1), file(4, "MAIN", 9),
                                file(5, "OLD_VERSION", 3), file(6, "MISCELLANEOUS", 2), file(7, "REMOVED", 9)});
    std::vector<int64_t> ids;
    for (const auto& x : f) ids.push_back(x.file_id);
    CHECK(ids == std::vector<int64_t>{4, 3, 2, 6, 5, 1, 7});
    CHECK(contents_at_once("MAIN"));
    CHECK_FALSE(contents_at_once("OPTIONAL"));  // only MAIN is looked inside automatically now
    CHECK_FALSE(contents_at_once("ARCHIVED"));
}

TEST_CASE("app browse: recorded replies stand in for Nexus; a file without a recorded listing has none") {
    const emf::nexus::Client client("0.2.0", fixture_transport(fixture()));
    const auto search = client.search({});
    REQUIRE_MESSAGE(search.ok(), search.error);
    CHECK(search.value.mods.size() == 4);
    const auto mod = client.mod(170);
    REQUIRE(mod.ok());
    CHECK(mod.value.name == "Auto Loot Plus");
    const auto files = client.files(170);
    REQUIRE(files.ok());
    CHECK(files.value.size() == 5);
    const auto listed = client.contents(170, 929);
    REQUIRE(listed.ok());
    CHECK(listed.value.entries.size() == 10);
    const auto none = client.contents(170, 1198);  // nothing recorded: no listing, as for new uploads
    REQUIRE(none.ok());
    CHECK(none.value.entries.empty());
    CHECK_FALSE(client.mod(999).ok());
    CHECK(fixture_image(fixture(), "https://staticdelivery.nexusmods.com/mods/5811/images/thumbnails/170/170-1792285344-301928562.jpg") != L"");
    CHECK(fixture_image(fixture(), "https://staticdelivery.nexusmods.com/mods/missing.png") == L"");
    CHECK(fixture_image(fixture(), "https://example.com/170-1792285344-301928562.jpg") == L"");
}

TEST_CASE("app browse: the page's JSON for a search, a file and a mod") {
    const emf::nexus::Client client("0.2.0", fixture_transport(fixture()));
    const json::Value s = parse(search_reply(5, 20, client.search({})));
    CHECK(s.get("type")->s == "nexus_search");
    CHECK(s.get("seq")->n == 5);
    CHECK(s.get("offset")->n == 20);
    CHECK(s.get("total")->n == 5);
    const json::Value& first = s.get("mods")->arr[0];
    CHECK(first.get("id")->n == 170);
    CHECK(first.get("downloads")->n > 0);
    CHECK(first.get("url")->s == "https://www.nexusmods.com/enshrouded/mods/170");

    emf::nexus::Result<emf::nexus::SearchPage> failed;
    failed.error = "could not reach Nexus: <b>no network</b>";
    const json::Value f = parse(search_reply(1, 0, failed));
    CHECK_FALSE(f.get("ok")->b);
    CHECK(f.get("error")->s == "could not reach Nexus: <b>no network</b>");  // the page shows it as text

    FileFacts facts;
    facts.file = file(929, "ARCHIVED", 1);
    facts.checked = true;
    facts.contents = client.contents(170, 929);
    emf::nexus::GameState g;
    g.known = true;
    g.proxies = {{"dinput8.dll", "emf"}};
    const json::Value fj = parse(file_json(facts, g));
    CHECK(fj.get("listed")->b);
    CHECK(fj.get("kind_label")->s == "DLL mod (dinput8.dll)");
    CHECK_FALSE(fj.get("notes")->arr.empty());
    facts.contents = client.contents(170, 1197);
    const json::Value none = parse(file_json(facts, g));
    CHECK(none.get("checked")->b);
    CHECK_FALSE(none.get("listed")->b);
    CHECK(none.get("kind")->s.empty());
}
