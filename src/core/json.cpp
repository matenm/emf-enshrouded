#include "json.h"

#include <cstdlib>
#include <unordered_map>

namespace emf::core::json {

const Value* Value::get(std::string_view key) const {
    if (type != Type::Object) return nullptr;
    for (auto it = obj.rbegin(); it != obj.rend(); ++it)
        if (it->first == key) return &it->second;
    return nullptr;
}

namespace {

constexpr int kMaxDepth = 64;

class Parser {
public:
    Parser(std::string_view t, const ParseOptions& o) : t_(t), opts_(o) {
        if (t_.size() >= 3 && t_.substr(0, 3) == "\xEF\xBB\xBF") p_ = 3;
    }

    ParseResult run() {
        ParseResult r;
        Value v;
        skip_ws();
        if (!value(v, 0)) return fail(r);
        skip_ws();
        if (p_ != t_.size()) {
            err_ = "unexpected text after the value";
            return fail(r);
        }
        r.value = std::move(v);
        return r;
    }

private:
    ParseResult& fail(ParseResult& r) {
        r.error = err_;
        size_t line = 1, col = 1;
        for (size_t i = 0; i < err_pos_ && i < t_.size(); ++i) {
            if (t_[i] == '\n') {
                ++line;
                col = 1;
            } else if (t_[i] != '\r') {
                ++col;
            }
        }
        r.line = line;
        r.column = col;
        r.error += " (line " + std::to_string(line) + ", column " + std::to_string(col) + ")";
        return r;
    }
    bool error(const char* what) {
        if (err_.empty()) {
            err_ = what;
            err_pos_ = p_;
        }
        return false;
    }
    void skip_ws() {
        while (p_ < t_.size() && (t_[p_] == ' ' || t_[p_] == '\t' || t_[p_] == '\n' || t_[p_] == '\r')) ++p_;
    }
    bool literal(std::string_view word) {
        if (t_.substr(p_, word.size()) != word) return error("unknown word (expected true, false or null)");
        p_ += word.size();
        return true;
    }

    bool value(Value& v, int depth) {
        if (p_ >= t_.size()) return error("a value is missing");
        const char c = t_[p_];
        if (c == '{') return object(v, depth + 1);
        if (c == '[') return array(v, depth + 1);
        if (c == '"') {
            v.type = Value::Type::String;
            return string(v.s);
        }
        if (c == 't') {
            v.type = Value::Type::Bool;
            v.b = true;
            return literal("true");
        }
        if (c == 'f') {
            v.type = Value::Type::Bool;
            v.b = false;
            return literal("false");
        }
        if (c == 'n') {
            v.type = Value::Type::Null;
            return literal("null");
        }
        if (c == '-' || (c >= '0' && c <= '9')) return number(v);
        return error("a value is missing");
    }

    bool object(Value& v, int depth) {
        if (depth > kMaxDepth) return error("nested too deeply (more than 64 levels)");
        v.type = Value::Type::Object;
        ++p_;  // {
        skip_ws();
        if (p_ < t_.size() && t_[p_] == '}') {
            ++p_;
            return true;
        }
        // A repeated key keeps its last value: found by a scan while the object is small, then through an index (an
        // object with thousands of keys stays linear).
        std::unordered_map<std::string, size_t> index;
        for (;;) {
            skip_ws();
            if (p_ >= t_.size() || t_[p_] != '"') return error("a key in double quotes is expected");
            std::string key;
            if (!string(key)) return false;
            skip_ws();
            if (p_ >= t_.size() || t_[p_] != ':') return error("':' is expected after a key");
            ++p_;
            skip_ws();
            Value member;
            if (!value(member, depth)) return false;
            constexpr size_t kScanUpTo = 16;
            if (v.obj.size() >= kScanUpTo && index.empty())
                for (size_t i = 0; i < v.obj.size(); ++i) index.emplace(v.obj[i].first, i);
            bool replaced = false;
            if (!index.empty()) {
                const auto it = index.find(key);
                if (it != index.end()) {
                    v.obj[it->second].second = std::move(member);
                    replaced = true;
                } else {
                    index.emplace(key, v.obj.size());
                }
            } else {
                for (auto& kv : v.obj)
                    if (kv.first == key) {
                        kv.second = std::move(member);
                        replaced = true;
                        break;
                    }
            }
            if (!replaced) v.obj.emplace_back(std::move(key), std::move(member));
            skip_ws();
            if (p_ < t_.size() && t_[p_] == ',') {
                ++p_;
                skip_ws();
                if (p_ < t_.size() && t_[p_] == '}') return error("a trailing comma is not allowed");
                continue;
            }
            if (p_ < t_.size() && t_[p_] == '}') {
                ++p_;
                return true;
            }
            return error("',' or '}' is expected");
        }
    }

