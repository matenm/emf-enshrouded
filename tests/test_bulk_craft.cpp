// Bulk Craft (mods/bulk_craft): the rules without the game (bc_logic.h): the amount asked for and its caps, what the
// materials pay for, the pacing, the refusals, the command's words, the recipes a player has crafted by hand (the
// knowledge check), telling the game's crafts from the mod's own, and the order runner.
#include <doctest.h>

#include <map>
#include <string>
#include <vector>

#include "../mods/bulk_craft/bc_logic.h"

using namespace bulkcraft;

namespace {
constexpr uint32_t kTwigs = 0x11, kFibre = 0x12, kArrows = 0x21, kString = 0x22, kStone = 0x13, kBlocks = 0x23;

Recipe arrows() {  // 10 arrows <- 2 twigs, by hand
    Recipe r;
    r.id = 0x278240d5;
    r.workshop = 0;
    r.inputs = {{kTwigs, 2}};
    r.outputs = {{kArrows, 10}};
    return r;
}
constexpr uint32_t kFibreCategory = 0xC0C0, kFlax = 0x14;
Recipe string_recipe() {  // 1 string <- 1 of the fibre category (fibre counts x3, flax x2)
    Recipe r;
    r.id = 0x7e4e353a;
    r.inputs = {{0, 1, kFibreCategory}};
    r.category_input = true;
    r.outputs = {{kString, 1}};
    return r;
}
Recipe blocks() {  // 100 blocks <- 2 stone, at the workbench
    Recipe r;
    r.id = 0x852c5432;
    r.workshop = 0x4da88f73;
    r.inputs = {{kStone, 2}};
    r.outputs = {{kBlocks, 100}};
    return r;
}
Settings paced() { return Settings{}; }
Settings instant() {
    Settings s;
    s.pace = Pace::Instant;
    return s;
}
}  // namespace

TEST_CASE("bulk_craft: amounts: a number, xN, the times sign, max/all; nothing else") {
    CHECK(parse_amount("5").n == 5);
    CHECK(parse_amount("x10").n == 10);
    CHECK(parse_amount("X25").n == 25);
    CHECK(parse_amount("\xC3\x97" "25").n == 25);  // ×25
    CHECK(parse_amount("max").max);
    CHECK(parse_amount("ALL").max);
    CHECK(parse_amount("max").ok);
    CHECK_FALSE(parse_amount("0").ok);
    CHECK_FALSE(parse_amount("-3").ok);
    CHECK_FALSE(parse_amount("ten").ok);
    CHECK_FALSE(parse_amount("").ok);
    CHECK_FALSE(parse_amount("5x").ok);
    CHECK_FALSE(parse_amount("99999999999").ok);
    CHECK(parse_amount("100000").ok);  // big numbers are fine: the cap cuts them
}

TEST_CASE("bulk_craft: an order is capped by the setting and by what the materials pay for") {
    CHECK(order_size(parse_amount("25"), 40, 100) == 25);
    CHECK(order_size(parse_amount("25"), 7, 100) == 7);    // only 7 paid for
    CHECK(order_size(parse_amount("500"), kUnknown, 100) == 100);
    CHECK(order_size(parse_amount("max"), 40, 100) == 40);
    CHECK(order_size(parse_amount("max"), 400, 100) == 100);
    CHECK(order_size(parse_amount("max"), kUnknown, 100) == 100);  // category inputs: up to the cap, stops when out
    CHECK(order_size(parse_amount("5"), 0, 100) == 0);
    CHECK(order_size(parse_amount("5"), 5, 0) == 0);
}

