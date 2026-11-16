// A player's character name from the server's table of custom player strings (private research
// character_name_2026-11-10): the CustomPlayerString id leads to an entry in CustomPlayerStringsSystem's map; the text is
// taken only when it hashes to the id.
#include <doctest.h>

#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "common/fnv.h"
#include "core/manifest.h"
#include "core/player_strings.h"

using namespace emf::core;

namespace {

template <class T>
void put(uint8_t* mem, size_t off, T v) {
    std::memcpy(mem + off, &v, sizeof v);
}

// A fake CustomPlayerStringsSystem: its Keen map at +0x190 (8 slots, u64 keys, entry pointers) and the entries.
struct FakeTable {
    std::vector<uint8_t> sys = std::vector<uint8_t>(0x200, 0);
    std::array<uint64_t, 1> bits{};
    std::array<uint64_t, 8> keys{};
    std::array<const uint8_t*, 8> values{};
    std::array<std::array<uint8_t, 0x140>, 8> entries{};
    std::array<std::string, 8> texts;
    uint32_t count = 0;

    FakeTable() { write_header(); }
    void write_header() {
        uint8_t* map = sys.data() + kCustomStringsMap;
        put<const void*>(map, 0x08, bits.data());
        put<uint64_t>(map, 0x10, 8);
        put<const void*>(map, 0x28, keys.data());
        put<const void*>(map, 0x40, values.data());
        put<uint32_t>(map, 0x54, count);
    }
    // An entry in `slot` under `key`, holding `text` (its own id and length unless given).
    void add(size_t slot, uint64_t key, const std::string& text, uint64_t entry_id = 0, int64_t len = -1) {
        texts[slot] = text;
        bits[0] |= 1ull << slot;
        keys[slot] = key;
        uint8_t* e = entries[slot].data();
        put<uint64_t>(e, kEntryId, entry_id ? entry_id : key);
        put<const char*>(e, kEntryText, texts[slot].data());
        put<uint64_t>(e, kEntryLen, len < 0 ? texts[slot].size() : static_cast<uint64_t>(len));
        values[slot] = e;
        ++count;
        write_header();
    }
    const uint8_t* system() const { return sys.data(); }
};

uint64_t id_of(const std::string& s) { return emf::fnv1a64(s); }
size_t home(uint64_t id) { return static_cast<uint32_t>(id) & 7u; }

}  // namespace

TEST_CASE("custom_player_string: the entry's text when its id is the text's FNV-1a64") {
    FakeTable t;
    const uint64_t id = id_of("Character 2");
    t.add(home(id), id, "Character 2");
    const auto name = custom_player_string(t.system(), id);
    REQUIRE(name.has_value());
    CHECK(*name == "Character 2");
}

TEST_CASE("custom_player_string: a colliding slot is probed past") {
    FakeTable t;
    const uint64_t id = id_of("Character 2");
    const uint64_t other = id_of("Somebody else");
    t.add(home(id), other, "Somebody else");  // another name sits in this id's home slot
    t.add((home(id) + 1) & 7u, id, "Character 2");
    const auto name = custom_player_string(t.system(), id);
    REQUIRE(name.has_value());
    CHECK(*name == "Character 2");
}

TEST_CASE("custom_player_string: nothing for a missing id, id 0 or no system") {
    FakeTable t;
    const uint64_t id = id_of("Character 2");
    t.add(home(id), id, "Character 2");
    CHECK_FALSE(custom_player_string(t.system(), id_of("Nobody")).has_value());
    CHECK_FALSE(custom_player_string(t.system(), 0).has_value());
    CHECK_FALSE(custom_player_string(nullptr, id).has_value());
    FakeTable empty;
    CHECK_FALSE(custom_player_string(empty.system(), id).has_value());  // a map with no entries
}

TEST_CASE("custom_player_string: nothing when the text does not hash to the id") {  // Review Focus 1
    FakeTable t;
    const uint64_t id = id_of("Character 2");
    t.add(home(id), id, "Character 3");  // a reused entry: the key is still the old id
    CHECK_FALSE(custom_player_string(t.system(), id).has_value());
    FakeTable u;
    u.add(home(id), id, "Character 2", id_of("Character 3"));  // the entry says another id
    CHECK_FALSE(custom_player_string(u.system(), id).has_value());
}

