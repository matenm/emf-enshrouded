// Loot Magnet (mods/loot_magnet): which corpses are looted into which player, and what is never touched.
#include <doctest.h>

#include <map>
#include <string>
#include <vector>

#include "../mods/loot_magnet/magnet_logic.h"

using namespace magnet;

namespace {
constexpr uint64_t kSec = 1000;

// A fresh enemy corpse at `pos` holding furs: Health, an inventory and an interaction offer (what the game's loot event
// needs: docs/engine_parts/re_villages.md, immediate_loot_action).
Body corpse(uint32_t id, Vec3 pos, std::vector<std::string> items = {"Material_T1_Animal_Fur"}) {
    Body b;
    b.id = id;
    b.exists = true;
    b.pos = pos;
    b.has_health = true;
    b.has_inventory = true;
    b.has_offer = true;
    b.dead = true;
    b.faction = 3;  // Vukah
    b.items = std::move(items);
    return b;
}
Rules rules() {
    Rules r;
    r.radius = 8;
    r.delay_ms = 1000;
    r.window_ms = 11000;  // counted from the death
    r.base_radius = 30;
    r.materials_only = true;
    r.include_ammo = true;
    return r;
}
const Vec3 kHere{1000, 20, 1000};
const std::vector<Vec3> kNoBases;
}  // namespace

TEST_CASE("loot magnet: a fresh corpse near the player is looted after the delay, within the window") {
    const Death d{500, kHere, 10 * kSec, 1};
    const Body b = corpse(500, kHere);
    const Vec3 player{kHere.x + 3, kHere.y, kHere.z};
    CHECK(judge(d, b, player, kNoBases, rules(), 10 * kSec + 500, false) == Why::TooSoon);
    CHECK(judge(d, b, player, kNoBases, rules(), 11 * kSec, false) == Why::Ok);
    CHECK(judge(d, b, player, kNoBases, rules(), 21 * kSec, false) == Why::Ok);
    CHECK(judge(d, b, player, kNoBases, rules(), 21 * kSec + 1, false) == Why::TooLate);  // an old corpse: exploring
    CHECK(judge(d, b, {kHere.x + 9, kHere.y, kHere.z}, kNoBases, rules(), 12 * kSec, false) == Why::TooFar);
}

TEST_CASE("loot magnet: never a corpse twice, a player, an ally, a tame or owned creature, a villager, or one that moved") {
    const Death d{500, kHere, 0, 1};
    const Vec3 p = kHere;
    const auto at = 2 * kSec;
    Body b = corpse(500, kHere);
    CHECK(judge(d, b, p, kNoBases, rules(), at, true) == Why::Looted);
    b = corpse(500, kHere);
    b.player = true;  // a dead player (their own things)
    CHECK(judge(d, b, p, kNoBases, rules(), at, false) == Why::Protected);
    for (int faction : {7, 8, 11, 12}) {  // Player, AutomatedPlayer, PlayerAlly (a companion), FarmAnimal
        b = corpse(500, kHere);
        b.faction = faction;
        CAPTURE(faction);
        CHECK(judge(d, b, p, kNoBases, rules(), at, false) == Why::Protected);
    }
    b = corpse(500, kHere);
    b.owner = 1;
    CHECK(judge(d, b, p, kNoBases, rules(), at, false) == Why::Protected);
    b = corpse(500, kHere);
    b.tame = true;
    CHECK(judge(d, b, p, kNoBases, rules(), at, false) == Why::Protected);
    b = corpse(500, kHere);
    b.villager = true;
    CHECK(judge(d, b, p, kNoBases, rules(), at, false) == Why::Protected);
    b = corpse(500, {kHere.x + 6, kHere.y, kHere.z});  // 6 m from where it died: not the same body (an id reused?)
    CHECK(judge(d, b, {kHere.x + 6, kHere.y, kHere.z}, kNoBases, rules(), at, false) == Why::Moved);
    b = corpse(500, kHere);
    b.exists = false;
    CHECK(judge(d, b, p, kNoBases, rules(), at, false) == Why::Gone);
}

