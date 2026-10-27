// The game's save slot records as the main menu holds them (src/core/save_slots.h), read from a fake Save2System in
// memory, and the recorder that turns them into save-info.json.
#include <doctest.h>

#include <cstring>
#include <string>
#include <vector>

#include "core/menu_start.h"
#include "core/save_slots.h"
#include "save_test_files.h"
#include "saves/save_info.h"

using namespace emf::core;
using save_test::FakePc;
namespace L = emf::core::menu_layout;
namespace S = emf::core::slot_layout;

namespace {

// A Save2System-sized block of memory with names in a block of their own; reads outside both fail like a fault.
struct FakeSave2 {
    std::vector<uint8_t> mem = std::vector<uint8_t>(0x120000, 0);
    std::vector<char> names = std::vector<char>(4096, 0);
    size_t used = 0;
    uintptr_t base() const { return reinterpret_cast<uintptr_t>(mem.data()); }
    template <class T>
    void put(uint32_t off, T v) {
        std::memcpy(mem.data() + off, &v, sizeof v);
    }
    void name(uint32_t field, const std::string& s) {
        std::memcpy(names.data() + used, s.data(), s.size());
        put<uint64_t>(field, reinterpret_cast<uintptr_t>(names.data() + used));
        put<uint64_t>(field + 8, s.size());
        used += s.size() + 1;
    }
    uint32_t character(int storage, int index) const {
        return L::kCharacterRecords + storage * L::kCharacterStorageStride + index * L::kCharacterRecordSize;
    }
    uint32_t world(int storage, int index) const { return L::kWorldRecords + storage * L::kWorldStorageStride + index * L::kWorldRecordSize; }
    void lists_loaded() {
        put<uint8_t>(L::kWorldListJob, 0);
        put<uint8_t>(L::kWorldListState, 3);
        put<uint8_t>(L::kCharacterListJob, 0);
        for (int st = 0; st < 2; ++st) {
            for (int i = 0; i < 16; ++i) put<uint8_t>(character(st, i), 1);  // empty
            for (int i = 0; i < 10; ++i) put<uint8_t>(world(st, i), 1);
        }
    }
    SlotMemoryRead reader() const {
        return [this](uintptr_t a, void* out, size_t n) {
            const auto in = [&](uintptr_t lo, size_t size) { return a >= lo && a + n <= lo + size; };
            if (!in(base(), mem.size()) && !in(reinterpret_cast<uintptr_t>(names.data()), names.size())) return false;
            std::memcpy(out, reinterpret_cast<const void*>(a), n);
            return true;
        };
    }
};

const emf::saves::SlotSeen* find(const std::vector<emf::saves::SlotSeen>& v, const char* storage, const char* type, int slot) {
    for (const auto& s : v)
        if (s.storage == storage && s.type == type && s.slot == slot) return &s;
    return nullptr;
}

void usual(FakeSave2& f) {
    f.lists_loaded();
    f.put<uint8_t>(f.character(0, 1), 2);
    f.name(f.character(0, 1) + L::kCharacterRecordName, "Character 2");
    f.put<uint32_t>(f.character(0, 1) + S::kCharacterLevel, 1);
    f.put<uint8_t>(f.character(1, 0), 4);
    f.name(f.character(1, 0) + L::kCharacterRecordName, "Astrid");
    f.put<uint32_t>(f.character(1, 0) + S::kCharacterLevel, 8);
    f.put<uint8_t>(f.world(0, 0), 2);
    f.name(f.world(0, 0) + L::kWorldRecordName, "My World 1");
    f.put<int64_t>(f.world(0, 0) + S::kWorldLastPlayed, 1793000000);
    f.put<int64_t>(f.world(0, 0) + S::kWorldPlayTimeNs, 7200ll * 1000000000ll);
}

}  // namespace

TEST_CASE("save slots: nothing until the game has loaded its save lists") {
    FakeSave2 f;
    CHECK_FALSE(read_menu_slots(f.base(), f.reader()));  // world list state 0
    f.lists_loaded();
    f.put<uint8_t>(L::kCharacterListJob, 1);  // the character list is still being read
    CHECK_FALSE(read_menu_slots(f.base(), f.reader()));
    f.put<uint8_t>(L::kCharacterListJob, 0);
    CHECK(read_menu_slots(f.base(), f.reader()));
    CHECK_FALSE(read_menu_slots(0, f.reader()));
    CHECK_FALSE(read_menu_slots(f.base() + 0x200000, f.reader()));  // unreadable
}

TEST_CASE("save slots: names, levels and the world's times from the records") {
    FakeSave2 f;
    usual(f);
    const auto seen = read_menu_slots(f.base(), f.reader());
    REQUIRE(seen);
    CHECK(seen->size() == 52);  // 2 x (16 + 10)
    const auto* c = find(*seen, "local", "character", 1);
    REQUIRE(c);
    CHECK(c->state == 2);
    CHECK(c->name == "Character 2");
    CHECK(c->level == 1);
    const auto* m = find(*seen, "steam", "character", 0);
    REQUIRE(m);
    CHECK(m->state == 4);
    CHECK(m->name == "Astrid");
    CHECK(m->level == 8);
    const auto* w = find(*seen, "local", "world", 0);
    REQUIRE(w);
    CHECK(w->name == "My World 1");
    CHECK(w->played == 1793000000);
    CHECK(w->play_seconds == 7200);
    CHECK(find(*seen, "local", "world", 3)->state == 1);
    CHECK(find(*seen, "local", "world", 3)->name.empty());
}

