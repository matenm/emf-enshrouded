// Field access by name through the generated reflection table (used by the Lua host).
#include <doctest.h>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>

#include "emf/generated/components.hpp"
#include "lua_host/fields.h"

using namespace emf::lua;
using emf::gen::reflect::Kind;

TEST_CASE("lua fields: types by short or full name; nested fields add up to the generated offsets") {
    const auto* t = find_type("CurrentTransform");
    REQUIRE(t);
    CHECK(t == find_type("keen::ecs::CurrentTransform"));
    CHECK(t->hash == emf::gen::ecs::CurrentTransform::kHash);
    CHECK(t->size == sizeof(emf::gen::ecs::CurrentTransform));
    CHECK(find_type("NoSuchThing") == nullptr);

    const auto* transform = find_field(*t, "transform");
    REQUIRE(transform);
    CHECK(transform->kind == Kind::Struct);
    const auto* wt = type_at(transform->type);
    REQUIRE(wt);
    const auto* position = find_field(*wt, "position");
    REQUIRE(position);
    const auto* x = find_field(*type_at(position->type), "x");
    REQUIRE(x);
    CHECK(transform->offset + position->offset + x->offset ==
          offsetof(emf::gen::ecs::CurrentTransform, transform) + offsetof(emf::gen::WorldTransform, position) +
              offsetof(emf::gen::WorldPosition, x));
    CHECK(x->kind == Kind::I64);
    CHECK(find_field(*t, "nope") == nullptr);

    const auto* hit = find_type("HitEvent");
    REQUIRE(hit);
    CHECK((hit->flags & 2) != 0);  // an event
    CHECK(hit->size == emf::gen::ecs::HitEvent::kSize);
    CHECK(find_type("keen::ecs::HitEvent") == hit);  // the game's full name
    CHECK(find_type("keen::HitEvent") == hit);       // a leading keen:: is dropped when nothing has that name
    CHECK(find_type("keen::") == nullptr);
}

TEST_CASE("lua fields: scalar reads and writes with range checks") {
    uint8_t mem[32] = {};
    auto field = [](Kind k, uint32_t off, uint32_t size, uint32_t count = 1) {
        emf::gen::reflect::Field f{"f", off, size, k, 0xffff, count};
        return f;
    };
    std::string why;
    const auto u8 = field(Kind::U8, 0, 1);
    CHECK(write_field(mem, u8, 0, Value::integer(200), &why));
    CHECK(read_field(mem, u8, 0).i == 200);
    CHECK_FALSE(write_field(mem, u8, 0, Value::integer(300), &why));
    CHECK(why.find("0 to 255") != std::string::npos);
    CHECK_FALSE(write_field(mem, u8, 0, Value::number(1.5), &why));

    const auto i32 = field(Kind::I32, 4, 4);
    CHECK(write_field(mem, i32, 0, Value::integer(-5), &why));
    CHECK(read_field(mem, i32, 0).i == -5);
    CHECK(write_field(mem, i32, 0, Value::number(7.0), &why));  // a whole number given as a float is fine
    CHECK(read_field(mem, i32, 0).i == 7);
    const auto u32 = field(Kind::U32, 8, 4);
    CHECK_FALSE(write_field(mem, u32, 0, Value::integer(-1), &why));

    const auto f32 = field(Kind::F32, 12, 4);
    CHECK(write_field(mem, f32, 0, Value::number(2.5), &why));
    CHECK(read_field(mem, f32, 0).k == Value::Number);
    CHECK(read_field(mem, f32, 0).n == 2.5);

    const auto b = field(Kind::Bool, 16, 1);
    CHECK(write_field(mem, b, 0, Value::boolean(true), &why));
    CHECK(read_field(mem, b, 0).b);
    CHECK_FALSE(write_field(mem, b, 0, Value::integer(1), &why));  // booleans only

    const auto arr = field(Kind::U16, 18, 6, 3);  // uint16_t[3]
    CHECK(write_field(mem, arr, 2, Value::integer(4242), &why));
    CHECK(read_field(mem, arr, 2).i == 4242);
    CHECK(read_field(mem, arr, 3).k == Value::Nil);  // past the end
    CHECK_FALSE(write_field(mem, arr, 3, Value::integer(1), &why));

    const auto f64 = field(Kind::F64, 24, 8);
    for (const double bad : {std::nan(""), HUGE_VAL, -HUGE_VAL}) {
        CHECK_FALSE(write_field(mem, f32, 0, Value::number(bad), &why));
        CHECK(why.find("finite") != std::string::npos);
        CHECK_FALSE(write_field(mem, f64, 0, Value::number(bad), &why));
    }
    CHECK(read_field(mem, f32, 0).n == 2.5);  // unchanged
    CHECK_FALSE(write_field(mem, f32, 0, Value::number(1e300), &why));  // too big for a float
    CHECK(write_field(mem, f64, 0, Value::number(1e300), &why));

    uint8_t guid[16] = {0x17, 0x52, 0xd0, 0xd5, 0xdc, 0x52, 0x7a, 0x46, 0x91, 0x92, 0x26, 0xb4, 0xf6, 0x61, 0xd8, 0xd9};
    const auto g = field(Kind::Guid, 0, 16);
    const Value gv = read_field(guid, g, 0);
    CHECK(gv.k == Value::String);
    CHECK(gv.s == "d5d05217-52dc-467a-9192-26b4f661d8d9");
    CHECK_FALSE(write_field(guid, g, 0, Value::string("x"), &why));
}

TEST_CASE("lua fields: a u64 at or above 2^63 reads as a positive number (not a negative integer) and writes back") {
    uint8_t mem[8] = {};
    const emf::gen::reflect::Field u64{"f", 0, 8, Kind::U64, 0xffff, 1};
    std::string why;
    const uint64_t big = 0xF000000000000000ull;  // exactly representable as a double
    std::memcpy(mem, &big, 8);
    const Value v = read_field(mem, u64, 0);
    CHECK(v.k == Value::Number);
    CHECK(v.n == static_cast<double>(big));
    std::memset(mem, 0, 8);
    CHECK(write_field(mem, u64, 0, v, &why));
    uint64_t back = 0;
    std::memcpy(&back, mem, 8);
    CHECK(back == big);
    CHECK_FALSE(write_field(mem, u64, 0, Value::number(2e19), &why));  // past 2^64
    CHECK_FALSE(write_field(mem, u64, 0, Value::integer(-1), &why));
    CHECK(write_field(mem, u64, 0, Value::integer(5), &why));
    CHECK(read_field(mem, u64, 0).k == Value::Int);  // below 2^63: an integer as before
    CHECK(read_field(mem, u64, 0).i == 5);
}