TEST_CASE("bulk_craft: what the materials pay for counts plain inputs; category inputs are unknown") {
    std::map<uint32_t, uint32_t> have{{kTwigs, 9}};
    CHECK(can_pay(arrows(), have) == 4);
    CHECK(can_pay(arrows(), {}) == 0);
    CHECK(missing_input(arrows(), {}) == kTwigs);
    CHECK(missing_input(arrows(), have) == 0);
    CHECK(can_pay(string_recipe(), {}) == kUnknown);  // only a category input: the game decides
    Recipe mixed = arrows();
    mixed.inputs.push_back({0, 1, kFibreCategory});
    mixed.category_input = true;
    CHECK(can_pay(mixed, have) == 4);  // the plain part still limits it
    Recipe none = arrows();
    none.inputs.clear();
    CHECK(can_pay(none, {}) == 0);  // a recipe without inputs is never bulk-crafted
}

TEST_CASE("bulk_craft: pacing: paced waits the crafting time or the floor; instant never beats the crafting time") {
    Settings s = paced();
    s.floor_s = 0.5;
    Recipe r = arrows();
    CHECK(interval_ms(r, s) == 500);
    CHECK(batch(r, s, 25) == 1);
    r.duration_ns = 3000000000ll;  // 3 s
    CHECK(interval_ms(r, s) == 3000);
    CHECK(batch(r, s, 25) == 1);
    Settings i = instant();
    r.duration_ns = 0;
    CHECK(batch(r, i, 25) == 25);  // all at once: the game makes as many as the materials pay for
    CHECK(batch(r, i, 70000) == 65535);
    CHECK(interval_ms(r, i) == 0);
    r.duration_ns = 120000000000ll;  // a timed recipe stays one per crafting time even in instant mode
    CHECK(batch(r, i, 25) == 1);
    CHECK(interval_ms(r, i) == 120000);
    s.floor_s = 0;
    r.duration_ns = 0;
    CHECK(interval_ms(r, s) == kMinIntervalMs);  // never two sends in the same moment
}

TEST_CASE("bulk_craft: the time an order takes, for the first note") {
    Settings s = paced();
    s.floor_s = 0.5;
    CHECK(eta_seconds(arrows(), s, 25) == doctest::Approx(12.5));
    CHECK(eta_seconds(arrows(), instant(), 25) == doctest::Approx(0));
    Recipe r = arrows();
    r.duration_ns = 2000000000ll;
    CHECK(eta_seconds(r, instant(), 3) == doctest::Approx(6));
}

TEST_CASE("bulk_craft: refusals: not crafted by hand, upgrades, factories, the altar, another station's recipe") {
    const Recipe r = blocks();
    CHECK(check_recipe(r, 0x4da88f73, false, true) == Refusal::None);
    CHECK(check_recipe(r, 0x4da88f73, false, false) == Refusal::NotLearned);
    CHECK(check_recipe(r, 0, false, true) == Refusal::WrongStation);
    CHECK(check_recipe(r, 0x4da88f73, true, true) == Refusal::Factory);
    Recipe up = r;
    up.upgrade = true;
    CHECK(check_recipe(up, 0x4da88f73, false, true) == Refusal::Upgrade);
    Recipe altar = r;
    altar.workshop = 0xb326e042;
    CHECK(check_recipe(altar, 0xb326e042, false, true) == Refusal::Altar);
    Recipe empty = r;
    empty.outputs.clear();
    CHECK(check_recipe(empty, 0x4da88f73, false, true) == Refusal::NoOutput);
    for (auto x : {Refusal::NotLearned, Refusal::Upgrade, Refusal::Factory, Refusal::Altar, Refusal::WrongStation,
                   Refusal::NoOutput})
        CHECK_FALSE(refusal_text(x, "Wood Logs").empty());
    CHECK(refusal_text(Refusal::NotLearned, "Stone Blocks").find("by hand") != std::string::npos);
}

TEST_CASE("bulk_craft: the command's words: an amount, then an optional part of the recipe's name") {
    auto c = parse_craft_args({"craft", "10"});
    CHECK(c.ok);
    CHECK(c.amount.n == 10);
    CHECK(c.filter.empty());
    c = parse_craft_args({"craft", "max", "wooden", "arrow"});
    CHECK(c.ok);
    CHECK(c.amount.max);
    CHECK(c.filter == "wooden arrow");
    c = parse_craft_args({"craft", "x5", "String"});
    CHECK(c.amount.n == 5);
    CHECK(c.filter == "string");
    CHECK_FALSE(parse_craft_args({"craft"}).ok);
    CHECK_FALSE(parse_craft_args({"craft", "lots"}).ok);
    CHECK_FALSE(parse_craft_args({"craft", "lots"}).error.empty());
}

