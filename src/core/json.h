#pragma once
// A small JSON reader for mod manifests (mod.json): the full JSON grammar, strings decoded to UTF-8, numbers as
// double, nesting up to 64 levels, a leading UTF-8 BOM skipped. Errors give a message with line and column.
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace emf::core::json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool b = false;
    double n = 0.0;
    std::string s;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;  // in file order; a repeated key keeps its last value

    bool is(Type t) const { return type == t; }
    // The member `key` of an object, or nullptr (also for non-objects).
    const Value* get(std::string_view key) const;
};

struct ParseResult {
    std::optional<Value> value;  // empty on an error
    std::string error;
    size_t line = 0, column = 0;  // 1-based position of the error
};

struct ParseOptions {
    // A lone UTF-16 surrogate in a \\u escape ("\\ud800" without its pair) becomes U+FFFD instead of an error. For
    // JSON from other programs (Nexus); mod manifests stay strict, so their authors hear about it.
    bool replace_lone_surrogates = false;
};

ParseResult parse(std::string_view text, const ParseOptions& options = {});

}  // namespace emf::core::json
