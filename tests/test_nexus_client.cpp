// The Nexus client with a fake transport: the headers Nexus asks apps to send, the address, and the paging of long
// archive listings (no network in tests).
#include <doctest.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/json.h"
#include "nexus/client.h"

using namespace emf::nexus;

namespace {
std::string fixture(const char* name) {
    std::ifstream f(std::string(EMF_SOURCE_DIR) + "/tests/fixtures/nexus/" + name, std::ios::binary);
    REQUIRE_MESSAGE(f.good(), name);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

std::string header(const HttpRequest& r, const std::string& name) {
    for (const auto& [k, v] : r.headers)
        if (k == name) return v;
    return "<missing>";
}

double variable(const HttpRequest& r, const char* name) {
    const auto doc = emf::core::json::parse(r.body);
    REQUIRE(doc.value.has_value());
    const auto* v = doc.value->get("variables")->get(name);
    REQUIRE(v != nullptr);
    return v->n;
}

std::string page(int total, int count, int first) {
    std::string s = "{\"data\":{\"modFileContents\":{\"totalCount\":" + std::to_string(total) + ",\"nodes\":[";
    for (int i = 0; i < count; ++i)
        s += (i ? "," : "") + std::string("{\"filePath\":\"f") + std::to_string(first + i) + ".txt\",\"fileSize\":\"1\"}";
    return s + "]}}}";
}
}  // namespace

TEST_CASE("nexus client: every request names EMF as Nexus asks, over HTTPS to the GraphQL endpoint") {
    std::vector<HttpRequest> sent;
    Client c("0.2.0", [&](const HttpRequest& r) {
        sent.push_back(r);
        return HttpResponse{200, fixture("search_loot.json"), ""};
    });
    SearchQuery q;
    q.text = "loot";
    const auto r = c.search(q);
    REQUIRE_MESSAGE(r.ok(), r.error);
    CHECK(r.value.mods.size() == 4);
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].method == "POST");
    CHECK(sent[0].url == "https://api.nexusmods.com/v2/graphql");
    CHECK(header(sent[0], "Application-Name") == "EMF");
    CHECK(header(sent[0], "Application-Version") == "0.2.0");
    CHECK(header(sent[0], "Content-Type") == "application/json");
    CHECK(sent[0].user_agent.rfind("EMF/0.2.0", 0) == 0);
    CHECK(sent[0].body == search_request(q));
    CHECK(sent[0].timeout_ms > 0);
    // a blank version is never sent
    CHECK(header(Client("").request_for("{}"), "Application-Version") == "unknown");
}

TEST_CASE("nexus client: no reply and HTTP errors become error texts") {
    Client down("0.2.0", [](const HttpRequest&) { return HttpResponse{0, "", "send: timed out (12002)"}; });
    auto m = down.mod(1);
    CHECK_FALSE(m.ok());
    CHECK(m.error == "could not reach Nexus: send: timed out (12002)");
    CHECK_FALSE(down.files(1).ok());
    CHECK_FALSE(down.contents(1, 2).ok());

    Client busy("0.2.0", [](const HttpRequest&) { return HttpResponse{429, "", ""}; });
    CHECK(busy.search({}).error.find("too many") != std::string::npos);

    Client ok("0.2.0", [](const HttpRequest& r) {
        CHECK(r.body == files_request(170));
        return HttpResponse{200, fixture("files_170.json"), ""};
    });
    CHECK(ok.files(170).value.size() == 5);
}

TEST_CASE("nexus client: long archive listings come in pages of 500") {
    std::vector<double> offsets;
    Client c("0.2.0", [&](const HttpRequest& r) {
        const int offset = static_cast<int>(variable(r, "offset"));
        CHECK(variable(r, "count") == 500);
        offsets.push_back(offset);
        return HttpResponse{200, page(1200, offset >= 1000 ? 200 : 500, offset), ""};
    });
    const auto r = c.contents(170, 973);
    REQUIRE_MESSAGE(r.ok(), r.error);
    CHECK(offsets == std::vector<double>{0, 500, 1000});
    CHECK(r.value.total == 1200);
    REQUIRE(r.value.entries.size() == 1200);
    CHECK(r.value.entries[1199].path == "f1199.txt");
}

