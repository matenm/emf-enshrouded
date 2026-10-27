#pragma once
// A small JSON writer for the debug bridge's replies: JsonObj().str("k", v).num("n", 1).text().
#include <string>
#include <string_view>
#include <vector>

#include "core/json.h"

namespace emf::core::bridge {

// A JSON string literal: quotes, backslashes and control characters escaped; bytes that are not UTF-8 become U+FFFD.
std::string json_quote(std::string_view s);
// A number: whole numbers without a fraction, others with up to 15 digits; NaN and infinity become 0.
std::string json_number(double v);
// [a, b, ...] from items that are JSON already.
std::string json_list(const std::vector<std::string>& items);
// A parsed value written back as JSON (a request's id, echoed in the reply).
std::string to_json(const json::Value& v);

// One JSON object: the calls add members in order, text() closes it.
class JsonObj {
public:
    JsonObj& str(const char* k, std::string_view v) { return raw(k, json_quote(v)); }
    JsonObj& num(const char* k, double v) { return raw(k, json_number(v)); }
    JsonObj& boolean(const char* k, bool v) { return raw(k, v ? "true" : "false"); }
    JsonObj& null(const char* k) { return raw(k, "null"); }
    JsonObj& raw(const char* k, const std::string& json) {
        text_ += (text_.size() > 1 ? "," : "") + json_quote(k) + ":" + json;
        return *this;
    }
    // The members so far without the braces ("a":1,"b":2), for splicing into a reply.
    std::string members() const { return text_.substr(1); }
    std::string text() const { return text_ + "}"; }

private:
    std::string text_ = "{";
};

}  // namespace emf::core::bridge