TEST_CASE("bulk_craft: names: debug names made readable; a filter matches the readable or the debug name") {
    CHECK(pretty_item("Material_T1_Wood_Logs") == "Wood Logs");
    CHECK(pretty_item("Ammo_T0_Arrow_Wooden_Grassland_lvl3") == "Arrow Wooden Grassland lvl3");
    CHECK(pretty_item("Shield_T0_Wooden") == "Shield Wooden");
    CHECK(pretty_item("Torch") == "Torch");
    CHECK(pretty_item("Block_T1_Stone_Rough_CRAFTING") == "Stone Rough");
    CHECK(name_matches("Material_T1_String", "string"));
    CHECK(name_matches("Ammo_T0_Arrow_Wooden_Grassland_lvl3", "wooden arrow"));  // every word, any order
    CHECK_FALSE(name_matches("Material_T1_String", "rope"));
    CHECK(name_matches("Material_T1_String", ""));
}

TEST_CASE("bulk_craft: learned recipes per account, most recent first, saved and read back") {
    Learned l;
    CHECK_FALSE(l.knows(0xA, 1));
    CHECK(l.learn(0xA, 1) == LearnResult::New);
    CHECK(l.learn(0xA, 2) == LearnResult::New);
    CHECK(l.knows(0xA, 1));
    CHECK_FALSE(l.knows(0xB, 1));  // another player's hand crafts do not count
    CHECK(l.of(0xA) == std::vector<uint32_t>{2, 1});
    CHECK(l.learn(0xA, 2) == LearnResult::None);     // already the newest
    CHECK(l.learn(0xA, 1) == LearnResult::Recency);  // used again: newest (not worth a save)
    CHECK(l.of(0xA) == std::vector<uint32_t>{1, 2});
    CHECK(l.learn(0, 3) == LearnResult::None);       // no account: never
    CHECK(l.learn(0xA, 0) == LearnResult::None);
    l.learn(0xB, 7);
    Learned back;
    back.parse(l.text());
    CHECK(back.of(0xA) == std::vector<uint32_t>{1, 2});
    CHECK(back.of(0xB) == std::vector<uint32_t>{7});
    back.parse("garbage line\nacct zz 1\nacct 00000000000000c0 0000000g 00000005\n");
    CHECK(back.of(0xC0) == std::vector<uint32_t>{5});  // bad words skipped, the rest kept
}

TEST_CASE("bulk_craft: a file read later keeps what was learned this session before it") {
    Learned l;
    l.learn(0xA, 9);
    l.parse("acct 000000000000000a 00000001 00000002\n");
    CHECK(l.knows(0xA, 9));
    CHECK(l.knows(0xA, 1));
    CHECK(l.of(0xA).front() == 9);
}

TEST_CASE("bulk_craft: at most kMaxLearned recipes per account (the oldest go)") {
    Learned l;
    for (uint32_t i = 1; i <= kMaxLearned + 5; ++i) l.learn(0xA, i);
    CHECK(l.of(0xA).size() == kMaxLearned);
    CHECK_FALSE(l.knows(0xA, 1));
    CHECK(l.knows(0xA, kMaxLearned + 5));
}

TEST_CASE("bulk_craft: the mod's own sends are told apart from the game's crafts") {
    OwnSends own;
    own.sent(1, 0xAA);
    own.sent(1, 0xAA);
    CHECK(own.take(1, 0xAA));
    CHECK(own.take(1, 0xAA));
    CHECK_FALSE(own.take(1, 0xAA));  // a third one is the player's own craft
    CHECK_FALSE(own.take(2, 0xAA));
    own.sent(3, 0xBB);
    own.clear();
    CHECK_FALSE(own.take(3, 0xBB));
}

