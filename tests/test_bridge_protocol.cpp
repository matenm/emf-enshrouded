// The debug bridge's line protocol: request parsing, error replies, field limits and the JSON writer.
#include <windows.h>

#include <doctest.h>

#include <limits>
#include <string>

#include "core/bridge/json_out.h"
#include "core/bridge/protocol.h"

using namespace emf::core;
using namespace emf::core::bridge;

TEST_CASE("bridge protocol: a request has an id (number or string) and a cmd") {
    auto p = parse_request(R"({"id": 7, "cmd": "hello"})");
    REQUIRE(p.request);
    CHECK(p.request->id == "7");
    CHECK(p.request->cmd == "hello");
    CHECK(p.error.empty());

    p = parse_request(R"({"cmd":"state","id":"a\"b"})");
    REQUIRE(p.request);
    const std::string quoted_id = R"("a\"b")";  // (MSVC's preprocessor mishandles this raw string inside CHECK)
    CHECK(p.request->id == quoted_id);          // echoed as JSON text
    CHECK(p.request->cmd == "state");

    p = parse_request(R"({"id":1.5,"cmd":"x"})");
    REQUIRE(p.request);
    CHECK(p.request->id == "1.5");
}

TEST_CASE("bridge protocol: bad lines get a plain error and the id when it could be read") {
    auto p = parse_request("hello");
    CHECK_FALSE(p.request);
    CHECK(p.id == "null");
    CHECK(p.error.find("not JSON") == 0);

    p = parse_request("[1,2]");
    CHECK_FALSE(p.request);
    CHECK(p.error == "a request is one JSON object");

    p = parse_request(R"({"cmd":"hello"})");
    CHECK_FALSE(p.request);
    CHECK(p.id == "null");
    CHECK(p.error == "the request has no id");

    p = parse_request(R"({"id":true,"cmd":"hello"})");
    CHECK_FALSE(p.request);
    CHECK(p.error == "id must be a number or a string");

    p = parse_request(R"({"id":")" + std::string(201, 'x') + R"(","cmd":"hello"})");
    CHECK_FALSE(p.request);
    CHECK(p.error == "id is longer than 200 bytes");

    p = parse_request(R"({"id":3})");
    CHECK_FALSE(p.request);
    CHECK(p.id == "3");
    CHECK(p.error == "the request has no cmd");

    p = parse_request(R"({"id":3,"cmd":5})");
    CHECK_FALSE(p.request);
    CHECK(p.error == "the request has no cmd");
}

TEST_CASE("bridge protocol: a request with thousands of keys parses; a repeated key keeps its last value") {
    std::string line = "{\"id\":1,\"cmd\":\"hello\"";
    for (int i = 0; i < 5000; ++i) line += ",\"k" + std::to_string(i) + "\":" + std::to_string(i);
    line += ",\"k42\":\"last\",\"cmd\":\"state\"}";
    const ULONGLONG t0 = GetTickCount64();
    auto p = parse_request(line);
    CHECK(GetTickCount64() - t0 < 2000);
    REQUIRE(p.request);
    CHECK(p.request->cmd == "state");
    CHECK(p.request->body.obj.size() == 5002);
    CHECK(p.request->body.get("k42")->s == "last");
    CHECK(p.request->body.get("k4999")->n == 4999);
}

TEST_CASE("bridge protocol: reply lines") {
    CHECK(ok_reply("1", "") == R"({"id":1,"ok":true})");
    CHECK(ok_reply("\"a\"", R"("x":2)") == R"({"id":"a","ok":true,"x":2})");
    CHECK(error_reply("null", "bad_request", "the request has no id") ==
          R"({"id":null,"ok":false,"code":"bad_request","error":"the request has no id"})");
}

TEST_CASE("bridge protocol: optional fields with limits") {
    std::string error;
    uint32_t ms = 0;
    auto body = *json::parse(R"({"timeout_ms": 2500, "bad": "x", "frac": 1.5, "neg": -1, "big": 99999999})").value;
    CHECK(read_ms(body, "timeout_ms", 10000, 1, 120000, &ms, &error));
    CHECK(ms == 2500);
    CHECK(read_ms(body, "missing", 10000, 1, 120000, &ms, &error));
    CHECK(ms == 10000);
    CHECK_FALSE(read_ms(body, "bad", 10000, 1, 120000, &ms, &error));
    CHECK(error == "bad must be a whole number from 1 to 120000");
    CHECK_FALSE(read_ms(body, "frac", 10000, 1, 120000, &ms, &error));
    CHECK_FALSE(read_ms(body, "neg", 10000, 1, 120000, &ms, &error));
    CHECK_FALSE(read_ms(body, "big", 10000, 1, 120000, &ms, &error));

    std::optional<std::string> text;
    body = *json::parse(R"({"text": "near 10", "n": 3})").value;
    CHECK(read_text(body, "text", &text, &error));
    CHECK(text == "near 10");
    CHECK(read_text(body, "missing", &text, &error));
    CHECK_FALSE(text);
    CHECK_FALSE(read_text(body, "n", &text, &error));
    CHECK(error == "n must be text");

    std::optional<bool> flag;
    body = *json::parse(R"({"follow": true, "n": 1})").value;
    CHECK(read_bool(body, "follow", &flag, &error));
    CHECK(flag == true);
    CHECK_FALSE(read_bool(body, "n", &flag, &error));
    CHECK(error == "n must be true or false");

    std::optional<uint64_t> seq;
    body = *json::parse(R"({"since": 12, "neg": -3})").value;
    CHECK(read_seq(body, "since", &seq, &error));
    CHECK(seq == 12u);
    CHECK_FALSE(read_seq(body, "neg", &seq, &error));
}

TEST_CASE("bridge json: strings are escaped and bad UTF-8 is replaced") {
    const std::string escaped = R"("a\"b\\c")";
    CHECK(json_quote("a\"b\\c") == escaped);
    CHECK(json_quote("line\nnext\ttab") == R"("line\nnext\ttab")");
    CHECK(json_quote(std::string("\x01", 1)) == R"("\u0001")");
    CHECK(json_quote("caf\xC3\xA9") == "\"caf\xC3\xA9\"");             // valid UTF-8 stays
    CHECK(json_quote("bad \xFF byte") == "\"bad \xEF\xBF\xBD byte\"");  // U+FFFD
    CHECK(json_number(3) == "3");
    CHECK(json_number(-2.5) == "-2.5");
    CHECK(json_number(std::numeric_limits<double>::infinity()) == "0");
    CHECK(JsonObj().str("a", "x").num("b", 1).boolean("c", false).null("d").text() ==
          R"({"a":"x","b":1,"c":false,"d":null})");
    CHECK(JsonObj().num("a", 1).members() == R"("a":1)");
    CHECK(to_json(*json::parse(R"({"k":[1,"two",null,true]})").value) == R"({"k":[1,"two",null,true]})");
}
