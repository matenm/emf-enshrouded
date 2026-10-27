#include <doctest.h>

#include <clocale>
#include <locale>
#include <stdexcept>

#include "../mods/quick_stack/quickstack.h"

using namespace quickstack;

TEST_CASE("quick stack: each carried stack goes to the nearest chest that already holds that item") {
    const std::vector<Carried> carried = {
        {0x100000001f4ull, 0x11, 30},         // logs
        {0x200000001f4ull, 0x22, 5},          // stone: no chest holds it
        {0x300000001f4ull, 0x11, 12},         // more logs
        {0x400000001f4ull, 0x33, 1, true},    // a sword with its own data: never
        {0x500000001f5ull, 0x11, 4, false, true},  // logs on the action bar: kept
    };
    const std::vector<Chest> chests = {
        {6830, 8.0, {{0x11, 1}}},
        {6831, 3.0, {{0x11, 50}, {0x33, 1}}},
        {6832, 1.0, {{0x44, 9}}},
    };
    const auto moves = plan(carried, chests);
    REQUIRE(moves.size() == 2);
    CHECK(moves[0].chest == 6831);  // the nearest of the two that hold logs
    CHECK(moves[0].item == 0x11);
    CHECK(moves[0].count == 30);
    CHECK(moves[0].slot == 0x100000001f4ull);
    CHECK(moves[1].count == 12);
    CHECK(plan(carried, {}).empty());
    CHECK(plan({}, chests).empty());
}

TEST_CASE("quick stack: the summary says what moved and what was skipped (press again), never a wrong reason") {
    CHECK(summary(901, 1, 0) == "Quick stack: 901 item(s) into 1 chest(s)");
    CHECK(summary(30, 2, 3) == "Quick stack: 30 item(s) into 2 chest(s); 3 stack(s) skipped (busy): press again");
    CHECK(summary(0, 0, 2) == "Quick stack: 2 stack(s) could not be taken right now: press again");
    CHECK(summary(0, 0, 0) == "Quick stack: nothing fitted (are the chests full?)");
}

// ---- Chest Machines (quick_stack 1.2): filters, marks, sorting, recycling, the key ----

TEST_CASE("chest machines: the filters keep arrows, food and named items with the player") {
    Filters f;
    CHECK_FALSE(filtered("Ammo_Arrow_Wood", f));
    f.keep_ammo = true;
    CHECK(filtered("Ammo_Arrow_Wood", f));
    CHECK_FALSE(filtered("Food_T1_Berries", f));
    f.keep_food = true;
    CHECK(filtered("Food_T1_Berries", f));
    CHECK(filtered("Material_T2_Fish_Food_Raw", f));  // _food_ in the middle counts too
    CHECK_FALSE(filtered("Material_T1_Wood_Logs", f));
    f.exclude = parse_exclude(" torch, Resin ,,bait ");
    REQUIRE(f.exclude.size() == 3);
    CHECK(f.exclude[1] == "resin");
    CHECK(filtered("Material_T1_Resin", f));
    CHECK(filtered("Tool_Torch_01", f));
    CHECK_FALSE(filtered("Material_T1_Stone_Flint", f));
    CHECK(parse_exclude("").empty());
}

TEST_CASE("chest machines: marks are kept by place (ids change with a reload), one per chest, saved as text") {
    Marks m;
    CHECK(m.set({10, 5, 3}, Mark::Sort) == Mark::None);
    CHECK(m.set({20, 5, 3}, Mark::Recycle) == Mark::None);
    CHECK(m.at({10.2, 5, 3.1}) == Mark::Sort);  // within 0.5 m: the same chest
    CHECK(m.at({11, 5, 3}) == Mark::None);      // another chest
    CHECK(m.set({10.1, 5, 3}, Mark::Overflow) == Mark::Sort);  // re-marked, not added twice
    CHECK(m.all().size() == 2);
    CHECK(m.set({10, 5, 3}, Mark::None) == Mark::Overflow);
    CHECK(m.all().size() == 1);
    m.set({1.5, -2.25, 300}, Mark::Overflow);
    const std::string text = m.text();
    CHECK(text == "recycle 20.00 5.00 3.00\noverflow 1.50 -2.25 300.00\n");
    Marks back;
    back.parse(text + "garbage line\nsort 1 2\n");  // bad lines are skipped
    CHECK(back.all().size() == 2);
    CHECK(back.at({20, 5, 3}) == Mark::Recycle);
    CHECK(mark_of("sort") == Mark::Sort);
    CHECK(mark_of("RECYCLE") == Mark::Recycle);
    CHECK(mark_of("none") == Mark::None);
    CHECK(mark_of("nonsense") == Mark::None);
    CHECK(std::string(mark_name(Mark::Overflow)) == "overflow");
}