TEST_CASE("bulk_craft: the learned recipes of a workshop, filtered by name, most recent first") {
    const std::vector<uint32_t> learned{0x852c5432, 0x278240d5, 0x7e4e353a};
    std::vector<Recipe> station{arrows(), string_recipe()};
    auto name = [](uint32_t item) -> std::string {
        return item == kArrows ? "Ammo_T0_Arrow_Wooden_Grassland_lvl3" : item == kString ? "Material_T1_String" : "?";
    };
    auto got = learned_here(learned, station, "", name);
    REQUIRE(got.size() == 2);
    CHECK(got[0].id == 0x278240d5);
    CHECK(got[1].id == 0x7e4e353a);
    got = learned_here(learned, station, "string", name);
    REQUIRE(got.size() == 1);
    CHECK(got[0].id == 0x7e4e353a);
    CHECK(learned_here({}, station, "", name).empty());
}

// ---- the order runner ----
namespace {
struct Rig {
    Runner run;
    std::vector<Action> acts;
    uint64_t ms = 1000;
    uint32_t tick = 100;
    void step(const StopCheck& check = nullptr, const ContentsFn& contents = nullptr) {
        auto a = run.step(ms, tick, check, contents);
        acts.insert(acts.end(), a.begin(), a.end());
    }
    Order order(const Recipe& r, uint32_t want, const Settings& s) {
        Order o;
        o.player = 1;
        o.account = 0xA1;
        o.station = 6877;
        o.recipe = r;
        o.want = want;
        o.settings = s;
        return o;
    }
    size_t sends() const {
        size_t n = 0;
        for (const auto& a : acts) n += a.kind == Action::Send;
        return n;
    }
    const Action* finished() const {
        for (const auto& a : acts)
            if (a.kind == Action::Finished) return &a;
        return nullptr;
    }
};
}  // namespace

TEST_CASE("bulk_craft: a paced order sends one craft, waits for it to arrive, then the pace, then the next") {
    Rig g;
    Settings s = paced();
    s.floor_s = 0.5;
    REQUIRE(g.run.start(g.order(arrows(), 3, s), g.ms));
    CHECK_FALSE(g.run.start(g.order(arrows(), 3, s), g.ms));  // one order per player
    g.step();
    REQUIRE(g.sends() == 1);
    CHECK(g.acts[0].amount == 1);
    CHECK(g.acts[0].station == 6877);
    CHECK(g.acts[0].recipe == 0x278240d5);
    g.ms += 16, g.tick += 1;
    g.step();
    CHECK(g.sends() == 1);                      // still waiting for the arrows
    g.run.on_pickup(1, kArrows, 10, 0);            // the craft arrived
    g.run.on_pickup(2, kArrows, 10, 0);            // another player's pickup: not ours
    g.ms += 16, g.tick += 1;
    g.step();
    CHECK(g.sends() == 1);                      // the pace: 500 ms after the send
    g.ms += 500, g.tick += 30;
    g.step();
    CHECK(g.sends() == 2);
    g.run.on_pickup(1, kArrows, 10, 0);
    g.ms += 500, g.tick += 30;
    g.step();
    CHECK(g.sends() == 3);
    g.run.on_pickup(1, kArrows, 10, 0);
    g.ms += 16, g.tick += 1;
    g.step();
    const Action* f = g.finished();
    REQUIRE(f);
    CHECK(f->order.made == 3);
    CHECK(f->outcome == Outcome::Done);
    CHECK(g.run.empty());
}

TEST_CASE("bulk_craft: a craft that never arrives stops the order (out of materials, no room, a requirement)") {
    Rig g;
    REQUIRE(g.run.start(g.order(string_recipe(), 10, paced()), g.ms));
    g.step();
    g.run.on_pickup(1, kString, 1, 0);
    g.ms += 600, g.tick += 36;
    g.step();
    REQUIRE(g.sends() == 2);
    g.ms += 600, g.tick += kConfirmTicks + 1;  // nothing came
    g.step();
    const Action* f = g.finished();
    REQUIRE(f);
    CHECK(f->order.made == 1);
    CHECK(f->outcome == Outcome::GameMadeNoMore);
    CHECK(g.sends() == 2);
}