TEST_CASE("custom_player_string: nothing for length 0 or over 256") {
    FakeTable t;
    const uint64_t id = id_of("Character 2");
    t.add(home(id), id, "Character 2", 0, 0);
    CHECK_FALSE(custom_player_string(t.system(), id).has_value());
    FakeTable u;
    u.add(home(id), id, "Character 2", 0, 0x101);
    CHECK_FALSE(custom_player_string(u.system(), id).has_value());
}

TEST_CASE("custom_player_string: nothing for control characters or bad UTF-8") {  // Review Focus 4
    for (const std::string bad : {std::string("Bad\nname"), std::string("Bad\x01name"), std::string("Bad\xff name")}) {
        FakeTable t;
        const uint64_t id = id_of(bad);
        t.add(home(id), id, bad);
        CHECK_FALSE_MESSAGE(custom_player_string(t.system(), id).has_value(), bad);
    }
}

TEST_CASE("custom_player_string: nothing for C1 controls or the bidi controls that reorder the text around them") {
    // a C1 control (U+0085), bidi overrides and isolates (U+202E, U+202A, U+2066, U+2069), marks (U+200F, U+061C)
    const char* const kSpoofs[] = {"Bad\xc2\x85name", "Bad\xe2\x80\xaeman", "Bad\xe2\x80\xaaname", "Bad\xe2\x81\xa6name",
                                   "Bad\xe2\x81\xa9name", "Bad\xe2\x80\x8fname", "Bad\xd8\x9cname", "Bad\xc2\x9b[2J"};
    for (const char* bad : kSpoofs) {
        FakeTable t;
        const uint64_t id = id_of(bad);
        t.add(home(id), id, bad);
        CHECK_FALSE_MESSAGE(custom_player_string(t.system(), id).has_value(), bad);
    }
}

TEST_CASE("display_name_ok: well-formed UTF-8 without control characters or bidi controls") {
    CHECK(display_name_ok("Ember"));
    CHECK(display_name_ok("\xc3\x86r\xc3\xb8sk\xc3\xb8"));          // AEroskoe with its letters
    CHECK(display_name_ok("\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d"));    // Hebrew letters (right to left, no controls)
    CHECK(display_name_ok("\xe4\xb8\xb8\xe5\xa4\xaa"));            // CJK
    CHECK(display_name_ok("A\xe2\x80\x90" "B"));                   // U+2010 (a hyphen), next to the controls
    CHECK(display_name_ok("A\xc2\xa0" "B"));                       // U+00A0 (no-break space), right after C1
    CHECK_FALSE(display_name_ok(""));
    CHECK_FALSE(display_name_ok("Bad\nname"));
    CHECK_FALSE(display_name_ok("Bad\x7f" "name"));
    CHECK_FALSE(display_name_ok("Bad\xff"));
    CHECK_FALSE(display_name_ok("Cut\xc3"));
    // a C1 control (U+0085), bidi overrides and isolates (U+202E, U+202A, U+2066, U+2069), marks (U+200F, U+061C)
    const char* const kBad[] = {"Bad\xc2\x85name", "Bad\xe2\x80\xaeman", "Bad\xe2\x80\xaaname", "Bad\xe2\x81\xa6name",
                                "Bad\xe2\x81\xa9name", "Bad\xe2\x80\x8fname", "Bad\xd8\x9cname", "Bad\xc2\x9b[2J"};
    for (const char* bad : kBad) CHECK_FALSE_MESSAGE(display_name_ok(bad), bad);
}

TEST_CASE("custom_player_string: a name with non-ASCII letters") {
    const std::string name = "\xc3\x86r\xc3\xb8sk\xc3\xb8" "bing";  // Ærøskøbing
    FakeTable t;
    const uint64_t id = id_of(name);
    t.add(home(id), id, name);
    const auto got = custom_player_string(t.system(), id);
    REQUIRE(got.has_value());
    CHECK(*got == name);
}