TEST_CASE("chest machines: the marks file is the same under a comma-decimal locale (written and read)") {
    // the game, a mod or a library may switch the C and C++ locales to the player's: "12,50" must never be written
    struct Locales {
        std::string c = std::setlocale(LC_ALL, nullptr);
        std::locale cpp;
        ~Locales() {
            std::locale::global(cpp);
            std::setlocale(LC_ALL, c.c_str());
        }
    } restore;
    std::locale german;
    try {
        german = std::locale("de-DE");
    } catch (const std::exception&) {
        german = std::locale("German_Germany.1252");
    }
    std::locale::global(german);  // sets the C locale too
    char probe[16];
    std::snprintf(probe, sizeof probe, "%.2f", 1.5);
    REQUIRE(std::string(probe) == "1,50");  // the locale really writes commas here

    Marks m;
    m.set({12.5, 40, -3.25}, Mark::Sort);
    m.set({1234.75, -0.5, 7}, Mark::Overflow);
    CHECK(m.text() == "sort 12.50 40.00 -3.25\noverflow 1234.75 -0.50 7.00\n");
    Marks back;
    CHECK(back.parse(m.text()) == 2);
    CHECK(back.at({12.5, 40, -3.25}) == Mark::Sort);
    CHECK(back.at({1234.75, -0.5, 7}) == Mark::Overflow);
    // a file written earlier reads the same; a number with a comma is a bad line (skipped), not a wrong place
    Marks old;
    CHECK(old.parse("recycle 20.00 5.00 3.00\nsort 1,50 2,00 3,00\n") == 1);
    REQUIRE(old.all().size() == 1);
    CHECK(old.all()[0].pos.x == 20.0);
    CHECK(old.all()[0].pos.z == 3.0);
}

TEST_CASE("chest machines: a sorting chest sends each plain stack to the nearest chest that holds it; gear never moves") {
    const std::vector<Carried> in_sorter = {
        {0x100000001f4ull, 0x11, 50},       // logs
        {0x200000001f4ull, 0x22, 20},       // stones
        {0x300000001f4ull, 0x33, 1, true},  // a sword: stays
        {0x400000001f4ull, 0x44, 7},        // nobody holds it
    };
    std::vector<Chest> targets = {{700, 12.0, {{0x11, 3}}}, {701, 4.0, {{0x22, 9}}}, {702, 2.0, {{0x11, 1}}}};
    FullBook full;
    auto moves = sort_plan(in_sorter, targets, full, 0);
    REQUIRE(moves.size() == 2);
    CHECK(moves[0].chest == 702);  // logs: the nearest holder
    CHECK(moves[0].count == 50);
    CHECK(moves[1].chest == 701);
    // an overflow chest takes what nobody holds (not gear)
    targets.push_back({703, 25.0, {}, true});
    moves = sort_plan(in_sorter, targets, full, 0);
    REQUIRE(moves.size() == 3);
    CHECK(moves[2].chest == 703);
    CHECK(moves[2].item == 0x44);
    // a chest that took nothing of an item (full) is skipped for it for a minute; the next holder gets it
    full.mark(702, 0x11, 1000);
    moves = sort_plan(in_sorter, targets, full, 2000);
    CHECK(moves[0].chest == 700);
    moves = sort_plan(in_sorter, targets, full, 1000 + FullBook::kMs);
    CHECK(moves[0].chest == 702);
}

namespace {
const uint32_t kWall = 0x500, kWood = 0x11, kResin = 0x12, kStew = 0x600, kPlank = 0x700, kGear = 0x800;
Recipe wall_recipe() { return {0xA1, {{kWood, 4}, {kResin, 1}}, {{kWall, 1}}}; }
}  // namespace