TEST_CASE("bulk_craft: a pickup split over two stacks still counts as one craft") {
    Rig g;
    REQUIRE(g.run.start(g.order(arrows(), 1, paced()), g.ms));
    g.step();
    g.run.on_pickup(1, kArrows, 4, 0);
    g.run.on_pickup(1, kArrows, 6, 0);
    g.ms += 16, g.tick += 1;
    g.step();
    REQUIRE(g.finished());
    CHECK(g.finished()->order.made == 1);
    CHECK(g.finished()->outcome == Outcome::Done);
}

TEST_CASE("bulk_craft: an instant order asks for everything at once and counts what came") {
    Rig g;
    REQUIRE(g.run.start(g.order(blocks(), 5, instant()), g.ms));
    g.step();
    REQUIRE(g.sends() == 1);
    CHECK(g.acts[0].amount == 5);
    g.run.on_pickup(1, kBlocks, 100, 0);
    g.run.on_pickup(1, kBlocks, 100, 0);  // only two were paid for
    g.ms += 1000, g.tick += kConfirmTicks + 1;
    g.step();
    const Action* f = g.finished();
    REQUIRE(f);
    CHECK(f->order.made == 2);
    CHECK(f->outcome == Outcome::GameMadeNoMore);
    CHECK(g.sends() == 1);
}

TEST_CASE("bulk_craft: an instant order whose crafts all arrive ends at once") {
    Rig g;
    REQUIRE(g.run.start(g.order(blocks(), 2, instant()), g.ms));
    g.step();
    g.run.on_pickup(1, kBlocks, 200, 0);
    g.ms += 16, g.tick += 1;
    g.step();
    REQUIRE(g.finished());
    CHECK(g.finished()->outcome == Outcome::Done);
    CHECK(g.finished()->order.made == 2);
}

TEST_CASE("bulk_craft: the checks before each send stop an order with their reason") {
    Rig g;
    REQUIRE(g.run.start(g.order(arrows(), 5, paced()), g.ms));
    g.step([](const Order&) { return Stop{"you walked away from the station"}; });
    CHECK(g.sends() == 0);
    const Action* f = g.finished();
    REQUIRE(f);
    CHECK(f->outcome == Outcome::Stopped);
    CHECK(f->why == "you walked away from the station");
}

TEST_CASE("bulk_craft: stop ends an order; a pickup after it changes nothing") {
    Rig g;
    REQUIRE(g.run.start(g.order(arrows(), 5, paced()), g.ms));
    g.step();
    auto o = g.run.stop(1);
    REQUIRE(o);
    CHECK(o->made == 0);
    CHECK_FALSE(g.run.stop(1));
    g.run.on_pickup(1, kArrows, 10, 0);
    g.ms += 1000, g.tick += 60;
    g.step();
    CHECK(g.sends() == 1);
    CHECK_FALSE(g.finished());
    CHECK(g.run.running(1) == nullptr);
}

TEST_CASE("bulk_craft: clear drops every order (a new world)") {
    Rig g;
    REQUIRE(g.run.start(g.order(arrows(), 5, paced()), g.ms));
    g.run.clear();
    CHECK(g.run.empty());
    g.step();
    CHECK(g.sends() == 0);
}

