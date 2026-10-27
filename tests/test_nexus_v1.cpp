// Nexus's REST API v1 (the calls that need the player's API key): checking a key, a file's details, its download
// links, the rate-limit headers, the headers every request carries, and the HTTP helpers (reply headers, redirects).
// Replies are saved in tests/fixtures/nexus/v1_*.json (no network in tests).
#include <doctest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "nexus/http.h"
#include "nexus/v1.h"

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

HttpHeaders limits_headers(const char* daily, const char* hourly) {
    return {{"X-RL-Daily-Limit", "20000"},  {"X-RL-Daily-Remaining", daily},
            {"X-RL-Daily-Reset", "2026-10-27T00:00:00+00:00"},
            {"X-RL-Hourly-Limit", "500"}, {"x-rl-hourly-remaining", hourly},
            {"X-RL-Hourly-Reset", "2026-10-26T20:00:00+00:00"}};
}
}  // namespace

TEST_CASE("nexus v1: a lone UTF-16 surrogate in a reply becomes U+FFFD") {
    const auto r = parse_validate("{\"user_id\":5,\"name\":\"Ember\\udc01\",\"is_premium\":false,\"is_supporter\":false}", 200);
    REQUIRE_MESSAGE(r.ok(), r.error);
    CHECK(r.value.name == "Ember\xEF\xBF\xBD");
}

TEST_CASE("nexus v1: validate gives the user, Premium and supporter, and never keeps the email") {
    const auto r = parse_validate(fixture("v1_validate_premium.json"), 200);
    REQUIRE_MESSAGE(r.ok(), r.error);
    CHECK(r.value.user_id == 12345);
    CHECK(r.value.name == "EmberTester");
    CHECK(r.value.premium);
    CHECK_FALSE(r.value.supporter);
    CHECK(r.status == 200);
    // the user has no email field at all; nothing that was read holds it
    for (const std::string& s : {r.value.name, r.value.profile_url}) CHECK(s.find("example.invalid") == std::string::npos);

    const auto free = parse_validate(fixture("v1_validate_free.json"), 200);
    REQUIRE(free.ok());
    CHECK(free.value.name == "Free Walker");
    CHECK_FALSE(free.value.premium);
    CHECK(free.value.supporter);

    const auto old = parse_validate(fixture("v1_validate_old.json"), 200);  // only the "is_premium?" spelling
    REQUIRE(old.ok());
    CHECK(old.value.premium);
    CHECK(old.value.supporter);
}

TEST_CASE("nexus v1: a refused key, a reply without a user and a broken reply are errors in words") {
    auto r = parse_validate(fixture("v1_validate_401.json"), 401);  // the real reply without a key
    CHECK_FALSE(r.ok());
    CHECK(r.status == 401);
    CHECK(r.error.find("did not accept the API key") != std::string::npos);
    CHECK(r.error.find("Please provide an authentication method") != std::string::npos);
    r = parse_validate(fixture("v1_validate_401_bad_key.json"), 401);
    CHECK(r.error.find("valid API Key") != std::string::npos);
    r = parse_validate(R"({"name":"x"})", 200);
    CHECK_FALSE(r.ok());
    r = parse_validate("<html>bad gateway</html>", 502);
    CHECK(r.error.find("Nexus had a problem (502)") != std::string::npos);
    r = parse_validate("not json", 200);
    CHECK(r.error.find("not JSON") != std::string::npos);
}

TEST_CASE("nexus v1: a file's details give its archive name and exact size") {
    const auto r = parse_file_info(fixture("v1_file_170_1197.json"), 200);
    REQUIRE_MESSAGE(r.ok(), r.error);
    CHECK(r.value.file_id == 1197);
    CHECK(r.value.name == "AutoLootPlus 1.1.8");
    CHECK(r.value.version == "1.1.8");
    CHECK(r.value.category == "MAIN");
    CHECK(r.value.file_name == "AutoLootPlus 1.1.8-170-1-1-8-1792790875.zip");
    CHECK(r.value.size_known);
    CHECK(r.value.size_bytes == 735506);

    const auto old = parse_file_info(fixture("v1_file_no_size.json"), 200);
    REQUIRE(old.ok());
    CHECK_FALSE(old.value.size_known);
    CHECK(old.value.file_name == "old.7z");

    const auto missing = parse_file_info(R"({"message":"File ID '9' not found"})", 404);
    CHECK_FALSE(missing.ok());
    CHECK(missing.error.find("not found") != std::string::npos);
}