    bool array(Value& v, int depth) {
        if (depth > kMaxDepth) return error("nested too deeply (more than 64 levels)");
        v.type = Value::Type::Array;
        ++p_;  // [
        skip_ws();
        if (p_ < t_.size() && t_[p_] == ']') {
            ++p_;
            return true;
        }
        for (;;) {
            skip_ws();
            Value item;
            if (!value(item, depth)) return false;
            v.arr.push_back(std::move(item));
            skip_ws();
            if (p_ < t_.size() && t_[p_] == ',') {
                ++p_;
                skip_ws();
                if (p_ < t_.size() && t_[p_] == ']') return error("a trailing comma is not allowed");
                continue;
            }
            if (p_ < t_.size() && t_[p_] == ']') {
                ++p_;
                return true;
            }
            return error("',' or ']' is expected");
        }
    }

    static void put_utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(uint32_t& out) {
        if (p_ + 4 > t_.size()) return error("\\u needs four hex digits");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = t_[p_++];
            out <<= 4;
            if (h >= '0' && h <= '9') out |= h - '0';
            else if (h >= 'a' && h <= 'f') out |= h - 'a' + 10;
            else if (h >= 'A' && h <= 'F') out |= h - 'A' + 10;
            else return error("\\u needs four hex digits");
        }
        return true;
    }

    bool string(std::string& out) {
        ++p_;  // "
        for (;;) {
            if (p_ >= t_.size()) return error("a string is not closed");
            const char c = t_[p_];
            if (c == '"') {
                ++p_;
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) return error("a control character inside a string (use \\n)");
            if (c != '\\') {
                out += c;
                ++p_;
                continue;
            }
            ++p_;
            if (p_ >= t_.size()) return error("a string is not closed");
            const char e = t_[p_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {  // a high surrogate: a low one must follow
                        uint32_t lo = 0;
                        const size_t after = p_;
                        const bool paired = t_.substr(p_, 2) == "\\u" && (p_ += 2, hex4(lo)) && lo >= 0xDC00 && lo <= 0xDFFF;
                        if (paired) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            if (!opts_.replace_lone_surrogates) {
                                if (!err_.empty()) return false;  // \\u with bad digits after it
                                return error("a lone surrogate in \\u escapes");
                            }
                            err_.clear();  // what follows is read on its own (bad hex digits are an error there)
                            p_ = after;
                            cp = 0xFFFD;
                        }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        if (!opts_.replace_lone_surrogates) return error("a lone surrogate in \\u escapes");
                        cp = 0xFFFD;
                    }
                    put_utf8(out, cp);
                    break;
                }
                default: --p_; return error("unknown escape in a string");
            }
        }
    }

    bool number(Value& v) {
        const size_t start = p_;
        if (t_[p_] == '-') ++p_;
        if (p_ >= t_.size() || t_[p_] < '0' || t_[p_] > '9') return error("a digit is expected");
        if (t_[p_] == '0' && p_ + 1 < t_.size() && t_[p_ + 1] >= '0' && t_[p_ + 1] <= '9')
            return error("numbers do not start with 0");
        while (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9') ++p_;
        if (p_ < t_.size() && t_[p_] == '.') {
            ++p_;
            if (p_ >= t_.size() || t_[p_] < '0' || t_[p_] > '9') return error("a digit is expected after '.'");
            while (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9') ++p_;
        }
        if (p_ < t_.size() && (t_[p_] == 'e' || t_[p_] == 'E')) {
            ++p_;
            if (p_ < t_.size() && (t_[p_] == '+' || t_[p_] == '-')) ++p_;
            if (p_ >= t_.size() || t_[p_] < '0' || t_[p_] > '9') return error("a digit is expected in the exponent");
            while (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9') ++p_;
        }
        const std::string text(t_.substr(start, p_ - start));
        v.type = Value::Type::Number;
        v.n = std::strtod(text.c_str(), nullptr);
        return true;
    }

    std::string_view t_;
    ParseOptions opts_;
    size_t p_ = 0;
    std::string err_;
    size_t err_pos_ = 0;
};

}  // namespace

ParseResult parse(std::string_view text, const ParseOptions& options) { return Parser(text, options).run(); }

}  // namespace emf::core::json