TEST_CASE("bulk_craft: the notes say what was made in items, and why it stopped") {
    Order o;
    o.recipe = arrows();
    o.want = 5;
    o.made = 5;
    CHECK(finish_text(o, Outcome::Done, "", "Arrows") == "Bulk craft: made 50 Arrows (5 crafts)");
    o.made = 2;
    CHECK(finish_text(o, Outcome::GameMadeNoMore, "", "Arrows") ==
          "Bulk craft: made 20 Arrows (2 of 5 crafts); the game made no more (out of materials, no room, or a requirement "
          "not met)");
    CHECK(finish_text(o, Outcome::Stopped, "you walked away from the station", "Arrows") ==
          "Bulk craft: made 20 Arrows (2 of 5 crafts); stopped: you walked away from the station");
    o.made = 0;
    CHECK(finish_text(o, Outcome::Stopped, "out of Twigs", "Arrows") == "Bulk craft: made nothing; stopped: out of Twigs");
    o.made = 1;
    o.awaiting = true;  // stopped between a send and its item
    CHECK(finish_text(o, Outcome::Stopped, "you asked", "Arrows") ==
          "Bulk craft: made 10 Arrows (1 of 5 crafts); stopped: you asked (the craft on its way may still arrive)");
}


// ---- edge cases ----
TEST_CASE("bulk_craft fix: a category input is filled with the choice saved from the hand craft, and counted") {
    const CategoryLookup lookup = [](uint32_t cat) {
        return cat == kFibreCategory ? std::vector<CategoryItem>{{kFibre, 3.0f}, {kFlax, 2.0f}} : std::vector<CategoryItem>{};
    };
    Choices c{};
    c[0] = {kFibreCategory, 1};  // flax
    auto r = resolve(string_recipe(), c, lookup);
    REQUIRE(r.result == Resolve::Ok);
    REQUIRE(r.recipe.inputs.size() == 1);
    CHECK(r.recipe.inputs[0].item == kFlax);
    CHECK(r.recipe.inputs[0].count == 2);
    CHECK_FALSE(r.recipe.category_input);
    CHECK(can_pay(r.recipe, {{kFlax, 7}, {kFibre, 99}}) == 3);  // the rare fibre is never touched
    CHECK(missing_input(r.recipe, {{kFibre, 99}}) == kFlax);
    c[0].index = 0;
    r = resolve(string_recipe(), c, lookup);
    REQUIRE(r.result == Resolve::Ok);
    CHECK(r.recipe.inputs[0].item == kFibre);
    CHECK(r.recipe.inputs[0].count == 3);
    CHECK(resolve(string_recipe(), Choices{}, lookup).result == Resolve::NoChoice);  // no choice saved: refused
    c[0].index = 2;
    CHECK(resolve(string_recipe(), c, lookup).result == Resolve::BadChoice);  // not in the category
    Choices other{};
    other[0] = {0xDEAD, 0};
    CHECK(resolve(string_recipe(), other, lookup).result == Resolve::NoChoice);
    // a recipe without category inputs needs no choice
    CHECK(resolve(arrows(), Choices{}, lookup).result == Resolve::Ok);
    // scaling rounds up; a plain input of the same item adds up
    Recipe mixed = arrows();
    mixed.inputs.push_back({0, 1, kFibreCategory});
    const CategoryLookup half = [](uint32_t) { return std::vector<CategoryItem>{{kTwigs, 1.5f}}; };
    Choices t{};
    t[0] = {kFibreCategory, 0};
    r = resolve(mixed, t, half);
    REQUIRE(r.result == Resolve::Ok);
    REQUIRE(r.recipe.inputs.size() == 1);
    CHECK(r.recipe.inputs[0].item == kTwigs);
    CHECK(r.recipe.inputs[0].count == 4);  // 2 + ceil(1.5)
}

