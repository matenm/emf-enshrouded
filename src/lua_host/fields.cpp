#include "fields.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>

namespace emf::lua {

namespace {

const std::unordered_map<std::string_view, const reflect::Type*>& names() {
    static const auto map = [] {
        std::unordered_map<std::string_view, const reflect::Type*> m;
        for (uint32_t i = 0; i < reflect::kTypeCount; ++i) {
            const auto& t = reflect::kTypes[i];
            m.emplace(t.name, &t);
            m.emplace(t.short_name, &t);  // the first type with a short name keeps it
        }
        return m;
    }();
    return map;
}

uint32_t element_size(const reflect::Field& f) { return f.count ? f.size / f.count : f.size; }

bool fail(std::string* why, const std::string& text) {
    if (why) *why = text;
    return false;
}

template <class T>
bool put_int(uint8_t* at, const Value& v, int64_t lo, int64_t hi, const char* range, std::string* why) {
    int64_t x = 0;
    if (v.k == Value::Int) {
        x = v.i;
    } else if (v.k == Value::Number && std::isfinite(v.n) && v.n == std::floor(v.n) && std::fabs(v.n) < 9.2e18) {
        x = static_cast<int64_t>(v.n);
    } else {
        return fail(why, std::string("needs a whole number (") + range + ")");
    }
    if (x < lo || x > hi) return fail(why, std::string("out of range (") + range + ")");
    const T t = static_cast<T>(x);
    std::memcpy(at, &t, sizeof t);
    return true;
}

}  // namespace

const reflect::Type* find_type(std::string_view name) {
    const auto& m = names();
    auto it = m.find(name);
    if (it != m.end()) return it->second;
    // "keen::HitEvent" (a namespace off): the short name
    for (std::string_view prefix : {std::string_view("keen::ecs::"), std::string_view("keen::")})
        if (name.size() > prefix.size() && name.substr(0, prefix.size()) == prefix) {
            it = m.find(name.substr(prefix.size()));
            return it == m.end() ? nullptr : it->second;
        }
    return nullptr;
}

const reflect::Type* type_at(uint16_t index) {
    return index < reflect::kTypeCount ? &reflect::kTypes[index] : nullptr;
}

const reflect::Field* find_field(const reflect::Type& t, std::string_view name) {
    for (uint32_t i = 0; i < t.field_count; ++i) {
        const auto& f = reflect::kFields[t.first_field + i];
        if (name == f.name) return &f;
    }
    return nullptr;
}

const char* kind_name(reflect::Kind k) {
    switch (k) {
        case reflect::Kind::Bool: return "bool";
        case reflect::Kind::U8: return "u8";
        case reflect::Kind::I8: return "i8";
        case reflect::Kind::U16: return "u16";
        case reflect::Kind::I16: return "i16";
        case reflect::Kind::U32: return "u32";
        case reflect::Kind::I32: return "i32";
        case reflect::Kind::U64: return "u64";
        case reflect::Kind::I64: return "i64";
        case reflect::Kind::F32: return "f32";
        case reflect::Kind::F64: return "f64";
        case reflect::Kind::Struct: return "struct";
        case reflect::Kind::Guid: return "guid";
        default: return "bytes";
    }
}

Value read_field(const uint8_t* base, const reflect::Field& f, uint32_t element) {
    if (element >= f.count) return {};
    const uint8_t* at = base + f.offset + element * element_size(f);
    auto get = [at](auto t) {
        std::memcpy(&t, at, sizeof t);
        return t;
    };
    switch (f.kind) {
        case reflect::Kind::Bool: return Value::boolean(get(uint8_t{}) != 0);
        case reflect::Kind::U8: return Value::integer(get(uint8_t{}));
        case reflect::Kind::I8: return Value::integer(get(int8_t{}));
        case reflect::Kind::U16: return Value::integer(get(uint16_t{}));
        case reflect::Kind::I16: return Value::integer(get(int16_t{}));
        case reflect::Kind::U32: return Value::integer(get(uint32_t{}));
        case reflect::Kind::I32: return Value::integer(get(int32_t{}));
        case reflect::Kind::U64: {  // from 2^63 a Lua integer would turn negative: a (positive) float instead
            const uint64_t u = get(uint64_t{});
            if (u > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return Value::number(static_cast<double>(u));
            return Value::integer(static_cast<int64_t>(u));
        }
        case reflect::Kind::I64: return Value::integer(get(int64_t{}));
        case reflect::Kind::F32: return Value::number(get(float{}));
        case reflect::Kind::F64: return Value::number(get(double{}));
        case reflect::Kind::Guid: {
            char b[40];
            std::snprintf(b, sizeof b, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", at[3], at[2],
                          at[1], at[0], at[5], at[4], at[7], at[6], at[8], at[9], at[10], at[11], at[12], at[13], at[14],
                          at[15]);
            return Value::string(b);
        }
        default: return {};
    }
}

bool write_field(uint8_t* base, const reflect::Field& f, uint32_t element, const Value& v, std::string* why) {
    if (element >= f.count) return fail(why, "index past the end of the array");
    uint8_t* at = base + f.offset + element * element_size(f);
    switch (f.kind) {
        case reflect::Kind::Bool:
            if (v.k != Value::Bool) return fail(why, "needs true or false");
            *at = v.b ? 1 : 0;
            return true;
        case reflect::Kind::U8: return put_int<uint8_t>(at, v, 0, 255, "0 to 255", why);
        case reflect::Kind::I8: return put_int<int8_t>(at, v, -128, 127, "-128 to 127", why);
        case reflect::Kind::U16: return put_int<uint16_t>(at, v, 0, 65535, "0 to 65535", why);
        case reflect::Kind::I16: return put_int<int16_t>(at, v, -32768, 32767, "-32768 to 32767", why);
        case reflect::Kind::U32: return put_int<uint32_t>(at, v, 0, 4294967295ll, "0 to 4294967295", why);
        case reflect::Kind::I32: return put_int<int32_t>(at, v, -2147483648ll, 2147483647ll, "a 32-bit integer", why);
        case reflect::Kind::U64:
            // 2^63 .. 2^64 - 1 come as floats (as read_field gives them)
            if (v.k == Value::Number && v.n >= 9223372036854775808.0 && v.n < 18446744073709551616.0 &&
                v.n == std::floor(v.n)) {
                const uint64_t t = static_cast<uint64_t>(v.n);
                std::memcpy(at, &t, sizeof t);
                return true;
            }
            return put_int<uint64_t>(at, v, 0, std::numeric_limits<int64_t>::max(), "0 to 2^64 - 1", why);
        case reflect::Kind::I64:
            return put_int<int64_t>(at, v, std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max(),
                                    "a 64-bit integer", why);
        case reflect::Kind::F32:
        case reflect::Kind::F64: {
            double x = 0.0;
            if (v.k == Value::Number) x = v.n;
            else if (v.k == Value::Int) x = static_cast<double>(v.i);
            else return fail(why, "needs a number");
            if (!std::isfinite(x)) return fail(why, "needs a finite number (not NaN or infinity)");
            if (f.kind == reflect::Kind::F32) {
                if (std::fabs(x) > std::numeric_limits<float>::max()) return fail(why, "too big for a 32-bit float");
                const float t = static_cast<float>(x);
                std::memcpy(at, &t, sizeof t);
            } else {
                std::memcpy(at, &x, sizeof x);
            }
            return true;
        }
        default:
            return fail(why, std::string("a ") + kind_name(f.kind) + " field cannot be written from a script");
    }
}

}  // namespace emf::lua