TEST_CASE("loot magnet: only what the game's loot event can take: a dead creature with an inventory") {
    const Death d{500, kHere, 0, 1};
    Body b = corpse(500, kHere);
    b.has_inventory = false;
    CHECK(judge(d, b, kHere, kNoBases, rules(), 2 * kSec, false) == Why::NoLoot);
    b = corpse(500, kHere);
    b.has_health = false;  // not a creature: a chest, a prop
    CHECK(judge(d, b, kHere, kNoBases, rules(), 2 * kSec, false) == Why::NoLoot);
    b = corpse(500, kHere, {});  // nothing in it (yet): wait
    CHECK(judge(d, b, kHere, kNoBases, rules(), 2 * kSec, false) == Why::Empty);
}

TEST_CASE("loot magnet: nothing inside a base (the radius rule), wherever the player stands") {
    const Death d{500, kHere, 0, 1};
    const std::vector<Vec3> bases = {{kHere.x + 25, kHere.y + 10, kHere.z}};
    CHECK(judge(d, corpse(500, kHere), kHere, bases, rules(), 2 * kSec, false) == Why::InBase);
    const std::vector<Vec3> far = {{kHere.x + 31, kHere.y, kHere.z}};
    CHECK(judge(d, corpse(500, kHere), kHere, far, rules(), 2 * kSec, false) == Why::Ok);
}

TEST_CASE("loot magnet: materials only takes a corpse whose every stack is a material (ammo by choice); else everything") {
    CHECK(is_material("Material_T1_Animal_Fur", false));
    CHECK(is_material("material_t1_animal_fur", false));
    CHECK(is_material("Food_T1_raw_bad_meat_LeanMeat", false));
    CHECK(is_material("_Food_raw_T3_fish_common_03", false));
    CHECK(is_material("Build_TerrainMaterial_T2_TallTree_Bark", false));
    CHECK_FALSE(is_material("Weapon_T1_1H_SwordEpic_01", true));
    CHECK_FALSE(is_material("Ammo_Arrow_T1", false));
    CHECK(is_material("Ammo_Arrow_T1", true));
    CHECK_FALSE(is_material("Material", false));
    const Death d{500, kHere, 0, 1};
    const Body mixed = corpse(500, kHere, {"Material_T1_Animal_Fur", "Weapon_T1_1H_SwordEpic_01"});
    CHECK(judge(d, mixed, kHere, kNoBases, rules(), 2 * kSec, false) == Why::Filtered);  // the player opens that one
    Rules all = rules();
    all.materials_only = false;
    CHECK(judge(d, mixed, kHere, kNoBases, all, 2 * kSec, false) == Why::Ok);
    Rules no_ammo = rules();
    no_ammo.include_ammo = false;
    CHECK(judge(d, corpse(500, kHere, {"Ammo_Arrow_T1"}), kHere, kNoBases, no_ammo, 2 * kSec, false) == Why::Filtered);
}

namespace {
struct World {
    std::map<uint32_t, Body> bodies;
    Lookup lookup() {
        return [this](uint32_t id) {
            auto it = bodies.find(id);
            if (it == bodies.end()) {
                Body gone;
                gone.id = id;
                return gone;
            }
            return it->second;
        };
    }
};
}  // namespace

TEST_CASE("loot magnet: each corpse goes to the nearest player whose magnet is on; a corpse is sent once") {
    Magnet m;
    World w;
    w.bodies[500] = corpse(500, kHere);
    w.bodies[501] = corpse(501, {kHere.x + 50, kHere.y, kHere.z});
    m.on_death(500, kHere, 0, 1);
    m.on_death(501, {kHere.x + 50, kHere.y, kHere.z}, 0, 1);
    m.on_death(502, kHere, 0, 1);  // never had a body we could read: dropped quietly
    const std::vector<Looter> players = {{1, {kHere.x + 5, kHere.y, kHere.z}, true},
                                         {2, {kHere.x + 2, kHere.y, kHere.z}, false},  // nearer, but off
                                         {3, {kHere.x + 52, kHere.y, kHere.z}, true}};
    auto sent = m.tick(2 * kSec, 1, players, w.lookup(), kNoBases, rules());
    REQUIRE(sent.size() == 2);
    CHECK(sent[0].corpse == 500u);
    CHECK(sent[0].player == 1u);
    CHECK(sent[1].corpse == 501u);
    CHECK(sent[1].player == 3u);
    CHECK(m.tick(3 * kSec, 1, players, w.lookup(), kNoBases, rules()).empty());  // never twice
}

