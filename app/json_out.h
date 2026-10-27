#pragma once
// A small JSON writer for EMF.exe's messages to the page and its install records: Obj().str("k", v).num("n", 1).text().
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace emf::app {

// A JSON string literal: quotes, backslashes and control characters escaped; bytes that are not UTF-8 become U+FFFD.
std::string json_quote(const std::string& s);  // bridge.cpp

inline std::string json_number(double v) {
    char b[64];
    if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 1e15) std::snprintf(b, sizeof b, "%lld", static_cast<long long>(v));
    else std::snprintf(b, sizeof b, "%.15g", std::isfinite(v) ? v : 0.0);
    return b;
}

// [a, b, ...] from items that are JSON already.
inline std::string json_list(const std::vector<std::string>& items) {
    std::string a = "[";
    for (size_t i = 0; i < items.size(); ++i) a += (i ? "," : "") + items[i];
    return a + "]";
}

inline std::string json_strings(const std::vector<std::string>& v) {
    std::string a = "[";
    for (size_t i = 0; i < v.size(); ++i) a += (i ? "," : "") + json_quote(v[i]);
    return a + "]";
}

// One JSON object: the calls add members in order, text() closes it.
class Obj {
public:
    Obj& str(const char* k, const std::string& v) { return raw(k, json_quote(v)); }
    Obj& boolean(const char* k, bool v) { return raw(k, v ? "true" : "false"); }
    Obj& num(const char* k, double v) { return raw(k, json_number(v)); }
    Obj& strs(const char* k, const std::vector<std::string>& v) { return raw(k, json_strings(v)); }
    Obj& raw(const char* k, const std::string& json) {
        text_ += (text_.size() > 1 ? "," : "") + json_quote(k) + ":" + json;
        return *this;
    }
    std::string text() const { return text_ + "}"; }

private:
    std::string text_ = "{";
};

}  // namespace emf::app
