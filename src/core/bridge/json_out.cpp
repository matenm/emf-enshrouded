#include "json_out.h"

#include <cmath>
#include <cstdio>

namespace emf::core::bridge {

std::string json_quote(std::string_view s) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20 || c == 0x7F) {
                        out += "\\u00";
                        out += kHex[c >> 4];
                        out += kHex[c & 15];
                    } else {
                        out += static_cast<char>(c);
                    }
            }
            ++i;
            continue;
        }
        // The length of a valid UTF-8 sequence starting here, or 0 (RFC 3629: no overlongs, no surrogates).
        auto cont = [&](size_t k, unsigned lo, unsigned hi) {
            if (i + k >= s.size()) return false;
            const auto b = static_cast<unsigned char>(s[i + k]);
            return b >= lo && b <= hi;
        };
        size_t len = 0;
        if (c >= 0xC2 && c <= 0xDF) len = cont(1, 0x80, 0xBF) ? 2 : 0;
        else if (c == 0xE0) len = cont(1, 0xA0, 0xBF) && cont(2, 0x80, 0xBF) ? 3 : 0;
        else if ((c >= 0xE1 && c <= 0xEC) || c == 0xEE || c == 0xEF) len = cont(1, 0x80, 0xBF) && cont(2, 0x80, 0xBF) ? 3 : 0;
        else if (c == 0xED) len = cont(1, 0x80, 0x9F) && cont(2, 0x80, 0xBF) ? 3 : 0;
        else if (c == 0xF0) len = cont(1, 0x90, 0xBF) && cont(2, 0x80, 0xBF) && cont(3, 0x80, 0xBF) ? 4 : 0;
        else if (c >= 0xF1 && c <= 0xF3) len = cont(1, 0x80, 0xBF) && cont(2, 0x80, 0xBF) && cont(3, 0x80, 0xBF) ? 4 : 0;
        else if (c == 0xF4) len = cont(1, 0x80, 0x8F) && cont(2, 0x80, 0xBF) && cont(3, 0x80, 0xBF) ? 4 : 0;
        if (len == 0) {
            out += "\xEF\xBF\xBD";
            ++i;
        } else {
            out.append(s.substr(i, len));
            i += len;
        }
    }
    out += '"';
    return out;
}

std::string json_number(double v) {
    char b[64];
    if (!std::isfinite(v)) return "0";
    if (v == std::floor(v) && std::fabs(v) < 1e15)
        std::snprintf(b, sizeof b, "%lld", static_cast<long long>(v));
    else
        std::snprintf(b, sizeof b, "%.15g", v);
    return b;
}

std::string json_list(const std::vector<std::string>& items) {
    std::string a = "[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) a += ',';
        a += items[i];
    }
    return a + "]";
}

std::string to_json(const json::Value& v) {
    using T = json::Value::Type;
    switch (v.type) {
        case T::Null: return "null";
        case T::Bool: return v.b ? "true" : "false";
        case T::Number: return json_number(v.n);
        case T::String: return json_quote(v.s);
        case T::Array: {
            std::vector<std::string> items;
            for (const auto& e : v.arr) items.push_back(to_json(e));
            return json_list(items);
        }
        case T::Object: {
            std::string o = "{";
            for (size_t i = 0; i < v.obj.size(); ++i) {
                if (i) o += ',';
                o += json_quote(v.obj[i].first) + ":" + to_json(v.obj[i].second);
            }
            return o + "}";
        }
    }
    return "null";
}

}  // namespace emf::core::bridge