TEST_CASE("loot magnet: a corpse nobody with a magnet is near waits for its window; switched off, nothing is looted") {
    Magnet m;
    World w;
    w.bodies[500] = corpse(500, kHere);
    m.on_death(500, kHere, 0, 1);
    std::vector<Looter> players = {{1, {kHere.x + 20, kHere.y, kHere.z}, true}};
    CHECK(m.tick(2 * kSec, 1, players, w.lookup(), kNoBases, rules()).empty());
    players[0].on = false;
    players[0].pos = kHere;
    CHECK(m.tick(3 * kSec, 1, players, w.lookup(), kNoBases, rules()).empty());
    players[0].on = true;  // walks up and switches it on within the window
    CHECK(m.tick(4 * kSec, 1, players, w.lookup(), kNoBases, rules()).size() == 1);
    CHECK(m.watched() == 0);  // done: forgotten
}

TEST_CASE("loot magnet: deaths of an older session are forgotten (entity ids are reused after a reload)") {
    Magnet m;
    World w;
    w.bodies[500] = corpse(500, kHere);
    m.on_death(500, kHere, 0, 1);
    const std::vector<Looter> players = {{1, kHere, true}};
    CHECK(m.tick(2 * kSec, 2, players, w.lookup(), kNoBases, rules()).empty());
    CHECK(m.watched() == 0);
}

TEST_CASE("loot magnet: at most 256 deaths are watched; the oldest go first") {
    Magnet m;
    for (uint32_t i = 0; i < 300; ++i) m.on_death(1000 + i, kHere, i, 1);
    CHECK(m.watched() == 256);
}

TEST_CASE("loot magnet: no pickup from a corpse that still holds loot within 2 s: no room (said once per player and session)") {
    Magnet m;
    m.on_sent(500, 1, 10 * kSec, 1);
    m.on_sent(501, 2, 10 * kSec, 1);
    m.on_pickup(2, 501);  // player 2 got theirs
    CHECK(m.overdue(11 * kSec).empty());
    const auto late = m.overdue(12 * kSec + 1);
    REQUIRE(late.size() == 1);
    CHECK(late[0].corpse == 500u);
    CHECK(late[0].player == 1u);
    CHECK(m.overdue(13 * kSec).empty());
    CHECK(m.should_tell_full(1, 1));
    CHECK_FALSE(m.should_tell_full(1, 1));
    CHECK(m.should_tell_full(1, 2));  // a new session
}

TEST_CASE("loot magnet: each player's switch is kept by account in the world's file; the default for everyone else") {
    Toggles t;
    CHECK(t.on(0xAA, true));
    CHECK_FALSE(t.on(0xAA, false));
    t.set(0xAA, false);
    t.set(0xBB, true);
    CHECK_FALSE(t.on(0xAA, true));
    CHECK(t.on(0xBB, false));
    const std::string text = t.text();
    CHECK(text == "0x00000000000000aa off\n0x00000000000000bb on\n");
    Toggles u;
    u.parse(text + "junk line\n0xzz on\n");
    CHECK_FALSE(u.on(0xAA, true));
    CHECK(u.on(0xBB, false));
    CHECK(u.text() == text);
    u.clear();
    CHECK(u.on(0xAA, true));
}

TEST_CASE("loot magnet: an item the game cannot name yet makes a materials-only corpse wait, not be dropped") {
    const Death d{500, kHere, 0, 1};
    const Body b = corpse(500, kHere, {"Material_T1_Animal_Fur", ""});  // "" = not named yet
    CHECK(judge(d, b, kHere, kNoBases, rules(), 2 * kSec, false) == Why::Empty);
    Rules all = rules();
    all.materials_only = false;
    CHECK(judge(d, b, kHere, kNoBases, all, 2 * kSec, false) == Why::Ok);
}

