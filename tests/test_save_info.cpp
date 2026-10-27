// save-info.json (src/saves/save_info.h): the file round trip and the merge rules for what the game's menu showed.
#include <doctest.h>

#include <string>
#include <vector>

#include "save_test_files.h"
#include "saves/save_info.h"

using namespace emf::saves;
using save_test::FakePc;

namespace {
SlotSeen seen(const char* storage, const char* type, int slot, uint8_t state, const char* name = "",
              std::optional<int> level = std::nullopt) {
    SlotSeen s;
    s.storage = storage;
    s.type = type;
    s.slot = slot;
    s.state = state;
    s.name = name;
    s.level = level;
    return s;
}
const SlotInfo* get(const SaveInfo& i, const char* storage, const char* type, int slot) {
    for (const auto& s : i.slots)
        if (s.storage == storage && s.type == type && s.slot == slot) return &s;
    return nullptr;
}
}  // namespace

TEST_CASE("save info: the menu's slots are merged in") {
    bool changed = false;
    SaveInfo info = merge_seen({}, {seen("local", "character", 1, 2, "Character 2", 1), seen("local", "world", 0, 4, "My World 1"),
                                    seen("steam", "character", 0, 2, "Astrid", 8), seen("steam", "world", 3, 1)},
                               1000, &changed);
    CHECK(changed);
    REQUIRE(info.slots.size() == 3);
    // sorted: Steam first, characters before worlds
    CHECK(info.slots[0].name == "Astrid");
    CHECK(info.slots[1].name == "Character 2");
    CHECK(info.slots[2].name == "My World 1");
    CHECK(get(info, "local", "character", 1)->level == 1);
    CHECK(get(info, "local", "character", 1)->seen == 1000);
    CHECK_FALSE(get(info, "local", "world", 0)->level.has_value());

    // the same again: nothing changed (only the time it was seen)
    info = merge_seen(info, {seen("local", "character", 1, 2, "Character 2", 1)}, 2000, &changed);
    CHECK_FALSE(changed);
    CHECK(get(info, "local", "character", 1)->seen == 2000);
    CHECK(get(info, "steam", "character", 0));  // not looked at: kept

    // a level up
    info = merge_seen(info, {seen("local", "character", 1, 2, "Character 2", 2)}, 3000, &changed);
    CHECK(changed);
    CHECK(get(info, "local", "character", 1)->level == 2);

    // a look without the level keeps the one known for that name
    info = merge_seen(info, {seen("local", "character", 1, 2, "Character 2")}, 3100, &changed);
    CHECK_FALSE(changed);
    CHECK(get(info, "local", "character", 1)->level == 2);

    // not scanned yet (0), being created (3), unknown (5): kept as they were
    info = merge_seen(info, {seen("steam", "character", 0, 0), seen("local", "character", 1, 3, "(unnamed)"),
                             seen("local", "world", 0, 5, "garbage")},
                      4000, &changed);
    CHECK_FALSE(changed);
    CHECK(get(info, "steam", "character", 0)->name == "Astrid");
    CHECK(get(info, "local", "character", 1)->name == "Character 2");
    CHECK(get(info, "local", "world", 0)->name == "My World 1");

    // another character in the slot: its level and load time start over
    info = note_loaded(info, "local", "character", 1, 4500, &changed);
    CHECK(changed);
    CHECK(get(info, "local", "character", 1)->last_loaded == 4500);
    info = merge_seen(info, {seen("local", "character", 1, 2, "Newcomer")}, 5000, &changed);
    CHECK(changed);
    CHECK(get(info, "local", "character", 1)->name == "Newcomer");
    CHECK_FALSE(get(info, "local", "character", 1)->level.has_value());
    CHECK(get(info, "local", "character", 1)->last_loaded == 0);

    // deleted (6) or empty (1): removed
    info = merge_seen(info, {seen("steam", "character", 0, 6, "Astrid"), seen("local", "world", 0, 1)}, 6000, &changed);
    CHECK(changed);
    CHECK_FALSE(get(info, "steam", "character", 0));
    CHECK_FALSE(get(info, "local", "world", 0));
    CHECK(info.slots.size() == 1);

    // an exists-state without a name says nothing
    info = merge_seen(info, {seen("local", "world", 2, 2, "")}, 7000, &changed);
    CHECK_FALSE(changed);
    CHECK_FALSE(get(info, "local", "world", 2));
}