TEST_CASE("nexus v1: download links keep the https ones in Nexus's order") {
    const auto r = parse_download_links(fixture("v1_download_link_170_1197.json"), 200);
    REQUIRE_MESSAGE(r.ok(), r.error);
    REQUIRE(r.value.size() == 2);
    CHECK(r.value[0].short_name == "Nexus CDN");
    CHECK(r.value[0].uri.rfind("https://cf-files.nexusmods.com/", 0) == 0);
    CHECK(r.value[1].name == "Amsterdam");

    const auto http_only = parse_download_links(R"([{"name":"a","short_name":"a","URI":"http://x.example/f.zip"}])", 200);
    CHECK_FALSE(http_only.ok());
    CHECK(parse_download_links("[]", 200).error.find("no https download link") != std::string::npos);
    CHECK_FALSE(parse_download_links(R"({"not":"a list"})", 200).ok());
}

TEST_CASE("nexus v1: a free account's refusal and an expired link say so, with Nexus's words") {
    const auto r = parse_download_links(fixture("v1_download_link_403.json"), 403);
    CHECK_FALSE(r.ok());
    CHECK(r.status == 403);
    CHECK(r.error.find("premium users only") != std::string::npos);
    const auto gone = parse_download_links(fixture("v1_download_link_410.json"), 410);
    CHECK(gone.status == 410);
    CHECK(gone.error.find("expired") != std::string::npos);
    const auto many = parse_download_links(fixture("v1_error_429.json"), 429);
    CHECK(many.error.find("limit") != std::string::npos);
}

TEST_CASE("nexus v1: the rate-limit headers, and the message when both allowances are used up") {
    auto l = parse_rate_limits(limits_headers("19990", "499"));
    CHECK(l.known);
    CHECK(l.daily_remaining == 19990);
    CHECK(l.hourly_remaining == 499);
    CHECK(l.daily_limit == 20000);
    CHECK(l.hourly_limit == 500);
    CHECK(l.hourly_reset == "2026-10-26T20:00:00+00:00");
    CHECK_FALSE(l.exhausted());

    l = parse_rate_limits(limits_headers("0", "12"));  // the daily allowance is gone; the hourly one still works
    CHECK_FALSE(l.exhausted());
    l = parse_rate_limits(limits_headers("0", "0"));
    CHECK(l.exhausted());
    const std::string m = rate_limit_message(l);
    CHECK(m.find("used up") != std::string::npos);
    CHECK(m.find("T20:00:00") == std::string::npos);  // not Nexus's raw UTC time
    CHECK(m.find("opens again at " + local_clock(1793044800)) != std::string::npos);  // the PC's own time
    CHECK(local_clock(1793044800).size() >= 5);

    CHECK_FALSE(parse_rate_limits({{"Content-Type", "application/json"}}).known);
    CHECK_FALSE(parse_rate_limits({{"X-RL-Daily-Remaining", "lots"}}).known);
}

TEST_CASE("nexus v1: every request is a GET to api.nexusmods.com/v1 with the key and EMF's names") {
    std::vector<HttpRequest> sent;
    HttpResponse reply{200, fixture("v1_validate_premium.json"), "", HttpFailure::None, limits_headers("100", "50")};
    V1Client c("0.3.0", [&](const HttpRequest& r) {
        sent.push_back(r);
        return reply;
    });
    const auto v = c.validate("my-key-123");
    REQUIRE_MESSAGE(v.ok(), v.error);
    CHECK(v.limits.known);
    CHECK(v.limits.daily_remaining == 100);
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].method == "GET");
    CHECK(sent[0].url == "https://api.nexusmods.com/v1/users/validate.json");
    CHECK(sent[0].body.empty());
    CHECK(header(sent[0], "apikey") == "my-key-123");
    CHECK(header(sent[0], "Application-Name") == "EMF");
    CHECK(header(sent[0], "Application-Version") == "0.3.0");
    CHECK(header(sent[0], "Accept") == "application/json");
    CHECK(sent[0].user_agent == "EMF/0.3.0 (Enshrouded Modding Framework; Windows)");
    CHECK(sent[0].url.find("my-key-123") == std::string::npos);  // the key travels in its header only

    reply.body = fixture("v1_file_170_1197.json");
    CHECK(c.file("my-key-123", 170, 1197).ok());
    CHECK(sent.back().url == "https://api.nexusmods.com/v1/games/enshrouded/mods/170/files/1197.json");

    reply.body = fixture("v1_download_link_170_1197.json");
    CHECK(c.download_links("my-key-123", 170, 1197, "", 0).ok());  // Premium: no parameters
    CHECK(sent.back().url == "https://api.nexusmods.com/v1/games/enshrouded/mods/170/files/1197/download_link.json");
    CHECK(c.download_links("my-key-123", 170, 1197, "ab+c/d=", 1792790875).ok());  // from an nxm link
    CHECK(sent.back().url ==
          "https://api.nexusmods.com/v1/games/enshrouded/mods/170/files/1197/download_link.json?key=ab%2Bc%2Fd%3D&expires=1792790875");
}

