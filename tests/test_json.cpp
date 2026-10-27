#include <doctest.h>

#include <string>

#include "core/json.h"

namespace json = emf::core::json;
using T = json::Value::Type;

TEST_CASE("json: objects, arrays, nesting and every value type") {
    auto r = json::parse(R"({"a": 1, "b": [true, false, null, "x"], "c": {"d": -2.5e1}})");
    REQUIRE(r.value);
    const auto& v = *r.value;
    CHECK(v.is(T::Object));
    REQUIRE(v.get("a"));
    CHECK(v.get("a")->n == 1.0);
    const auto* b = v.get("b");
    REQUIRE(b);
    REQUIRE(b->arr.size() == 4);
    CHECK(b->arr[0].is(T::Bool));
    CHECK(b->arr[0].b);
    CHECK_FALSE(b->arr[1].b);
    CHECK(b->arr[2].is(T::Null));
    CHECK(b->arr[3].s == "x");
    CHECK(v.get("c")->get("d")->n == -25.0);
    CHECK(v.get("missing") == nullptr);
}

TEST_CASE("json: string escapes, unicode escapes and surrogate pairs become UTF-8") {
    auto r = json::parse(R"(["a\"b\\c\/d\b\f\n\r\t", "æø", "😀", "plain æø"])");
    REQUIRE(r.value);
    const auto& a = r.value->arr;
    CHECK(a[0].s == "a\"b\\c/d\b\f\n\r\t");
    CHECK(a[1].s == "\xC3\xA6\xC3\xB8");
    CHECK(a[2].s == "\xF0\x9F\x98\x80");
    CHECK(a[3].s == "plain \xC3\xA6\xC3\xB8");
}

TEST_CASE("json: with replace_lone_surrogates (other programs' JSON), a lone UTF-16 surrogate becomes U+FFFD") {
    const json::ParseOptions lenient{true};
    const std::string fffd = "\xEF\xBF\xBD";
    const auto one = [&](const std::string& text) {
        const auto r = json::parse("[\"" + text + "\"]", lenient);
        REQUIRE_MESSAGE(r.value, r.error);
        return r.value->arr.at(0).s;
    };
    CHECK(one("a\\ud800b") == "a" + fffd + "b");            // a high one alone
    CHECK(one("\\udc00") == fffd);                          // a low one alone
    CHECK(one("\\ud800") == fffd);                          // at the end of the string
    CHECK(one("\\ud800\\u0041") == fffd + "A");              // followed by another escape: that one is kept
    CHECK(one("\\ud800\\ud83d\\ude00") == fffd + "\xF0\x9F\x98\x80");  // then a real pair
    CHECK(one("\\ud83d\\ude00") == "\xF0\x9F\x98\x80");      // a pair as before
    CHECK(one("\\ud800\\n") == fffd + "\n");
    // the default (mod manifests) still refuses it, with the position
    CHECK_FALSE(json::parse("[\"\\ud800\"]").value);
}

TEST_CASE("json: numbers") {
    auto r = json::parse("[0, -7, 3.25, 1e3, 2E-2, 12345678]");
    REQUIRE(r.value);
    const auto& a = r.value->arr;
    CHECK(a[0].n == 0.0);
    CHECK(a[1].n == -7.0);
    CHECK(a[2].n == 3.25);
    CHECK(a[3].n == 1000.0);
    CHECK(a[4].n == doctest::Approx(0.02));
    CHECK(a[5].n == 12345678.0);
}

TEST_CASE("json: a UTF-8 BOM, CRLF and surrounding whitespace are fine") {
    auto r = json::parse("\xEF\xBB\xBF\r\n  {\r\n  \"id\": \"clock\"\r\n}\r\n  ");
    REQUIRE(r.value);
    CHECK(r.value->get("id")->s == "clock");
}

TEST_CASE("json: errors say what and where (line, column)") {
    auto r = json::parse(R"({"a": })");
    CHECK_FALSE(r.value);
    CHECK(r.line == 1);
    CHECK(r.column == 7);
    CHECK_FALSE(r.error.empty());

    r = json::parse("{\n  \"a\": 1,\n}");  // trailing comma
    CHECK_FALSE(r.value);
    CHECK(r.line == 3);
    CHECK(r.column == 1);

    CHECK_FALSE(json::parse(R"(["abc)").value);       // unterminated string
    CHECK_FALSE(json::parse(R"({"a": 1} x)").value);  // garbage after the value
    CHECK_FALSE(json::parse("").value);
    CHECK_FALSE(json::parse("[1, 2,]").value);
    CHECK_FALSE(json::parse(R"({"a" 1})").value);      // missing colon
    const std::string bad_escape = "[\"\\x\"]", lone_surrogate = "[\"\\ud83d\"]";
    CHECK_FALSE(json::parse(bad_escape).value);
    CHECK_FALSE(json::parse(lone_surrogate).value);
    CHECK_FALSE(json::parse("[01]").value);           // leading zero
    CHECK_FALSE(json::parse("[tru]").value);
    CHECK_FALSE(json::parse("[\"a\nb\"]").value);     // raw control character in a string
}

TEST_CASE("json: nesting deeper than 64 levels is refused; duplicate keys keep the last value") {
    std::string deep(65, '[');
    deep += std::string(65, ']');
    CHECK_FALSE(json::parse(deep).value);
    std::string ok(64, '[');
    ok += std::string(64, ']');
    CHECK(json::parse(ok).value);

    auto r = json::parse(R"({"a": 1, "a": 2})");
    REQUIRE(r.value);
    CHECK(r.value->get("a")->n == 2.0);
}