TEST_CASE("loot magnet: an id that dies again in the same session (reused by a new creature) is a new corpse") {
    Magnet m;
    World w;
    w.bodies[500] = corpse(500, kHere);
    const std::vector<Looter> players = {{1, kHere, true}};
    m.on_death(500, kHere, 0, 1);
    REQUIRE(m.tick(2 * kSec, 1, players, w.lookup(), kNoBases, rules()).size() == 1);
    m.on_death(500, kHere, 60 * kSec, 1);  // later another creature with that id dies
    CHECK(m.tick(62 * kSec, 1, players, w.lookup(), kNoBases, rules()).size() == 1);
}

// ---- the Vukah brawler's corpse, and a decision for every corpse ----
TEST_CASE("loot magnet: the brawler's corpse as the game leaves it is looted: an Inventory added at death, no interaction offer") {
    // Enemy_Midboss_Vukah_Bigfoot_BossHealthBar has RandomLoot but no Inventory and no InteractionOfferRuntime in its
    // template (export/templates.csv); at death the game adds the Inventory (inv 6938 showed 3 stacks on it). A test in
    // game looted that corpse with the bypass event, so the offer is not needed.
    const Death d{6938, kHere, 0, 1};
    Body b = corpse(6938, kHere, {"Material_TX_Trophy_Vukah_head", "Material_T1_Animal_Fur", "Material_T2_Token_WeaponUpgrade_Rune02"});
    b.has_offer = false;
    CHECK(judge(d, b, kHere, kNoBases, rules(), 2 * kSec, false) == Why::Ok);
}

TEST_CASE("loot magnet: a body that is alive is no corpse (it waits); another template under the id is not the dead one") {
    Death d{500, kHere, 0, 1};
    Body b = corpse(500, kHere);
    b.dead = false;  // NetworkHealth not 0: a new creature that got the id, or not dead yet
    CHECK(judge(d, b, kHere, kNoBases, rules(), 2 * kSec, false) == Why::Alive);
    d.tpl = 0x1000;
    b = corpse(500, kHere);
    b.tpl = 0x2000;  // the id now belongs to an entity of another template
    CHECK(judge(d, b, kHere, kNoBases, rules(), 2 * kSec, false) == Why::Reused);
    b.tpl = 0x1000;
    CHECK(judge(d, b, kHere, kNoBases, rules(), 2 * kSec, false) == Why::Ok);
}

TEST_CASE("loot magnet: every corpse considered gets one decision: looted, skipped with its reason, or the window ended while it waited") {
    Magnet m;
    World w;
    w.bodies[500] = corpse(500, kHere);
    w.bodies[501] = corpse(501, {kHere.x + 100, kHere.y, kHere.z});
    w.bodies[502] = corpse(502, kHere);
    w.bodies[502].has_inventory = false;
    m.on_death(500, kHere, 0, 1);
    m.on_death(501, {kHere.x + 100, kHere.y, kHere.z}, 0, 1);
    m.on_death(502, kHere, 0, 1);
    const std::vector<Looter> players = {{1, {kHere.x + 3, kHere.y, kHere.z}, true}};
    std::vector<Decision> out;
    CHECK(m.tick(2 * kSec, 1, players, w.lookup(), kNoBases, rules(), &out).size() == 1);
    REQUIRE(out.size() == 1);  // 502 has no inventory yet: it waits (the game adds it 1-3 s after the death)
    CHECK(out[0].corpse == 500u);
    CHECK(out[0].why == Why::Ok);
    CHECK(out[0].player == 1u);
    CHECK(out[0].distance == doctest::Approx(3.0));
    out.clear();
    CHECK(m.tick(5 * kSec, 1, players, w.lookup(), kNoBases, rules(), &out).empty());
    CHECK(out.empty());  // 501 and 502 still wait: nothing to say yet
    m.tick(12 * kSec, 1, players, w.lookup(), kNoBases, rules(), &out);
    REQUIRE(out.size() == 2);
    CHECK(out[0].corpse == 501u);
    CHECK(out[0].why == Why::TooLate);
    CHECK(out[0].waited == Why::TooFar);  // nobody with a magnet came near
    CHECK(out[0].distance == doctest::Approx(97.0));  // the nearest player with a magnet
    CHECK(out[1].corpse == 502u);
    CHECK(out[1].why == Why::TooLate);
    CHECK(out[1].waited == Why::NoLoot);  // no loot appeared within the window
    CHECK(std::string(why_text(Why::NoLoot)).find("inventory") != std::string::npos);
    CHECK(std::string(why_text(Why::TooFar)).find("magnet") != std::string::npos);
}

