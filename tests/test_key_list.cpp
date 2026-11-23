// Every key EMF and its mods use, by key (GitHub #55): src/core/key_list.h.
#include <doctest.h>

#include <string>
#include <vector>

#include "core/key_list.h"

using emf::core::group_keys;
using emf::core::KeyGroup;
using emf::core::KeyUse;
using emf::core::scan_to_vk;

namespace {
KeyUse use(const char* owner, const char* name, const char* label, const char* key, bool on = true, bool shared = false) {
    KeyUse u;
    u.shared = shared;
    u.owner = owner;
    u.owner_name = name;
    u.label = label;
    u.key = key;
    u.on = on;
    return u;
}
// The key under Esc on a US layout (scan 0x29) is VK_OEM_3.
uint32_t us_layout(unsigned scan) { return scan == 0x29 ? 0xC0u : 0u; }
const KeyGroup* group(const std::vector<KeyGroup>& g, const std::string& name) {
    for (const auto& k : g)
        if (k.name == name) return &k;
    return nullptr;
}
}  // namespace

TEST_CASE("key list: keys are grouped as the key settings read them (f3 is F3); empty keys are left out") {
    const auto g = group_keys({use("necromancer", "Necromancer", "Wheel key", "F2"),
                               use("beastmaster", "Beastmaster", "Beasts wheel key", "f3"),
                               use("companion", "Companion", "Order wheel key", "F6"),
                               use("companion", "Companion", "Key: follow me", ""),
                               use("bounty", "Bounty Board", "Read key", "F3")},
                              us_layout);
    REQUIRE(g.size() == 3);
    const KeyGroup* f3 = group(g, "F3");
    REQUIRE(f3 != nullptr);
    CHECK(f3->uses.size() == 2);
    CHECK(f3->clash());
    CHECK_FALSE(group(g, "F2")->clash());
    CHECK(group(g, "F6")->uses.size() == 1);  // the empty one is not a key in use
}

TEST_CASE("key list: a mod switched off uses its key but clashes with nothing") {
    const auto g = group_keys({use("necromancer", "Necromancer", "Wheel key", "F2"),
                               use("summoner", "Summoner", "Wheel key", "F2", false)},
                              us_layout);
    REQUIRE(g.size() == 1);
    CHECK(g[0].uses.size() == 2);
    CHECK(g[0].on_uses() == 1);
    CHECK_FALSE(g[0].clash());
}

TEST_CASE("key list: EMF's console key is a scan code, named by where it is and grouped with the key it is") {
    KeyUse console = use("emf", "EMF", "Console key", "0x29");
    console.scan = true;
    const auto g = group_keys({console, use("chatty", "Chatty", "Talk key", "0xc0")}, us_layout);
    REQUIRE(g.size() == 1);
    CHECK(g[0].name == "the key under Esc (0x29)");
    CHECK(g[0].clash());
}

TEST_CASE("key list: keys shared on purpose (each acts on its own object) clash only with a key that is not") {
    auto g = group_keys({use("beastmaster", "Beastmaster", "Beasts window", "E", true, true),
                         use("bounty", "Bounty Board", "Read the board", "E", true, true)},
                        us_layout);
    REQUIRE(g.size() == 1);
    CHECK_FALSE(g[0].clash());
    g = group_keys({use("beastmaster", "Beastmaster", "Beasts window", "E", true, true),
                    use("quick", "Quick Stack", "Stack key", "E")},
                   us_layout);
    REQUIRE(g.size() == 1);
    CHECK(g[0].clash());  // Quick Stack acts everywhere: at the stable one press does both
}

TEST_CASE("key list: names sort as people read them (F2 before F10), in any case") {
    KeyUse console = use("emf", "EMF", "Console key", "0x29");
    console.scan = true;
    const auto g = group_keys({use("a", "A", "x", "F10"), use("b", "B", "y", "F2"), console, use("d", "D", "w", "Z")},
                              [](unsigned) { return 0u; });
    REQUIRE(g.size() == 4);
    CHECK(g[0].name == "F2");
    CHECK(g[1].name == "F10");
    CHECK(g[2].name == "the key under Esc (0x29)");
    CHECK(g[3].name == "Z");
}

TEST_CASE("key list: a console key on the numpad is that numpad key, not the arrow or Home it is with NumLock off") {
    CHECK(scan_to_vk(0x47) == 0x67u);  // VK_NUMPAD7 (Windows' own table says VK_HOME)
    CHECK(scan_to_vk(0x52) == 0x60u);  // VK_NUMPAD0
    CHECK(scan_to_vk(0x53) == 0x6Eu);  // VK_DECIMAL
    CHECK(scan_to_vk(0x10) == 0x51u);  // Q on a US layout: Windows' own table
    KeyUse console = use("emf", "EMF", "Console key", "0x47");
    console.scan = true;
    const auto g = group_keys({console, use("minimap", "Minimap", "Zoom key", "Numpad7")}, scan_to_vk);
    REQUIRE(g.size() == 1);
    CHECK(g[0].clash());
}

TEST_CASE("key list: the groups come sorted, the clashes first") {
    const auto g = group_keys({use("a", "A", "x", "K"), use("b", "B", "y", "F9"), use("c", "C", "z", "F9"),
                               use("d", "D", "w", "B")},
                              us_layout);
    REQUIRE(g.size() == 3);
    CHECK(g[0].name == "F9");  // the clash
    CHECK(g[1].name == "B");
    CHECK(g[2].name == "K");
}
