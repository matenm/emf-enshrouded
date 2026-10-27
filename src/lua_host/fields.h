#pragma once
// Fields of the game's structs by name (sdk/include/emf/generated/reflection.hpp): find a component or event type,
// walk its fields, read and write scalar values with range checks. Pure logic (unit-tested); the Lua host builds its
// component proxies and event tables on it.
#include <cstdint>
#include <string>
#include <string_view>

#include "emf/generated/reflection.hpp"

namespace emf::lua {

namespace reflect = emf::gen::reflect;

// A type by short name ("CurrentTransform", "HitEvent") or full name ("keen::ecs::CurrentTransform"; a leading
// "keen::ecs::" or "keen::" is dropped when no type has the full name); nullptr if none.
const reflect::Type* find_type(std::string_view name);
const reflect::Type* type_at(uint16_t index);  // a Struct field's type; nullptr for 0xffff or out of range
const reflect::Field* find_field(const reflect::Type& t, std::string_view name);

struct Value {
    enum Kind { Nil, Bool, Int, Number, String } k = Nil;
    bool b = false;
    int64_t i = 0;
    double n = 0.0;
    std::string s;
    static Value boolean(bool v) { Value x; x.k = Bool; x.b = v; return x; }
    static Value integer(int64_t v) { Value x; x.k = Int; x.i = v; return x; }
    static Value number(double v) { Value x; x.k = Number; x.n = v; return x; }
    static Value string(std::string v) { Value x; x.k = String; x.s = std::move(v); return x; }
};

// Element `element` (0-based) of a scalar field at `base`: Bool, Int (all integer kinds; a u64 from 2^63 is a Number
// so it stays positive), Number (floats), String (a Guid as text). Nil for Struct/Bytes fields and elements past the end.
Value read_field(const uint8_t* base, const reflect::Field& f, uint32_t element);
// Writes a scalar element; false (and *why) when the field is not a writable scalar, the element is past the end, or
// the value does not fit the field's type (range, a fraction into an integer, a non-boolean into a bool, NaN or
// infinity into a float).
bool write_field(uint8_t* base, const reflect::Field& f, uint32_t element, const Value& v, std::string* why);
const char* kind_name(reflect::Kind k);

}  // namespace emf::lua