TEST_CASE("nexus client: paging stops at a short page, at max_entries, and on an error") {
    // Nexus said 82 but sent 80 (seen on a real listing): a page shorter than 500 is the last
    int calls = 0;
    Client short_list("0.2.0", [&](const HttpRequest&) {
        ++calls;
        return HttpResponse{200, page(82, calls == 1 ? 80 : 0, 0), ""};
    });
    auto r = short_list.contents(10, 653);
    REQUIRE(r.ok());
    CHECK(calls == 1);
    CHECK(r.value.entries.size() == 80);
    CHECK(r.value.total == 82);

    // a server that claims 100000 entries but sends one per page: one request, not thousands
    calls = 0;
    Client trickle("0.2.0", [&](const HttpRequest& q) {
        ++calls;
        return HttpResponse{200, page(100000, 1, static_cast<int>(variable(q, "offset"))), ""};
    });
    r = trickle.contents(1, 2);
    REQUIRE(r.ok());
    CHECK(calls <= 4);
    CHECK(r.value.entries.size() == 1);

    calls = 0;
    Client huge("0.2.0", [&](const HttpRequest& q) {
        ++calls;
        return HttpResponse{200, page(100000, 500, static_cast<int>(variable(q, "offset"))), ""};
    });
    r = huge.contents(1, 2, 1200);
    REQUIRE(r.ok());
    CHECK(calls == 3);
    CHECK(r.value.entries.size() == 1200);
    calls = 0;
    r = huge.contents(1, 2, 99999);  // never more than 2000 entries, 4 requests
    REQUIRE(r.ok());
    CHECK(calls == 4);
    CHECK(r.value.entries.size() == 2000);

    calls = 0;
    Client failing("0.2.0", [&](const HttpRequest&) {
        ++calls;
        return calls == 1 ? HttpResponse{200, page(1000, 500, 0), ""} : HttpResponse{200, fixture("error_complexity.json"), ""};
    });
    r = failing.contents(1, 2);
    CHECK_FALSE(r.ok());
    CHECK(r.error.find("complexity") != std::string::npos);

    // nothing listed: one request
    calls = 0;
    Client empty("0.2.0", [&](const HttpRequest&) {
        ++calls;
        return HttpResponse{200, fixture("contents_170_1197.json"), ""};
    });
    r = empty.contents(170, 1197);
    REQUIRE(r.ok());
    CHECK(calls == 1);
    CHECK(r.value.entries.empty());
}

TEST_CASE("nexus client: a cancel flag and an overall deadline, checked before each request") {
    std::atomic<bool> cancel{true};
    int calls = 0;
    auto counting = [&](const HttpRequest& q) {
        ++calls;
        return HttpResponse{200, page(100000, 500, static_cast<int>(variable(q, "offset"))), ""};
    };
    Client c("0.2.0", counting);
    auto r = c.contents(1, 2, 2000, &cancel);
    CHECK(r.error == "cancelled");
    CHECK(calls == 0);
    CHECK(c.search({}, &cancel).error == "cancelled");
    CHECK(c.mod(1, &cancel).error == "cancelled");
    CHECK(c.files(1, &cancel).error == "cancelled");

    // cancelled while the first page was on its way: no second page
    cancel = false;
    calls = 0;
    Client stop("0.2.0", [&](const HttpRequest& q) {
        cancel = true;
        return counting(q);
    });
    r = stop.contents(1, 2, 2000, &cancel);
    CHECK(r.error == "cancelled");
    CHECK(calls == 1);

    // the deadline has passed
    calls = 0;
    r = c.contents(1, 2, 2000, nullptr, std::chrono::milliseconds(0));
    CHECK(r.error == "timed out");
    CHECK(calls == 0);
    CHECK(c.search({}, nullptr, std::chrono::milliseconds(-5)).error == "timed out");

    // the request carries both to the transport (WinHTTP checks them between reads)
    cancel = false;
    const auto before = std::chrono::steady_clock::now();
    Client seen("0.2.0", [&](const HttpRequest& q) {
        CHECK(q.cancel == &cancel);
        CHECK(q.deadline > before + std::chrono::seconds(29));
        CHECK(q.deadline < before + std::chrono::seconds(31));
        return HttpResponse{200, fixture("files_170.json"), ""};
    });
    CHECK(seen.files(170, &cancel).ok());
}

TEST_CASE("nexus client: how a failed request is reported") {
    auto failing = [](HttpFailure f, const char* text) {
        return Client("0.2.0", [=](const HttpRequest&) { return HttpResponse{0, "", text, f}; });
    };
    CHECK(failing(HttpFailure::Cancelled, "cancelled").mod(1).error == "cancelled");
    CHECK(failing(HttpFailure::TimedOut, "timed out").mod(1).error == "timed out");
    CHECK(failing(HttpFailure::TooLarge, "the reply is longer than 4194304 bytes").search({}).error ==
          "the reply was too large");
    CHECK(failing(HttpFailure::Network, "connect: could not connect (12029)").files(1).error ==
          "could not reach Nexus: connect: could not connect (12029)");
}

TEST_CASE("nexus http: refusals that need no network") {
    HttpRequest r;
    r.url = "http://api.nexusmods.com/v2/graphql";
    auto a = http_request(r);
    CHECK(a.status == 0);
    CHECK(a.error.find("https://") != std::string::npos);

    r.url = "https://api.nexusmods.com/v2/graphql";
    r.headers = {{"Application-Name", "EMF\r\nX-Evil: 1"}};
    CHECK(http_request(r).error.find("line break") != std::string::npos);
    r.headers = {{"Bad\nName", "x"}};
    CHECK(http_request(r).error.find("line break") != std::string::npos);
    r.headers = {};
    r.user_agent = "EMF/0.2.0\r\nX-Evil: 1";
    CHECK(http_request(r).error.find("line break") != std::string::npos);

    r.user_agent = "EMF/0.2.0";
    std::atomic<bool> cancel{true};
    r.cancel = &cancel;
    a = http_request(r);
    CHECK(a.failure == HttpFailure::Cancelled);
    CHECK(a.error == "cancelled");
    r.cancel = nullptr;
    r.deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    a = http_request(r);
    CHECK(a.failure == HttpFailure::TimedOut);
    CHECK(a.error == "timed out");
    CHECK(HttpRequest{}.max_body == 4 * 1024 * 1024);
}