TEST_CASE("save slots: values that do not fit are left out") {
    FakeSave2 f;
    usual(f);
    f.put<uint32_t>(f.character(0, 1) + S::kCharacterLevel, 0);  // levels start at 1
    f.put<uint32_t>(f.character(1, 0) + S::kCharacterLevel, 70000);
    f.put<int64_t>(f.world(0, 0) + S::kWorldLastPlayed, 12345);       // before 2020
    f.put<int64_t>(f.world(0, 0) + S::kWorldPlayTimeNs, -5);
    f.put<uint8_t>(f.world(0, 1), 2);
    f.put<uint64_t>(f.world(0, 1) + L::kWorldRecordName, 0xdead0000);  // a name pointer that cannot be read
    f.put<uint64_t>(f.world(0, 1) + L::kWorldRecordName + 8, 5);
    const auto seen = read_menu_slots(f.base(), f.reader());
    REQUIRE(seen);
    CHECK_FALSE(find(*seen, "local", "character", 1)->level.has_value());
    CHECK_FALSE(find(*seen, "steam", "character", 0)->level.has_value());
    CHECK(find(*seen, "local", "world", 0)->played == 0);
    CHECK(find(*seen, "local", "world", 0)->play_seconds == 0);
    CHECK(find(*seen, "local", "world", 1)->name.empty());  // and the merge then keeps what it knew
}

TEST_CASE("save slots: the selected character") {
    FakeSave2 f;
    CHECK_FALSE(selected_character_slot(f.base(), f.reader()));
    f.put<uint16_t>(L::kSelectedCharacter, 0x0002);
    CHECK_FALSE(selected_character_slot(f.base(), f.reader()));  // not marked valid
    f.put<uint8_t>(L::kSelectedCharacterValid, 1);
    CHECK(selected_character_slot(f.base(), f.reader()) == 0x0002);
    f.put<uint16_t>(L::kSelectedCharacter, 0x0300);  // not a slot id the layout knows
    CHECK_FALSE(selected_character_slot(f.base(), f.reader()));
}

TEST_CASE("save slots: the recorder writes save-info.json at the menu and notes what loaded") {
    FakePc pc(L"recorder");
    FakeSave2 f;
    std::vector<uint8_t> pregame(0x200, 0);
    const uintptr_t save2 = f.base();
    std::memcpy(pregame.data() + L::kPregameSave2, &save2, 8);
    int64_t now = 1800000000;
    SaveInfoRecorder::Deps d;
    d.path = pc.roots.info;
    d.read = [&](uintptr_t a, void* out, size_t n) {
        const auto pg = reinterpret_cast<uintptr_t>(pregame.data());
        if (a >= pg && a + n <= pg + pregame.size()) {
            std::memcpy(out, reinterpret_cast<const void*>(a), n);
            return true;
        }
        return f.reader()(a, out, n);
    };
    d.now = [&] { return now; };
    int sleeps = 0;
    d.sleep = [&](unsigned) {
        if (++sleeps == 2) usual(f);  // the game loads its save lists a moment after the menu came up
    };
    d.tries = 5;
    d.steam_user = [] { return std::string("111"); };  // Steam's logged-in account (its userdata folder)
    SaveInfoRecorder r(d);
    r.on_pregame(pregame.data(), 1, 0);  // another state: nothing
    CHECK(r.writes() == 0);
    r.on_pregame(pregame.data(), L::kMainMenuState, 0);
    CHECK(sleeps == 2);
    CHECK(r.writes() == 1);
    auto info = emf::saves::read_save_info(pc.roots.info);
    REQUIRE(info.slots.size() == 3);
    CHECK(info.slots[0].name == "Astrid");
    CHECK(info.slots[0].level == 8);
    CHECK(info.slots[0].steam_id == "111");  // Steam names belong to that account
    CHECK(info.slots[1].steam_id.empty());
    CHECK(info.slots[2].name == "My World 1");
    CHECK(info.slots[2].play_seconds == 7200);
    CHECK(info.updated == now);

    // the same menu again: nothing new, nothing written
    r.on_pregame(pregame.data(), L::kMainMenuState, 0);
    CHECK(r.writes() == 1);

    // Start Play with LOCAL character slot 2 selected, then LOCAL world slot 1 loads
    f.put<uint16_t>(L::kSelectedCharacter, 0x0002);
    f.put<uint8_t>(L::kSelectedCharacterValid, 1);
    r.on_pregame(pregame.data(), 3, 0);
    now += 60;
    r.on_world_loaded(0x0001);
    info = emf::saves::read_save_info(pc.roots.info);
    REQUIRE(info.slots.size() == 3);
    CHECK(info.slots[1].name == "Character 2");
    CHECK(info.slots[1].last_loaded == now);
    CHECK(info.slots[2].last_loaded == now);
    CHECK(info.slots[0].last_loaded == 0);

    // a menu visit whose lists never load writes nothing and gives up
    FakeSave2 never;
    const uintptr_t s2 = never.base();
    std::memcpy(pregame.data() + L::kPregameSave2, &s2, 8);
    d.read = never.reader();
    const size_t before = r.writes();
    r.on_pregame(pregame.data(), L::kMainMenuState, 0);
    CHECK(r.writes() == before);
}