TEST_CASE("bulk_craft fix: the choice is learned with the recipe, kept by a teach, saved and read back") {
    Learned l;
    Choices c{};
    c[0] = {kFibreCategory, 1};
    CHECK(l.learn(0xA, 0x7e4e353a, &c) == LearnResult::New);
    REQUIRE(l.choices_of(0xA, 0x7e4e353a));
    CHECK(l.choices_of(0xA, 0x7e4e353a)->at(0).category == kFibreCategory);
    CHECK(l.choices_of(0xA, 0x7e4e353a)->at(0).index == 1);
    CHECK(l.learn(0xA, 0x7e4e353a, &c) == LearnResult::None);
    Choices d = c;
    d[0].index = 0;
    CHECK(l.learn(0xA, 0x7e4e353a, &d) == LearnResult::Choice);  // another item picked by hand: saved
    CHECK(l.learn(0xA, 0x7e4e353a) == LearnResult::None);        // a teach keeps the choice
    CHECK(l.choices_of(0xA, 0x7e4e353a)->at(0).index == 0);
    CHECK_FALSE(l.choices_of(0xB, 0x7e4e353a));
    l.learn(0xA, 0x278240d5);
    Learned back;
    back.parse(l.text());
    REQUIRE(back.choices_of(0xA, 0x7e4e353a));
    CHECK(back.choices_of(0xA, 0x7e4e353a)->at(0).category == kFibreCategory);
    CHECK(back.choices_of(0xA, 0x7e4e353a)->at(0).index == 0);
    REQUIRE(back.choices_of(0xA, 0x278240d5));
    CHECK(back.choices_of(0xA, 0x278240d5)->at(0).category == 0);
    back.parse("acct 00000000000000c0 00000005/zz.1 00000006/0000c0c0.300 00000007/0000c0c0.2\n");
    CHECK(back.knows(0xC0, 5));                                       // a bad choice is dropped, the recipe kept
    CHECK(back.choices_of(0xC0, 6)->at(0).category == 0);             // index above 255: dropped
    CHECK(back.choices_of(0xC0, 7)->at(0).index == 2);
}

TEST_CASE("bulk_craft fix: a send carries the saved choice") {
    Rig g;
    Order o = g.order(string_recipe(), 1, paced());
    o.choices[0] = {kFibreCategory, 1};
    REQUIRE(g.run.start(o, g.ms));
    g.step();
    REQUIRE(g.sends() == 1);
    CHECK(g.acts[0].choices[0].category == kFibreCategory);
    CHECK(g.acts[0].choices[0].index == 1);
}

TEST_CASE("bulk_craft fix: the pace carries over to the next order of the same account, not to others") {
    Rig g;
    Settings s = paced();
    s.floor_s = 0.5;
    REQUIRE(g.run.start(g.order(arrows(), 1, s), g.ms));
    g.step();
    g.run.on_pickup(1, kArrows, 10, 0);
    g.ms += 16, g.tick += 1;
    g.step();
    REQUIRE(g.finished());
    REQUIRE(g.run.start(g.order(arrows(), 1, s), g.ms));  // /craft 1 again at once
    g.step();
    CHECK(g.sends() == 1);  // not before the pace
    g.ms += 484, g.tick += 29;
    g.step();
    CHECK(g.sends() == 2);  // 500 ms after the first send
    // stop and start again: still the pace
    REQUIRE(g.run.stop(1));
    REQUIRE(g.run.start(g.order(arrows(), 1, s), g.ms + 10));
    g.ms += 10, g.tick += 1;
    g.step();
    CHECK(g.sends() == 2);
    // another account (another player) is not held back
    Order other = g.order(arrows(), 1, s);
    other.player = 2;
    other.account = 0xB2;
    REQUIRE(g.run.start(other, g.ms));
    g.step();
    CHECK(g.sends() == 3);
    CHECK(g.acts.back().player == 2);
    // a new world forgets it
    g.run.clear();
    CHECK(g.run.ready_at(0xA1) == 0);
}

TEST_CASE("bulk_craft fix: a quiet stop (another account in that player's slot) says nothing to the newcomer") {
    Rig g;
    REQUIRE(g.run.start(g.order(arrows(), 5, paced()), g.ms));
    g.step([](const Order&) { return Stop{"another player has this slot now", true}; });
    const Action* f = g.finished();
    REQUIRE(f);
    CHECK(f->quiet);
    CHECK(g.sends() == 0);
}