TEST_CASE("nexus v1: a key with spaces or line breaks is never sent; a transport failure is an error") {
    int calls = 0;
    V1Client c("0.3.0", [&](const HttpRequest&) {
        ++calls;
        return HttpResponse{0, "", "connect: could not connect (12029)", HttpFailure::Network, {}};
    });
    CHECK_FALSE(c.validate("bad\r\nkey").ok());
    CHECK_FALSE(c.validate("").ok());
    CHECK_FALSE(c.validate("has space").ok());
    CHECK(calls == 0);
    const auto r = c.validate("good-key");
    CHECK(calls == 1);
    CHECK(r.error.find("could not reach Nexus") != std::string::npos);
    CHECK(api_key_text_ok("aGVsbG8=--d29ybGQ=--Zm9v"));
    CHECK_FALSE(api_key_text_ok("short"));
    CHECK_FALSE(api_key_text_ok(std::string(5000, 'a')));
}

TEST_CASE("http: reply headers are read from the raw block, names without case") {
    const HttpHeaders h = parse_raw_headers("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nX-RL-Daily-Remaining:  42 \r\n"
                                            "Location: https://x.example/a?b=c:d\r\n\r\n");
    REQUIRE(h.size() == 3);
    CHECK(header_value(h, "content-type") == "application/json");
    CHECK(header_value(h, "x-rl-daily-remaining") == "42");
    CHECK(header_value(h, "LOCATION") == "https://x.example/a?b=c:d");
    CHECK(header_value(h, "missing").empty());
}

TEST_CASE("http: a download follows redirects only to https addresses") {
    const std::string from = "https://cf-files.nexusmods.com/cdn/5811/170/file.zip?md5=x";
    CHECK(redirect_target(from, "https://other.example/f.zip").value_or("") == "https://other.example/f.zip");
    CHECK(redirect_target(from, "//edge.example/f.zip").value_or("") == "https://edge.example/f.zip");
    CHECK(redirect_target(from, "/other/f.zip").value_or("") == "https://cf-files.nexusmods.com/other/f.zip");
    CHECK(redirect_target(from, "g.zip?x=1").value_or("") == "https://cf-files.nexusmods.com/cdn/5811/170/g.zip?x=1");
    std::string why;
    CHECK_FALSE(redirect_target(from, "http://plain.example/f.zip", &why));
    CHECK(why.find("not https") != std::string::npos);
    CHECK_FALSE(redirect_target(from, "ftp://x.example/f.zip"));
    CHECK_FALSE(redirect_target(from, "javascript:alert(1)"));
    CHECK_FALSE(redirect_target(from, ""));
    CHECK_FALSE(redirect_target(from, "https://user:pw@x.example/f.zip"));
    CHECK_FALSE(redirect_target(from, "https:///nohost"));
    CHECK_FALSE(redirect_target(from, "https://x.example/a b"));
    CHECK_FALSE(redirect_target(from, "https://x.example\\evil"));
    CHECK_FALSE(redirect_target("http://plain.example/x", "/y"));  // relative to a non-https page
}

TEST_CASE("nexus v1: the reset times are read as Unix seconds") {
    CHECK(iso8601_to_unix("2026-10-26T20:00:00+00:00") == 1793044800);
    CHECK(iso8601_to_unix("2026-10-26T20:00:00Z") == 1793044800);
    CHECK(iso8601_to_unix("2026-10-26T22:00:00+02:00") == 1793044800);
    CHECK(iso8601_to_unix("2026-10-26T20:00:00.000+00:00") == 1793044800);
    CHECK(iso8601_to_unix("2026-10-26 20:00:00") == 1793044800);  // no zone: UTC
    CHECK(iso8601_to_unix("") == 0);
    CHECK(iso8601_to_unix("soon") == 0);
    CHECK(iso8601_to_unix("2026-13-40T99:00:00Z") == 0);
}