TEST_CASE("save info: a world's last-played time and play time") {
    SlotSeen w = seen("local", "world", 0, 2, "My World 1");
    w.played = 1700000000;
    w.play_seconds = 3600 * 12;
    bool changed = false;
    SaveInfo info = merge_seen({}, {w}, 100, &changed);
    CHECK(changed);
    CHECK(get(info, "local", "world", 0)->played == 1700000000);
    CHECK(get(info, "local", "world", 0)->play_seconds == 43200);
    SlotSeen later = w;
    later.played = 1700003600;
    later.play_seconds = 46800;
    info = merge_seen(info, {later}, 200, &changed);
    CHECK(changed);
    CHECK(get(info, "local", "world", 0)->play_seconds == 46800);
    SlotSeen without = w;  // a look without the times keeps them
    without.played = 0;
    without.play_seconds = 0;
    info = merge_seen(info, {without}, 300, &changed);
    CHECK_FALSE(changed);
    CHECK(get(info, "local", "world", 0)->played == 1700003600);
    // and they make the round trip
    const auto back = parse_save_info(save_info_json(info));
    REQUIRE(back);
    CHECK(back->slots[0].played == 1700003600);
    CHECK(back->slots[0].play_seconds == 46800);
}

TEST_CASE("save info: a world load is noted") {
    bool changed = true;
    SaveInfo info = merge_seen({}, {seen("local", "world", 0, 2, "My World 1")}, 100, nullptr);
    info = note_loaded(info, "local", "world", 0, 200, &changed);
    CHECK(changed);
    CHECK(get(info, "local", "world", 0)->last_loaded == 200);
    info = note_loaded(info, "local", "world", 4, 300, &changed);  // a slot EMF has no name for yet: nothing to note
    CHECK_FALSE(changed);
    CHECK(info.slots.size() == 1);
}

TEST_CASE("save info: the file round trip") {
    FakePc pc(L"info-file");
    SaveInfo info = merge_seen({}, {seen("steam", "character", 0, 2, "Björn \"the\" builder", 8), seen("local", "world", 9, 2, "W")},
                               123, nullptr);
    info.updated = 456;
    REQUIRE(write_save_info(pc.roots.info, info));
    const SaveInfo back = read_save_info(pc.roots.info);
    CHECK(back.updated == 456);
    REQUIRE(back.slots.size() == 2);
    CHECK(back.slots[0].name == "Björn \"the\" builder");
    CHECK(back.slots[0].level == 8);
    CHECK(back.slots[1].slot == 9);
    // no temporary file left next to it
    std::vector<std::wstring> names;
    for (const auto& e : std::filesystem::directory_iterator(std::filesystem::path(pc.roots.info).parent_path()))
        names.push_back(e.path().filename().wstring());
    CHECK(names == std::vector<std::wstring>{L"save-info.json"});
    // missing or broken: empty
    CHECK(read_save_info(pc.root + L"\\nothing.json").slots.empty());
    FakePc::write(pc.roots.info, "{oops");
    CHECK(read_save_info(pc.roots.info).slots.empty());
    std::string error;
    CHECK_FALSE(parse_save_info("[1,2]", &error));
    // entries EMF does not know are left out, the rest is read
    const auto partly = parse_save_info(
        R"({"format":1,"updated":5,"slots":[{"storage":"moon","type":"world","slot":0,"name":"x"},{"storage":"local","type":"world","slot":1,"name":"ok"}]})");
    REQUIRE(partly);
    REQUIRE(partly->slots.size() == 1);
    CHECK(partly->slots[0].name == "ok");
}