TEST_CASE("bulk_craft fix: recipes with station requirements are refused (props, comfort, level, water...)") {
    CHECK(check_recipe(blocks(), 0x4da88f73, false, true) == Refusal::None);
    for (int k = 0; k < 9; ++k) {
        Recipe r = blocks();
        switch (k) {
            case 0: r.required_props = 4; break;
            case 1: r.comfort = 3; break;
            case 2: r.level = 1; break;
            case 3: r.server_progress = 2; break;
            case 4: r.happy_npcs = 1; break;
            case 5: r.energy = 10; break;
            case 6: r.water = 5; break;
            case 7: r.water_charges = 1; break;
            default: r.sheltered = true; break;
        }
        CAPTURE(k);
        CHECK(check_recipe(r, 0x4da88f73, false, true) == Refusal::Requirements);
    }
    CHECK(refusal_text(Refusal::Requirements, "Bed").find("by hand") != std::string::npos);
    CHECK_FALSE(refusal_text(Refusal::NoChoice, "String").empty());
    CHECK_FALSE(refusal_text(Refusal::Blocked, "String").empty());
}

TEST_CASE("bulk_craft fix: settings are clamped to mod.json's limits") {
    Settings s;
    s.max_amount = 100000;
    s.range = 999;
    s.floor_s = 60;
    s = clamped(s);
    CHECK(s.max_amount == 500);
    CHECK(s.range == doctest::Approx(15));
    CHECK(s.floor_s == doctest::Approx(5));
    s.max_amount = 0;
    s.range = 0;
    s.floor_s = 0;
    s = clamped(s);
    CHECK(s.max_amount == 5);
    CHECK(s.range == doctest::Approx(2));
    CHECK(s.floor_s == doctest::Approx(0.1));
}

TEST_CASE("bulk_craft fix: only a pickup from nowhere (a craft's output) counts") {
    Rig g;
    REQUIRE(g.run.start(g.order(arrows(), 1, paced()), g.ms));
    g.step();
    g.run.on_pickup(1, kArrows, 10, 4711);  // arrows looted from a chest at that moment
    g.ms += 16, g.tick += 1;
    g.step();
    CHECK_FALSE(g.finished());
    g.run.on_pickup(1, kArrows, 10, 0);
    g.ms += 16, g.tick += 1;
    g.step();
    REQUIRE(g.finished());
    CHECK(g.finished()->outcome == Outcome::Done);
}

TEST_CASE("bulk_craft fix: materials that did not leave the bag stop the order (the game took them elsewhere)") {
    std::map<uint32_t, uint32_t> bag{{kStone, 10}};
    const ContentsFn contents = [&bag](const Order&) { return bag; };
    CHECK(materials_from_bag(blocks(), {{kStone, 10}}, {{kStone, 6}}, 2));
    CHECK_FALSE(materials_from_bag(blocks(), {{kStone, 10}}, {{kStone, 10}}, 1));
    CHECK_FALSE(materials_from_bag(blocks(), {{kStone, 10}}, {{kStone, 9}}, 1));
    CHECK(materials_from_bag(blocks(), {{kStone, 10}}, {{kStone, 10}}, 0));
    // in the runner: the first craft took 2 stone from the bag, the second took none
    Rig g;
    REQUIRE(g.run.start(g.order(blocks(), 3, paced()), g.ms));
    g.step(nullptr, contents);
    REQUIRE(g.sends() == 1);
    bag[kStone] = 8;
    g.run.on_pickup(1, kBlocks, 100, 0);
    g.ms += 600, g.tick += 36;
    g.step(nullptr, contents);
    REQUIRE(g.sends() == 2);
    CHECK_FALSE(g.finished());
    g.run.on_pickup(1, kBlocks, 100, 0);  // came, but the bag still holds 8
    g.ms += 16, g.tick += 1;
    g.step(nullptr, contents);
    const Action* f = g.finished();
    REQUIRE(f);
    CHECK(f->foreign);
    CHECK(f->outcome == Outcome::Stopped);
    CHECK(f->order.made == 2);
    CHECK(g.sends() == 2);
}