TEST_CASE("loot magnet: a loot event nothing came from is sent again (up to 5 tries, 2 s apart): nothing moved, nothing doubles") {
    Magnet m;
    m.on_sent(500, 1, 0, 1);
    auto late = m.overdue(2001);
    REQUIRE(late.size() == 1);
    CHECK(late[0].tries == 1);
    CHECK(m.should_retry(late[0]));
    m.on_sent(500, 1, 2001, 1, late[0].tries + 1);
    late = m.overdue(4002);
    REQUIRE(late.size() == 1);
    CHECK(late[0].tries == 2);
    late[0].tries = Magnet::kMaxTries;
    CHECK_FALSE(m.should_retry(late[0]));  // the last try: then "no room" (or the game refused)
}

// ---- the game adds the corpse's Inventory 1-3 s after the death ----
TEST_CASE("loot magnet: a corpse whose Inventory comes 2 s after the death is looted once it is there (it waits, not skipped)") {
    Magnet m;
    World w;
    Body b = corpse(6928, kHere);
    b.has_inventory = false;  // at the death and 1 s later: "entity 6928 has no inventory"
    b.items.clear();
    w.bodies[6928] = b;
    m.on_death(6928, kHere, 0, 1);
    const std::vector<Looter> players = {{1, {kHere.x + 1, kHere.y, kHere.z}, true}};
    std::vector<Decision> out;
    for (uint64_t t = 1000; t <= 2000; t += 500) CHECK(m.tick(t, 1, players, w.lookup(), kNoBases, rules(), &out).empty());
    CHECK(out.empty());   // nothing decided while it has no loot yet
    CHECK(m.watched() == 1);
    w.bodies[6928] = corpse(6928, kHere, {"Material_TX_Trophy_Vukah_head", "Material_T1_Animal_Fur"});  // at 3 s: 2 stacks
    const auto sent = m.tick(3000, 1, players, w.lookup(), kNoBases, rules(), &out);
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].corpse == 6928u);
    REQUIRE(out.size() == 1);
    CHECK(out[0].why == Why::Ok);
}

TEST_CASE("loot magnet: only lasting reasons skip a corpse at once (player, owned, tame, villager, ally, inside a base)") {
    const Death d{500, kHere, 0, 1};
    Body b = corpse(500, kHere);
    b.has_inventory = false;
    b.has_health = false;
    b.owner = 7;  // owned, and no loot yet: skipped for the owner, not kept waiting
    CHECK(judge(d, b, kHere, kNoBases, rules(), 2000, false) == Why::Protected);
    b.owner = 0;
    const std::vector<Vec3> bases = {kHere};
    CHECK(judge(d, b, kHere, bases, rules(), 2000, false) == Why::InBase);
    CHECK(judge(d, b, kHere, kNoBases, rules(), 2000, false) == Why::NoLoot);  // a wait (Magnet::tick keeps it)
    CHECK(is_final(Why::Protected));
    CHECK(is_final(Why::InBase));
    CHECK_FALSE(is_final(Why::NoLoot));
    CHECK_FALSE(is_final(Why::Empty));
    CHECK_FALSE(is_final(Why::Alive));
    CHECK_FALSE(is_final(Why::TooSoon));
    CHECK_FALSE(is_final(Why::TooFar));
}