TEST_CASE("chest machines: the recycler gives back a share of a recipe's inputs, rounded down, never more than was used") {
    const Carried walls{0x100000001f4ull, kWall, 4};
    auto r = recycle_plan(walls, {wall_recipe()}, 50);
    CHECK(r.skip == Skip::None);
    CHECK(r.take == 4u);
    REQUIRE(r.refund.size() == 2);
    CHECK(r.refund[0].item == kWood);
    CHECK(r.refund[0].count == 8u);  // 4 walls x 4 wood x 50 %
    CHECK(r.refund[1].count == 2u);  // 4 x 1 resin x 50 %
    // 1 wall at 50 %: 2 wood, and the half resin is lost (rounded down)
    r = recycle_plan({1, kWall, 1}, {wall_recipe()}, 50);
    REQUIRE(r.refund.size() == 1);
    CHECK(r.refund[0].count == 2u);
    // at 100 % it is an exact undo; a refund above 100 % is capped
    r = recycle_plan(walls, {wall_recipe()}, 250);
    CHECK(r.refund[0].count == 16u);
    CHECK(r.refund[1].count == 4u);
    // a recipe that makes 4 planks from 1 wood: 6 planks are 1 batch (2 stay); at 50 % that gives nothing: skipped
    const Recipe planks{0xA2, {{kWood, 1}}, {{kPlank, 4}}};
    r = recycle_plan({1, kPlank, 6}, {planks}, 50);
    CHECK(r.skip == Skip::NothingBack);
    r = recycle_plan({1, kPlank, 6}, {planks}, 100);
    CHECK(r.take == 4u);
    CHECK(r.refund[0].count == 1u);
    r = recycle_plan({1, kPlank, 3}, {planks}, 100);
    CHECK(r.skip == Skip::TooFew);
}

TEST_CASE("chest machines: the recycler leaves gear, items without a recipe, upgrades, cooking, category inputs and ambiguous items") {
    CHECK(recycle_plan({1, kGear, 1, true}, {Recipe{0xA3, {{kWood, 3}}, {{kGear, 1}}}}, 50).skip == Skip::Gear);
    CHECK(recycle_plan({1, kWood, 10}, {}, 50).skip == Skip::NoRecipe);
    Recipe up = wall_recipe();
    up.upgrade = true;
    CHECK(recycle_plan({1, kWall, 2}, {up}, 50).skip == Skip::Upgrade);
    const Recipe stew{0xA4, {{kWood, 1}}, {{kStew, 1}}, false, true};
    CHECK(recycle_plan({1, kStew, 2}, {stew}, 50).skip == Skip::Cooking);
    const Recipe any{0xA5, {{0, 2}, {kResin, 1}}, {{kWall, 1}}};  // "any wood": a category input
    CHECK(recycle_plan({1, kWall, 2}, {any}, 50).skip == Skip::Category);
    const Recipe two_out{0xA6, {{kWood, 2}}, {{kWall, 1}, {kResin, 1}}};
    CHECK(recycle_plan({1, kWall, 2}, {two_out}, 50).skip == Skip::MultiOutput);
    // two recipes with different inputs: one material could be turned into another, so never
    const Recipe stone_wall{0xA7, {{0x13, 4}}, {{kWall, 1}}};
    CHECK(recycle_plan({1, kWall, 2}, {wall_recipe(), stone_wall}, 50).skip == Skip::Ambiguous);
    // the same recipe at two stations is not ambiguous; an excluded one beside a plain one is ignored
    Recipe same = wall_recipe();
    same.id = 0xA8;
    CHECK(recycle_plan({1, kWall, 2}, {wall_recipe(), same}, 50).skip == Skip::None);
    CHECK(recycle_plan({1, kWall, 2}, {up, wall_recipe()}, 50).skip == Skip::None);
    CHECK(std::string(skip_text(Skip::Ambiguous)).find("several recipes") != std::string::npos);
}

TEST_CASE("chest machines: room for a refund is counted in stacks of each item's maximum") {
    const std::vector<Stack> refund = {{kWood, 600}, {kResin, 2}};
    CHECK(slots_needed(refund, [](uint32_t item) { return item == kWood ? 250u : 100u; }) == 4u);
    CHECK(slots_needed({}, [](uint32_t) { return 250u; }) == 0u);
    CHECK(slots_needed({{kWood, 5}}, [](uint32_t) { return 0u; }) == 5u);  // an unknown maximum counts each item
}

