// emf/keys.hpp: a mod's key setting and single presses.
#include <windows.h>

#include <doctest.h>

#include <string>

#include "emf/keys.hpp"

TEST_CASE("keys: a setting is a letter, a digit or a keyboard virtual-key number") {
    std::string name;
    CHECK(emf::keys::parse_key(L"G", &name) == 'G');
    CHECK(name == "G");
    CHECK(emf::keys::parse_key(L"g", &name) == 'G');
    CHECK(emf::keys::parse_key(L"0x71", &name) == 0x71);
    CHECK(name == "F2");  // a numbered key that has a name shows the name
    CHECK(emf::keys::parse_key(L"0x02", &name) == 0);  // mouse button
    CHECK(emf::keys::parse_key(L"0xA1", &name) == 0);  // VK_RSHIFT
    CHECK(emf::keys::parse_key(L"", &name) == 0);
}

TEST_CASE("keys: an edge turns held samples into one press per push") {
    emf::keys::Edge e;
    CHECK_FALSE(e.pressed(false));
    CHECK(e.pressed(true));
    CHECK_FALSE(e.pressed(true));  // still held
    CHECK_FALSE(e.pressed(false));
    CHECK(e.pressed(true));
}

TEST_CASE("keys: a number is hex with 0x, else decimal (a leading 0 is not octal); key settings store the name") {
    std::string name;
    CHECK(emf::keys::parse_key(L"0x71", &name) == VK_F2);
    CHECK(name == "F2");
    CHECK(emf::keys::parse_key(L"0X71") == VK_F2);
    CHECK(emf::keys::parse_key(L"113") == VK_F2);
    CHECK(emf::keys::parse_key(L"071") == 71u);  // 'G', not octal 57 ('9')
    CHECK(emf::keys::parse_key(L"09") == VK_TAB);  // decimal 9, not a broken octal number
    CHECK(emf::keys::parse_key(L"0x") == 0);
    CHECK(emf::keys::parse_key(L"0xZZ") == 0);
}

TEST_CASE("keys: parse_key works without asking for the key's name") {
    CHECK(emf::keys::parse_key(L"J") == 'J');
    CHECK(emf::keys::parse_key(L"0x71") == 0x71u);
}