TEST_CASE("chest machines: the key: a tap quick stacks on release; held, it opens the wheel once and its release is no tap") {
    TapHold k(350);
    CHECK(k.update(false, 0) == TapHold::None);
    CHECK(k.update(true, 100) == TapHold::None);
    CHECK(k.update(true, 300) == TapHold::None);
    CHECK(k.update(false, 320) == TapHold::Tap);
    CHECK(k.update(true, 1000) == TapHold::None);
    CHECK(k.update(true, 1349) == TapHold::None);
    CHECK(k.update(true, 1350) == TapHold::Hold);
    CHECK(k.update(true, 2000) == TapHold::None);   // once
    CHECK(k.update(false, 2100) == TapHold::None);  // the release after the wheel
    CHECK(k.update(true, 3000) == TapHold::None);
    CHECK(k.update(false, 3010) == TapHold::Tap);
}

TEST_CASE("chest machines: notes for sorting and recycling") {
    CHECK(sort_note(6, 2) == "Sorting chest: 6 stack(s) put away into 2 chest(s)");
    CHECK(recycle_note("Wood Wall", 4, {{"Wood Logs", 8}, {"Resin", 2}}) ==
          "Recycler: 4 x Wood Wall into 8 x Wood Logs, 2 x Resin");
}

TEST_CASE("chest machines: with each chest's room known, a stack goes only where it fits (split over the nearest), never to bounce back") {
    const std::vector<Carried> carried = {{0x100000001f4ull, 0x11, 100}, {0x200000001f4ull, 0x22, 10}};
    Chest near{6831, 2.0, {{0x11, 240}}};
    near.room_known = true;
    near.room[0x11] = 10;  // one stack of 250 with 240 in it, no free slot
    Chest far{6830, 8.0, {{0x11, 1}}};
    far.room_known = true;
    far.room[0x11] = 50;
    Chest full{6832, 1.0, {{0x22, 250}}};
    full.room_known = true;  // room for 0x22: none
    auto moves = plan(carried, {near, far, full});
    REQUIRE(moves.size() == 2);
    CHECK(moves[0].chest == 6831);
    CHECK(moves[0].count == 10u);
    CHECK(moves[1].chest == 6830);
    CHECK(moves[1].count == 50u);   // 40 logs stay with the player
    CHECK(moves[1].slot == moves[0].slot);
    // the same for a sorting chest
    std::vector<Carried> in_sorter = {{0x100000001f4ull, 0x11, 100}};
    FullBook book;
    moves = sort_plan(in_sorter, {near, far}, book, 0);
    REQUIRE(moves.size() == 2);
    CHECK(moves[0].count + moves[1].count == 60u);
    // a chest whose room is not known takes the whole stack (as before)
    Chest unknown{6833, 3.0, {{0x11, 1}}};
    moves = plan(carried, {unknown});
    REQUIRE(moves.size() == 1);
    CHECK(moves[0].count == 100u);
}

TEST_CASE("chest machines: a recycling never takes or gives back more than one put or take may carry (65535)") {
    const Recipe big{0xB1, {{kWood, 10}}, {{kWall, 1}}};
    auto r = recycle_plan({1, kWall, 9999}, {big}, 100);  // 99,990 logs would come back (big_stacks makes 9,999 stacks)
    CHECK(r.skip == Skip::None);
    CHECK(r.take == 6553u);
    CHECK(r.refund[0].count == 65530u);
    const Recipe many{0xB2, {{kWood, 1}}, {{kPlank, 2}}};
    r = recycle_plan({1, kPlank, 200000}, {many}, 50);  // more than one take may carry
    CHECK(r.take <= 65535u);
    CHECK(r.take % 2 == 0u);
}

TEST_CASE("chest machines: a marked chest is gone only after it was missed with a player near for 30 s, never in the first 30 s") {
    MissBook m;
    m.reset(1000);
    const Vec3 chest{10, 5, 3};
    CHECK_FALSE(m.missing(chest, true, 2000));    // just loaded: the place may still be streaming in
    CHECK_FALSE(m.missing(chest, true, 31000));   // the first miss after the grace starts the clock
    CHECK_FALSE(m.missing(chest, true, 50000));
    CHECK(m.missing(chest, true, 61000));         // missed for 30 s with a player near: gone
    // found again in between: the clock starts over
    m.reset(0);
    CHECK_FALSE(m.missing(chest, true, 40000));
    m.found(chest);
    CHECK_FALSE(m.missing(chest, true, 71000));
    CHECK(m.missing(chest, true, 101000));
    // nobody near: no conclusion, and the clock starts over
    m.reset(0);
    CHECK_FALSE(m.missing(chest, true, 40000));
    CHECK_FALSE(m.missing(chest, false, 80000));
    CHECK_FALSE(m.missing(chest, true, 90000));
    CHECK(m.missing(chest, true, 120000));
}

TEST_CASE("chest machines: a quick stack press moves at most 24 stacks and never more than the mod's room in the item queue") {
    using namespace quickstack;
    auto many = [](size_t n) { return std::vector<Move>(n, Move{9, 1, 1, 0}); };
    auto m = many(30);
    CHECK(cap_moves(m, 40) == 6);  // 24 now, 6 for the next press
    CHECK(m.size() == 24);
    m = many(30);
    CHECK(cap_moves(m, 10) == 20);  // the queue has room for 10 moves only
    CHECK(m.size() == 10);
    m = many(5);
    CHECK(cap_moves(m, 40) == 0);
    CHECK(m.size() == 5);
    m = many(5);
    CHECK(cap_moves(m, 0) == 5);
    CHECK(m.empty());
    m = many(5);
    CHECK(cap_moves(m, -3) == 5);
    CHECK(m.empty());
}

TEST_CASE("chest machines: a recycler's materials go to the nearest sorting chest with room, else a chest holding the first material, else any with room") {
    using namespace quickstack;
    const std::vector<Spot> sorters = {{10, 9.0, 5, false}, {11, 4.0, 0, false}};  // the nearer one is full
    const std::vector<Spot> chests = {{20, 2.0, 3, false}, {21, 6.0, 8, true}, {22, 7.0, 0, true}};
    CHECK(recycle_target(sorters, chests, 2, true) == 10);   // the full sorting chest is passed over
    CHECK(recycle_target(sorters, chests, 6, true) == 21);   // no sorting chest has room for 6: the chest that holds it
    CHECK(recycle_target(sorters, chests, 2, false) == 21);  // sorting chests off
    CHECK(recycle_target({}, {{20, 2.0, 3, false}, {22, 7.0, 0, true}}, 2, true) == 20);  // the holder is full: any with room
    CHECK(recycle_target(sorters, chests, 9, true) == 0);    // room nowhere: the stack stays in the recycler
    CHECK(recycle_target({}, {}, 1, true) == 0);
}

TEST_CASE("quick stack: an item's stack size is read again after a while, when a bigger stack is seen, and in a new session") {
    using namespace quickstack;
    StackSizes sizes;
    uint32_t game = 250;  // what the game says now
    int reads = 0;
    const auto read = [&](uint32_t) {
        ++reads;
        return game;
    };
    CHECK(sizes.get(0x11, 0, read) == 250u);
    game = 1000;  // Big Stacks raised it while the game runs
    CHECK(sizes.get(0x11, 1000, read) == 250u);  // known: not read again at once
    CHECK(reads == 1);
    CHECK(sizes.get(0x11, StackSizes::kMaxAgeMs, read) == 1000u);  // read again after a while
    CHECK(reads == 2);
    // a stack bigger than the known size: the size changed, the next look reads it again
    game = 4000;
    CHECK_FALSE(sizes.saw(0x11, 1000));  // not bigger: kept
    CHECK(sizes.get(0x11, StackSizes::kMaxAgeMs + 1, read) == 1000u);
    CHECK(sizes.saw(0x11, 3000));
    CHECK(sizes.get(0x11, StackSizes::kMaxAgeMs + 2, read) == 4000u);
    CHECK_FALSE(sizes.saw(0x22, 99));  // an item not known yet
    // a new session forgets them all
    game = 250;
    sizes.clear();
    CHECK(sizes.get(0x11, StackSizes::kMaxAgeMs + 3, read) == 250u);
    // a size the game does not know yet (0) is asked again next time
    game = 0;
    const int before = reads;
    CHECK(sizes.get(0x33, 0, read) == 0u);
    CHECK(sizes.get(0x33, 1, read) == 0u);
    CHECK(reads == before + 2);
}
