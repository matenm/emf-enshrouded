// Beastmaster (mods/beastmaster), run by the Lua host from a copy of the shipped folder, with fake engine events,
// components and commands (tests/lua_mod_rig.h).
#include "lua_mod_rig.h"

#include <regex>

#include "loader/portraits_plan.h"

namespace {
// The animation graph the game wolf (and the scavengers' hound, and the bear pack's bear) runs: its template data on the
// server world, where render models are not (seen in game: a spawned wolf has no ModelResource there).
constexpr const char* kWolfGraph = "3180813f-a50d-47fc-be92-29a2383bd47d";

// The game stores a GUID's first three groups little-endian (see read_field in src/lua_host/fields.cpp).
std::vector<uint8_t> guid_bytes(const std::string& text) {
    auto hex = [&](size_t at) { return static_cast<uint8_t>(std::stoul(text.substr(at, 2), nullptr, 16)); };
    return {hex(6), hex(4), hex(2), hex(0), hex(11), hex(9), hex(16), hex(14),
            hex(19), hex(21), hex(24), hex(26), hex(28), hex(30), hex(32), hex(34)};
}

// Player 1 (account 0xAA, the host, level 10) at (0, 10, 0); player 2 (0xBB) at (30, 10, 0).
// `net`: a v27 core (mod messages) from the start, so Beastmaster's channel is registered.
struct BeastRig : ModRig {
    explicit BeastRig(const std::map<std::string, std::string>& settings = {}, bool net = false, bool load = true)
        : ModRig("beastmaster", L"beastmaster", settings, [](Fake& f) {
              add_player(f, 1, 0xAA, 0, 10, 0);
              add_player(f, 2, 0xBB, 30, 10, 0);
              for (uint32_t p : {1u, 2u}) {
                  component<ecs::Level>(f, p).dataStorage[0] = 10;
                  add_entity(f, p);
                  f.attrs[{p, EMF_ATTR_MANA}] = 100;
                  f.attrs[{p, EMF_ATTR_MAX_MANA}] = 100;
              }
          }, {}, net ? std::function<void(EmfApi&)>([](EmfApi& a) { with_net(a); }) : nullptr, load) {
        with_attributes(r.api);  // a v18+ core: health, mana and damage through the game's attributes
        with_teleport(r.api);    // a v20 core
        with_windows(r.api);     // a v24 core
    }
    std::string saved() { return slurp(data); }
    // Player `p`'s camera at (x, y, z) looking along `yaw_deg` (0 = +z, 90 = +x), level.
    void look(uint32_t p, double x, double y, double z, double yaw_deg) {
        auto& in = component<ecs::PlayerInput>(r.fake, p);
        auto& cam = in.fromClient.camera;
        cam.position.x = static_cast<int64_t>(x * 4294967296.0);
        cam.position.y = static_cast<int64_t>(y * 4294967296.0);
        cam.position.z = static_cast<int64_t>(z * 4294967296.0);
        const double h = yaw_deg * 3.14159265358979 / 360.0;
        cam.orientation.x = 0;
        cam.orientation.y = static_cast<float>(std::sin(h));
        cam.orientation.z = 0;
        cam.orientation.w = static_cast<float>(std::cos(h));
    }
    // A creature running the animation graph `graph` (template data) at (x, 10, z), of `faction`, with health.
    void beast(uint32_t id, const char* graph, double x, double z, int faction, int32_t cur, int32_t max) {
        place(r.fake, id, x, 10, z);
        auto g = guid_bytes(graph);
        g.resize(ecs::AnimationGraphResourceComponent2::kSize, 0);
        r.fake.templates[{ecs::AnimationGraphResourceComponent2::kHash, id}] = g;
        component<ecs::Faction>(r.fake, id).faction = static_cast<emf::gen::FactionType>(faction);
        r.fake.attrs[{id, EMF_ATTR_HEALTH}] = cur;
        r.fake.attrs[{id, EMF_ATTR_MAX_HEALTH}] = max;
        add_entity(r.fake, id);
    }
    int faction(uint32_t id) { return r.fake.components[{ecs::Faction::kHash, id}][0]; }
    // StartGuardingEvents sent so far: (guarding, guarded)
    std::vector<std::pair<uint32_t, uint32_t>> guards() {
        send_events();
        std::vector<std::pair<uint32_t, uint32_t>> out;
        for (const auto& [name, b] : r.fake.emitted) {
            if (name != "StartGuardingEvent") continue;
            const auto* e = reinterpret_cast<const ecs::StartGuardingEvent*>(b.data());
            out.push_back({e->guardingEntityId.id, e->guardedEntityId.id});
        }
        return out;
    }
    // The creature the game makes for the next `n` spawns (ids 1234, 1235, ...): wild, level 1, 100 health.
    void spawnable(int n) {
        for (int i = 0; i < n; ++i) {
            const uint32_t id = r.fake.next_spawn_id + static_cast<uint32_t>(i);
            component<ecs::Faction>(r.fake, id).faction = static_cast<emf::gen::FactionType>(6);
            component<ecs::Level>(r.fake, id).dataStorage[0] = 1;
            r.fake.attrs[{id, EMF_ATTR_HEALTH}] = 100;
            r.fake.attrs[{id, EMF_ATTR_MAX_HEALTH}] = 100;
            add_entity(r.fake, id);
        }
    }
    bool added(uint32_t entity, uint32_t attr, int32_t delta) {
        for (const auto& [e, a, d] : r.fake.attr_adds)
            if (e == entity && a == attr && d == delta) return true;
        return false;
    }
};
constexpr int kWildBeast = 6, kPlayerAlly = 11;
constexpr uint32_t kDogBait = 0x4d0a0001;
constexpr uint32_t kLeanMeat = 0x4d0a0002;  // the wolves' bait
}  // namespace

TEST_CASE("beastmaster: the roster names beasts uniquely, holds 8 per player, and comes back from the world's data") {
    BeastRig b;
    for (int i = 1; i <= 8; ++i) CHECK(b.reply("beastmaster.give wolf").find("added") != std::string::npos);
    const std::string ninth = b.reply("beastmaster.give wolf");
    CHECK(ninth.find("8") != std::string::npos);
    CHECK(ninth.find("added") == std::string::npos);
    const std::string list = b.reply("beastmaster.list");
    CHECK(list.find("1. Wolf - wolf, loyalty 1, at home") != std::string::npos);
    CHECK(list.find("2. Wolf 2 - wolf") != std::string::npos);
    CHECK(list.find("8. Wolf 8 - wolf") != std::string::npos);
    b.seconds(2);
    CHECK(b.saved().find("b 0x00000000000000aa wolf ") != std::string::npos);
    // another player's collection is their own
    CHECK(b.reply("beastmaster.list", 2, 0xBB).find("no beasts") != std::string::npos);
    // the same world loads again later, with a different collection saved
    write(b.data, "b 0x00000000000000aa boar 0 0 Tusker\nb 0x00000000000000bb wolf 0 0 Grey\n");
    b.r.fake.session = 2;
    fire_load(1);
    b.seconds(1);
    const std::string again = b.reply("beastmaster.list");
    CHECK(again.find("Tusker") != std::string::npos);
    CHECK(again.find("Wolf 2") == std::string::npos);
    CHECK(b.reply("beastmaster.list", 2, 0xBB).find("Grey") != std::string::npos);
}

TEST_CASE("beastmaster: a beast just tamed cannot fall for 5 s, then can again") {
    // a swing it began as a wild beast can land on itself once it is an ally (seen in game: a tamed minotaur's
    // three-punch combo, 2 of 3 tames, killed it just after the channel)
    BeastRig b;
    b.look(1, 0, 10.5, 0, 0);
    b.beast(50, kWolfGraph, 0, 5, kWildBeast, 20, 100);
    auto& h = component<ecs::Health>(b.r.fake, 50);  // the game's Health storage: [0] health, [1] its minimum
    REQUIRE(b.reply("beastmaster.tame").find("Taming") != std::string::npos);
    b.seconds(1);
    CHECK(h.dataStorage[1] == 0);   // the channel: as it was
    b.seconds(3);                   // tamed, up to 50 % (50 of 100)
    REQUIRE(b.reply("beastmaster.list").find("1. Wolf - wolf, loyalty 1, out") != std::string::npos);
    CHECK(h.dataStorage[1] == 50);  // its health cannot go under that
    h.dataStorage[1] = 0;           // (the game may write it back)
    b.seconds(2);
    CHECK(h.dataStorage[1] == 50);  // still held
    b.seconds(4);
    CHECK(h.dataStorage[1] == 0);   // 5 s after: as before
}

TEST_CASE("beastmaster: species are known by animation graph and faction") {
    BeastRig b;
    CHECK(b.reply(std::string("beastmaster.species ") + kWolfGraph + " 6") == "wolf");   // WildBeast
    CHECK(b.reply(std::string("beastmaster.species ") + kWolfGraph + " 4") == "hound");  // Scavenger
    CHECK(b.reply(std::string("beastmaster.species ") + kWolfGraph + " 3") == "unknown");
    CHECK(b.reply("beastmaster.species aed0ecec-8a98-4fd3-a450-ab4c508b0ad4 3") == "boar");  // also the Vukah's boars
    CHECK(b.reply("beastmaster.species 9c1b120e-4901-495c-9cc0-5de134fbe140 6") == "sabretooth");
    CHECK(b.reply("beastmaster.species 00000000-0000-0000-0000-000000000001 6") == "unknown");
    CHECK(b.reply("beastmaster.give dragon").find("no species dragon") != std::string::npos);
}

TEST_CASE("wilds: the wilds pack's fighting animals are beasts; a creature on a game graph at one's size is no game kind") {
    BeastRig b;
    // a fox runs the game wolf's graph at 0.65, the coloured wolves at 1.1, the panther the sabretooth's at 0.8: which
    // of them it is only replace.lua's list knows, so by graph and size they are nothing, never a wolf or a sabretooth
    CHECK(b.reply(std::string("beastmaster.species ") + kWolfGraph + " 6 0.65") == "unknown");
    CHECK(b.reply(std::string("beastmaster.species ") + kWolfGraph + " 6 1.1") == "unknown");
    CHECK(b.reply("beastmaster.species 9c1b120e-4901-495c-9cc0-5de134fbe140 6 0.8") == "unknown");
    CHECK(b.reply(std::string("beastmaster.species ") + kWolfGraph + " 6") == "wolf");        // the game's: no scale
    CHECK(b.reply(std::string("beastmaster.species ") + kWolfGraph + " 6 1.4") == "bear");    // the bear pack's
    CHECK(b.reply("beastmaster.species 9c1b120e-4901-495c-9cc0-5de134fbe140 6") == "sabretooth");
    CHECK(b.reply("beastmaster.give fox").find("added") != std::string::npos);
    CHECK(b.reply("beastmaster.give snow_leopard").find("added") != std::string::npos);
    const std::string list = b.reply("beastmaster.list");
    CHECK(list.find("1. Fox - fox, loyalty 1, at home") != std::string::npos);
    CHECK(list.find("2. Snow leopard - snow_leopard, loyalty 1, at home") != std::string::npos);
}

TEST_CASE("beastmaster: a wolf at 20 % health in front of you is tamed after a 3 s channel") {
    BeastRig b;
    b.look(1, 0, 10.5, 0, 0);
    b.beast(50, kWolfGraph, 0, 5, kWildBeast, 20, 100);
    CHECK(b.reply("beastmaster.tame").find("Taming") != std::string::npos);
    // it calms down during the channel: an ally that comes to you (it stops fleeing, and your beasts leave it alone)
    CHECK(b.faction(50) == kPlayerAlly);
    const auto calm = b.guards();
    REQUIRE(calm.size() == 1);
    CHECK(calm[0] == std::make_pair(50u, 1u));
    CHECK(b.reply("beastmaster.list").find("no beasts") != std::string::npos);  // not yours before the channel ends
    b.seconds(4);
    CHECK(b.faction(50) == kPlayerAlly);
    for (const auto& g : b.guards()) CHECK(g == std::make_pair(50u, 1u));
    CHECK(b.added(50, EMF_ATTR_HEALTH, 30));  // up to 50 %
    CHECK(b.noted("You tamed a wolf: Wolf"));
    CHECK(b.reply("beastmaster.list").find("1. Wolf - wolf, loyalty 1, out") != std::string::npos);
}

TEST_CASE("beastmaster: a rat of houses and ruins is a beast too: known by its graph, its own bait, tamed like a wolf (GitHub #47)") {
    BeastRig b;
    const char* rat_graph = "57a2f080-1f1e-4d18-8170-2d80b756fe56";  // Enemy_Wildbeast_Rat_hook and its Coldheights kin
    CHECK(b.reply("beastmaster.list").find(", rat,") != std::string::npos);  // "no beasts yet" names it among the kinds
    CHECK(b.reply(std::string("beastmaster.species ") + rat_graph + " 6") == "rat");
    CHECK(b.r.fake.settings["beastmaster.bait_rat"] == "Food_T1_raw_fruit_Red_Berries");  // a rat takes berries
    b.look(1, 0, 10.5, 0, 0);
    b.beast(50, rat_graph, 0, 5, kWildBeast, 20, 100);
    CHECK(b.reply("beastmaster.tame").find("Taming") != std::string::npos);
    b.seconds(4);
    CHECK(b.noted("You tamed a rat: Rat"));
    CHECK(b.reply("beastmaster.list").find("1. Rat - rat, loyalty 1, out") != std::string::npos);
    CHECK(b.reply("beastmaster.give rat").find("added") != std::string::npos);
}

TEST_CASE("beastmaster: with nothing under the crosshair, taming takes another beast before a rat; no beast near names every kind") {
    BeastRig b;
    const std::string none = b.reply("beastmaster.tame");
    CHECK(none.find("wolf, boar, sabretooth, hound, rat, bear, aurochs or snake") != std::string::npos);
    const char* rat_graph = "57a2f080-1f1e-4d18-8170-2d80b756fe56";
    b.look(1, 0, 10.5, 0, 180);  // looking away from both: the nearest one counts
    b.beast(50, rat_graph, 0, 3, kWildBeast, 20, 100);  // rats are many in houses and ruins
    b.beast(51, kWolfGraph, 0, 6, kWildBeast, 20, 100);
    CHECK(b.reply("beastmaster.tame").find("Taming") != std::string::npos);
    b.seconds(4);
    CHECK(b.noted("You tamed a wolf"));
}

TEST_CASE("pack beast: the pack llama is a kind of its own: given, never wild, never tamed, not among the kinds to tame (GitHub #48)") {
    BeastRig b;
    CHECK(b.reply("beastmaster.give packllama").find("added Pack llama (packllama)") != std::string::npos);
    CHECK(b.reply("beastmaster.spawn packllama").find("never wild") != std::string::npos);
    CHECK(b.reply("beastmaster.tame").find("wolf, boar, sabretooth, hound, rat, bear, aurochs or snake") != std::string::npos);
    // its creature (the game's cat-deer companion's graph) is never a beast to tame
    CHECK(b.reply("beastmaster.species 73702659-0000-0000-0000-000000000000 11") == "unknown");
}

TEST_CASE("pack beast: the pack llama has a slot of its own: it comes with every active slot taken, one per player, outside the collection's count") {
    BeastRig b({{"call_cooldown_s", "0"}, {"call_mana_pct", "0"}});
    b.spawnable(8);
    for (int i = 0; i < 4; ++i) b.reply("beastmaster.give wolf");
    CHECK(b.reply("beastmaster.give packllama").find("added") != std::string::npos);
    CHECK(b.reply("beastmaster.give packllama").find("already") != std::string::npos);  // one per player
    // every active slot taken by wolves
    int out = 0;
    for (const char* name : {"Wolf", "Wolf 2", "Wolf 3", "Wolf 4"})
        if (b.reply(std::string("beastmaster.call ") + name).find("comes") != std::string::npos) ++out;
    REQUIRE(out >= 1);
    CHECK(b.reply("beastmaster.call Wolf 4").find("dismiss one first") != std::string::npos);
    CHECK(b.reply("beastmaster.call Pack llama").find("comes") != std::string::npos);  // its own slot
    CHECK(b.reply("beastmaster.call Wolf 4").find("dismiss one first") != std::string::npos);  // it took none of theirs
    const std::string list = b.reply("beastmaster.list");
    CHECK(list.find("Pack llama - packllama") != std::string::npos);
}

TEST_CASE("beastmaster: the channel breaks when you walk away (more than 8 m) or the beast dies") {
    BeastRig b;
    b.look(1, 0, 10.5, 0, 0);
    b.beast(50, kWolfGraph, 0, 5, kWildBeast, 20, 100);
    b.reply("beastmaster.tame");
    CHECK(b.faction(50) == kPlayerAlly);  // calm during the channel
    place(b.r.fake, 1, 0, 10, -10);  // 15 m away now
    b.seconds(4);
    CHECK(b.faction(50) == kWildBeast);  // wild again
    CHECK(b.noted("too far"));
    place(b.r.fake, 1, 0, 10, 0);
    b.reply("beastmaster.tame");
    deliver(b.r.fake, "EntityDiedEvent", died_event(50));
    b.r.fake.attrs[{50, EMF_ATTR_HEALTH}] = 0;
    b.seconds(4);
    CHECK(b.faction(50) == kWildBeast);
    CHECK(b.noted("died"));
    CHECK(b.reply("beastmaster.list").find("no beasts") != std::string::npos);
}

TEST_CASE("beastmaster: each beast's bait suits it and when you meet it: lean meat from level 1, the game's animal baits later") {
    BeastRig b;
    const std::map<std::string, std::string> want = {
        {"wolf", "Food_T1_raw_bad_meat_LeanMeat"},  // wolves are there from level 1: goats, hares and rats drop it
        {"boar", "Food_T1_raw_bad_meat_LeanMeat"},
        {"snake", "Food_T1_raw_bad_meat_LeanMeat"},
        {"hound", "Food_T2_Dog_Bait"},
        {"sabretooth", "Food_T2_Cat_Bait"},
        {"bear", "Food_T1_raw_fruit_Red_Berries"},
        {"aurochs", "Food_T1_Goat_Bait"},
        {"crimson_snake", "Food_T2_raw_bad_meat_Game"},
        {"golden_snake", "Food_T3_Turkey_Bait"},
        {"albino_snake", "Food_T5_Yak_Bait"},  // the rarest takes the hardest bait
    };
    for (const auto& [kind, item] : want) CHECK_MESSAGE(b.r.fake.settings["beastmaster.bait_" + kind] == item, kind);
}

TEST_CASE("beastmaster: a healthy beast is not tamed by defeat; with 3 baits it is tamed by feeding") {
    BeastRig b;
    b.r.fake.item_names[kLeanMeat] = "Food_T1_raw_bad_meat_LeanMeat";
    b.look(1, 0, 10.5, 0, 0);
    b.beast(50, kWolfGraph, 0, 5, kWildBeast, 100, 100);
    CHECK(b.reply("beastmaster.tame").find("too strong") != std::string::npos);
    for (int i = 0; i < 3; ++i) {
        b.reply("beastmaster.feed");
        REQUIRE(b.r.fake.takes.size() == static_cast<size_t>(i + 1));
        const EmfItemsRequest t = b.r.fake.takes.back();
        CHECK(t.item == kLeanMeat);
        CHECK(t.count == 1u);
        CHECK(t.entity == 1u);
        t.cb(kServer, t.entity, t.item, t.count, 1, t.user);  // one bait was taken
        b.seconds(1);
    }
    CHECK(b.faction(50) == kPlayerAlly);
    CHECK(b.noted("You tamed a wolf"));
}

TEST_CASE("beastmaster: a beast that is already someone's (Faction 11) or of an unknown species is refused") {
    BeastRig b;
    b.look(1, 0, 10.5, 0, 0);
    b.beast(50, kWolfGraph, 0, 5, kPlayerAlly, 10, 100);
    CHECK(b.reply("beastmaster.tame").find("no beast to tame") != std::string::npos);
    b.beast(50, "00000000-0000-0000-0000-000000000099", 0, 5, kWildBeast, 10, 100);
    CHECK(b.reply("beastmaster.tame").find("no beast to tame") != std::string::npos);
    b.seconds(4);
    CHECK(b.faction(50) == kWildBeast);
}

namespace {
std::string guid_text(const uint8_t g[16]) {
    char b[40];
    std::snprintf(b, sizeof b, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", g[3], g[2], g[1],
                  g[0], g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    return b;
}
constexpr const char* kWolfTemplate = "478381c5-585f-4264-b373-dbb1ea596eca";
bool contains(const std::vector<uint32_t>& v, uint32_t x) { return std::find(v.begin(), v.end(), x) != v.end(); }
}  // namespace

TEST_CASE("beastmaster: calling a wolf costs 15 % mana, spawns it beside you as your ally; a second wolf can come too") {
    BeastRig b;
    b.reply("beastmaster.give wolf");
    b.reply("beastmaster.give wolf");
    b.spawnable(2);
    CHECK(b.reply("beastmaster.call 1").find("comes") != std::string::npos);
    REQUIRE(b.r.fake.spawns.size() == 1);
    CHECK(guid_text(b.r.fake.spawns[0].guid) == kWolfTemplate);
    CHECK(std::abs(b.r.fake.spawns[0].x - 0.0) + std::abs(b.r.fake.spawns[0].z - 0.0) <= 3.0);  // beside you
    CHECK(b.added(1, EMF_ATTR_MANA, -15));
    CHECK(b.faction(1234) == kPlayerAlly);
    CHECK(b.r.fake.components[{ecs::Level::kHash, 1234}][8] == 10);  // the owner's level (dataStorage[0])
    const auto g = b.guards();
    CHECK(std::find(g.begin(), g.end(), std::make_pair(1234u, 1u)) != g.end());
    CHECK(b.reply("beastmaster.list").find("1. Wolf - wolf, loyalty 1, out") != std::string::npos);
    CHECK(b.reply("beastmaster.call Wolf").find("out already") != std::string::npos);
    b.seconds(11);
    CHECK(b.reply("beastmaster.call Wolf 2").find("comes") != std::string::npos);  // two slots: two wolves
    CHECK(b.r.fake.spawns.size() == 2);
}

TEST_CASE("beastmaster: at most 2 out; a 10 s cooldown; too little mana gives a note and nothing else") {
    BeastRig b;
    for (const char* s : {"wolf", "boar", "hound"}) b.reply(std::string("beastmaster.give ") + s);
    b.spawnable(3);
    b.reply("beastmaster.call Wolf");
    CHECK(b.reply("beastmaster.call Boar").find("wait") != std::string::npos);  // the cooldown
    b.seconds(11);
    CHECK(b.reply("beastmaster.call Boar").find("comes") != std::string::npos);
    b.seconds(11);
    CHECK(b.reply("beastmaster.call Hound").find("2 beasts out") != std::string::npos);
    CHECK(b.r.fake.spawns.size() == 2);
    b.reply("beastmaster.dismiss Boar");
    CHECK(contains(b.r.fake.destroyed, 1235));
    b.r.fake.attrs[{1, EMF_ATTR_MANA}] = 5;
    CHECK(b.reply("beastmaster.call Hound").find("Not enough mana") != std::string::npos);
    CHECK(b.r.fake.spawns.size() == 2);
}

TEST_CASE("beastmaster: a beast that dies rests 60 s, its body is removed, and it can be called again after") {
    BeastRig b({{"back_after_rest", "false"}});
    b.reply("beastmaster.give wolf");
    b.spawnable(2);
    b.reply("beastmaster.call Wolf");
    deliver(b.r.fake, "EntityDiedEvent", died_event(1234));
    b.r.fake.attrs[{1234, EMF_ATTR_HEALTH}] = 0;
    b.seconds(2);
    CHECK(contains(b.r.fake.destroyed, 1234));  // no loot from our own beasts
    CHECK(b.reply("beastmaster.list").find("rests 0:5") != std::string::npos);
    CHECK(b.reply("beastmaster.call Wolf").find("rests") != std::string::npos);
    b.seconds(60);
    CHECK(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    CHECK(b.r.fake.spawns.size() == 2);
}

TEST_CASE("beastmaster: after a reload the beasts that were out come back once (no mana), in the same world only") {
    BeastRig b;
    write(b.data, "b 0x00000000000000aa wolf 0 1 Fang\n");
    b.spawnable(2);
    b.r.fake.session = 2;
    fire_load(1);
    b.seconds(2);
    REQUIRE(b.r.fake.spawns.size() == 1);
    CHECK(guid_text(b.r.fake.spawns[0].guid) == kWolfTemplate);
    CHECK_FALSE(b.added(1, EMF_ATTR_MANA, -15));
    b.seconds(5);
    CHECK(b.r.fake.spawns.size() == 1);  // once
    CHECK(b.faction(1234) == kPlayerAlly);
}

// A player's report: the wolf fell and stayed home until called again; "let him come back by himself after the rest
// until dismissed".
TEST_CASE("beastmaster: a fallen beast comes back by itself after its rest, free, until dismissed; with the setting off it waits at home") {
    BeastRig b;
    b.reply("beastmaster.give wolf");
    b.spawnable(3);
    REQUIRE(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    deliver(b.r.fake, "EntityDiedEvent", died_event(1234));
    b.r.fake.attrs[{1234, EMF_ATTR_HEALTH}] = 0;
    b.seconds(2);
    CHECK(contains(b.r.fake.destroyed, 1234));  // its body
    CHECK(b.noted("Wolf fell; it comes back in 0:5"));
    const std::string list = b.reply("beastmaster.list");
    CHECK(list.find("Wolf - wolf, loyalty 1, rests 0:5") != std::string::npos);
    CHECK(list.find("then comes back") != std::string::npos);
    CHECK(b.hud("out").find("Wolf (rests 0:5") != std::string::npos);
    CHECK(b.reply("beastmaster.call Wolf").find("comes back by itself") != std::string::npos);
    b.seconds(55);
    CHECK(b.r.fake.spawns.size() == 1);
    b.seconds(5);
    REQUIRE(b.r.fake.spawns.size() == 2);  // back beside its owner at (0, 10, 0)
    CHECK(guid_text(b.r.fake.spawns[1].guid) == kWolfTemplate);
    CHECK(std::abs(b.r.fake.spawns[1].x - 2.0) <= 1.0);
    CHECK(b.noted("Wolf is back"));
    size_t paid = 0;
    for (const auto& [e, a, d] : b.r.fake.attr_adds) paid += e == 1 && a == EMF_ATTR_MANA && d == -15;
    CHECK(paid == 1);  // free: only the call cost mana
    CHECK(b.reply("beastmaster.list").find("Wolf - wolf, loyalty 1, out") != std::string::npos);
    // it falls again and is sent home while it rests: it stays there
    deliver(b.r.fake, "EntityDiedEvent", died_event(1235));
    b.r.fake.attrs[{1235, EMF_ATTR_HEALTH}] = 0;
    b.seconds(2);
    CHECK(b.reply("beastmaster.dismiss Wolf").find("goes home") != std::string::npos);
    b.seconds(70);
    CHECK(b.r.fake.spawns.size() == 2);
    CHECK(b.reply("beastmaster.list").find("Wolf - wolf, loyalty 1, at home") != std::string::npos);
    // the setting off: a fallen beast waits at home after its rest
    b.r.fake.settings["beastmaster.back_after_rest"] = "false";
    REQUIRE(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    deliver(b.r.fake, "EntityDiedEvent", died_event(1236));
    b.r.fake.attrs[{1236, EMF_ATTR_HEALTH}] = 0;
    b.seconds(2);
    CHECK(b.noted("Wolf fell; it rests for 0:5"));
    b.seconds(70);
    CHECK(b.r.fake.spawns.size() == 3);
}

// Seen live: the game's EntityDiedEvent comes a step or two after the health reads 0; a check of the beasts in between
// took the dead wolf for gone (sent home, no rest), so it did not come back after its rest.
TEST_CASE("beastmaster: a beast whose health reads 0 before the game's death event has fallen: it rests and comes back; the late event changes nothing") {
    BeastRig b({{"rest_s", "10"}});
    auto notes_with = [&](const std::string& needle) {
        size_t n = 0;
        for (const auto& x : b.r.fake.notes) n += x.find(needle) != std::string::npos;
        return n;
    };
    b.reply("beastmaster.give wolf");
    b.spawnable(2);
    REQUIRE(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    b.r.fake.attrs[{1234, EMF_ATTR_HEALTH}] = 0;  // dead, the event not here yet
    b.seconds(2);
    CHECK(notes_with("Wolf fell; it comes back in") == 1);
    CHECK(b.reply("beastmaster.list").find("then comes back") != std::string::npos);
    deliver(b.r.fake, "EntityDiedEvent", died_event(1234));  // the game's event, late
    b.seconds(2);
    CHECK(notes_with("Wolf fell") == 1);  // once
    CHECK(contains(b.r.fake.destroyed, 1234));      // its body
    b.seconds(8);
    REQUIRE(b.r.fake.spawns.size() == 2);  // back after its 10 s rest, not later
    CHECK(b.noted("Wolf is back"));
}

TEST_CASE("beastmaster: a dead creature of another side on a beast's id is no fall (no rest, no body removed); with no rest the fall is told before the beast is back") {
    {
        BeastRig b;
        b.reply("beastmaster.give wolf");
        b.spawnable(2);
        REQUIRE(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
        component<ecs::Faction>(b.r.fake, 1234).faction = static_cast<emf::gen::FactionType>(6);  // another creature's now
        b.r.fake.attrs[{1234, EMF_ATTR_HEALTH}] = 0;
        b.seconds(3);
        CHECK(!b.noted("Wolf fell"));
        CHECK(!contains(b.r.fake.destroyed, 1234));  // not ours: its loot stays
        CHECK(b.reply("beastmaster.list").find("Wolf - wolf, loyalty 1, at home") != std::string::npos);
    }
    {
        BeastRig b({{"rest_s", "0"}});
        b.reply("beastmaster.give wolf");
        b.spawnable(2);
        REQUIRE(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
        b.r.fake.attrs[{1234, EMF_ATTR_HEALTH}] = 0;  // dead, the game's event not here yet
        b.seconds(3);
        size_t fell = 0, back = 0;
        for (size_t i = 0; i < b.r.fake.notes.size(); ++i) {
            if (b.r.fake.notes[i].find("Wolf fell") != std::string::npos && !fell) fell = i + 1;
            if (b.r.fake.notes[i].find("Wolf is back") != std::string::npos && !back) back = i + 1;
        }
        REQUIRE(fell > 0);
        REQUIRE(back > 0);
        CHECK(fell < back);
    }
    {
        // a save with a beast resting and coming back: after the load its owner hears when it is back
        BeastRig c;
        write(c.data, "b 0x00000000000000aa wolf 20 1 Fang\n");
        c.spawnable(1);
        c.r.fake.session = 2;
        fire_load(1);
        c.seconds(25);
        CHECK(c.r.fake.spawns.size() == 1);
        CHECK(c.noted("Fang is back"));
    }
}

TEST_CASE("beastmaster: a resting beast that comes back holds its slot, comes back after a reload, and stays home when the setting goes off meanwhile") {
    {
        BeastRig b;
        b.reply("beastmaster.give wolf");
        b.reply("beastmaster.give boar");
        b.reply("beastmaster.give hound");
        b.spawnable(3);
        REQUIRE(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
        b.seconds(11);
        REQUIRE(b.reply("beastmaster.call Boar").find("comes") != std::string::npos);  // level 10: two active slots
        deliver(b.r.fake, "EntityDiedEvent", died_event(1234));
        b.r.fake.attrs[{1234, EMF_ATTR_HEALTH}] = 0;
        b.seconds(11);
        const std::string full = b.reply("beastmaster.call Hound");
        CHECK(full.find("dismiss one first") != std::string::npos);
        CHECK(full.find("comes back by itself") != std::string::npos);  // says why the slot is taken
        CHECK(b.reply("beastmaster.dismiss Wolf").find("goes home") != std::string::npos);
        CHECK(b.reply("beastmaster.call Hound").find("comes") != std::string::npos);
    }
    {
        // a save with a beast that rests and comes back (out 1, rest 30): after a load it comes after the rest
        BeastRig c;
        write(c.data, "b 0x00000000000000aa wolf 30 1 Fang\n");
        c.spawnable(2);
        c.r.fake.session = 2;
        fire_load(1);
        c.seconds(5);
        CHECK(c.r.fake.spawns.empty());
        c.seconds(30);
        CHECK(c.r.fake.spawns.size() == 1);
    }
    {
        // the setting switched off while a beast rests: it stays home
        BeastRig d;
        d.reply("beastmaster.give wolf");
        d.spawnable(2);
        REQUIRE(d.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
        deliver(d.r.fake, "EntityDiedEvent", died_event(1234));
        d.r.fake.attrs[{1234, EMF_ATTR_HEALTH}] = 0;
        d.seconds(5);
        d.r.fake.settings["beastmaster.back_after_rest"] = "false";
        d.seconds(70);
        CHECK(d.r.fake.spawns.size() == 1);
        CHECK(d.reply("beastmaster.list").find("Wolf - wolf, loyalty 1, at home") != std::string::npos);
    }
}

TEST_CASE("beastmaster: a remembered id that now names another entity is dropped, never written") {
    BeastRig b;
    b.reply("beastmaster.give wolf");
    b.spawnable(1);
    REQUIRE(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    REQUIRE(b.faction(1234) == kPlayerAlly);
    // the wolf is gone without a death event and its id now names a scavenger
    component<ecs::Faction>(b.r.fake, 1234).faction = static_cast<emf::gen::FactionType>(4);
    b.seconds(2);
    CHECK(b.faction(1234) == 4);  // never written back to ours
    CHECK_FALSE(contains(b.r.fake.destroyed, 1234));
    CHECK(b.reply("beastmaster.list").find("1. Wolf - wolf, loyalty 1, at home") != std::string::npos);
}

TEST_CASE("beastmaster: a guest who leaves loses their beasts from the world but keeps them in the roster") {
    BeastRig b;
    write(b.data, "b 0x00000000000000bb wolf 0 1 Grey\n");
    b.spawnable(2);
    b.r.fake.session = 2;
    fire_load(1);
    b.seconds(2);
    REQUIRE(b.r.fake.spawns.size() == 1);
    remove_entity(b.r.fake, 2);  // the guest left
    b.seconds(2);
    CHECK(contains(b.r.fake.destroyed, 1234));
    CHECK(b.saved().find("b 0x00000000000000bb wolf 0 1 Grey") != std::string::npos);
    add_player(b.r.fake, 2, 0xBB, 30, 10, 0);  // back again
    add_entity(b.r.fake, 2);
    b.seconds(2);
    CHECK(b.r.fake.spawns.size() == 2);
}

namespace {
size_t teleports_of(const Fake& f, uint32_t entity) {
    size_t n = 0;
    for (const auto& t : f.teleports)
        if (t.entity == entity) ++n;
    return n;
}
// Attack's order lives in the beast's own pick: SelectedTargets slot 0 and DynamicAggro's target switches (a fake beast
// has neither until armed()).
void armed(BeastRig& b, uint32_t id) {
    component<ecs::SelectedTargets>(b.r.fake, id);
    component<ecs::DynamicAggro>(b.r.fake, id).allowTargetSwitches = true;
}
uint32_t ordered(BeastRig& b, uint32_t id) {
    const auto& v = b.r.fake.components.at({ecs::SelectedTargets::kHash, id});
    return reinterpret_cast<const ecs::SelectedTargets*>(v.data())->targets[0].targetEntityId.id;
}
void unpick(BeastRig& b, uint32_t id) {  // the game picks something else for it
    auto& v = b.r.fake.components.at({ecs::SelectedTargets::kHash, id});
    reinterpret_cast<ecs::SelectedTargets*>(v.data())->targets[0].targetEntityId.id = 0;
}
bool held(BeastRig& b, uint32_t id) {  // its switches off: the game does not pick for it
    const auto& v = b.r.fake.components.at({ecs::DynamicAggro::kHash, id});
    return !reinterpret_cast<const ecs::DynamicAggro*>(v.data())->allowTargetSwitches;
}
// A wolf called out (entity 1234) for player 1.
void wolf_out(BeastRig& b) {
    b.reply("beastmaster.give wolf");
    b.spawnable(1);
    REQUIRE(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
}
}  // namespace

TEST_CASE("beastmaster: a beast 70 m away for 3 s, or 130 m away at once, is teleported beside its owner") {
    BeastRig b;
    wolf_out(b);
    place(b.r.fake, 1234, 50, 10, 0);  // within 60 m: never
    b.seconds(4);
    CHECK(teleports_of(b.r.fake, 1234) == 0);
    place(b.r.fake, 1234, 70, 10, 0);
    b.seconds(1);
    CHECK(teleports_of(b.r.fake, 1234) == 0);  // not yet: 3 s
    b.seconds(3);
    REQUIRE(teleports_of(b.r.fake, 1234) >= 1);
    const EmfTeleport& t = b.r.fake.teleports.back();
    CHECK(std::abs(t.x - 0.0) + std::abs(t.z - 0.0) <= 3.0);  // beside its owner at (0, 10, 0)
    CHECK((t.flags & EMF_TELEPORT_FIND_GROUND) != 0);
    place(b.r.fake, 1234, 2, 10, 0);  // it arrived
    b.seconds(4);
    const size_t before = b.r.fake.teleports.size();
    place(b.r.fake, 1234, 130, 10, 0);  // far at once
    b.seconds(1);
    CHECK(b.r.fake.teleports.size() == before + 1);
}

// Seen live: "Wolf attacks (2)", but the beasts took a while: the game's AddThreatTargetEvent only raises a player's threat
// (research enemy_targets_2026-11-12), so they went only once their own senses found the creature.
TEST_CASE("beastmaster: attack writes the creature you look at as every beast's pick at once, each step for 20 s or until it dies") {
    BeastRig b;
    wolf_out(b);
    armed(b, 1234);
    b.look(1, 0, 10.5, 0, 0);
    b.beast(60, "00000000-0000-0000-0000-000000000099", 0, 6, 4 /* Scavenger */, 100, 100);
    CHECK(b.reply("beastmaster.attack") == "Your beasts attack (1).");
    CHECK(ordered(b, 1234) == 60u);  // at once
    CHECK(held(b, 1234));
    unpick(b, 1234);
    b.seconds(1);
    CHECK(ordered(b, 1234) == 60u);  // written again
    b.seconds(20);
    CHECK_FALSE(held(b, 1234));      // 20 s on: the game picks again
    unpick(b, 1234);
    b.seconds(1);
    CHECK(ordered(b, 1234) == 0u);

    REQUIRE(b.reply("beastmaster.attack") == "Your beasts attack (1).");
    b.r.fake.attrs[{60, EMF_ATTR_HEALTH}] = 0;  // it died
    b.seconds(1);
    CHECK_FALSE(held(b, 1234));
}

TEST_CASE("pack beast: the pack llama never fights: Attack orders only the others, and a fight the game gives it is taken away at once") {
    BeastRig b({{"call_cooldown_s", "0"}, {"call_mana_pct", "0"}});
    wolf_out(b);  // 1234
    armed(b, 1234);
    b.reply("beastmaster.give packllama");
    b.spawnable(1);
    REQUIRE(b.reply("beastmaster.call Pack llama").find("comes") != std::string::npos);  // 1235
    armed(b, 1235);
    b.look(1, 0, 10.5, 0, 0);
    b.beast(60, "00000000-0000-0000-0000-000000000099", 0, 6, 4 /* Scavenger */, 100, 100);
    CHECK(b.reply("beastmaster.attack") == "Your beasts attack (1).");
    CHECK(ordered(b, 1234) == 60u);
    CHECK(ordered(b, 1235) == 0u);
    // the game's own senses pick the monster for it: the next step takes it away
    reinterpret_cast<ecs::SelectedTargets*>(b.r.fake.components.at({ecs::SelectedTargets::kHash, 1235}).data())
        ->targets[0].targetEntityId.id = 60;
    b.seconds(1);
    CHECK(ordered(b, 1235) == 0u);
}

TEST_CASE("beastmaster: attack with nothing under the crosshair goes for the nearest monster where you look, never an animal there or a monster behind") {
    BeastRig b;
    wolf_out(b);
    armed(b, 1234);
    b.look(1, 0, 10.5, 0, 0);  // along +z
    b.beast(61, "00000000-0000-0000-0000-000000000099", 5, 20, 4 /* Scavenger */, 100, 100);  // ahead, 14 degrees aside
    b.beast(62, "00000000-0000-0000-0000-000000000099", 0, -10, 4, 100, 100);                // behind, nearer
    b.beast(63, "00000000-0000-0000-0000-000000000099", 2, 8, 5 /* Wildlife */, 100, 100);   // an animal ahead, nearer
    b.beast(64, "00000000-0000-0000-0000-000000000099", 30, 12, 4, 100, 100);                // a monster far aside
    CHECK(b.reply("beastmaster.attack") == "Your beasts attack (1).");
    CHECK(ordered(b, 1234) == 61u);
    b.look(1, 0, 10.5, 0, 90);  // along +x: only 64 there
    CHECK(b.reply("beastmaster.attack") == "Your beasts attack (1).");
    CHECK(ordered(b, 1234) == 64u);
    b.look(1, 0, 10.5, 0, 0);
    b.r.fake.attrs[{61, EMF_ATTR_HEALTH}] = 0;  // gone: the animal is not taken instead
    CHECK(b.reply("beastmaster.attack") == "No monster where you look (within 40 m): look at the creature to attack.");
}

// Review: an order now really forces the attack, so the crosshair must not send beasts at a player's side.
TEST_CASE("beastmaster: attack never sends beasts at a farm animal, an automated player or an ally under the crosshair") {
    BeastRig b;
    wolf_out(b);
    armed(b, 1234);
    b.look(1, 0, 10.5, 0, 0);
    for (int faction : {12 /* FarmAnimal */, 8 /* AutomatedPlayer */, 11 /* PlayerAlly */}) {
        b.beast(65, "00000000-0000-0000-0000-000000000099", 0, 6, faction, 100, 100);
        CAPTURE(faction);
        CHECK(b.reply("beastmaster.attack").find("No monster where you look") == 0);
        CHECK(ordered(b, 1234) == 0u);
    }
}

// Review: a target that turns friendly (taming starts on it: faction 11) is no longer fought.
TEST_CASE("beastmaster: an Attack order ends when its target turns friendly") {
    BeastRig b;
    wolf_out(b);
    armed(b, 1234);
    b.look(1, 0, 10.5, 0, 0);
    b.beast(60, "00000000-0000-0000-0000-000000000099", 0, 6, 4 /* Scavenger */, 100, 100);
    REQUIRE(b.reply("beastmaster.attack") == "Your beasts attack (1).");
    REQUIRE(held(b, 1234));
    component<ecs::Faction>(b.r.fake, 60).faction = static_cast<emf::gen::FactionType>(11);
    b.seconds(1);
    CHECK_FALSE(held(b, 1234));
    unpick(b, 1234);
    b.seconds(1);
    CHECK(ordered(b, 1234) == 0u);
}

// Review: aggressive mode called Attack's order on the nearest foe every second, over the player's own Attack.
TEST_CASE("beastmaster: aggressive mode keeps a live Attack order (it hunts only beasts without one); a mode change ends orders") {
    BeastRig b;
    wolf_out(b);
    armed(b, 1234);
    b.reply("beastmaster.mode aggressive");
    b.beast(70, "00000000-0000-0000-0000-000000000099", 0, 20, 4 /* Scavenger */, 100, 100);  // looked at
    b.beast(71, "00000000-0000-0000-0000-000000000099", 8, 4, 4, 100, 100);                    // nearer the owner
    b.look(1, 0, 10.5, 0, 0);
    REQUIRE(b.reply("beastmaster.attack") == "Your beasts attack (1).");
    b.seconds(3);
    CHECK(ordered(b, 1234) == 70u);  // not the nearest
    b.reply("beastmaster.mode passive");
    CHECK_FALSE(held(b, 1234));      // the order ended with the mode
}

TEST_CASE("beastmaster: our beast's hit that leaves a foe at 20 % hands the killing blow to its owner, once") {
    BeastRig b;
    wolf_out(b);
    b.beast(60, "00000000-0000-0000-0000-000000000099", 0, 6, 4, 20, 100);
    deliver(b.r.fake, "HitEvent", hit_event(1234, 60));
    deliver(b.r.fake, "HitEvent", hit_event(1234, 60));
    b.seconds(1);
    REQUIRE(b.r.fake.damages.size() == 1);
    CHECK(b.r.fake.damages[0] == std::make_tuple(1u, 60u, 20));
}

TEST_CASE("beastmaster: no credit for hits on players, allies, or foes above 25 %") {
    BeastRig b;
    wolf_out(b);
    b.beast(61, "00000000-0000-0000-0000-000000000099", 0, 6, kPlayerAlly, 10, 100);  // an ally
    b.beast(62, "00000000-0000-0000-0000-000000000099", 0, 7, 4, 50, 100);            // a foe at 50 %
    b.r.fake.attrs[{2, EMF_ATTR_HEALTH}] = 5;
    b.r.fake.attrs[{2, EMF_ATTR_MAX_HEALTH}] = 100;
    for (uint32_t target : {2u, 61u, 62u}) deliver(b.r.fake, "HitEvent", hit_event(1234, target));
    deliver(b.r.fake, "HitEvent", hit_event(99, 62));  // not our beast
    b.seconds(1);
    CHECK(b.r.fake.damages.empty());
    b.reply("beastmaster.give boar");  // the setting switches it off too
    b.r.fake.settings["beastmaster.xp_credit"] = "false";
    b.r.fake.attrs[{62, EMF_ATTR_HEALTH}] = 10;
    deliver(b.r.fake, "HitEvent", hit_event(1234, 62));
    b.seconds(1);
    CHECK(b.r.fake.damages.empty());
}

namespace {
std::map<uint32_t, bool> g_held;  // the fake keyboard for these tests: vk -> held
// Presses `vk` once on the server world (this game hosts) and lets go.
void press(BeastRig& b, uint32_t vk) {
    b.host->set_key_reader([](uint32_t k) { return g_held[k]; });
    g_held[vk] = true;
    b.host->tick(kServer);
    g_held[vk] = false;
    b.host->tick(kServer);
}
int index_of(const std::vector<std::string>& items, const std::string& start) {
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].rfind(start, 0) == 0) return static_cast<int>(i);
    return -1;
}
}  // namespace

TEST_CASE("beastmaster: the wheel lists the actions and up to 6 beasts to call; picking Call Wolf calls it") {
    BeastRig b;
    for (const char* s : {"wolf", "boar", "hound", "sabretooth", "wolf", "boar", "hound"}) b.reply(std::string("beastmaster.give ") + s);
    b.spawnable(1);
    press(b, VK_F3);
    REQUIRE(b.r.fake.wheels.size() == 1);
    const auto& w = b.r.fake.wheels[0];
    CHECK(w.hold == VK_F3);
    for (const char* a : {"Tame", "Feed", "Attack"}) CHECK(index_of(w.items, a) >= 0);
    CHECK(index_of(w.items, "Dismiss") < 0);  // none out: nothing to dismiss
    int calls = 0;
    for (const auto& it : w.items) calls += it.rfind("Call ", 0) == 0;
    CHECK(calls == 6);  // 7 beasts at home: 6 on the wheel
    const int wolf = index_of(w.items, "Call Wolf");
    REQUIRE(wolf >= 0);
    w.cb(kClient, wolf, w.user);  // the wheel answers on the client world
    b.seconds(1);                 // the call runs on the server's next step
    REQUIRE(b.r.fake.spawns.size() == 1);
    CHECK(guid_text(b.r.fake.spawns[0].guid) == kWolfTemplate);
    CHECK(b.noted("Wolf the wolf comes"));
    CHECK(b.hud("out").find("Wolf (out)") != std::string::npos);
}

// ---- modes: passive, defending (the first), aggressive; one for all of a player's beasts ----
namespace {
// A creature at (x, 10, z): faction, alive (100 of 100).
void creature(BeastRig& b, uint32_t id, double x, double z, int faction) {
    place(b.r.fake, id, x, 10, z);
    component<ecs::Faction>(b.r.fake, id).faction = static_cast<emf::gen::FactionType>(faction);
    b.r.fake.attrs[{id, EMF_ATTR_HEALTH}] = 100;
    b.r.fake.attrs[{id, EMF_ATTR_MAX_HEALTH}] = 100;
    add_entity(b.r.fake, id);
}
constexpr int kVukahFoe = 3, kSkeletonFoe = 10, kWildlifeAnimal = 5, kFarmAnimal = 12;
}  // namespace

TEST_CASE("beastmaster: mode is defending at first; /beastmaster mode sets passive, defending or aggressive, kept with the world") {
    BeastRig b;
    CHECK(b.reply("beastmaster.mode").find("defending") != std::string::npos);
    CHECK(b.reply("beastmaster.mode aggressive").find("aggressive") != std::string::npos);
    CHECK(b.reply("beastmaster.mode").find("aggressive") != std::string::npos);
    CHECK(b.reply("beastmaster.mode sleepy").find("passive, defending or aggressive") != std::string::npos);
    CHECK(b.reply("beastmaster.mode").find("aggressive") != std::string::npos);  // unchanged
    CHECK(b.reply("beastmaster.mode Passive").find("passive") != std::string::npos);
    CHECK(b.reply("beastmaster.mode", 2, 0xBB).find("defending") != std::string::npos);  // each player their own
    b.seconds(2);
    CHECK(b.saved().find("m 0x00000000000000aa passive") != std::string::npos);
    // the same world loads again: the mode comes back with the beasts
    write(b.data, "m 0x00000000000000aa aggressive\nb 0x00000000000000aa wolf 0 0 Fang\n");
    b.r.fake.session = 2;
    fire_load(1);
    b.seconds(1);
    CHECK(b.reply("beastmaster.mode").find("aggressive") != std::string::npos);
    CHECK(b.reply("beastmaster.list").find("Fang") != std::string::npos);
}

TEST_CASE("beastmaster: the wheel's Mode item steps defending, aggressive, passive; the line at the bottom left names it") {
    BeastRig b;
    wolf_out(b);
    b.seconds(2);
    CHECK(b.hud("out").find("Beasts (defending): Wolf (out)") != std::string::npos);
    for (const char* next : {"aggressive", "passive", "defending"}) {
        CAPTURE(next);
        b.r.fake.wheels.clear();
        press(b, VK_F3);
        REQUIRE(b.r.fake.wheels.size() == 1);
        const auto w = b.r.fake.wheels[0];
        const int mode = index_of(w.items, "Mode: ");
        REQUIRE(mode >= 0);
        w.cb(kClient, mode, w.user);
        b.seconds(2);
        CHECK(b.noted(std::string("Your beasts are ") + next));
        CHECK(b.hud("out").find(std::string("Beasts (") + next + "): Wolf (out)") != std::string::npos);
    }
}

TEST_CASE("beastmaster: aggressive beasts go at the nearest hostile creature within 25 m of their owner") {
    BeastRig b;
    wolf_out(b);
    creature(b, 70, 0, 20, kSkeletonFoe);    // 20 m from the owner at (0, 10, 0)
    creature(b, 71, 0, 12, kVukahFoe);       // 12 m: the nearest foe
    creature(b, 72, 3, 3, kFarmAnimal);      // a farm animal: never
    creature(b, 73, -3, 3, kWildlifeAnimal); // wildlife (a goat, a hare): never
    creature(b, 74, 4, 0, 11);               // another ally: never
    creature(b, 75, 0, 40, kSkeletonFoe);    // beyond 25 m: not yet
    armed(b, 1234);
    b.seconds(2);
    CHECK(ordered(b, 1234) == 0u);           // defending: they go only at what attacks you or them
    b.reply("beastmaster.mode aggressive");
    b.seconds(2);
    CHECK(ordered(b, 1234) == 71u);
    b.r.fake.attrs[{71, EMF_ATTR_HEALTH}] = 0;  // dead: the next nearest
    b.seconds(3);
    CHECK(ordered(b, 1234) == 70u);
    b.reply("beastmaster.mode passive");
    b.seconds(21);                           // the last order runs out
    unpick(b, 1234);
    b.seconds(3);
    CHECK(ordered(b, 1234) == 0u);           // passive: none
}

TEST_CASE("beastmaster: aggressive beasts leave bosses and players alone") {
    BeastRig b;
    wolf_out(b);
    creature(b, 80, 0, 10, kSkeletonFoe);
    component<ecs::BossHealthBar>(b.r.fake, 80);  // a boss
    armed(b, 1234);
    b.reply("beastmaster.mode aggressive");
    b.seconds(3);
    CHECK(ordered(b, 1234) == 0u);
}

namespace {
// What the game's AI of `id` fights: an alarmed aggro target and a chosen target, both `foe`.
void fighting(BeastRig& b, uint32_t id, uint32_t foe) {
    auto& a = component<ecs::DynamicAggro>(b.r.fake, id);
    a.targets[0].state = ecs::AggroTargetState::Alarmed;
    a.targets[0].aggro = 50.0f;
    a.targets[0].entityId.id = foe;
    component<ecs::SelectedTargets>(b.r.fake, id).targets[0].targetEntityId.id = foe;
}
// A component as it is now (component() makes a fresh one).
template <class T>
const T& now_of(BeastRig& b, uint32_t id) {
    return *reinterpret_cast<const T*>(b.r.fake.components.at({T::kHash, id}).data());
}
bool still_fighting(BeastRig& b, uint32_t id) {
    const auto& a = now_of<ecs::DynamicAggro>(b, id);
    return a.targets[0].state != ecs::AggroTargetState::Unknown || a.targets[0].aggro != 0.0f ||
           now_of<ecs::SelectedTargets>(b, id).targets[0].targetEntityId.id != 0;
}
}  // namespace

TEST_CASE("beastmaster: passive beasts' aggro and chosen targets are cleared every step; defending ones are left to fight") {
    BeastRig b;
    wolf_out(b);
    creature(b, 70, 0, 10, kSkeletonFoe);
    fighting(b, 1234, 70);
    b.seconds(2);
    CHECK(still_fighting(b, 1234));  // defending: it fights
    b.reply("beastmaster.mode passive");
    b.seconds(1);
    CHECK_FALSE(still_fighting(b, 1234));
    fighting(b, 1234, 70);           // the game picks it again: cleared again
    b.seconds(1);
    CHECK_FALSE(still_fighting(b, 1234));
}

namespace {
// The game keeps `id` peaceful toward players and their allies (its PacifiedState: the game's own Beastmaster skill
// pacifies Vukah this way until the player attacks them).
void pacify(BeastRig& b, uint32_t id, bool on) {
    auto& p = component<ecs::PacifiedState>(b.r.fake, id);
    p.canBePacified = true;
    p.isPacified = on;
    p.pacifiedBy.id = on ? 1 : 0;
}
}  // namespace

// Spirimtin (2026-11-22): with the game's Beastmaster skill the Vukah leave the player alone until attacked, but the
// beasts went for them, and then the Vukah fought back.
TEST_CASE("beastmaster (1.20): aggressive beasts and Attack's cone leave a creature the game keeps pacified; its peace over, it is a foe again") {
    BeastRig b;
    wolf_out(b);
    creature(b, 71, 5, 20, kVukahFoe);  // 21 m from the owner, 14 degrees aside of +z: in the cone, not under the crosshair
    pacify(b, 71, true);
    armed(b, 1234);
    b.reply("beastmaster.mode aggressive");
    b.seconds(3);
    CHECK(ordered(b, 1234) == 0u);
    b.look(1, 0, 10.5, 0, 0);  // looking along +z: not taken from the cone either
    CHECK(b.reply("beastmaster.attack") == "No monster where you look (within 40 m): look at the creature to attack.");
    pacify(b, 71, false);  // the player hit it: the game ended its peace
    b.seconds(3);
    CHECK(ordered(b, 1234) == 71u);
}

TEST_CASE("beastmaster (1.20): a beast's own fight against a pacified creature is cleared every step; Attack on it is the player's call") {
    BeastRig b;
    wolf_out(b);
    creature(b, 71, 0, 3, kVukahFoe);
    pacify(b, 71, true);
    creature(b, 70, 3, 0, kSkeletonFoe);
    fighting(b, 1234, 71);  // its own senses picked the pacified Vukah
    b.seconds(1);
    CHECK_FALSE(still_fighting(b, 1234));
    fighting(b, 1234, 70);  // a foe that is not pacified: left to fight
    b.seconds(1);
    CHECK(still_fighting(b, 1234));
    armed(b, 1234);
    b.look(1, 0, 10.5, 0, 0);  // the player aims right at the pacified Vukah and sends the beasts
    CHECK(b.reply("beastmaster.attack") == "Your beasts attack (1).");
    fighting(b, 1234, 71);
    b.r.fake.tick += 1;  // one step, between the order's writes (once a second): its target is not cleared
    b.host->tick(kServer);
    CHECK(ordered(b, 1234) == 71u);
    CHECK(still_fighting(b, 1234));
}

// Review: a hunting order in aggressive mode is an order too; the beast's own pick of a pacified Vukah beside it must
// still be cleared, and a hunt whose target becomes pacified ends.
TEST_CASE("beastmaster (1.20): a hunting beast's own fight against a pacified creature is cleared; a hunt on one that becomes pacified ends") {
    BeastRig b;
    wolf_out(b);
    creature(b, 70, 0, 12, kSkeletonFoe);
    creature(b, 71, 3, 3, kVukahFoe);
    pacify(b, 71, true);
    armed(b, 1234);
    b.reply("beastmaster.mode aggressive");
    b.seconds(2);
    REQUIRE(ordered(b, 1234) == 70u);  // it hunts the skeleton
    auto& a = component<ecs::DynamicAggro>(b.r.fake, 1234);
    a.allowTargetSwitches = false;
    a.targets[1].state = ecs::AggroTargetState::Alarmed;  // its senses also took the pacified Vukah
    a.targets[1].aggro = 80.0f;
    a.targets[1].entityId.id = 71;
    b.r.fake.tick += 1;
    b.host->tick(kServer);
    const auto& now = now_of<ecs::DynamicAggro>(b, 1234);
    CHECK(now.targets[1].aggro == 0.0f);  // cleared
    CHECK(now.targets[1].state == ecs::AggroTargetState::Unknown);
    CHECK(ordered(b, 1234) == 70u);       // the hunt goes on
    pacify(b, 70, true);                  // its quarry is pacified too: the hunt ends at the next check
    b.seconds(1);
    CHECK_FALSE(held(b, 1234));
}

TEST_CASE("beastmaster (1.20): the pacified clearing leaves a defending beast's AI state alone when it cleared nothing") {
    BeastRig b;
    wolf_out(b);
    armed(b, 1234);
    b.seconds(1);
    component<ecs::DynamicEnemy01BehaviorState>(b.r.fake, 1234).state = emf::gen::enemy01::State::Combat;
    b.r.fake.tick += 1;
    b.host->tick(kServer);
    CHECK(now_of<ecs::DynamicEnemy01BehaviorState>(b, 1234).state == emf::gen::enemy01::State::Combat);
}

TEST_CASE("beastmaster: passive beasts still go where Attack sends them, for 20 s") {
    BeastRig b;
    wolf_out(b);
    b.reply("beastmaster.mode passive");
    creature(b, 70, 0, 10, kSkeletonFoe);
    b.look(1, 0, 10.5, 0, 0);        // looking at it
    REQUIRE(b.reply("beastmaster.attack").find("attack") != std::string::npos);
    fighting(b, 1234, 70);
    b.seconds(5);
    CHECK(still_fighting(b, 1234));  // sent: left to fight
    b.seconds(16);
    CHECK_FALSE(still_fighting(b, 1234));  // 20 s on: passive again
}

// An order that ended early (its target died or turned friendly, the chase was dropped) left Attack's 20 s running: the
// passive beast fought whatever the game picked next until then.
TEST_CASE("beastmaster: a passive beast is calm again as soon as Attack's order ends early (target dead or friendly, chase dropped)") {
    for (int how = 0; how < 3; ++how) {
        CAPTURE(how);
        BeastRig b;
        wolf_out(b);
        b.reply("beastmaster.mode passive");
        creature(b, 70, 0, 10, kSkeletonFoe);
        creature(b, 71, 4, 10, kSkeletonFoe);
        b.look(1, 0, 10.5, 0, 0);  // looking at 70
        REQUIRE(b.reply("beastmaster.attack").find("attack") != std::string::npos);
        fighting(b, 1234, 70);
        b.seconds(2);
        REQUIRE(still_fighting(b, 1234));  // sent: left to fight
        if (how == 0) b.r.fake.attrs[{70, EMF_ATTR_HEALTH}] = 0;  // it died
        if (how == 1) component<ecs::Faction>(b.r.fake, 70).faction = static_cast<emf::gen::FactionType>(kPlayerAlly);
        if (how == 2) place(b.r.fake, 1234, 0, 10, 50);  // chased beyond 45 m: it drops the chase and runs back
        b.seconds(1);                                    // the order ends at the check
        if (how == 2) {
            place(b.r.fake, 1234, 0, 10, 5);  // back beside its owner
            b.seconds(4);                     // the 3 s of coming back are over
        }
        fighting(b, 1234, 71);  // the game picks another foe for it
        b.seconds(1);
        CHECK_FALSE(still_fighting(b, 1234));  // passive again at once, not after the rest of the 20 s
    }
}

TEST_CASE("beastmaster: a beast more than 45 m from its owner drops its chase and is told to guard them again, in any mode") {
    BeastRig b;
    wolf_out(b);
    component<ecs::DynamicEnemy01BehaviorState>(b.r.fake, 1234).guardEntityId.id = 1;  // it guards its owner
    creature(b, 70, 0, 60, kSkeletonFoe);
    place(b.r.fake, 1234, 0, 10, 40);  // chasing, 40 m away: still within
    fighting(b, 1234, 70);
    b.seconds(2);
    CHECK(still_fighting(b, 1234));
    const size_t guards_before = b.guards().size();
    place(b.r.fake, 1234, 0, 10, 50);  // 50 m: beyond 45
    b.seconds(1);
    CHECK_FALSE(still_fighting(b, 1234));
    CHECK(b.guards().size() > guards_before);  // told to come back to its owner
    CHECK(teleports_of(b.r.fake, 1234) == 0);  // it runs back: within 60 m it is not teleported
}

// ---- loyalty: grows with the kills a beast takes part in; kept with the world ----
TEST_CASE("loyalty: a beast's loyalty and name are kept with the world; an older save loads at loyalty 1") {
    BeastRig b;
    b.reply("beastmaster.give wolf");
    CHECK(b.reply("beastmaster.loyalty Wolf 3").find("3") != std::string::npos);
    CHECK(b.reply("beastmaster.rename Wolf Grey Fang").find("Grey Fang") != std::string::npos);
    b.seconds(2);
    CHECK(b.saved().find("y 0x00000000000000aa 30 Grey Fang") != std::string::npos);  // 10 + 20 points
    write(b.data, "b 0x00000000000000aa wolf 0 0 Grey Fang\ny 0x00000000000000aa 30 Grey Fang\n");
    b.r.fake.session = 2;
    fire_load(1);
    b.seconds(1);
    CHECK(b.reply("beastmaster.list").find("1. Grey Fang - wolf, loyalty 3, at home") != std::string::npos);
    write(b.data, "b 0x00000000000000aa wolf 0 0 Fang\n");  // a save from before loyalty
    b.r.fake.session = 3;
    fire_load(1);
    b.seconds(1);
    CHECK(b.reply("beastmaster.list").find("1. Fang - wolf, loyalty 1, at home") != std::string::npos);
}

TEST_CASE("loyalty: the admin loyalty command is for the host's console only") {
    BeastRig b;
    b.reply("beastmaster.give wolf");
    CHECK(b.reply("beastmaster.loyalty Wolf 11").find("1-10") != std::string::npos);
    CHECK(b.reply("beastmaster.loyalty Wolf 5", 1, 0xAA).find("console") != std::string::npos);  // from the chat
    CHECK(b.reply("beastmaster.list").find("loyalty 1,") != std::string::npos);
}

TEST_CASE("loyalty: rename takes 1-20 characters and no name the collection has") {
    BeastRig b;
    b.reply("beastmaster.give wolf");
    b.reply("beastmaster.give wolf");  // Wolf, Wolf 2
    CHECK(b.reply("beastmaster.rename 1 Rex").find("Rex") != std::string::npos);
    CHECK(b.reply("beastmaster.list").find("1. Rex - wolf") != std::string::npos);
    CHECK(b.reply("beastmaster.rename Rex wolf 2").find("taken") != std::string::npos);
    CHECK(b.reply("beastmaster.rename 2").find("1-20") != std::string::npos);
    CHECK(b.reply("beastmaster.rename 2 " + std::string(21, 'a')).find("1-20") != std::string::npos);
    CHECK(b.reply("beastmaster.rename 9 Max").find("No such beast") != std::string::npos);
    CHECK(b.reply("beastmaster.list").find("2. Wolf 2 - wolf") != std::string::npos);
}

namespace {
// A component as it is now, to change (component() makes a fresh one).
template <class T>
T& live_of(BeastRig& b, uint32_t id) {
    return *reinterpret_cast<T*>(b.r.fake.components.at({T::kHash, id}).data());
}
constexpr int64_t kSpawnNs = 7000000000000LL;  // the game's time (ns) at a called beast's spawn: its SpawnTime
// The creature the next call spawns (id): its template's damage multiplier 1, its base health 100, its SpawnTime
// kSpawnNs, optionally a scale.
void next_beast(BeastRig& b, uint32_t id, float scale = 0.0f) {
    b.spawnable(1);
    component<ecs::DamageMod>(b.r.fake, id).dataStorage[2] = 1.0f;
    component<ecs::Health>(b.r.fake, id).dataStorage[2] = 100;
    component<ecs::SpawnTime>(b.r.fake, id).spawnTime.value = kSpawnNs;
    if (scale > 0.0f) component<ecs::ScaleComponent>(b.r.fake, id).uniformScale = scale;
}
}  // namespace

// The owner (2026-11-23), after Spirimtin's "wolves are weak at my level": a bit stronger, and a setting for it. Seen
// live: a wolf with 70 health fell to two hits of a Vukah warrior (48 each), staggered by each.
TEST_CASE("beastmaster (1.20): beasts are a bit stronger by default (health 150 %, damage 125 % of their role); the settings change it live") {
    BeastRig b;
    b.reply("beastmaster.give wolf");
    next_beast(b, 1234);
    REQUIRE(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    b.seconds(1);
    CHECK(now_of<ecs::Health>(b, 1234).dataStorage[3] == 80);  // the adder: 100 x 1.2 x 1.5 - 100
    CHECK(now_of<ecs::DamageMod>(b, 1234).dataStorage[2] == doctest::Approx(1.15 * 1.25));
    b.r.fake.settings["beastmaster.beast_health"] = "200";
    b.r.fake.settings["beastmaster.beast_damage"] = "100";
    for (auto& [mod, cb] : b.r.fake.setting_watchers)  // the Mods tab changed them
        if (mod == "beastmaster") {
            cb.first(kServer, "beastmaster", "beast_health", cb.second);
            cb.first(kServer, "beastmaster", "beast_damage", cb.second);
        }
    b.seconds(1);
    CHECK(now_of<ecs::Health>(b, 1234).dataStorage[3] == 140);  // 100 x 1.2 x 2 - 100, on the beast out at once
    CHECK(now_of<ecs::DamageMod>(b, 1234).dataStorage[2] == doctest::Approx(1.15));
}

TEST_CASE("loyalty: a called wolf has its role, 1.2x health and 1.15x damage; at loyalty 10 1.44x and 1.38x") {
    BeastRig b({{"beast_health", "100"}, {"beast_damage", "100"}});
    b.reply("beastmaster.give wolf");
    next_beast(b, 1234);
    REQUIRE(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    b.seconds(1);
    CHECK(now_of<ecs::Health>(b, 1234).dataStorage[3] == 20);  // the adder: 100 x 0.2
    CHECK(now_of<ecs::DamageMod>(b, 1234).dataStorage[2] == doctest::Approx(1.15));
    b.reply("beastmaster.dismiss all");
    b.reply("beastmaster.loyalty Wolf 10");
    next_beast(b, 1235);
    b.seconds(11);
    REQUIRE(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    b.seconds(1);
    CHECK(now_of<ecs::Health>(b, 1235).dataStorage[3] == 44);  // 100 x 1.2 x 1.2 - 100
    CHECK(now_of<ecs::DamageMod>(b, 1235).dataStorage[2] == doctest::Approx(1.38));
}

TEST_CASE("loyalty: a tamed bear's health is its role (2.2x), not the wild bear's 2.5x") {
    BeastRig b({{"beast_health", "100"}, {"beast_damage", "100"}});
    b.reply("beastmaster.give bear");
    next_beast(b, 1234, 1.4f);
    REQUIRE(b.reply("beastmaster.call Bear").find("comes") != std::string::npos);
    b.seconds(1);
    CHECK(now_of<ecs::Health>(b, 1234).dataStorage[3] == 120);
    CHECK(now_of<ecs::ScaleComponent>(b, 1234).uniformScale == doctest::Approx(1.4));  // loyalty 1: its own size
}

namespace {
// The size a spawn was given (spawn_ex's ScaleComponent), or -1 when none.
float scale_given(const Fake::Spawn& sp) {
    for (const auto& [hash, data] : sp.extra)
        if (hash == ecs::ScaleComponent::kHash && data.size() >= sizeof(ecs::ScaleComponent))
            return reinterpret_cast<const ecs::ScaleComponent*>(data.data())->uniformScale;
    return -1.0f;
}
}  // namespace

TEST_CASE("loyalty: a beast is called at its loyalty's size (a bear 1.4 x 1.25 at 10, a game wolf 1.25); at loyalty 1 its own") {
    // the game draws a creature at the size it is made with: a ScaleComponent changed later is not drawn
    BeastRig b;
    b.reply("beastmaster.give bear");
    b.reply("beastmaster.loyalty Bear 10");
    next_beast(b, 1234, 1.4f);
    REQUIRE(b.reply("beastmaster.call Bear").find("comes") != std::string::npos);
    REQUIRE(b.r.fake.spawns.size() == 1);
    CHECK(scale_given(b.r.fake.spawns[0]) == doctest::Approx(1.75));
    b.reply("beastmaster.give wolf");
    b.reply("beastmaster.loyalty Wolf 10");
    next_beast(b, 1235);
    b.seconds(11);
    REQUIRE(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    REQUIRE(b.r.fake.spawns.size() == 2);
    CHECK(scale_given(b.r.fake.spawns[1]) == doctest::Approx(1.25));  // the game's wolf grows too
    b.reply("beastmaster.give boar");
    b.reply("beastmaster.dismiss all");  // two out: the most
    next_beast(b, 1236);
    b.seconds(11);
    REQUIRE(b.reply("beastmaster.call Boar").find("comes") != std::string::npos);
    REQUIRE(b.r.fake.spawns.size() == 3);
    CHECK(scale_given(b.r.fake.spawns[2]) == -1.0f);  // loyalty 1: as its template makes it
}

TEST_CASE("loyalty: a beast out follows its owner's level, also when the owner levels up") {
    BeastRig b;
    b.reply("beastmaster.give wolf");
    next_beast(b, 1234);
    REQUIRE(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    CHECK(now_of<ecs::Level>(b, 1234).dataStorage[0] == 10);  // its owner's
    component<ecs::DynamicEnemyScaling>(b.r.fake, 1234).nextUpdateTime.value = 999;
    live_of<ecs::Level>(b, 1).dataStorage[0] = 11;  // the owner levels up
    b.seconds(2);
    CHECK(now_of<ecs::Level>(b, 1234).dataStorage[0] == 11);
    CHECK(now_of<ecs::DynamicEnemyScaling>(b, 1234).nextUpdateTime.value == 0);  // the game rescales it
}

namespace {
bool healed(BeastRig& b, uint32_t id) {
    for (const auto& [e, a, d] : b.r.fake.attr_adds)
        if (e == id && a == EMF_ATTR_HEALTH && d > 0) return true;
    return false;
}
}  // namespace

TEST_CASE("loyalty: a beast heals 5 % of its maximum health a second, 5 s after its last hit") {
    BeastRig b;
    b.reply("beastmaster.give wolf");
    next_beast(b, 1234);
    REQUIRE(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    b.seconds(6);
    b.r.fake.attrs[{1234, EMF_ATTR_HEALTH}] = 40;  // 40 of 100, hurt in a fight
    deliver(b.r.fake, "HitEvent", hit_event(60, 1234));
    b.r.fake.attr_adds.clear();
    b.seconds(3);
    CHECK_FALSE(healed(b, 1234));                 // 3 s after its last hit: not yet
    b.seconds(3);
    CHECK(b.added(1234, EMF_ATTR_HEALTH, 5));     // 5 % of 100
    b.r.fake.attr_adds.clear();
    b.r.fake.attrs[{1234, EMF_ATTR_HEALTH}] = 100;
    b.seconds(2);
    CHECK_FALSE(healed(b, 1234));                 // full: nothing more
}

TEST_CASE("loyalty: a hostile foe the beast hit in the last 30 s dies: a point; wildlife, farm animals and old hits give none") {
    BeastRig b;
    b.reply("beastmaster.give wolf");
    b.reply("beastmaster.give boar");
    next_beast(b, 1234);
    REQUIRE(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    next_beast(b, 1235);
    b.seconds(11);
    REQUIRE(b.reply("beastmaster.call Boar").find("comes") != std::string::npos);
    creature(b, 70, 0, 10, kSkeletonFoe);
    deliver(b.r.fake, "HitEvent", hit_event(1234, 70));
    deliver(b.r.fake, "HitEvent", hit_event(1235, 70));
    deliver(b.r.fake, "EntityDiedEvent", died_event(70));
    b.seconds(1);
    CHECK(b.saved().find("y 0x00000000000000aa 1 Wolf") != std::string::npos);  // both took part: a point each
    CHECK(b.saved().find("y 0x00000000000000aa 1 Boar") != std::string::npos);
    creature(b, 72, 3, 3, kFarmAnimal);  // a farm animal: no point
    deliver(b.r.fake, "HitEvent", hit_event(1234, 72));
    deliver(b.r.fake, "EntityDiedEvent", died_event(72));
    creature(b, 71, 0, 12, kSkeletonFoe);  // hit 31 s before it dies: no point
    deliver(b.r.fake, "HitEvent", hit_event(1234, 71));
    b.seconds(31);
    deliver(b.r.fake, "EntityDiedEvent", died_event(71));
    creature(b, 73, 0, 14, kSkeletonFoe);  // it only hit the wolf: no point
    deliver(b.r.fake, "HitEvent", hit_event(73, 1234));
    deliver(b.r.fake, "EntityDiedEvent", died_event(73));
    b.seconds(1);
    CHECK(b.saved().find("y 0x00000000000000aa 1 Wolf") != std::string::npos);
}

TEST_CASE("loyalty: the tenth point brings loyalty 2: the owner hears it, the beast is stronger at once and stays so") {
    BeastRig b({{"beast_health", "100"}, {"beast_damage", "100"}, {"rest_s", "5"}});
    b.reply("beastmaster.give wolf");
    next_beast(b, 1234);
    REQUIRE(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    for (uint32_t i = 0; i < 10; ++i) {
        if (i == 9) CHECK(b.reply("beastmaster.list").find("loyalty 1,") != std::string::npos);
        creature(b, 100 + i, 0, 10, kSkeletonFoe);
        deliver(b.r.fake, "HitEvent", hit_event(1234, 100 + i));
        deliver(b.r.fake, "EntityDiedEvent", died_event(100 + i));
        b.seconds(1);
    }
    CHECK(b.noted("Wolf reached loyalty 2: a talent point to spend at a stable."));
    CHECK(b.reply("beastmaster.list").find("loyalty 2,") != std::string::npos);
    const double damage = 1.15 * (1 + 0.2 / 9);
    CHECK(now_of<ecs::DamageMod>(b, 1234).dataStorage[2] == doctest::Approx(damage));
    deliver(b.r.fake, "EntityDiedEvent", died_event(1234));  // it falls
    next_beast(b, 1235);
    b.seconds(12);
    REQUIRE(b.noted("Wolf is back"));  // by itself after its rest
    b.seconds(1);
    CHECK(b.reply("beastmaster.list").find("loyalty 2,") != std::string::npos);
    CHECK(now_of<ecs::DamageMod>(b, 1235).dataStorage[2] == doctest::Approx(damage));
}

TEST_CASE("loyalty: a beast that falls rests 60 s, so it is back for the next fight but not in this one") {
    BeastRig b;
    b.reply("beastmaster.give wolf");
    next_beast(b, 1234);
    REQUIRE(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    deliver(b.r.fake, "EntityDiedEvent", died_event(1234));
    CHECK(b.reply("beastmaster.list").find("rests 1:00") != std::string::npos);
    CHECK(b.r.fake.settings["beastmaster.loyalty_pace"] == "1");
}

TEST_CASE("loyalty: a name that is a number or 'all' is refused (beasts are called and sent home by number, and all)") {
    BeastRig b;
    b.reply("beastmaster.give wolf");
    for (const char* name : {"2", "007", "1e0", "all", "ALL"}) {
        CAPTURE(name);
        CHECK(b.reply(std::string("beastmaster.rename Wolf ") + name).find("number") != std::string::npos);
    }
    CHECK(b.reply("beastmaster.list").find("1. Wolf - wolf") != std::string::npos);
}

TEST_CASE("loyalty: a y line belongs to the beast of that name, also a name that looks like a number (an older save)") {
    BeastRig b;
    write(b.data, "b 0x00000000000000aa wolf 0 0 2\nb 0x00000000000000aa boar 0 0 Fang\ny 0x00000000000000aa 30 2\n");
    b.r.fake.session = 2;
    fire_load(1);
    b.seconds(1);
    const std::string list = b.reply("beastmaster.list");
    CHECK(list.find("1. 2 - wolf, loyalty 3") != std::string::npos);
    CHECK(list.find("2. Fang - boar, loyalty 1") != std::string::npos);
}

TEST_CASE("loyalty: a beast on an entity id the game gave out before gets its own template's damage, not the old beast's") {
    BeastRig b({{"beast_health", "100"}, {"beast_damage", "100"}});
    b.reply("beastmaster.give bear");
    b.reply("beastmaster.give wolf");
    next_beast(b, 1234, 1.4f);
    component<ecs::DamageMod>(b.r.fake, 1234).dataStorage[2] = 1.5f;  // the pack bear's template
    REQUIRE(b.reply("beastmaster.call Bear").find("comes") != std::string::npos);
    b.seconds(1);
    component<ecs::Faction>(b.r.fake, 1234).faction = static_cast<emf::gen::FactionType>(6);  // the id names another creature now
    b.seconds(2);
    b.r.fake.next_spawn_id = 1234;  // the game gives the id out again: to the wolf
    next_beast(b, 1234);
    b.seconds(11);
    REQUIRE(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    b.seconds(1);
    CHECK(now_of<ecs::DamageMod>(b, 1234).dataStorage[2] == doctest::Approx(1.15));
}

TEST_CASE("loyalty: a beast the game will neither bring back nor call again leaves the mod running") {
    BeastRig b;
    b.reply("beastmaster.give wolf");
    next_beast(b, 1234);
    REQUIRE(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    b.r.fake.teleport_ok = false;
    b.r.fake.spawn_fails = true;
    place(b.r.fake, 1234, 130, 10, 0);  // far: it is brought back, and both ways fail
    b.seconds(3);
    CHECK(b.reply("beastmaster.mode").find("defending") != std::string::npos);  // the mod still answers
}

TEST_CASE("loyalty: when the game sets a beast's damage multiplier again (a rescale), its role and loyalty go on top") {
    BeastRig b({{"beast_health", "100"}, {"beast_damage", "100"}});
    b.reply("beastmaster.give wolf");
    next_beast(b, 1234);
    REQUIRE(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    b.seconds(1);
    REQUIRE(now_of<ecs::DamageMod>(b, 1234).dataStorage[2] == doctest::Approx(1.15));
    live_of<ecs::DamageMod>(b, 1234).dataStorage[2] = 2.0f;  // the game's rescale (a world's enemy damage factor)
    b.seconds(2);
    CHECK(now_of<ecs::DamageMod>(b, 1234).dataStorage[2] == doctest::Approx(2.3));
    b.seconds(2);
    CHECK(now_of<ecs::DamageMod>(b, 1234).dataStorage[2] == doctest::Approx(2.3));  // not again on top of itself
}

// ---- the stable: active slots, the Beasts window ----
TEST_CASE("stable: active flags and the stabled flag are kept with the world; an older save loads with none active, not stabled") {
    BeastRig b;
    write(b.data, "b 0x00000000000000aa wolf 0 0 Fang\nb 0x00000000000000aa boar 0 0 Tusk\n"
                  "a 0x00000000000000aa Tusk\ns 1\n");
    b.r.fake.session = 2;
    fire_load(1);
    b.seconds(1);
    b.reply("beastmaster.mode passive");  // a change: the world's data is written
    b.seconds(1);
    CHECK(b.saved().find("a 0x00000000000000aa Tusk") != std::string::npos);
    CHECK(b.saved().find("a 0x00000000000000aa Fang") == std::string::npos);
    CHECK(b.saved().find("s 1") != std::string::npos);
    write(b.data, "b 0x00000000000000aa wolf 0 0 Fang\n");  // a 1.5.0 save
    b.r.fake.session = 3;
    fire_load(1);
    b.seconds(1);
    b.reply("beastmaster.mode aggressive");
    b.seconds(1);
    CHECK(b.saved().find("\na ") == std::string::npos);
    CHECK(b.saved().find("s 1") == std::string::npos);
}

TEST_CASE("stable: one active slot at the start, more at levels 10, 20 and 30; a level of 0 turns a slot off") {
    BeastRig b;
    component<ecs::Level>(b.r.fake, 1).dataStorage[0] = 5;
    CHECK(b.reply("beastmaster.slots").find("Active slots: 1 (slot 2 at level 10)") != std::string::npos);
    component<ecs::Level>(b.r.fake, 1).dataStorage[0] = 25;
    CHECK(b.reply("beastmaster.slots").find("Active slots: 3 (slot 4 at level 30)") != std::string::npos);
    component<ecs::Level>(b.r.fake, 1).dataStorage[0] = 30;
    CHECK(b.reply("beastmaster.slots").find("Active slots: 4") != std::string::npos);
    BeastRig off({{"slot3_level", "0"}});
    component<ecs::Level>(off.r.fake, 1).dataStorage[0] = 25;
    CHECK(off.reply("beastmaster.slots").find("Active slots: 2 (slot 4 at level 30)") != std::string::npos);
}


// The render model of the game's animal bed (animalprops_farm_animal_sleep_01, the item
// Animal_Farm_Housing_Sleeping_Bed): a stable (mods/beastmaster/stable.lua MODELS).
constexpr const char* kStableModel = "daeb81bb-8398-4128-9107-c75b2e3e7d5b";
// The large animal bed (animalprops_farm_animal_sleep_large_01, Animal_Farm_Housing_Sleeping_Bed_YAK_Sanddigger).
constexpr const char* kLargeStableModel = "7dd9139a-fba7-4e60-8b5a-fa853c250ef7";
// A flame altar (the heart of a base) at (x, 10, z).
void flame_altar(BeastRig& b, uint32_t id, double x, double z) {
    place(b.r.fake, id, x, 10, z);
    component<ecs::BaseIdComponent>(b.r.fake, id);
    add_entity(b.r.fake, id);
}
// A placed prop with a render model (the stable's by default; none: nullptr) at (x, y, z).
void stable_prop(BeastRig& b, uint32_t id, double x, double y, double z, const char* model = kStableModel) {
    place(b.r.fake, id, x, y, z);
    if (model) b.r.fake.templates[{ecs::ModelResource::kHash, id}] = guid_bytes(model);
    component<ecs::StaticCollider>(b.r.fake, id);
    add_entity(b.r.fake, id);
}

TEST_CASE("stable: a built prop with the stable's model in a base is a stable; another prop, or one outside every base, is not") {
    BeastRig b;
    flame_altar(b, 900, 0, 0);
    stable_prop(b, 950, 5, 10, 0);
    stable_prop(b, 951, 8, 10, 0, "2ff37fdb-4bf7-4074-a8c9-b75f47511d24");  // another prop in the base: a dog bed
    stable_prop(b, 952, 300, 10, 0);                                         // a village's, far from every base
    CHECK(b.reply("beastmaster.stable list") == "No stable in this world yet: build an animal bed (the straw bed for farm "
                                                     "animals) in your base, or mark a prop with beastmaster.stable mark.");
    b.seconds(32);  // the next search of the world (one every 30 s)
    const std::string list = b.reply("beastmaster.stable list");
    CHECK(list.find("1. Animal bed at 5, 10, 0 (5 m from you)") != std::string::npos);
    CHECK(list.find("300") == std::string::npos);
    CHECK(list.find("2. ") == std::string::npos);
    stable_prop(b, 953, -6, 10, 0, kLargeStableModel);
    b.seconds(32);
    CHECK(b.reply("beastmaster.stable list").find("2. Large animal bed at -6, 10, 0") != std::string::npos);
}

TEST_CASE("stable: on the host, a stable whose model only the client world has is found") {
    BeastRig b;
    flame_altar(b, 900, 0, 0);
    stable_prop(b, 950, 10, 10, 0);
    // render models are client-side: the model is there while the host's client world steps, not on the server's
    const auto model = b.r.fake.templates[{ecs::ModelResource::kHash, 950}];
    for (int i = 0; i < 4; ++i) {
        b.r.fake.templates[{ecs::ModelResource::kHash, 950}] = model;
        b.host->tick(kClient);
        b.r.fake.templates.erase({ecs::ModelResource::kHash, 950});
        b.seconds(1);
    }
    CHECK(b.reply("beastmaster.stable list").find("1. Animal bed at 10, 10, 0") != std::string::npos);
}

TEST_CASE("stable: on the host, a bed built while the world runs counts within 10 s; one taken down stops counting as fast") {
    BeastRig b;
    flame_altar(b, 900, 0, 0);
    // render models are client-side: the model is on the client world only, which steps with the server's
    const auto model = guid_bytes(kStableModel);
    auto step_both = [&](int secs) {
        for (int i = 0; i < secs * 60; ++i) {
            b.r.fake.templates[{ecs::ModelResource::kHash, 950}] = model;
            b.host->tick(kClient);
            b.r.fake.templates.erase({ecs::ModelResource::kHash, 950});
            b.r.fake.tick += 1;
            b.host->tick(kServer);
        }
    };
    step_both(35);  // the first searches of both worlds are done
    stable_prop(b, 950, 10, 10, 0, nullptr);
    step_both(10);
    CHECK(b.reply("beastmaster.stable list").find("1. Animal bed at 10, 10, 0") != std::string::npos);
    remove_entity(b.r.fake, 950);  // taken down
    step_both(10);
    CHECK(b.reply("beastmaster.stable list").rfind("No stable in this world now (there was one: only active beasts can be "
                                                      "called): build an animal bed", 0) == 0);
}

TEST_CASE("stable: the host marks a prop as a stable, it is kept with the world and found again by place after a reload") {
    BeastRig b;
    stable_prop(b, 960, 0, 10, 5, nullptr);  // any prop (a post, a trough): its model unknown here
    b.look(1, 0, 11.5, 0, 0);                // looking along +z at it
    CHECK(b.reply("beastmaster.stable mark") == "Marked: this prop is a stable now (at 0, 10, 5).");
    b.seconds(1);
    CHECK(b.saved().find("k 0.00 10.00 5.00 -") != std::string::npos);
    CHECK(b.reply("beastmaster.stable list").find("1. marked prop at 0, 10, 5 (5 m from you)") != std::string::npos);
    b.r.fake.session = 2;  // a reload: the mark comes back with the world's data, by its place
    fire_load(1);
    b.seconds(1);
    CHECK(b.reply("beastmaster.stable list").find("1. marked prop at 0, 10, 5") != std::string::npos);
    CHECK(b.reply("beastmaster.stable mark") == "That is a stable already.");
    b.look(1, 0, 11.5, 0, 180);  // looking away
    CHECK(b.reply("beastmaster.stable mark").rfind("Look at a prop", 0) == 0);
    for (const auto& c : b.r.fake.commands)  // the host's and admins' command, not every player's
        if (c.name == "beastmaster.stable") CHECK(c.flags == 0u);
}

TEST_CASE("stable: unmark takes the mark off; without camera data mark takes the prop nearest the sender within 3 m") {
    BeastRig b;
    stable_prop(b, 960, 0, 10, 2, nullptr);  // 2 m from the host, whose camera the server does not know
    CHECK(b.reply("beastmaster.stable mark") == "Marked: the prop nearest to you (at 0, 10, 2) is a stable now.");
    CHECK(b.reply("beastmaster.stable unmark") == "Unmarked: the prop at 0, 10, 2 is no stable now.");
    CHECK(b.reply("beastmaster.stable list").rfind("No stable in this world now (there was one: only active beasts can be "
                                                      "called): build an animal bed", 0) == 0);
    b.seconds(1);
    CHECK(b.saved().find("k 0.00") == std::string::npos);
    CHECK(b.reply("beastmaster.stable unmark") == "No marked stable within 8 m.");
    place(b.r.fake, 960, 0, 10, 6);  // too far for a mark without camera data
    CHECK(b.reply("beastmaster.stable mark").rfind("Look at a prop", 0) == 0);
}

namespace {
constexpr uint32_t kE = 0x45;  // the stable_key default
// A prop 3 m in front of player 1, marked as a stable, with player 1 looking at it.
void at_stable(BeastRig& b) {
    stable_prop(b, 960, 0, 10, 3, nullptr);
    b.look(1, 0, 11.5, 0, 0);
    REQUIRE(b.reply("beastmaster.stable mark").rfind("Marked: ", 0) == 0);
}
// The Beasts window as last drawn: the last update, else the window opened.
const Fake::Win& shown(BeastRig& b) {
    return b.r.fake.window_updates.empty() ? b.r.fake.windows.back() : b.r.fake.window_updates.back();
}
int row_at(const Fake::Win& w, const std::string& key) {
    for (size_t i = 0; i < w.keys.size(); ++i)
        if (w.keys[i] == key) return static_cast<int>(i);
    return -1;
}
std::string text_at(const Fake::Win& w, const std::string& key) {
    const int i = row_at(w, key);
    return i < 0 ? "<none>" : w.texts[static_cast<size_t>(i)];
}
uint32_t flags_at(const Fake::Win& w, const std::string& key) {
    const int i = row_at(w, key);
    return i < 0 ? 0xFFFFFFFFu : w.widgets[static_cast<size_t>(i)].flags;
}
bool shows(const Fake::Win& w, const std::string& part) {
    for (const auto& t : w.texts)
        if (t.find(part) != std::string::npos) return true;
    return false;
}
// A click on the open window's row or button `key` (the core's event, on the server world).
void click(BeastRig& b, const std::string& key, uint32_t kind = EMF_UI_CLICK) {
    REQUIRE(!b.r.fake.windows.empty());
    const Fake::Win w = b.r.fake.windows.back();
    EmfUiEvent e{};
    e.size = sizeof e;
    e.kind = kind;
    e.window = w.id.c_str();
    e.key = key.c_str();
    e.from_key = "";
    e.reason = EMF_UI_BY_PLAYER;
    w.cb(kServer, &e, w.user);
}
// Player 1's beasts from a save: Fang (wolf, active), Tusk (boar), Rex (hound); the world has had a stable.
void three_beasts(BeastRig& b) {
    write(b.data, "b 0x00000000000000aa wolf 0 0 Fang\nb 0x00000000000000aa boar 0 0 Tusk\n"
                  "b 0x00000000000000aa hound 0 0 Rex\na 0x00000000000000aa Fang\ns 1\n");
    b.r.fake.session = 2;
    fire_load(1);
    b.seconds(1);
}
}  // namespace

TEST_CASE("stable: E at a stable opens the Beasts window with the slots and the stable list") {
    BeastRig b;  // level 10: two slots, slot 3 at level 20, slot 4 at level 30
    three_beasts(b);
    at_stable(b);
    press(b, kE);
    REQUIRE(b.r.fake.windows.size() == 1u);
    const Fake::Win& w = b.r.fake.windows.back();
    CHECK(w.id == "beastmaster.stable");
    CHECK(w.title == "Beasts");
    CHECK(w.close_key == kE);
    CHECK(w.place == EMF_UI_PLACE_CENTRE);
    CHECK(w.kind == EMF_WORLD_SERVER);
    CHECK(shows(w, "Active 1/2"));
    CHECK(text_at(w, "active.1") == "Fang");
    CHECK((flags_at(w, "active.2") & EMF_UI_EMPTY) != 0u);
    CHECK(text_at(w, "active.3") == "Level 20");
    CHECK((flags_at(w, "active.3") & EMF_UI_LOCKED) != 0u);
    CHECK(text_at(w, "active.4") == "Level 30");
    CHECK(shows(w, "Stable 2/8"));  // Tusk and Rex: the active Fang is in its slot only
    CHECK(row_at(w, "beast.Fang") == -1);
    CHECK(text_at(w, "beast.Tusk") == "Tusk (boar, loyalty 1, at home)");
    CHECK(text_at(w, "beast.Rex") == "Rex (hound, loyalty 1, at home)");
    for (const char* button : {"make_active", "store", "release"})  // nothing selected yet
        CHECK((flags_at(w, button) & EMF_UI_DISABLED) != 0u);
}

TEST_CASE("pack beast: the Beasts window has a Pack beast row (Call, then Send home); the llama is not in the stable's list") {
    BeastRig b({{"call_cooldown_s", "0"}, {"call_mana_pct", "0"}});
    three_beasts(b);
    b.reply("beastmaster.give packllama");
    b.spawnable(1);
    at_stable(b);
    press(b, kE);
    REQUIRE(b.r.fake.windows.size() == 1u);
    CHECK(shows(shown(b), "Pack beast"));
    CHECK(shows(shown(b), "Stable 2/8"));  // Tusk and Rex
    CHECK(row_at(shown(b), "beast.Pack llama") == -1);
    CHECK(text_at(shown(b), "pack") == "Call");
    click(b, "pack");
    CHECK(text_at(shown(b), "pack") == "Send home");
    click(b, "pack");
    CHECK(shows(shown(b), "Pack llama goes home."));
    CHECK(text_at(shown(b), "pack") == "Call");
}

TEST_CASE("stable: Make active puts the selected beast into a free slot; Store takes it out (sent home if out)") {
    BeastRig b;
    three_beasts(b);
    b.spawnable(1);
    REQUIRE(b.reply("beastmaster.call Fang").find("comes") != std::string::npos);
    b.seconds(1);
    const uint32_t fang = 1234;
    at_stable(b);
    press(b, kE);
    REQUIRE(b.r.fake.windows.size() == 1u);
    click(b, "beast.Tusk");  // selecting a beast opens its tab
    CHECK(text_at(shown(b), "tab.beast") == "Tusk");
    CHECK((flags_at(shown(b), "tab.beast") & EMF_UI_SELECTED) != 0u);
    CHECK((flags_at(shown(b), "make_active") & EMF_UI_DISABLED) == 0u);
    CHECK((flags_at(shown(b), "store") & EMF_UI_DISABLED) != 0u);  // not active
    click(b, "make_active");
    CHECK(shows(shown(b), "Tusk is active."));
    click(b, "tab.stable");
    CHECK(text_at(shown(b), "active.2") == "Tusk");
    CHECK((flags_at(shown(b), "active.2") & EMF_UI_SELECTED) != 0u);  // still selected, in its slot now
    CHECK(row_at(shown(b), "beast.Tusk") == -1);  // not in the stable any more
    CHECK(shows(shown(b), "Active 2/2"));
    CHECK(shows(shown(b), "Tusk is active."));
    CHECK(b.saved().find("a 0x00000000000000aa Tusk") != std::string::npos);
    click(b, "beast.Rex");  // both slots taken
    click(b, "make_active");
    CHECK(shows(shown(b), "No free active slot: store a beast first."));
    CHECK(b.saved().find("a 0x00000000000000aa Rex") == std::string::npos);
    click(b, "tab.stable");
    click(b, "active.1");  // a slot selects its beast and opens its tab
    CHECK(text_at(shown(b), "tab.beast") == "Fang");
    click(b, "store");
    CHECK(shows(shown(b), "Fang goes to the stable."));
    click(b, "tab.stable");
    CHECK(text_at(shown(b), "active.1") == "Tusk");
    CHECK((flags_at(shown(b), "active.2") & EMF_UI_EMPTY) != 0u);
    CHECK(b.saved().find("a 0x00000000000000aa Fang") == std::string::npos);
    CHECK(std::find(b.r.fake.destroyed.begin(), b.r.fake.destroyed.end(), fang) != b.r.fake.destroyed.end());
    CHECK(text_at(shown(b), "beast.Fang") == "Fang (wolf, loyalty 1, at home)");
}

TEST_CASE("stable: Release needs a second click within 5 s, removes the beast (its body too when out) and frees its slot") {
    BeastRig b;
    three_beasts(b);
    b.spawnable(1);
    REQUIRE(b.reply("beastmaster.call Fang").find("comes") != std::string::npos);
    b.seconds(1);
    at_stable(b);
    press(b, kE);
    click(b, "beast.Fang");
    click(b, "release");
    CHECK(shows(shown(b), "Click Release again within 5 s to let Fang go for good."));
    CHECK(b.saved().find("Fang") != std::string::npos);
    b.seconds(6);  // too late: asks again
    click(b, "release");
    CHECK(shows(shown(b), "Click Release again within 5 s"));
    click(b, "release");
    CHECK(shows(shown(b), "Fang is gone: you let it go."));
    CHECK(b.saved().find("Fang") == std::string::npos);
    CHECK(std::find(b.r.fake.destroyed.begin(), b.r.fake.destroyed.end(), 1234u) != b.r.fake.destroyed.end());
    CHECK(row_at(shown(b), "beast.Fang") == -1);
    CHECK((flags_at(shown(b), "active.1") & EMF_UI_EMPTY) != 0u);
    CHECK(shows(shown(b), "Stable 2/8"));
    for (const char* button : {"make_active", "store", "release"})  // the selection went with it
        CHECK((flags_at(shown(b), button) & EMF_UI_DISABLED) != 0u);
}

TEST_CASE("stable: the window redraws when a beast falls; once closed it is not drawn again") {
    BeastRig b;
    three_beasts(b);
    b.spawnable(1);
    REQUIRE(b.reply("beastmaster.call Fang").find("comes") != std::string::npos);
    b.seconds(1);
    at_stable(b);
    press(b, kE);
    REQUIRE(b.r.fake.windows.size() == 1u);
    auto tip = [&](const std::string& key) {
        const Fake::Win& w = shown(b);
        const int i = row_at(w, key);
        return i < 0 ? std::string("<none>") : w.tips[static_cast<size_t>(i)];
    };
    CHECK(tip("active.1") == "wolf, loyalty 1, out");
    deliver(b.r.fake, "EntityDiedEvent", died_event(1234));
    b.seconds(2);
    CHECK(tip("active.1").find("wolf, loyalty 1, rests ") == 0);
    CHECK(row_at(shown(b), "beast.Tusk") >= 0);
    click(b, "", EMF_UI_CLOSED);
    const size_t drawn = b.r.fake.window_updates.size();
    b.seconds(3);
    CHECK(b.r.fake.window_updates.size() == drawn);
}

TEST_CASE("stable: E away from a stable, or looking away from it, opens nothing") {
    BeastRig b;
    three_beasts(b);
    press(b, kE);  // no stable in the world
    CHECK(b.r.fake.windows.empty());
    stable_prop(b, 960, 0, 10, 3, nullptr);
    b.look(1, 0, 11.5, 0, 0);
    REQUIRE(b.reply("beastmaster.stable mark").rfind("Marked: ", 0) == 0);
    b.look(1, 0, 11.5, 0, 180);  // looking away
    press(b, kE);
    CHECK(b.r.fake.windows.empty());
    place(b.r.fake, 1, 0, 10, -6);  // 9 m away, looking at it
    b.look(1, 0, 11.5, -6, 0);
    press(b, kE);
    CHECK(b.r.fake.windows.empty());
}

namespace {
void set_level(BeastRig& b, uint32_t player, uint32_t level) {
    component<ecs::Level>(b.r.fake, player).dataStorage[0] = level;
}
bool noted(BeastRig& b, const std::string& part) {
    for (const auto& n : b.r.fake.notes)
        if (n.find(part) != std::string::npos) return true;
    return false;
}
// Tames the wild wolf 50 in front of player 1 (beaten down to 20 of 100).
void tame_wolf(BeastRig& b) {
    b.look(1, 0, 10.5, 0, 0);
    b.beast(50, kWolfGraph, 0, 5, kWildBeast, 20, 100);
    REQUIRE(b.reply("beastmaster.tame").find("Taming") != std::string::npos);
    b.seconds(4);
}
}  // namespace

// The stable holds collection_max beasts besides the active ones (seen live: one wolf, active, showed "Stable 1/8"
// and was listed in the stable too).
TEST_CASE("stable: the Stable list and its count are the stored beasts; the active ones are only in their slots") {
    BeastRig b;
    three_beasts(b);  // Fang active, Tusk and Rex stored
    at_stable(b);
    press(b, kE);
    REQUIRE(b.r.fake.windows.size() == 1u);
    CHECK(shows(shown(b), "Stable 2/8"));
    CHECK(row_at(shown(b), "beast.Fang") == -1);
    CHECK(text_at(shown(b), "active.1") == "Fang");
    CHECK(text_at(shown(b), "beast.Tusk") == "Tusk (boar, loyalty 1, at home)");
    CHECK(b.reply("beastmaster.list").rfind("Your beasts (1 active, 2/8 in the stable):", 0) == 0);
}

TEST_CASE("stable: a full stable takes no more stored beasts, but a beast can still be tamed into a free active slot") {
    BeastRig b;  // level 10: two slots; Fang active, Tusk and Rex stored
    three_beasts(b);
    b.r.fake.settings["beastmaster.collection_max"] = "2";
    at_stable(b);
    CHECK(b.reply("beastmaster.store Fang") == "Your stable is full (2 beasts): release one first.");
    CHECK(b.saved().find("a 0x00000000000000aa Fang") != std::string::npos);  // still active
    CHECK(b.reply("beastmaster.give wolf") == "your stable is full (2 beasts)");
    tame_wolf(b);  // the second slot is free: it joins as active
    CHECK(noted(b, "You tamed a wolf: Wolf"));
    CHECK(b.saved().find("a 0x00000000000000aa Wolf") != std::string::npos);
    b.look(1, 0, 10.5, 0, 0);
    b.beast(51, kWolfGraph, 0, 5, kWildBeast, 20, 100);
    CHECK(b.reply("beastmaster.tame") == "Your stable is full.");  // both slots taken and the stable full
}

TEST_CASE("stable: two wolves can be out with two slots; one slot allows one beast out") {
    BeastRig b;  // level 10: two slots
    b.reply("beastmaster.give wolf");
    b.reply("beastmaster.give wolf");
    b.spawnable(2);
    CHECK(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    b.seconds(11);
    CHECK(b.reply("beastmaster.call Wolf 2").find("comes") != std::string::npos);
    CHECK(b.r.fake.spawns.size() == 2);

    BeastRig one;
    set_level(one, 1, 5);  // one slot
    one.reply("beastmaster.give wolf");
    one.reply("beastmaster.give boar");
    one.spawnable(2);
    CHECK(one.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    one.seconds(11);
    CHECK(one.reply("beastmaster.call Boar") == "You have 1 beast out (1 active slot): dismiss one first.");
    CHECK(one.r.fake.spawns.size() == 1);
}

TEST_CASE("stable: before a stable any beast can be called; after it only active ones (a stored one says to make it active at a stable)") {
    BeastRig b;  // level 10: two slots
    write(b.data, "b 0x00000000000000aa wolf 0 0 Fang\nb 0x00000000000000aa boar 0 0 Tusk\n"
                  "b 0x00000000000000aa hound 0 0 Rex\n");
    b.r.fake.session = 2;
    fire_load(1);
    b.seconds(1);
    b.spawnable(2);
    CHECK(b.reply("beastmaster.call Rex").find("comes") != std::string::npos);  // no stable yet: any beast
    b.reply("beastmaster.dismiss Rex");
    at_stable(b);  // the world's first stable: Fang and Tusk (list order) take the two slots
    b.seconds(1);
    CHECK(b.saved().find("s 1") != std::string::npos);
    b.seconds(11);
    CHECK(b.reply("beastmaster.call Rex") == "Rex is in the stable: make it active at a stable (an animal bed in your base).");
    CHECK(b.reply("beastmaster.call Tusk").find("comes") != std::string::npos);
    press(b, VK_F3);
    REQUIRE(b.r.fake.wheels.size() == 1);
    CHECK(index_of(b.r.fake.wheels[0].items, "Call Fang") >= 0);
    CHECK(index_of(b.r.fake.wheels[0].items, "Call Rex") < 0);  // stored: not on the wheel
}

TEST_CASE("pack beast: `pack` calls the pack llama or sends it home; with a stable it is on the wheel and not in the stable's count") {
    BeastRig b({{"call_cooldown_s", "0"}, {"call_mana_pct", "0"}});
    CHECK(b.reply("beastmaster.pack").find("no pack beast") != std::string::npos);
    write(b.data, "b 0x00000000000000aa wolf 0 0 Fang\nb 0x00000000000000aa boar 0 0 Tusk\n"
                  "b 0x00000000000000aa hound 0 0 Rex\nb 0x00000000000000aa packllama 0 0 Pack llama\n");
    b.r.fake.session = 2;
    fire_load(1);
    b.seconds(1);
    b.spawnable(3);
    at_stable(b);  // Fang and Tusk take the two active slots
    b.seconds(1);
    CHECK(b.reply("beastmaster.list").find("(2 active, 1/") != std::string::npos);  // Rex in the stable; the llama apart
    CHECK(b.reply("beastmaster.pack").find("comes") != std::string::npos);
    CHECK(b.reply("beastmaster.pack") == "Pack llama goes home.");
    press(b, VK_F3);
    REQUIRE(b.r.fake.wheels.size() == 1);
    CHECK(index_of(b.r.fake.wheels[0].items, "Call Pack llama") >= 0);  // not an active beast, still on the wheel
}

TEST_CASE("stable: a beast tamed with no free slot leaves: home before a stable, to the stable after") {
    BeastRig home;
    set_level(home, 1, 5);  // one slot
    home.reply("beastmaster.give boar");
    home.spawnable(1);
    REQUIRE(home.reply("beastmaster.call Boar").find("comes") != std::string::npos);
    tame_wolf(home);
    CHECK(noted(home, "You tamed a wolf: Wolf. It goes home (1 active slot, 1 beast out)."));
    CHECK(contains(home.r.fake.destroyed, 50));
    CHECK(home.reply("beastmaster.list").find("2. Wolf - wolf, loyalty 1, at home") != std::string::npos);

    BeastRig full;
    set_level(full, 1, 5);
    write(full.data, "b 0x00000000000000aa boar 0 0 Tusk\na 0x00000000000000aa Tusk\ns 1\n");
    full.r.fake.session = 2;
    fire_load(1);
    full.seconds(1);
    tame_wolf(full);
    CHECK(noted(full, "You tamed a wolf: Wolf. It goes to your stable (no free active slot)."));
    CHECK(contains(full.r.fake.destroyed, 50));
    CHECK(full.saved().find("a 0x00000000000000aa Wolf") == std::string::npos);

    BeastRig free;  // level 10: a second slot is free
    write(free.data, "b 0x00000000000000aa boar 0 0 Tusk\na 0x00000000000000aa Tusk\ns 1\n");
    free.r.fake.session = 2;
    fire_load(1);
    free.seconds(1);
    tame_wolf(free);
    CHECK(noted(free, "You tamed a wolf: Wolf"));
    CHECK_FALSE(contains(free.r.fake.destroyed, 50));
    const std::string kept = free.saved();
    CAPTURE(kept);
    CHECK(kept.find("a 0x00000000000000aa Wolf") != std::string::npos);
    CHECK(free.reply("beastmaster.list").find("2. Wolf - wolf, loyalty 1, out") != std::string::npos);
}

TEST_CASE("stable: a 1.5.0 save: beasts out beyond the slots go home, all stay callable; at the first stable the beasts out become active, then list order") {
    BeastRig one;
    set_level(one, 1, 5);  // one slot
    write(one.data, "b 0x00000000000000aa wolf 0 1 Fang\nb 0x00000000000000aa boar 0 1 Tusk\n"
                    "b 0x00000000000000aa hound 0 0 Rex\n");
    one.spawnable(2);
    one.r.fake.session = 2;
    fire_load(1);
    one.seconds(2);
    CHECK(one.r.fake.spawns.size() == 1);  // Fang comes back; Tusk goes home
    CHECK(one.reply("beastmaster.list").find("2. Tusk - boar, loyalty 1, at home") != std::string::npos);
    one.reply("beastmaster.dismiss Fang");
    CHECK(one.reply("beastmaster.call Rex").find("comes") != std::string::npos);  // nothing lost, all callable
    at_stable(one);
    one.seconds(1);
    CHECK(one.saved().find("a 0x00000000000000aa Rex") != std::string::npos);  // the one out
    CHECK(one.saved().find("a 0x00000000000000aa Fang") == std::string::npos);
    CHECK(one.saved().find("a 0x00000000000000aa Tusk") == std::string::npos);

    BeastRig three;
    set_level(three, 1, 25);  // three slots
    write(three.data, "b 0x00000000000000aa wolf 0 0 Fang\nb 0x00000000000000aa boar 0 1 Tusk\n"
                      "b 0x00000000000000aa hound 0 0 Rex\nb 0x00000000000000aa sabretooth 0 1 Moss\n");
    three.spawnable(2);
    three.r.fake.session = 2;
    fire_load(1);
    three.seconds(2);
    REQUIRE(three.r.fake.spawns.size() == 2);
    at_stable(three);
    three.seconds(1);
    for (const char* name : {"Tusk", "Moss", "Fang"})
        CHECK(three.saved().find(std::string("a 0x00000000000000aa ") + name) != std::string::npos);
    CHECK(three.saved().find("a 0x00000000000000aa Rex") == std::string::npos);
}

namespace {
// Player 2's beasts from a save: Grey (wolf, active), Hog (boar); a built stable 3 m from player 2 (at 30, 10, 0).
void guest_at_stable(BeastRig& b) {
    write(b.data, "b 0x00000000000000bb wolf 0 0 Grey\nb 0x00000000000000bb boar 0 0 Hog\n"
                  "a 0x00000000000000bb Grey\ns 1\n");
    b.r.fake.session = 2;
    fire_load(1);
    flame_altar(b, 900, 30, 0);
    stable_prop(b, 961, 33, 10, 0);
    b.seconds(32);  // the next search of the world finds it
}
}  // namespace

TEST_CASE("stable: a joined player makes a beast active and stores it by chat near a stable; away from one they are told to go to a stable") {
    BeastRig b;
    guest_at_stable(b);
    CHECK(b.reply("beastmaster.active Hog", 2, 0xBB) == "Hog is active.");
    CHECK(b.saved().find("a 0x00000000000000bb Hog") != std::string::npos);
    CHECK(b.reply("beastmaster.list", 2, 0xBB).find("2. Hog - boar, loyalty 1, at home, active") != std::string::npos);
    CHECK(b.reply("beastmaster.active 2", 2, 0xBB) == "Hog is active already.");
    CHECK(b.reply("beastmaster.store 1", 2, 0xBB) == "Grey goes to the stable.");
    CHECK(b.reply("beastmaster store Grey", 2, 0xBB) == "Grey is in the stable already.");  // a word of /beastmaster
    CHECK(b.saved().find("a 0x00000000000000bb Grey") == std::string::npos);
    place(b.r.fake, 2, 60, 10, 0);  // 27 m from it
    CHECK(b.reply("beastmaster.active Grey", 2, 0xBB) == "Go to a stable (within 10 m) to do that.");
    CHECK(b.reply("beastmaster.active Wolfie", 2, 0xBB) == "No such beast (beastmaster.list shows yours).");
}

TEST_CASE("stable: chat release needs the same command again within 10 s") {
    BeastRig b;
    guest_at_stable(b);
    CHECK(b.reply("beastmaster.release Hog", 2, 0xBB) == "Type the same command again within 10 s to let Hog go for good.");
    b.seconds(11);  // too late: asks again
    CHECK(b.reply("beastmaster.release Hog", 2, 0xBB) == "Type the same command again within 10 s to let Hog go for good.");
    CHECK(b.reply("beastmaster.release Grey", 2, 0xBB).rfind("Type the same command again", 0) == 0);  // another beast
    CHECK(b.reply("beastmaster.release Hog", 2, 0xBB).rfind("Type the same command again", 0) == 0);   // asks again
    CHECK(b.reply("beastmaster.release Hog", 2, 0xBB) == "Hog is gone: you let it go.");
    CHECK(b.saved().find("Hog") == std::string::npos);
    CHECK(b.reply("beastmaster.release Hog", 2, 0xBB) == "No such beast (beastmaster.list shows yours).");
}

TEST_CASE("stable: a change saved more than 2 s of real time after the rig's last tick still lands (the rig's file clock)") {
    // the flake this pins: the world-data file once went by the wall clock, so a slow test machine between two ticks
    // made save_world_data refuse and the file kept the old text
    BeastRig b;
    guest_at_stable(b);
    Sleep(2100);
    CHECK(b.reply("beastmaster.active Hog", 2, 0xBB) == "Hog is active.");
    CHECK(b.saved().find("a 0x00000000000000bb Hog") != std::string::npos);
}

TEST_CASE("stable: a release asked before the world loaded again needs asking again") {
    BeastRig b;
    guest_at_stable(b);
    REQUIRE(b.reply("beastmaster.release Hog", 2, 0xBB).rfind("Type the same command again", 0) == 0);
    b.r.fake.session = 3;  // the world loads again (its data as saved), within the 10 s
    fire_load(1);
    b.seconds(1);
    CHECK(b.reply("beastmaster.release Hog", 2, 0xBB).rfind("Type the same command again", 0) == 0);
    CHECK(b.saved().find("Hog") != std::string::npos);
}

TEST_CASE("stable: a bed out of the host's sight still counts while the server world has it; a guest uses it there") {
    BeastRig b;
    write(b.data, "b 0x00000000000000bb wolf 0 0 Grey\nb 0x00000000000000bb boar 0 0 Hog\n"
                  "a 0x00000000000000bb Grey\ns 1\n");
    b.r.fake.session = 2;
    fire_load(1);
    flame_altar(b, 900, 30, 0);
    stable_prop(b, 961, 33, 10, 0, nullptr);  // the bed; its model is on the client world while the host sees it
    const auto model = guid_bytes(kStableModel);
    bool seen = true;
    auto step_both = [&](int secs) {
        for (int i = 0; i < secs * 60; ++i) {
            if (seen) b.r.fake.templates[{ecs::ModelResource::kHash, 961}] = model;
            b.host->tick(kClient);
            b.r.fake.templates.erase({ecs::ModelResource::kHash, 961});
            b.r.fake.tick += 1;
            b.host->tick(kServer);
        }
    };
    step_both(10);
    REQUIRE(b.reply("beastmaster.stable list").find("1. Animal bed at 33, 10, 0") != std::string::npos);
    place(b.r.fake, 1, 600, 10, 0);  // the host goes far away: the client world no longer shows the bed
    seen = false;
    step_both(40);
    CHECK(b.reply("beastmaster.stable list").find("1. Animal bed at 33, 10, 0") != std::string::npos);
    CHECK(b.reply("beastmaster.active Hog", 2, 0xBB) == "Hog is active.");  // the guest beside it
    remove_entity(b.r.fake, 961);  // taken down while the host is away: gone at the server world's next search
    step_both(35);
    CHECK(b.reply("beastmaster.stable list").rfind("No stable in this world now", 0) == 0);
}

TEST_CASE("stable: before the world has a stable the wheel says what to build; a stored beast's refusal says what a stable is") {
    BeastRig b;
    b.reply("beastmaster.give wolf");
    press(b, VK_F3);
    REQUIRE(b.r.fake.wheels.size() == 1);
    const int hint = index_of(b.r.fake.wheels[0].items, "Stable: animal bed");
    REQUIRE(hint >= 0);
    b.r.fake.wheels[0].cb(kClient, hint, b.r.fake.wheels[0].user);
    b.seconds(1);
    CHECK(noted(b, "Build an animal bed (the straw bed for farm animals) in your base"));
    three_beasts(b);  // a world that has had a stable; Tusk is stored
    at_stable(b);
    b.seconds(1);
    b.r.fake.wheels.clear();
    press(b, VK_F3);
    REQUIRE(b.r.fake.wheels.size() == 1);
    CHECK(index_of(b.r.fake.wheels[0].items, "Stable: animal bed") < 0);
    CHECK(b.reply("beastmaster.call Tusk") ==
          "Tusk is in the stable: make it active at a stable (an animal bed in your base).");
}

// The beasts pack's Stable and Large Stable (tools/models/packs/beastmaster_stable.lua): their render models of their
// own, and the Stable's item.
constexpr const char* kOwnStableModel = "1a49170a-b3d2-498d-9062-8946f6469613";
constexpr const char* kLargeOwnStableModel = "5879f304-0050-4ecb-96f4-5cc0e66155d9";
constexpr uint32_t kStableItem = 0x5AB1E0u;

TEST_CASE("stable: the beasts pack's Stable (its own model) in a base is a stable named Stable") {
    BeastRig b;
    flame_altar(b, 900, 0, 0);
    stable_prop(b, 950, 5, 10, 0, kOwnStableModel);
    b.seconds(32);  // the next search of the world
    CHECK(b.reply("beastmaster.stable list").find("1. Stable at 5, 10, 0 (5 m from you)") != std::string::npos);
}

TEST_CASE("stable: the beasts pack's Large Stable (its own model) in a base is a stable named Large Stable") {
    BeastRig b;
    flame_altar(b, 900, 0, 0);
    stable_prop(b, 950, 5, 10, 0, kLargeOwnStableModel);
    b.seconds(32);
    CHECK(b.reply("beastmaster.stable list").find("1. Large Stable at 5, 10, 0 (5 m from you)") != std::string::npos);
}

TEST_CASE("stable: with the beasts pack's Stable item the hints name the Stable crafted at the Workbench, besides the "
          "animal bed") {
    BeastRig b;
    b.r.fake.item_names[kStableItem] = "Prop_Beastmaster_Stable";
    CHECK(b.reply("beastmaster.stable list") == "No stable in this world yet: build a Stable or a Large Stable (crafted "
                                                 "at the Workbench) or an animal bed in your base, or mark a prop with "
                                                 "beastmaster.stable mark.");
    b.reply("beastmaster.give wolf");
    press(b, VK_F3);
    REQUIRE(b.r.fake.wheels.size() == 1);
    CHECK(index_of(b.r.fake.wheels[0].items, "Stable: animal bed") < 0);
    const int hint = index_of(b.r.fake.wheels[0].items, "Stable: Workbench");
    REQUIRE(hint >= 0);
    b.r.fake.wheels[0].cb(kClient, hint, b.r.fake.wheels[0].user);
    b.seconds(1);
    CHECK(noted(b, "Build a Stable or a Large Stable (crafted at the Workbench) or an animal bed in your base"));
    three_beasts(b);  // Tusk is stored
    at_stable(b);
    b.seconds(1);
    CHECK(b.reply("beastmaster.call Tusk") ==
          "Tusk is in the stable: make it active at a stable (a Stable, a Large Stable or an animal bed in your base).");
}

TEST_CASE("stable: without the beasts pack the Stable's item is looked up at most once a minute (the wheel is built every "
          "second while the world has no stable)") {
    BeastRig b;
    b.reply("beastmaster.give wolf");
    b.seconds(130);
    CHECK(b.r.fake.item_lookups["Prop_Beastmaster_Stable"] >= 1);
    CHECK(b.r.fake.item_lookups["Prop_Beastmaster_Stable"] <= 4);
}

TEST_CASE("stable: a player whose level cannot be read yet keeps their beasts out (no slot trimming on a level of 0)") {
    BeastRig b;
    set_level(b, 1, 25);  // three slots
    write(b.data, "b 0x00000000000000aa wolf 0 1 Fang\nb 0x00000000000000aa boar 0 1 Tusk\n"
                  "b 0x00000000000000aa hound 0 1 Rex\n");
    b.spawnable(3);
    b.r.fake.components.erase({ecs::Level::kHash, 1});  // not there yet (a player who just joined or loaded)
    b.r.fake.session = 2;
    fire_load(1);
    b.seconds(3);
    CHECK(b.r.fake.spawns.size() == 3);
    CHECK(b.reply("beastmaster.list").find("goes home") == std::string::npos);
    set_level(b, 1, 25);
    b.seconds(2);
    CHECK(b.reply("beastmaster.list").find("3. Rex - hound, loyalty 1, out") != std::string::npos);
}
TEST_CASE("beastmaster: each beast out has its own Dismiss item, Dismiss all comes with two out; /beastmaster dismiss alone sends all home") {
    BeastRig b;
    b.reply("beastmaster.give wolf");
    b.reply("beastmaster.give boar");
    b.spawnable(2);
    CHECK(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    b.seconds(11);
    CHECK(b.reply("beastmaster.call Boar").find("comes") != std::string::npos);
    b.seconds(2);
    press(b, VK_F3);
    REQUIRE(b.r.fake.wheels.size() == 1);
    const auto w = b.r.fake.wheels[0];
    for (const char* a : {"Dismiss Wolf", "Dismiss Boar", "Dismiss all"}) CHECK_MESSAGE(index_of(w.items, a) >= 0, a);
    w.cb(kClient, index_of(w.items, "Dismiss Wolf"), w.user);
    b.seconds(1);
    CHECK(b.noted("Wolf goes home"));
    const std::string list = b.reply("beastmaster.list");
    CHECK(list.find("Wolf - wolf, loyalty 1, at home") != std::string::npos);
    CHECK(list.find("Boar - boar, loyalty 1, out") != std::string::npos);
    press(b, VK_F3);  // one out: its own item, no Dismiss all
    REQUIRE(b.r.fake.wheels.size() == 2);
    CHECK(index_of(b.r.fake.wheels[1].items, "Dismiss Boar") >= 0);
    CHECK(index_of(b.r.fake.wheels[1].items, "Dismiss all") < 0);
    CHECK(index_of(b.r.fake.wheels[1].items, "Dismiss Wolf") < 0);
    CHECK(b.reply("beastmaster.dismiss").find("Boar goes home") != std::string::npos);  // no name: all
    CHECK(b.reply("beastmaster.dismiss all").find("No beast is out") != std::string::npos);
    // the help says how
    const std::string help = b.reply("beastmaster");
    CHECK(help.find("dismiss") != std::string::npos);
    CHECK(help.find("all") != std::string::npos);
    bool told = false;
    for (const auto& c : b.r.fake.commands)
        if (c.name == "beastmaster.dismiss") told = c.help.find("all") != std::string::npos && c.help.find("nothing") != std::string::npos;
    CHECK(told);
}

TEST_CASE("beastmaster: a guest's /beastmaster call acts on the guest's own beasts") {
    BeastRig b;
    b.reply("beastmaster.give wolf");  // the host's Wolf
    write(b.data, "b 0x00000000000000aa wolf 0 0 Wolf\nb 0x00000000000000bb boar 0 0 Grunt\n");
    b.r.fake.session = 2;
    fire_load(1);
    b.seconds(1);
    b.spawnable(1);
    CHECK(b.reply("beastmaster.call Wolf", 2, 0xBB).find("No such beast") != std::string::npos);  // not theirs
    CHECK(b.reply("beastmaster.call Grunt", 2, 0xBB).find("comes") != std::string::npos);
    REQUIRE(b.r.fake.spawns.size() == 1);
    CHECK(std::abs(b.r.fake.spawns[0].x - 30.0) <= 3.0);  // beside the guest at (30, 10, 0)
    CHECK(b.added(2, EMF_ATTR_MANA, -15));
}

namespace {
constexpr const char* kBearTemplate = "b3a7c2e1-5f4d-4c8b-9e21-7a0d4b6e2f02";
constexpr const char* kAurochsTemplate = "d5f2b8e3-7c4a-4e9f-8b21-6a3c8d4e0f02";  // the beasts pack's
size_t logged(const Fake& f, const std::string& needle) {
    size_t n = 0;
    for (const auto& l : f.log) n += l.find(needle) != std::string::npos;
    return n;
}
// The beasts pack's rare snakes (a wild snake is one now and then): missing, for the tests of the plain picks.
void no_rare_snakes(Fake& f) {
    for (const char* t : {"d4c87337-8c53-41df-8488-8079046e1512", "d4c87337-8c53-41df-8488-8079046e1522",
                          "d4c87337-8c53-41df-8488-8079046e1532"})
        f.missing_templates.insert(guid_bytes(t));
}
}  // namespace

TEST_CASE("wild bears: one spawns 50-70 m from a player when none exists; none when the pack is missing") {
    BeastRig b({{"wild_bear_minutes", "1"}});
    b.r.fake.missing_templates.insert(guid_bytes(kAurochsTemplate));  // only the bear pack: the bear every time
    b.r.fake.missing_templates.insert(guid_bytes("d4c87337-8c53-41df-8488-8079046e1502"));  // (no beasts pack snake)
    no_rare_snakes(b.r.fake);
    b.spawnable(3);
    b.seconds(30);
    CHECK(b.r.fake.spawns.empty());  // not before a minute of play
    b.seconds(31);
    REQUIRE(b.r.fake.spawns.size() == 1);
    component<ecs::ScaleComponent>(b.r.fake, 1234).uniformScale = 1.4f;  // the pack's bear (seen in game: 1.40)
    const auto s = b.r.fake.spawns[0];
    CHECK(guid_text(s.guid) == kBearTemplate);
    CHECK(s.y == doctest::Approx(12.0));  // the player's height + 2 m
    const double d1 = std::hypot(s.x - 0.0, s.z - 0.0), d2 = std::hypot(s.x - 30.0, s.z - 0.0);
    CHECK(((d1 >= 50.0 && d1 <= 70.0) || (d2 >= 50.0 && d2 <= 70.0)));
    CHECK(teleports_of(b.r.fake, 1234) == 0);  // not at once: the game refuses a creature that is not set up yet
    b.seconds(2);
    REQUIRE(teleports_of(b.r.fake, 1234) == 1);  // then onto the ground there
    CHECK((b.r.fake.teleports.back().flags & EMF_TELEPORT_FIND_GROUND) != 0);
    CHECK(b.faction(1234) == kWildBeast);  // wild: tamed like any beast
    CHECK(b.r.fake.components[{ecs::Level::kHash, 1234}][8] == 10);  // at the player's level
    b.seconds(60);
    CHECK(b.r.fake.spawns.size() == 1);  // one at a time
    b.r.fake.attrs[{1234, EMF_ATTR_HEALTH}] = 0;  // killed
    b.seconds(61);
    CHECK(b.r.fake.spawns.size() == 2);

    // without the bear pack the game has no bear: wild bears switch off, with one log line
    BeastRig m({{"wild_bear_minutes", "1"}});
    m.r.fake.spawn_fails = true;
    m.seconds(200);
    CHECK(m.r.fake.spawns.empty());
    CHECK(logged(m.r.fake, "wild bears") == 1);

    // and the setting switches them off
    BeastRig off({{"wild_bears", "false"}, {"wild_bear_minutes", "1"}});
    off.spawnable(1);
    off.seconds(200);
    CHECK(off.r.fake.spawns.empty());
}

// Health's attribute storage (seen in game): [0] health, [1] min, [2] Health_Max_Base (the game sets it from the level),
// [3] Health_Max_Adder, ..., [7] the maximum (base + adder, computed by the game).
TEST_CASE("beastmaster: a called bear's health adder keeps its maximum at its role, 2.2x the game's base, also after a level change; a wolf at 1.2x") {
    BeastRig b({{"beast_health", "100"}, {"beast_damage", "100"}});
    b.reply("beastmaster.give bear");
    b.reply("beastmaster.give wolf");
    b.spawnable(2);
    auto& bear = component<ecs::Health>(b.r.fake, 1234);
    auto& wolf = component<ecs::Health>(b.r.fake, 1235);
    CHECK(b.reply("beastmaster.call Bear").find("comes") != std::string::npos);
    b.seconds(2);
    CHECK(bear.dataStorage[3] == 0);  // no base yet: nothing to multiply
    bear.dataStorage[2] = 34;         // the game's base at level 1 (the earlygame wolf's)
    b.seconds(1);
    CHECK(bear.dataStorage[3] == 41);  // 34 x 1.2 = 40.8: 75 = 2.2 x 34 (a tamed bear's role, not the wild 2.5)
    bear.dataStorage[2] = 65;          // the game rescaled it to a new level
    b.seconds(2);
    CHECK(bear.dataStorage[3] == 78);  // 65 x 1.2
    wolf.dataStorage[2] = 34;
    b.seconds(11);
    CHECK(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    b.seconds(2);
    CHECK(wolf.dataStorage[3] == 7);  // 34 x 0.2 = 6.8: the wolf's role, 1.2x
}

TEST_CASE("beastmaster: /beastmaster <word> in the chat runs the players' commands; no word or an unknown one lists them") {
    BeastRig b;
    write(b.data, "b 0x00000000000000bb boar 0 0 Grunt\n");
    b.r.fake.session = 2;
    fire_load(1);
    b.seconds(1);
    b.spawnable(1);
    CHECK(b.reply("beastmaster list", 2, 0xBB).find("1. Grunt - boar") != std::string::npos);
    CHECK(b.reply("beastmaster call Grunt", 2, 0xBB).find("comes") != std::string::npos);
    REQUIRE(b.r.fake.spawns.size() == 1);
    CHECK(std::abs(b.r.fake.spawns[0].x - 30.0) <= 3.0);  // beside the guest
    for (const char* asked : {"beastmaster", "beastmaster fly"}) {
        const std::string help = b.reply(asked, 2, 0xBB);
        for (const char* word : {"list", "tame", "feed", "call", "dismiss", "attack"})
            CHECK_MESSAGE(help.find(word) != std::string::npos, asked, " ", word);
        CHECK(help.find("give") == std::string::npos);  // the host's console only
    }
}

TEST_CASE("beastmaster: with nothing under the crosshair, tame takes the nearest weakened beast within 8 m and feed the nearest beast") {
    BeastRig b;
    b.r.fake.item_names[kLeanMeat] = "Food_T1_raw_bad_meat_LeanMeat";
    b.look(1, 0, 10.5, 0, 180);                              // looking away from them all (-z)
    b.beast(50, kWolfGraph, 3, 1, kWildBeast, 100, 100);    // beside you, healthy
    b.beast(51, kWolfGraph, -4, 2, kWildBeast, 20, 100);    // beside you, beaten down
    b.beast(52, kWolfGraph, 0, 12, kWildBeast, 10, 100);    // beaten down, but 12 m away
    CHECK(b.reply("beastmaster.tame").find("Taming the wolf") != std::string::npos);
    b.seconds(4);
    CHECK(b.faction(51) == kPlayerAlly);
    CHECK(b.faction(50) == kWildBeast);
    CHECK(b.faction(52) == kWildBeast);
    CHECK(b.reply("beastmaster.feed").find("You offer the wolf") != std::string::npos);  // the healthy one beside you
    REQUIRE(b.r.fake.takes.size() == 1);
    b.r.fake.attrs[{50, EMF_ATTR_HEALTH}] = 100;
    place(b.r.fake, 50, 0, 10, 20);                          // gone far away: nothing near to tame
    b.seconds(11);
    CHECK(b.reply("beastmaster.tame").find("no beast to tame") != std::string::npos);
}

TEST_CASE("beastmaster: attack with nothing under the crosshair goes for the creature that hit you in the last 10 s") {
    BeastRig b;
    wolf_out(b);
    b.look(1, 0, 10.5, 0, 180);
    b.beast(60, "00000000-0000-0000-0000-000000000099", 0, 6, 4 /* Scavenger */, 100, 100);
    armed(b, 1234);
    CHECK(b.reply("beastmaster.attack").find("No monster where you look") != std::string::npos);  // behind; nothing hit you
    deliver(b.r.fake, "HitEvent", hit_event(60, 1));
    b.seconds(1);
    CHECK(b.reply("beastmaster.attack").find("Your beasts attack") != std::string::npos);
    CHECK(ordered(b, 1234) == 60u);
    b.seconds(11);
    CHECK(b.reply("beastmaster.attack").find("No monster where you look") != std::string::npos);  // forgotten after 10 s
}

TEST_CASE("beastmaster: a bear is the wolf's graph at 1.4x scale (the pack's ScaleComponent); a wolf, or a reused id, is a wolf") {
    BeastRig b;
    b.beast(1234, kWolfGraph, 2, 2, kWildBeast, 20, 100);  // a wild bear: it came to you, and you beat it down
    component<ecs::ScaleComponent>(b.r.fake, 1234).uniformScale = 1.4f;
    b.look(1, 0, 10.5, 0, 45);
    CHECK(b.reply("beastmaster.tame").find("Taming the bear") != std::string::npos);
    b.seconds(4);
    CHECK(b.reply("beastmaster.list").find("1. Bear - bear, loyalty 1, out") != std::string::npos);
    b.beast(70, kWolfGraph, -2, 2, kWildBeast, 20, 100);  // a game wolf: still a wolf
    CHECK(b.reply("beastmaster.tame").find("Taming the wolf") != std::string::npos);
}

TEST_CASE("beastmaster: the bear pack 2's bear is its own graph (its own skeleton) at 1.4x; the older pack's is the wolf's") {
    constexpr const char* kBearGraph = "b3a7c2e1-5f4d-4c8b-9e21-7a0d4b6e2f05";
    BeastRig b;
    CHECK(b.reply(std::string("beastmaster.species ") + kBearGraph + " 6 1.4") == "bear");
    CHECK(b.reply(std::string("beastmaster.species ") + kWolfGraph + " 6 1.4") == "bear");  // the bear pack 1.0
    CHECK(b.reply(std::string("beastmaster.species ") + kWolfGraph + " 6 1") == "wolf");
    b.beast(60, kBearGraph, 2, 2, kWildBeast, 20, 100);
    component<ecs::ScaleComponent>(b.r.fake, 60).uniformScale = 1.4f;
    b.look(1, 0, 10.5, 0, 45);
    CHECK(b.reply("beastmaster.tame").find("Taming the bear") != std::string::npos);
    b.seconds(4);
    CHECK(b.reply("beastmaster.list").find("1. Bear - bear, loyalty 1, out") != std::string::npos);
}

TEST_CASE("beastmaster: a creature that gets a dead wild bear's id is not a bear") {
    BeastRig b({{"wild_bear_minutes", "1"}});
    b.spawnable(1);
    b.seconds(61);
    REQUIRE(b.r.fake.spawns.size() == 1);
    deliver(b.r.fake, "EntityDiedEvent", died_event(1234));  // the wild bear dies; the game gives its id to a wolf
    b.beast(1234, kWolfGraph, 2, 2, kWildBeast, 20, 100);
    b.look(1, 0, 10.5, 0, 45);
    CHECK(b.reply("beastmaster.tame").find("Taming the wolf") != std::string::npos);
}

TEST_CASE("beastmaster: a calmed beast that is out of range for under 2 s (it was fleeing) is still tamed; 2 s breaks it") {
    BeastRig b;
    b.look(1, 0, 10.5, 0, 0);
    b.beast(50, kWolfGraph, 0, 5, kWildBeast, 20, 100);
    CHECK(b.reply("beastmaster.tame").find("Taming") != std::string::npos);
    place(b.r.fake, 50, 0, 10, 10);  // it runs on for a moment: 10 m
    b.seconds(1);
    place(b.r.fake, 50, 0, 10, 2);   // and comes to you
    b.seconds(3);
    CHECK(b.noted("You tamed a wolf"));
    CHECK(!b.noted("too far"));
    b.beast(51, kWolfGraph, 0, 4, kWildBeast, 20, 100);
    CHECK(b.reply("beastmaster.tame").find("Taming") != std::string::npos);
    place(b.r.fake, 51, 0, 10, 12);
    b.seconds(3);
    CHECK(b.noted("too far"));
    CHECK(b.faction(51) == kWildBeast);
}

// ---- edge cases ----

TEST_CASE("beastmaster: no XP credit (no killing blow) on bosses, mini-bosses or the player side") {
    BeastRig b;
    wolf_out(b);
    b.beast(62, "00000000-0000-0000-0000-000000000099", 0, 7, 3 /* Vukah */, 20, 100);  // a boss at 20 %
    component<ecs::Boss>(b.r.fake, 62);
    b.beast(63, "00000000-0000-0000-0000-000000000099", 0, 8, 3, 20, 100);  // a mini-boss (its boss bar)
    component<ecs::BossHealthBar>(b.r.fake, 63);
    b.beast(64, "00000000-0000-0000-0000-000000000099", 0, 9, 7 /* Player */, 20, 100);
    b.beast(65, "00000000-0000-0000-0000-000000000099", 0, 10, 8 /* AutomatedPlayer */, 20, 100);
    for (uint32_t target : {62u, 63u, 64u, 65u}) deliver(b.r.fake, "HitEvent", hit_event(1234, target));
    b.seconds(1);
    CHECK(b.r.fake.damages.empty());
}

TEST_CASE("beastmaster: a second tame or feed during a channel turns the first calmed beast wild again") {
    BeastRig b;
    b.r.fake.item_names[kLeanMeat] = "Food_T1_raw_bad_meat_LeanMeat";
    b.look(1, 0, 10.5, 0, 0);
    b.beast(50, kWolfGraph, 0, 5, kWildBeast, 20, 100);   // under the crosshair
    b.beast(51, kWolfGraph, 3, -2, kWildBeast, 20, 100);  // beside you
    CHECK(b.reply("beastmaster.tame").find("Taming") != std::string::npos);
    CHECK(b.faction(50) == kPlayerAlly);  // calmed
    b.look(1, 0, 10.5, 0, 180);           // you turn: 50 is no longer under the crosshair; 51 is the nearest wild one
    CHECK(b.reply("beastmaster.tame").find("Taming") != std::string::npos);
    CHECK(b.faction(51) == kPlayerAlly);
    CHECK(b.faction(50) == kWildBeast);   // not left a stray ally
    CHECK(b.reply("beastmaster.feed").find("You offer the wolf") != std::string::npos);  // 50, wild again and nearest
    CHECK(b.faction(51) == kWildBeast);   // one channel at a time: feeding ends the taming of 51
}

TEST_CASE("beastmaster: a new world drops the old world's taming channel, and the HUD keeps up when the game clock restarts") {
    BeastRig b;
    b.look(1, 0, 10.5, 0, 0);
    b.beast(50, kWolfGraph, 0, 5, kWildBeast, 20, 100);
    CHECK(b.reply("beastmaster.tame").find("Taming") != std::string::npos);
    b.reply("beastmaster.give boar");
    b.spawnable(1);
    CHECK(b.reply("beastmaster.call Boar").find("comes") != std::string::npos);
    b.seconds(1);
    CHECK(b.hud("out").find("Boar (out)") != std::string::npos);
    b.r.fake.session = 2;  // another world: entity 50 is someone else there, and the clock starts again
    b.r.fake.tick = 0;
    component<ecs::Faction>(b.r.fake, 50).faction = static_cast<emf::gen::FactionType>(kWildBeast);
    fire_load(1);
    b.seconds(4);
    CHECK(b.faction(50) == kWildBeast);  // not tamed, not touched
    CHECK(!b.noted("You tamed"));
    b.reply("beastmaster.dismiss all");
    b.seconds(2);
    CHECK(b.hud("out").find("Boar (out)") == std::string::npos);
}

TEST_CASE("beastmaster: a remembered id that now guards someone else is not ours any more; a body is removed only while dead") {
    BeastRig b;
    wolf_out(b);
    b.seconds(1);
    component<ecs::DynamicEnemy01BehaviorState>(b.r.fake, 1234).guardEntityId.id = 999;  // another entity's guard now
    b.seconds(2);
    CHECK(b.reply("beastmaster.list").find("Wolf - wolf, loyalty 1, out") != std::string::npos);  // not at the first look
    b.seconds(2);
    CHECK(b.reply("beastmaster.list").find("Wolf - wolf, loyalty 1, at home") != std::string::npos);
    b.r.fake.destroyed.clear();
    b.reply("beastmaster.dismiss all");
    CHECK(!contains(b.r.fake.destroyed, 1234));
    // a beast falls; before its body goes, the game gives the id to a living creature
    b.spawnable(1);
    b.seconds(11);
    CHECK(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    deliver(b.r.fake, "EntityDiedEvent", died_event(1235));
    b.r.fake.attrs[{1235, EMF_ATTR_HEALTH}] = 100;  // alive: another creature
    b.seconds(3);
    CHECK(!contains(b.r.fake.destroyed, 1235));
}

// EMF 0.6.8 move orders: another mod may walk a beast somewhere (it guards a hidden waypoint meanwhile): Beastmaster
// neither takes it for another creature nor sends it back to its owner while it moves.
TEST_CASE("beastmaster: a beast on another mod's move order is left alone (not dropped, not sent back to its owner)") {
    BeastRig b;
    wolf_out(b);
    b.seconds(1);
    b.r.fake.moving[1234] = 7;
    component<ecs::DynamicEnemy01BehaviorState>(b.r.fake, 1234).guardEntityId.id = 999;  // the move's waypoint
    const size_t before = b.guards().size();
    b.seconds(5);
    CHECK(b.reply("beastmaster.list").find("Wolf - wolf, loyalty 1, out") != std::string::npos);
    const auto g = b.guards();
    for (size_t i = before; i < g.size(); ++i) CHECK(g[i].first != 1234u);
    b.r.fake.moving.erase(1234);  // the move ended: it guards its owner again (EMF does that); a stray look starts over
    component<ecs::DynamicEnemy01BehaviorState>(b.r.fake, 1234).guardEntityId.id = 1;
    b.seconds(2);
    CHECK(b.reply("beastmaster.list").find("Wolf - wolf, loyalty 1, out") != std::string::npos);
}

// A beast on a move is not leashed, so a far move can take it out of the world (unloaded); Beastmaster counted it home.
TEST_CASE("beastmaster: a beast that leaves the world on another mod's move stays out and comes back beside its owner") {
    BeastRig b;
    wolf_out(b);
    b.seconds(1);
    b.r.fake.moving[1234] = 7;
    component<ecs::DynamicEnemy01BehaviorState>(b.r.fake, 1234).guardEntityId.id = 999;  // the move's waypoint
    b.seconds(2);
    remove_entity(b.r.fake, 1234);  // far away: unloaded; EMF's move ends (gone)
    b.r.fake.moving.erase(1234);
    b.spawnable(1);
    b.seconds(1);
    CHECK(b.reply("beastmaster.list").find("Wolf - wolf, loyalty 1, out") != std::string::npos);
    b.seconds(1);
    REQUIRE(b.r.fake.spawns.size() == 2);
    CHECK(std::abs(b.r.fake.spawns[1].x - 2.0) < 0.01);  // beside its owner at (0, 10, 0)
    CHECK(b.reply("beastmaster.list").find("Wolf - wolf, loyalty 1, out") != std::string::npos);
}

// Spirimtin (2026-11-22): "The wolf does not always come back when I glide. Sometimes I need to call it." A beast left
// far behind (a long glide, a fast travel) is unloaded by the game: its creature is gone, and it went home.
TEST_CASE("beastmaster (1.20): a beast whose creature the game removed (left far behind, unloaded) stays out and comes back beside its owner") {
    BeastRig b;
    wolf_out(b);
    b.seconds(1);
    remove_entity(b.r.fake, 1234);  // unloaded far behind its owner, no death
    b.spawnable(1);
    b.seconds(1);
    CHECK(b.reply("beastmaster.list").find("Wolf - wolf, loyalty 1, out") != std::string::npos);
    b.seconds(1);
    REQUIRE(b.r.fake.spawns.size() == 2);
    CHECK(std::abs(b.r.fake.spawns[1].x - 2.0) < 0.01);  // beside its owner at (0, 10, 0)
    CHECK(b.faction(1235) == kPlayerAlly);
}

// Review: the game gives ids out again. A creature that is not ours dying on the old id is no fall of ours, and its body
// (its loot) is left alone.
TEST_CASE("beastmaster (1.20): another creature dying on a gone beast's old id is no fall, and its body is not removed") {
    BeastRig b;
    wolf_out(b);
    b.seconds(1);
    remove_entity(b.r.fake, 1234);  // unloaded behind its owner
    b.spawnable(1);
    b.seconds(1);                   // the check: gone, it stays out
    creature(b, 1234, 5, 5, kWildBeast);  // the id given to a wild beast, which dies at once
    b.r.fake.attrs[{1234, EMF_ATTR_HEALTH}] = 0;
    deliver(b.r.fake, "EntityDiedEvent", died_event(1234));
    b.seconds(2);
    CHECK_FALSE(b.noted("Wolf fell"));
    CHECK_FALSE(contains(b.r.fake.destroyed, 1234));  // its loot stays
    CHECK(b.r.fake.spawns.size() == 2);                // back without a rest
    CHECK(b.reply("beastmaster.list").find("Wolf - wolf, loyalty 1, out") != std::string::npos);
}

// Review: on a fast travel the new place loads many creatures at once; one may get the old id before the check looks.
TEST_CASE("beastmaster (1.20): a beast whose id a new creature took before the check (its SpawnTime differs) comes back beside its owner") {
    BeastRig b;
    b.reply("beastmaster.give wolf");
    next_beast(b, 1234);
    REQUIRE(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    b.seconds(2);  // its SpawnTime is read
    component<ecs::SpawnTime>(b.r.fake, 1234).spawnTime.value = kSpawnNs + 5000000000LL;  // another creature now
    component<ecs::Faction>(b.r.fake, 1234).faction = static_cast<emf::gen::FactionType>(kWildBeast);
    b.spawnable(1);
    b.seconds(2);
    REQUIRE(b.r.fake.spawns.size() == 2);
    CHECK(b.reply("beastmaster.list").find("Wolf - wolf, loyalty 1, out") != std::string::npos);
    CHECK(b.faction(1234) == kWildBeast);  // the stranger is left as it is
}

TEST_CASE("beastmaster (1.20): a beast the game takes out of the world again and again right after each call stays home") {
    BeastRig b;
    wolf_out(b);
    for (uint32_t id = 1234; id < 1237; ++id) {
        b.seconds(1);
        remove_entity(b.r.fake, id);  // gone each time within seconds of its spawn
        b.spawnable(1);
        b.seconds(1);
    }
    b.seconds(2);
    CHECK(b.noted("Wolf stays home: the game keeps taking it out of the world"));
    CHECK(b.reply("beastmaster.list").find("Wolf - wolf, loyalty 1, at home") != std::string::npos);
}

// Spirimtin: "Sometimes when it dies it's not coming back after rest." A death that takes the body at once leaves no
// dead creature for the check to see; the game's death event comes a step or two later.
TEST_CASE("beastmaster (1.20): a beast whose body is gone before the death event arrives still rests and comes back after") {
    BeastRig b({{"rest_s", "10"}});
    wolf_out(b);
    b.seconds(1);
    remove_entity(b.r.fake, 1234);  // died, its body gone at once
    b.spawnable(2);
    b.seconds(1);                   // the check sees no creature: it stays out
    deliver(b.r.fake, "EntityDiedEvent", died_event(1234));  // the game's event, late
    b.seconds(1);
    CHECK(b.noted("Wolf fell; it comes back in"));
    CHECK(b.reply("beastmaster.list").find("then comes back") != std::string::npos);
    CHECK(b.r.fake.spawns.size() == 1);  // not back before its rest
    b.seconds(10);
    REQUIRE(b.r.fake.spawns.size() == 2);
    CHECK(b.noted("Wolf is back"));
}

TEST_CASE("beastmaster: an unknown bait name gives a note and the mod keeps running") {
    BeastRig b({{"bait_wolf", "No_Such_Item"}});
    b.look(1, 0, 10.5, 0, 0);
    b.beast(50, kWolfGraph, 0, 5, kWildBeast, 100, 100);
    CHECK(b.reply("beastmaster.feed").find("No_Such_Item") != std::string::npos);
    CHECK(b.host->status(b.id) == "running");
    CHECK(b.reply("beastmaster.list").find("no beasts") != std::string::npos);
}

TEST_CASE("wild bears: none near a base (40 m of a flame altar), neither by a player there nor at the spot") {
    BeastRig b({{"wild_bear_minutes", "1"}});
    b.spawnable(2);
    place(b.r.fake, 900, 15, 10, 0);  // a flame altar between the two players
    component<ecs::BaseIdComponent>(b.r.fake, 900);
    add_entity(b.r.fake, 900);
    b.seconds(130);
    CHECK(b.r.fake.spawns.empty());  // both players are at the base
    place(b.r.fake, 1, 0, 10, -300);  // player 1 goes out: a bear may come, but never within 40 m of the altar
    place(b.r.fake, 2, 30, 10, -300);
    b.seconds(61);
    REQUIRE(b.r.fake.spawns.size() == 1);
    const auto s = b.r.fake.spawns[0];
    CHECK(std::hypot(s.x - 15.0, s.z - 0.0) > 40.0);
}

TEST_CASE("beastmaster: a wheel pick calls the beast that was shown, even when the list moved meanwhile (shared and own wheel)") {
    for (const char* shared : {"true", "false"}) {
        BeastRig b({{"shared_wheel", shared}});
        b.reply("beastmaster.give wolf");
        b.reply("beastmaster.give boar");
        b.spawnable(2);
        b.seconds(2);
        press(b, VK_F3);
        REQUIRE(b.r.fake.wheels.size() == 1);
        const auto w = b.r.fake.wheels[0];
        const int boar = index_of(w.items, "Call Boar");
        REQUIRE(boar >= 0);
        CHECK(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);  // meanwhile: the list moves up
        b.seconds(11);                                                                 // past the call cooldown
        w.cb(kClient, boar, w.user);
        b.seconds(1);
        REQUIRE_MESSAGE(b.r.fake.spawns.size() == 2, shared);
        CHECK_MESSAGE(guid_text(b.r.fake.spawns[1].guid) == "255fd8d9-4447-445d-8ffc-4d9dc0afdd16", shared);  // the boar
    }
}

// ---- new beasts (the beasts pack): the Aurochs ----
namespace {
constexpr const char* kBoarGraph = "aed0ecec-8a98-4fd3-a450-ab4c508b0ad4";
}  // namespace

TEST_CASE("beastmaster: an aurochs is the boar graph at 1.2x or more (WildBeast); a game boar or a scaled hound pet is not") {
    BeastRig b({{"beast_health", "100"}, {"beast_damage", "100"}});
    CHECK(b.reply(std::string("beastmaster.species ") + kBoarGraph + " 6 1.3") == "aurochs");
    CHECK(b.reply(std::string("beastmaster.species ") + kBoarGraph + " 6") == "boar");
    CHECK(b.reply(std::string("beastmaster.species ") + kBoarGraph + " 3 1.3") == "unknown");  // a Vukah's big boar
    CHECK(b.reply(std::string("beastmaster.species ") + kWolfGraph + " 4 1.25") == "hound");     // the scavengers' pet
    b.beast(50, kBoarGraph, 0, 5, kWildBeast, 20, 100);
    component<ecs::ScaleComponent>(b.r.fake, 50).uniformScale = 1.3f;
    b.look(1, 0, 10.5, 0, 0);
    CHECK(b.reply("beastmaster.tame").find("Taming the aurochs") != std::string::npos);
    b.seconds(4);
    CHECK(b.reply("beastmaster.list").find("1. Aurochs - aurochs, loyalty 1, out") != std::string::npos);
    // called: the pack's template, with 3x health
    b.reply("beastmaster.dismiss all");
    b.spawnable(1);
    b.seconds(11);
    CHECK(b.reply("beastmaster.call Aurochs").find("comes") != std::string::npos);
    REQUIRE(b.r.fake.spawns.size() == 1);
    CHECK(guid_text(b.r.fake.spawns[0].guid) == kAurochsTemplate);
    auto& h = component<ecs::Health>(b.r.fake, 1234);
    h.dataStorage[2] = 40;
    b.seconds(1);
    CHECK(h.dataStorage[3] == 40);  // 40 + 40 = 80 = 2 x 40: a tamed aurochs' role (a wild one has 3x)
}

TEST_CASE("beastmaster: a beast whose pack is missing goes home with one note; it is not called again every second") {
    BeastRig b;
    write(b.data, "b 0x00000000000000aa aurochs 0 1 Aurochs\n");  // saved as out
    b.r.fake.session = 2;
    b.r.fake.spawn_fails = true;  // this game has no aurochs (the beasts pack is not installed)
    fire_load(1);
    b.seconds(5);
    size_t notes = 0;
    for (const auto& n : b.r.fake.notes) notes += n.find("Aurochs") != std::string::npos;
    CHECK(notes == 1);
    CHECK(b.reply("beastmaster.list").find("Aurochs - aurochs, loyalty 1, at home") != std::string::npos);
    CHECK(b.reply("beastmaster.call Aurochs").find("pack") != std::string::npos);
}

TEST_CASE("beastmaster: a pack beast that cannot come says it comes with the EMF creature packs and how to install them") {
    BeastRig b;
    b.reply("beastmaster.give bear");
    b.r.fake.spawn_fails = true;  // no bear in this game: no pack, or its content was not built
    const std::string call = b.reply("beastmaster.call Bear");
    CHECK(call.find("EMF creature packs") != std::string::npos);
    CHECK(call.find("Install them with EMF.exe") != std::string::npos);
    CHECK(call.find("need EML") == std::string::npos);  // EMF builds the packs itself (0.4.0)
    CHECK(call.find("Install EML") == std::string::npos);
    CHECK(call.find("emf_beastmaster") == std::string::npos);  // no folder names
    write(b.data, "b 0x00000000000000aa bear 0 1 Bear\n");  // saved as out
    b.r.fake.session = 2;
    fire_load(1);
    b.seconds(5);
    std::string note;
    for (const auto& n : b.r.fake.notes)
        if (n.find("Bear") != std::string::npos) note = n;
    CHECK(note.find("stays home") != std::string::npos);
    CHECK(note.find("EMF creature packs") != std::string::npos);
    CHECK(note.find("EML") == std::string::npos);
}

TEST_CASE("wild beasts: with only the beasts pack installed, wild aurochs and snakes come (the missing bear is skipped)") {
    BeastRig b({{"wild_bear_minutes", "1"}});
    b.r.fake.missing_templates.insert(guid_bytes(kBearTemplate));
    no_rare_snakes(b.r.fake);
    b.spawnable(8);
    for (int round = 0; round < 4; ++round) {
        const size_t before = b.r.fake.spawns.size();
        b.seconds(61);
        REQUIRE(b.r.fake.spawns.size() == before + 1);
        const std::string spawned = guid_text(b.r.fake.spawns.back().guid);
        const bool aurochs = spawned == kAurochsTemplate;
        CHECK((aurochs || spawned == "d4c87337-8c53-41df-8488-8079046e1502"));  // the pack's aurochs or snake
        const uint32_t id = 1234 + static_cast<uint32_t>(before);
        component<ecs::ScaleComponent>(b.r.fake, id).uniformScale = aurochs ? 1.3f : 1.0f;  // the pack's sizes
        CHECK(b.faction(id) == kWildBeast);
        b.seconds(60);
        CHECK(b.r.fake.spawns.size() == before + 1);  // one at a time: it is still about
        b.r.fake.attrs[{id, EMF_ATTR_HEALTH}] = 0;     // killed
    }
    CHECK(logged(b.r.fake, "wild bears") <= 1);
}

// ---- the beasts pack's snake: a skeleton of its own, played by its own copy of the wolf's graph ----
namespace {
constexpr const char* kSnakeGraph = "d4c87337-8c53-41df-8488-8079046e1505";
constexpr const char* kSnakeTemplate = "d4c87337-8c53-41df-8488-8079046e1502";
}  // namespace

TEST_CASE("beastmaster: a snake is its own graph (WildBeast): tamed, listed, called with the pack's template and its role's health") {
    BeastRig b({{"beast_health", "100"}, {"beast_damage", "100"}});
    CHECK(b.reply(std::string("beastmaster.species ") + kSnakeGraph + " 6 1") == "snake");
    CHECK(b.reply(std::string("beastmaster.species ") + kSnakeGraph + " 6") == "snake");
    b.beast(50, kSnakeGraph, 0, 5, kWildBeast, 20, 100);
    component<ecs::ScaleComponent>(b.r.fake, 50).uniformScale = 1.0f;
    b.look(1, 0, 10.5, 0, 0);
    CHECK(b.reply("beastmaster.tame").find("Taming the snake") != std::string::npos);
    b.seconds(4);
    CHECK(b.reply("beastmaster.list").find("1. Snake - snake, loyalty 1, out") != std::string::npos);
    b.reply("beastmaster.dismiss all");
    b.spawnable(1);
    b.seconds(11);
    CHECK(b.reply("beastmaster.call Snake").find("comes") != std::string::npos);
    REQUIRE(b.r.fake.spawns.size() == 1);
    CHECK(guid_text(b.r.fake.spawns[0].guid) == kSnakeTemplate);
    auto& h = component<ecs::Health>(b.r.fake, 1234);
    h.dataStorage[2] = 40;
    b.seconds(1);
    CHECK(h.dataStorage[3] == 8);  // 40 x 0.2: a tamed snake's role, 1.2x (a wild one has 2x)
    CHECK(b.reply("beastmaster.give snake").find("snake") != std::string::npos);
}

TEST_CASE("wild beasts: wild snakes come with the beasts pack, one at a time") {
    BeastRig b({{"wild_bear_minutes", "1"}});
    b.r.fake.missing_templates.insert(guid_bytes(kBearTemplate));
    b.r.fake.missing_templates.insert(guid_bytes(kAurochsTemplate));
    no_rare_snakes(b.r.fake);
    b.spawnable(8);
    for (int round = 0; round < 3; ++round) {
        const size_t before = b.r.fake.spawns.size();
        b.seconds(61);
        REQUIRE(b.r.fake.spawns.size() == before + 1);
        CHECK(guid_text(b.r.fake.spawns.back().guid) == kSnakeTemplate);
        const uint32_t id = 1234 + static_cast<uint32_t>(before);
        component<ecs::ScaleComponent>(b.r.fake, id).uniformScale = 1.0f;  // the pack's snake
        CHECK(b.faction(id) == kWildBeast);
        b.seconds(60);
        CHECK(b.r.fake.spawns.size() == before + 1);  // one at a time: it is still about
        b.r.fake.attrs[{id, EMF_ATTR_HEALTH}] = 0;     // killed
    }
}

// ---- the beasts pack's rare snakes: crimson, golden king and albino, each its own copy of the snake's graph ----
namespace {
constexpr const char* kCrimsonGraph = "d4c87337-8c53-41df-8488-8079046e1515";
constexpr const char* kCrimsonTemplate = "d4c87337-8c53-41df-8488-8079046e1512";
constexpr const char* kGoldenGraph = "d4c87337-8c53-41df-8488-8079046e1525";
constexpr const char* kGoldenTemplate = "d4c87337-8c53-41df-8488-8079046e1522";
constexpr const char* kAlbinoGraph = "d4c87337-8c53-41df-8488-8079046e1535";
constexpr const char* kAlbinoTemplate = "d4c87337-8c53-41df-8488-8079046e1532";
constexpr uint32_t kRune = 0x5e000001, kRareRune = 0x5e000002, kEpicSword = 0x5e000003;

// Beastmaster with every math.random answer fixed by `beastmaster.test_roll <r>`: math.random() is r, math.random(m, n)
// is m below 0.5 and n from 0.5 (math.random(n) as math.random(1, n)).
struct RollRig : BeastRig {
    explicit RollRig(const std::map<std::string, std::string>& settings = {}) : BeastRig(settings) {
        r.fake.item_names[kRune] = "Material_T1_Token_WeaponUpgrade_Rune01";
        r.fake.item_names[kRareRune] = "Material_T2_Token_WeaponUpgrade_Rune02";
        r.fake.item_names[kEpicSword] = "Weapon_T1_1H_SwordEpic_01";
        edit_main("local wild_mod = require(\"wild\")\n",
                  "TEST_ROLL = 0.5\n"
                  "math.random = function(m, n)\n"
                  "  if m == nil then return TEST_ROLL end\n"
                  "  if n == nil then m, n = 1, m end\n"
                  "  if TEST_ROLL < 0.5 then return m end\n"
                  "  return n\n"
                  "end\n"
                  "emf.command(\"test_roll\", \"<r>\", function(w, a) TEST_ROLL = tonumber(a[1]) end)\n");
    }
    void roll(double v) { reply("beastmaster.test_roll " + std::to_string(v)); }
    // The puts made into `entity`'s inventory: item -> (count, level, rarity or -1)
    std::map<uint32_t, std::tuple<uint32_t, uint32_t, int>> puts_to(uint32_t entity) {
        std::map<uint32_t, std::tuple<uint32_t, uint32_t, int>> out;
        for (const auto& p : r.fake.puts)
            if (p.entity == entity)
                out[p.item] = {p.count, p.level, (p.flags & EMF_ITEMS_RARITY) ? static_cast<int>(p.rarity) : -1};
        return out;
    }
};
}  // namespace

TEST_CASE("beastmaster: the rare snakes are their own graphs: tamed, called with their role's health, fed their baits") {
    BeastRig b({{"beast_health", "100"}, {"beast_damage", "100"}});
    CHECK(b.reply(std::string("beastmaster.species ") + kCrimsonGraph + " 6 1.15") == "crimson_snake");
    CHECK(b.reply(std::string("beastmaster.species ") + kGoldenGraph + " 6 1.25") == "golden_snake");
    CHECK(b.reply(std::string("beastmaster.species ") + kAlbinoGraph + " 6 1.35") == "albino_snake");
    CHECK(b.r.fake.settings["beastmaster.bait_crimson_snake"] == "Food_T2_raw_bad_meat_Game");
    CHECK(b.r.fake.settings["beastmaster.bait_golden_snake"] == "Food_T3_Turkey_Bait");
    CHECK(b.r.fake.settings["beastmaster.bait_albino_snake"] == "Food_T5_Yak_Bait");
    b.beast(50, kAlbinoGraph, 0, 5, kWildBeast, 20, 100);
    component<ecs::ScaleComponent>(b.r.fake, 50).uniformScale = 1.35f;
    b.look(1, 0, 10.5, 0, 0);
    CHECK(b.reply("beastmaster.tame").find("Taming the albino snake") != std::string::npos);
    b.seconds(4);
    CHECK(b.noted("You tamed an albino snake: Albino snake"));
    CHECK(b.reply("beastmaster.list").find("1. Albino snake - albino_snake, loyalty 1, out") != std::string::npos);
    b.reply("beastmaster.dismiss all");
    b.spawnable(1);
    b.seconds(11);
    CHECK(b.reply("beastmaster.call 1").find("comes") != std::string::npos);
    REQUIRE(b.r.fake.spawns.size() == 1);
    CHECK(guid_text(b.r.fake.spawns[0].guid) == kAlbinoTemplate);
    auto& h = component<ecs::Health>(b.r.fake, 1234);
    h.dataStorage[2] = 40;
    b.seconds(1);
    CHECK(h.dataStorage[3] == 8);  // 40 x 0.2: a tamed albino snake's role, 1.2x (a wild one has 5x)
    for (const char* id : {"crimson_snake", "golden_snake"}) CHECK(b.reply(std::string("beastmaster.give ") + id).find(id) != std::string::npos);
}

TEST_CASE("wild beasts: a wild snake is now and then a rare one: albino 0.25 %, golden king 0.4 %, crimson 1 % of them") {
    RollRig b({{"wild_bear_minutes", "1"}});
    b.r.fake.missing_templates.insert(guid_bytes(kBearTemplate));
    b.r.fake.missing_templates.insert(guid_bytes(kAurochsTemplate));
    b.spawnable(8);
    // the roll in [0, 1): albino under 0.0025, golden king under 0.0065, crimson under 0.0165, else a snake
    const std::vector<std::pair<double, const char*>> rounds = {
        {0.001, kAlbinoTemplate}, {0.005, kGoldenTemplate}, {0.012, kCrimsonTemplate}, {0.02, kSnakeTemplate},
        {0.3, kSnakeTemplate}};
    for (const auto& [roll, want] : rounds) {
        CAPTURE(roll);
        b.roll(roll);
        const size_t before = b.r.fake.spawns.size();
        b.seconds(61);
        REQUIRE(b.r.fake.spawns.size() == before + 1);
        CHECK(guid_text(b.r.fake.spawns.back().guid) == want);
        const uint32_t id = 1234 + static_cast<uint32_t>(before);
        component<ecs::ScaleComponent>(b.r.fake, id).uniformScale = 1.0f;
        b.r.fake.attrs[{id, EMF_ATTR_HEALTH}] = 0;  // killed: the next one may come
    }

    // a rare snake the game does not have (an older beasts pack) comes as a snake, with one log line
    RollRig old({{"wild_bear_minutes", "1"}});
    for (const char* t : {kBearTemplate, kAurochsTemplate, kCrimsonTemplate}) old.r.fake.missing_templates.insert(guid_bytes(t));
    old.spawnable(4);
    old.roll(0.012);
    old.seconds(61);
    REQUIRE(old.r.fake.spawns.size() == 1);
    CHECK(guid_text(old.r.fake.spawns[0].guid) == kSnakeTemplate);
    CHECK(logged(old.r.fake, "wild crimson snakes") == 1);
}

TEST_CASE("wild beasts: the wilds pack's animals never come as rare wild beasts (they come in place of the game's)") {
    RollRig b({{"wild_bear_minutes", "1"}});
    for (const char* t : {kBearTemplate, kAurochsTemplate, kSnakeTemplate, kCrimsonTemplate, kGoldenTemplate,
                          kAlbinoTemplate})
        b.r.fake.missing_templates.insert(guid_bytes(t));
    b.spawnable(4);
    b.seconds(61);
    CHECK(b.r.fake.spawns.empty());  // the fox, the bears, the panthers... are not among them
}

TEST_CASE("wild beasts: a wild albino snake keeps 5x the game's base health") {
    RollRig b({{"wild_bear_minutes", "1"}});
    b.r.fake.missing_templates.insert(guid_bytes(kBearTemplate));
    b.r.fake.missing_templates.insert(guid_bytes(kAurochsTemplate));
    b.spawnable(1);
    b.roll(0.001);
    b.seconds(61);
    REQUIRE(b.r.fake.spawns.size() == 1);
    CHECK(guid_text(b.r.fake.spawns[0].guid) == kAlbinoTemplate);
    auto& h = component<ecs::Health>(b.r.fake, 1234);
    h.dataStorage[2] = 30;
    b.seconds(1);
    CHECK(h.dataStorage[3] == 120);  // 30 + 120 = 150 = 5 x 30
}

TEST_CASE("rare snakes: the slayer gets runes, maybe the tier-2 rune and gear at their level; nobody far away does") {
    RollRig b;
    // an albino, lucky rolls: the fewest runes (6), the tier-2 rune (50 %) and legendary gear (25 %)
    b.roll(0.001);
    b.beast(70, kAlbinoGraph, 0, 5, kWildBeast, 0, 100);
    component<ecs::ScaleComponent>(b.r.fake, 70).uniformScale = 1.35f;
    deliver(b.r.fake, "HitEvent", hit_event(1, 70));
    deliver(b.r.fake, "EntityDiedEvent", died_event(70));
    b.seconds(1);
    auto got = b.puts_to(1);
    REQUIRE(got.count(kRune) == 1);
    CHECK(std::get<0>(got[kRune]) == 6);
    REQUIRE(got.count(kRareRune) == 1);
    CHECK(std::get<0>(got[kRareRune]) == 1);
    REQUIRE(got.count(kEpicSword) == 1);
    CHECK(std::get<0>(got[kEpicSword]) == 1);
    CHECK(std::get<1>(got[kEpicSword]) == 10);  // the slayer's level
    CHECK(std::get<2>(got[kEpicSword]) == 4);   // legendary
    CHECK(b.puts_to(2).empty());

    // a crimson, unlucky rolls: the most runes (5), nothing else; player 2 hit it last
    b.r.fake.puts.clear();
    b.roll(0.99);
    b.beast(71, kCrimsonGraph, 30, 5, kWildBeast, 0, 100);
    deliver(b.r.fake, "HitEvent", hit_event(1, 71));
    deliver(b.r.fake, "HitEvent", hit_event(2, 71));
    deliver(b.r.fake, "EntityDiedEvent", died_event(71));
    b.seconds(1);
    got = b.puts_to(2);
    REQUIRE(got.size() == 1);
    CHECK(std::get<0>(got[kRune]) == 5);
    CHECK(b.puts_to(1).empty());

    // a golden king with lucky rolls: epic gear (10 %), runes 4
    b.r.fake.puts.clear();
    b.roll(0.001);
    b.beast(72, kGoldenGraph, 0, 5, kWildBeast, 0, 100);
    deliver(b.r.fake, "HitEvent", hit_event(1, 72));
    deliver(b.r.fake, "EntityDiedEvent", died_event(72));
    b.seconds(1);
    got = b.puts_to(1);
    CHECK(std::get<0>(got[kRune]) == 4);
    CHECK(std::get<2>(got[kEpicSword]) == 3);  // epic

    // no player's hit (a beast or a fall killed it): the nearest player within 60 m
    b.r.fake.puts.clear();
    b.beast(73, kCrimsonGraph, 28, 0, kWildBeast, 0, 100);
    deliver(b.r.fake, "EntityDiedEvent", died_event(73));
    b.seconds(1);
    CHECK(b.puts_to(2).count(kRune) == 1);
    CHECK(b.puts_to(1).empty());

    // far from every player and no player's hit: no bonus; nor for a tamed one, a plain snake, or a wolf
    b.r.fake.puts.clear();
    b.beast(74, kCrimsonGraph, 200, 200, kWildBeast, 0, 100);
    deliver(b.r.fake, "EntityDiedEvent", died_event(74));
    b.beast(75, kCrimsonGraph, 0, 5, kPlayerAlly, 0, 100);
    deliver(b.r.fake, "HitEvent", hit_event(1, 75));
    deliver(b.r.fake, "EntityDiedEvent", died_event(75));
    b.beast(76, kSnakeGraph, 0, 5, kWildBeast, 0, 100);
    deliver(b.r.fake, "HitEvent", hit_event(1, 76));
    deliver(b.r.fake, "EntityDiedEvent", died_event(76));
    b.beast(77, kWolfGraph, 0, 5, kWildBeast, 0, 100);
    deliver(b.r.fake, "HitEvent", hit_event(1, 77));
    deliver(b.r.fake, "EntityDiedEvent", died_event(77));
    b.seconds(1);
    CHECK(b.r.fake.puts.empty());
}

// ---- the sabretooth graph: no pack beast on it ----
namespace {
constexpr const char* kSabretoothGraph = "9c1b120e-4901-495c-9cc0-5de134fbe140";
}

TEST_CASE("beastmaster: no panther: a small sabretooth is a sabretooth, give panther is refused") {
    BeastRig b;
    CHECK(b.reply(std::string("beastmaster.species ") + kSabretoothGraph + " 6 0.85") == "sabretooth");
    CHECK(b.reply("beastmaster.give panther").find("no species panther") != std::string::npos);
    CHECK(b.reply("beastmaster.give panther").find("aurochs") != std::string::npos);  // the list names every species
}

TEST_CASE("beastmaster: a panther saved by Beastmaster 1.1 loads as a sabretooth (it keeps its name and slot)") {
    BeastRig b;
    write(b.data, "b 0x00000000000000aa panther 0 0 Shadow\n");
    b.r.fake.session = 2;
    fire_load(1);
    b.seconds(1);
    const std::string list = b.reply("beastmaster.list");
    CHECK(list.find("Shadow - sabretooth") != std::string::npos);
    CHECK(list.find("panther") == std::string::npos);
}

TEST_CASE("beastmaster: an aurochs that was out comes back after its first call-backs fail for a moment (no position yet, one refused spawn)") {
    BeastRig b;
    write(b.data, "b 0x00000000000000aa aurochs 0 1 Aurochs\n");
    b.r.fake.session = 2;
    b.r.fake.components.erase(std::make_pair(ecs::CurrentTransform::kHash, 1u));  // the player is not placed yet
    fire_load(1);
    b.seconds(2);
    place(b.r.fake, 1, 0, 10, 0);
    b.r.fake.spawn_fails = true;  // then the game refuses one spawn
    b.seconds(1);
    b.r.fake.spawn_fails = false;
    b.spawnable(1);
    b.seconds(2);
    REQUIRE(b.r.fake.spawns.size() == 1);
    CHECK(guid_text(b.r.fake.spawns[0].guid) == kAurochsTemplate);
    CHECK(b.reply("beastmaster.list").find("Aurochs - aurochs, loyalty 1, out") != std::string::npos);
    size_t notes = 0;
    for (const auto& x : b.r.fake.notes) notes += x.find("Aurochs") != std::string::npos;
    CHECK(notes == 0);
}

// ---- holding a beast during the taming channel (a beaten beast that flees runs out of range) ----
namespace {
// Movement's attribute storage: [0] movement, [1] the multiplier (1 normally), [2..] speeds
float move_mul(Fake& f, uint32_t id) {  // read as it is (component<> would reset it)
    const auto& v = f.components[{ecs::Movement::kHash, id}];
    return v.size() < sizeof(ecs::Movement) ? -1.0f : reinterpret_cast<const ecs::Movement*>(v.data())->dataStorage[1];
}
void set_move_mul(Fake& f, uint32_t id, float m) { component<ecs::Movement>(f, id).dataStorage[1] = m; }
}  // namespace

TEST_CASE("beastmaster: a beast being tamed is held: its movement multiplier is 0 for the channel and back after") {
    BeastRig b;
    b.beast(50, kWolfGraph, 0, 5, kWildBeast, 20, 100);
    set_move_mul(b.r.fake, 50, 1.0f);
    b.look(1, 0, 10.5, 0, 0);
    CHECK(b.reply("beastmaster.tame").find("Taming") != std::string::npos);
    CHECK(move_mul(b.r.fake, 50) == 0.0f);
    b.seconds(4);
    CHECK(b.noted("You tamed a wolf"));
    CHECK(move_mul(b.r.fake, 50) == 1.0f);
}

TEST_CASE("beastmaster: a held beast is let go when the channel breaks or another one starts") {
    BeastRig b;
    b.beast(50, kWolfGraph, 0, 5, kWildBeast, 20, 100);
    set_move_mul(b.r.fake, 50, 1.0f);
    b.look(1, 0, 10.5, 0, 0);
    b.reply("beastmaster.tame");
    CHECK(move_mul(b.r.fake, 50) == 0.0f);
    // another beaten wolf under the crosshair, the first one aside: a new channel lets the first go
    place(b.r.fake, 50, 6, 10, 3);
    b.beast(51, kWolfGraph, 0, 5, kWildBeast, 20, 100);
    set_move_mul(b.r.fake, 51, 1.0f);
    CHECK(b.reply("beastmaster.tame").find("Taming") != std::string::npos);
    CHECK(move_mul(b.r.fake, 50) == 1.0f);
    CHECK(move_mul(b.r.fake, 51) == 0.0f);
    // the player walks far away: the held beast stays where it is (it is not dragged along), the channel breaks and
    // the beast is let go
    place(b.r.fake, 1, 0, 10, -40);
    b.seconds(4);
    CHECK(teleports_of(b.r.fake, 51) == 0);
    CHECK(b.noted("too far"));
    CHECK(move_mul(b.r.fake, 51) == 1.0f);
}

TEST_CASE("beastmaster: a beast that still runs off during the taming is put back where it was held, and tamed") {
    BeastRig b;
    b.beast(50, kWolfGraph, 0, 5, kWildBeast, 20, 100);
    b.look(1, 0, 10.5, 0, 0);
    b.reply("beastmaster.tame");
    place(b.r.fake, 50, 0, 10, 12);  // it runs off
    b.seconds(1);
    REQUIRE(teleports_of(b.r.fake, 50) == 1);  // once in that second, not every step
    const auto t = b.r.fake.teleports.back();
    CHECK(std::hypot(t.x - 0.0, t.z - 5.0) <= 0.5);  // where it was when the taming began (0, 10, 5)
    CHECK((t.flags & EMF_TELEPORT_FIND_GROUND) != 0);
    place(b.r.fake, 50, t.x, 10, t.z);  // the game moved it
    b.seconds(4);
    CHECK(b.noted("You tamed a wolf"));
    CHECK(!b.noted("too far"));
    CHECK(teleports_of(b.r.fake, 50) == 1);
}

TEST_CASE("beastmaster: feeding holds the beast too, and lets it go when the feeding ends") {
    BeastRig b;
    b.r.fake.item_names[kLeanMeat] = "Food_T1_raw_bad_meat_LeanMeat";
    b.beast(50, kWolfGraph, 0, 5, kWildBeast, 100, 100);
    set_move_mul(b.r.fake, 50, 1.0f);
    b.look(1, 0, 10.5, 0, 0);
    b.reply("beastmaster.feed");
    CHECK(move_mul(b.r.fake, 50) == 0.0f);
    b.seconds(11);  // no more bait: it loses interest
    CHECK(b.noted("lost interest"));
    CHECK(move_mul(b.r.fake, 50) == 1.0f);
}

TEST_CASE("beastmaster: a feed with an unknown bait name leaves the beast free to move") {
    BeastRig b({{"bait_wolf", "No_Such_Item"}});
    b.look(1, 0, 10.5, 0, 0);
    b.beast(50, kWolfGraph, 0, 5, kWildBeast, 100, 100);
    set_move_mul(b.r.fake, 50, 1.0f);
    CHECK(b.reply("beastmaster.feed").find("No_Such_Item") != std::string::npos);
    CHECK(move_mul(b.r.fake, 50) == 1.0f);
}

TEST_CASE("beastmaster: a beast in one player's taming cannot be fed or tamed by another, and is let go once") {
    BeastRig b;
    b.r.fake.item_names[kLeanMeat] = "Food_T1_raw_bad_meat_LeanMeat";
    b.beast(50, kWolfGraph, 0, 5, kWildBeast, 20, 100);
    set_move_mul(b.r.fake, 50, 1.0f);
    b.look(1, 0, 10.5, 0, 0);
    b.reply("beastmaster.feed");  // player 1 holds it (feeding)
    CHECK(move_mul(b.r.fake, 50) == 0.0f);
    place(b.r.fake, 2, 0, 10, 9);  // player 2 right behind it, looking at it
    b.look(2, 0, 10.5, 9, 180);
    CHECK(b.reply("beastmaster.feed", 2, 0xBB).find("Someone else") != std::string::npos);
    CHECK(b.reply("beastmaster.tame", 2, 0xBB).find("Someone else") != std::string::npos);
    b.seconds(11);  // player 1's feeding ends: the wolf moves again, and nothing sets it back to 0
    CHECK(b.noted("lost interest"));
    CHECK(move_mul(b.r.fake, 50) == 1.0f);
    b.seconds(11);
    CHECK(move_mul(b.r.fake, 50) == 1.0f);
}

// ---- stepping aside: the game keeps a no-build zone around every creature with the enemy AI (add_no_buildzones_enemy),
// so a beast at your side blocks building and placing there ----
namespace {
// Player `p` holds something to build or place (the game's building cursor has an item), or not.
void building(Fake& f, uint32_t p, bool on) {
    component<ecs::ServerCursor>(f, p).currentBuildingItemInstance.itemId.value = on ? 0x1234u : 0u;
}
size_t mana_paid(const Fake& f, uint32_t p, int32_t cost) {
    size_t n = 0;
    for (const auto& [e, a, d] : f.attr_adds) n += e == p && a == EMF_ATTR_MANA && d == -cost;
    return n;
}
size_t notes_with(const Fake& f, const std::string& needle) {
    size_t n = 0;
    for (const auto& x : f.notes) n += x.find(needle) != std::string::npos;
    return n;
}
}  // namespace

TEST_CASE("beastmaster: beasts step aside while their owner builds and come back after, free; a call meanwhile waits") {
    BeastRig b;
    b.reply("beastmaster.give wolf");
    b.reply("beastmaster.give boar");
    b.spawnable(4);
    CHECK(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    b.seconds(2);
    REQUIRE(b.r.fake.spawns.size() == 1);
    building(b.r.fake, 1, true);
    b.seconds(2);
    CHECK(std::find(b.r.fake.destroyed.begin(), b.r.fake.destroyed.end(), 1234u) != b.r.fake.destroyed.end());
    CHECK(notes_with(b.r.fake, "step aside while you build") == 1);
    CHECK(b.reply("beastmaster.list").find("Wolf - wolf, loyalty 1, aside") != std::string::npos);  // still out: it comes back
    CHECK(b.hud("out").find("Wolf (aside)") != std::string::npos);
    b.seconds(11);
    CHECK(b.r.fake.spawns.size() == 1);  // not while building
    CHECK(b.reply("beastmaster.call Boar").find("while you build") != std::string::npos);
    building(b.r.fake, 1, false);
    b.seconds(1);
    CHECK(b.r.fake.spawns.size() == 1);  // a moment after (the hammer goes back and forth)
    b.seconds(4);
    REQUIRE(b.r.fake.spawns.size() == 2);  // the wolf is back beside its owner
    CHECK(guid_text(b.r.fake.spawns[1].guid) == kWolfTemplate);
    CHECK(std::abs(b.r.fake.spawns[1].x - 2.0) <= 1.0);
    CHECK(mana_paid(b.r.fake, 1, 15) == 1);  // free: only the first call cost mana
    CHECK(b.hud("out").find("Wolf (out)") != std::string::npos);
    // a second time: no second note; with make_room off they stay
    building(b.r.fake, 1, true);
    b.seconds(2);
    CHECK(std::find(b.r.fake.destroyed.begin(), b.r.fake.destroyed.end(), 1235u) != b.r.fake.destroyed.end());
    CHECK(notes_with(b.r.fake, "step aside while you build") == 1);
    b.r.fake.settings["beastmaster.make_room"] = "false";
    b.seconds(2);
    CHECK(b.r.fake.spawns.size() == 3);
    b.seconds(2);
    CHECK(b.r.fake.spawns.size() == 3);  // and stay
}

// ---- in the air: a beast far from its owner is brought beside them with the game's ground search (10 m at most), so
// beside an owner on the glider it stood in the air and fell to its death (a player's report) ----
namespace {
// Player `p`'s movement as the game's DynamicLocomotion.state: 0 on the ground, 2 falling, 4 on the glider.
void moving_as(Fake& f, uint32_t p, int state) {
    component<ecs::DynamicLocomotion>(f, p).state = static_cast<emf::gen::ecs::LocomotionState>(state);
}
}  // namespace

TEST_CASE("beastmaster: beasts step aside while their owner glides and come back beside them a moment after landing, free") {
    BeastRig b;
    b.reply("beastmaster.give wolf");
    b.reply("beastmaster.give boar");
    b.spawnable(4);
    CHECK(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    b.seconds(2);
    REQUIRE(b.r.fake.spawns.size() == 1);
    moving_as(b.r.fake, 1, 4);  // the glider out, 50 m up
    place(b.r.fake, 1, 0, 60, 0);
    b.seconds(1);
    CHECK(contains(b.r.fake.destroyed, 1234));
    CHECK(notes_with(b.r.fake, "step aside while you are in the air") == 1);
    CHECK(b.reply("beastmaster.list").find("Wolf - wolf, loyalty 1, aside") != std::string::npos);  // still out
    CHECK(b.hud("out").find("Wolf (aside)") != std::string::npos);
    CHECK(b.reply("beastmaster.call Boar").find("once you land") != std::string::npos);
    for (int i = 1; i <= 10; ++i) {  // a long glide, 8 m a second
        place(b.r.fake, 1, 8.0 * i, 60.0 - i, 0);
        b.seconds(1);
    }
    CHECK(b.r.fake.spawns.size() == 1);  // not while in the air
    CHECK(teleports_of(b.r.fake, 1234) == 0);
    moving_as(b.r.fake, 1, 0);  // landed
    place(b.r.fake, 1, 90, 10, 0);
    b.seconds(1);
    CHECK(b.r.fake.spawns.size() == 1);  // a moment after
    b.seconds(1);
    REQUIRE(b.r.fake.spawns.size() == 2);  // the wolf is back beside its owner, where they landed
    CHECK(guid_text(b.r.fake.spawns[1].guid) == kWolfTemplate);
    CHECK(std::abs(b.r.fake.spawns[1].x - 92.0) <= 1.0);
    CHECK(std::abs(b.r.fake.spawns[1].y - 10.5) <= 1.0);
    CHECK(mana_paid(b.r.fake, 1, 15) == 1);  // free: only the first call cost mana
    CHECK(b.hud("out").find("Wolf (out)") != std::string::npos);
    // a second glide: no second note
    moving_as(b.r.fake, 1, 4);
    place(b.r.fake, 1, 100, 40, 0);
    b.seconds(1);
    CHECK(contains(b.r.fake.destroyed, 1235));
    CHECK(notes_with(b.r.fake, "step aside while you are in the air") == 1);
}

TEST_CASE("beastmaster: a jump keeps the beasts out, a fall of 1.5 s sends them aside; an air state while standing still does not") {
    BeastRig b;
    wolf_out(b);
    b.spawnable(1);
    b.seconds(1);
    moving_as(b.r.fake, 1, 2);  // a jump: falling for a moment
    place(b.r.fake, 1, 0, 11, 1);
    b.seconds(1);
    moving_as(b.r.fake, 1, 0);
    place(b.r.fake, 1, 0, 10, 2);
    b.seconds(3);
    CHECK(!contains(b.r.fake.destroyed, 1234));
    // the glider's state, but the owner does not move (a state the game left over, not a glide): they stay
    moving_as(b.r.fake, 1, 4);
    b.seconds(4);
    CHECK(!contains(b.r.fake.destroyed, 1234));
    // a long fall: aside once it lasted 1.5 s
    moving_as(b.r.fake, 1, 2);
    place(b.r.fake, 1, 0, 40, 2);
    b.seconds(1);
    place(b.r.fake, 1, 0, 35, 2);
    b.seconds(1);
    CHECK(!contains(b.r.fake.destroyed, 1234));
    place(b.r.fake, 1, 0, 25, 2);
    b.seconds(1);
    CHECK(contains(b.r.fake.destroyed, 1234));
    // another player's glide does not send them aside
    moving_as(b.r.fake, 1, 0);
    place(b.r.fake, 1, 0, 10, 2);
    b.seconds(3);
    REQUIRE(b.r.fake.spawns.size() == 2);
    moving_as(b.r.fake, 2, 4);
    place(b.r.fake, 2, 30, 50, 0);
    b.seconds(2);
    CHECK(!contains(b.r.fake.destroyed, 1235));
}

TEST_CASE("beastmaster: beastmaster.air prints each player's movement state and whether they count as in the air") {
    BeastRig b;
    b.seconds(1);  // seen on the ground first
    moving_as(b.r.fake, 1, 4);
    place(b.r.fake, 1, 0, 60, 0);
    b.seconds(1);
    const std::string text = b.reply("beastmaster.air");
    CHECK(text.find("player 1: movement state 4") != std::string::npos);
    CHECK(text.find("height 60.0, in the air true") != std::string::npos);
    CHECK(text.find("player 2: movement state nil") != std::string::npos);  // no DynamicLocomotion
}

TEST_CASE("beastmaster: an air state while walking on flat ground (a state the game left over) keeps the beasts out; a fall must drop 6 m") {
    BeastRig b;
    wolf_out(b);
    b.seconds(1);
    moving_as(b.r.fake, 1, 2);  // falling, but walking on flat ground 2 m a second
    for (int i = 1; i <= 4; ++i) {
        place(b.r.fake, 1, 2.0 * i, 10, 0);
        place(b.r.fake, 1234, 2.0 * i + 2, 10, 0);
        b.seconds(1);
    }
    CHECK(!contains(b.r.fake.destroyed, 1234));
    moving_as(b.r.fake, 1, 4);  // the glider's state, walking on flat ground
    for (int i = 5; i <= 8; ++i) {
        place(b.r.fake, 1, 2.0 * i, 10, 0);
        place(b.r.fake, 1234, 2.0 * i + 2, 10, 0);
        b.seconds(1);
    }
    CHECK(!contains(b.r.fake.destroyed, 1234));
    moving_as(b.r.fake, 1, 2);  // a slow slide down a slope: 2 m in 2 s is no fall
    place(b.r.fake, 1, 20, 9, 0);
    b.seconds(1);
    place(b.r.fake, 1, 22, 8, 0);
    b.seconds(1);
    place(b.r.fake, 1, 24, 7, 0);
    b.seconds(1);
    CHECK(!contains(b.r.fake.destroyed, 1234));
}

TEST_CASE("beastmaster: /beastmaster aside sends the beasts away until typed again; calling one brings them back") {
    BeastRig b;
    b.reply("beastmaster.give wolf");
    b.reply("beastmaster.give boar");
    b.spawnable(5);
    CHECK(b.reply("beastmaster.aside").find("No beast of yours is out") != std::string::npos);
    CHECK(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    b.seconds(2);
    CHECK(b.reply("beastmaster.aside").find("step aside") != std::string::npos);
    b.seconds(2);
    CHECK(std::find(b.r.fake.destroyed.begin(), b.r.fake.destroyed.end(), 1234u) != b.r.fake.destroyed.end());
    b.seconds(5);
    CHECK(b.r.fake.spawns.size() == 1);  // away until called back
    CHECK(b.reply("beastmaster.aside").find("come back") != std::string::npos);
    b.seconds(2);
    CHECK(b.r.fake.spawns.size() == 2);
    CHECK(b.reply("beastmaster.aside").find("step aside") != std::string::npos);
    b.seconds(11);
    CHECK(b.reply("beastmaster.call Boar").find("comes") != std::string::npos);  // a call ends it
    b.seconds(2);
    CHECK(b.r.fake.spawns.size() == 4);  // the boar, and the wolf back
    CHECK(mana_paid(b.r.fake, 1, 15) == 2);
    // a guest's own
    CHECK(b.reply("beastmaster aside", 2, 0xBB).find("No beast of yours is out") != std::string::npos);
    const std::string help = b.reply("beastmaster");
    CHECK(help.find("aside") != std::string::npos);
}

TEST_CASE("beastmaster: an aside by the command ends with the last beast gone home, and a newly tamed beast brings the others back") {
    BeastRig b;
    b.reply("beastmaster.give boar");
    b.spawnable(3);
    CHECK(b.reply("beastmaster.call Boar").find("comes") != std::string::npos);
    b.seconds(2);
    CHECK(b.reply("beastmaster.aside").find("step aside") != std::string::npos);
    b.seconds(2);
    CHECK(b.reply("beastmaster.dismiss").find("Boar goes home") != std::string::npos);
    b.beast(50, kWolfGraph, 0, 5, kWildBeast, 20, 100);  // a weakened wild wolf in front
    b.look(1, 0, 10.5, 0, 0);
    CHECK(b.reply("beastmaster.tame").find("Taming") != std::string::npos);
    b.seconds(5);
    CHECK(b.reply("beastmaster.list").find("Wolf - wolf, loyalty 1, out") != std::string::npos);
    CHECK(std::find(b.r.fake.destroyed.begin(), b.r.fake.destroyed.end(), 50u) == b.r.fake.destroyed.end());  // stays
    CHECK(b.hud("out").find("Wolf (out)") != std::string::npos);
    // aside with a beast out, then a tame: the tamed one stays and the others come back
    CHECK(b.reply("beastmaster.aside").find("step aside") != std::string::npos);
    b.seconds(2);
    CHECK(std::find(b.r.fake.destroyed.begin(), b.r.fake.destroyed.end(), 50u) != b.r.fake.destroyed.end());
    b.beast(51, "aed0ecec-8a98-4fd3-a450-ab4c508b0ad4", 0, 5, kWildBeast, 20, 100);  // a weakened wild boar
    CHECK(b.reply("beastmaster.tame").find("Taming") != std::string::npos);
    b.seconds(5);
    CHECK(std::find(b.r.fake.destroyed.begin(), b.r.fake.destroyed.end(), 51u) == b.r.fake.destroyed.end());
    CHECK(b.hud("out").find("(aside)") == std::string::npos);
    CHECK(b.r.fake.spawns.size() == 2);  // the boar's first call, and the wolf back
}

TEST_CASE("beastmaster: while aside by the command the wheel has Come back and the list says aside; picking it brings them back") {
    BeastRig b;
    b.reply("beastmaster.give wolf");
    b.spawnable(3);
    CHECK(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    b.seconds(2);
    press(b, VK_F3);
    REQUIRE(b.r.fake.wheels.size() == 1);
    CHECK(index_of(b.r.fake.wheels[0].items, "Come back") < 0);
    CHECK(b.reply("beastmaster.aside").find("Come back") != std::string::npos);  // says how
    b.seconds(2);
    const std::string list = b.reply("beastmaster.list");
    CHECK(list.find("Wolf - wolf, loyalty 1, aside") != std::string::npos);
    CHECK(list.find("/beastmaster aside") != std::string::npos);
    press(b, VK_F3);
    REQUIRE(b.r.fake.wheels.size() == 2);
    const auto w = b.r.fake.wheels[1];
    const int back = index_of(w.items, "Come back");
    REQUIRE(back >= 0);
    w.cb(kClient, back, w.user);
    b.seconds(1);
    CHECK(b.noted("Your beasts come back"));
    b.seconds(2);
    CHECK(b.r.fake.spawns.size() == 2);
    CHECK(b.reply("beastmaster.list").find("Wolf - wolf, loyalty 1, out") != std::string::npos);
}

TEST_CASE("beastmaster: beasts also step aside while another player builds within 20 m of them, and come back when that player is gone") {
    BeastRig b;
    b.reply("beastmaster.give wolf");
    b.spawnable(3);
    CHECK(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    b.seconds(2);
    building(b.r.fake, 2, true);  // the guest builds 30 m away: too far
    b.seconds(3);
    CHECK(std::find(b.r.fake.destroyed.begin(), b.r.fake.destroyed.end(), 1234u) == b.r.fake.destroyed.end());
    place(b.r.fake, 2, 12, 10, 0);  // now 10 m from the wolf
    b.seconds(2);
    CHECK(std::find(b.r.fake.destroyed.begin(), b.r.fake.destroyed.end(), 1234u) != b.r.fake.destroyed.end());
    CHECK(notes_with(b.r.fake, "a player builds near them") == 1);
    b.seconds(11);
    CHECK(b.reply("beastmaster.call Wolf").find("is out already") != std::string::npos);
    b.reply("beastmaster.give boar");
    CHECK(b.reply("beastmaster.call Boar").find("while a player builds near you") != std::string::npos);
    place(b.r.fake, 2, 60, 10, 0);  // the builder walks off
    b.seconds(5);
    CHECK(b.r.fake.spawns.size() == 2);  // the wolf is back
}

TEST_CASE("beastmaster: building is noticed in the step it starts; aside off while still building says when they come") {
    BeastRig b;
    b.reply("beastmaster.give wolf");
    b.spawnable(3);
    CHECK(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    b.seconds(2);
    building(b.r.fake, 1, true);
    b.r.fake.tick += 1;  // the next game step (1/60 s later), not the next second
    b.host->tick(kServer);
    CHECK(std::find(b.r.fake.destroyed.begin(), b.r.fake.destroyed.end(), 1234u) != b.r.fake.destroyed.end());
    CHECK(b.reply("beastmaster.aside").find("step aside") != std::string::npos);
    const std::string off = b.reply("beastmaster.aside");
    CHECK(off.find("a few seconds after you stop building") != std::string::npos);
    CHECK(off != "Your beasts come back.");
    b.reply("beastmaster.give boar");
    CHECK(b.reply("beastmaster.call Boar").find("a few seconds after you stop building") != std::string::npos);
}

// ---- species_extra.lua: more beasts from a file beside Beastmaster's own (a creature pack that cannot be shared, a
// modder's own creature), read when the mod starts. The GUIDs here are made up. ----
namespace {
constexpr const char* kTrollGraph = "7e0d1c2b-3a4f-4b5c-8d6e-0f1a2b3c4d05";
constexpr const char* kTrollTemplate = "7e0d1c2b-3a4f-4b5c-8d6e-0f1a2b3c4d02";
constexpr const char* kOgreGraph = "7e0d1c2b-3a4f-4b5c-8d6e-0f1a2b3c4d15";
constexpr const char* kOgreTemplate = "7e0d1c2b-3a4f-4b5c-8d6e-0f1a2b3c4d12";
constexpr int kVukah = 3;

// Writes species_extra.lua beside the copied mod and lets the host reload it (`step`: a later write needs a later one),
// then plays a second so the world's data reaches the reloaded mod (as edit_main).
void extra_species(BeastRig& b, const std::string& lua, int step = 1) {
    write_later(b.folder + L"\\species_extra.lua", lua, step);
    b.host->check_reload_now();
    REQUIRE_MESSAGE(b.host->status(b.id) == "running", b.host->status(b.id));
    b.seconds(1);
}

std::string entry(const char* id, const char* name, const char* tpl, const char* graph, const std::string& more = "") {
    return std::string("\t{ id = \"") + id + "\", name = \"" + name + "\", template = \"" + tpl + "\", graph = \"" +
           graph + "\"" + more + " },\n";
}
}  // namespace

TEST_CASE("beastmaster: a species_extra.lua beside the mod adds its beasts: known by graph and faction, tamed, listed, called with its template and health") {
    BeastRig b({{"beast_health", "100"}, {"beast_damage", "100"}});
    CHECK(b.reply(std::string("beastmaster.species ") + kTrollGraph + " 3") == "unknown");
    extra_species(b, "return {\n" + entry("troll", "Troll", kTrollTemplate, kTrollGraph,
                                          ", plural = \"trolls\", factions = { [3] = true }, health_mul = 3, role_health = 3") + "}\n");
    CHECK(b.reply(std::string("beastmaster.species ") + kTrollGraph + " 3") == "troll");
    CHECK(b.reply(std::string("beastmaster.species ") + kTrollGraph + " 6") == "unknown");  // not its faction
    CHECK(b.reply(std::string("beastmaster.species ") + kWolfGraph + " 6") == "wolf");       // the mod's own stay
    b.beast(50, kTrollGraph, 0, 5, kVukah, 20, 100);
    b.look(1, 0, 10.5, 0, 0);
    CHECK(b.reply("beastmaster.tame").find("Taming the troll") != std::string::npos);
    b.seconds(4);
    CHECK(b.noted("You tamed a troll: Troll"));
    CHECK(b.reply("beastmaster.list").find("1. Troll - troll, loyalty 1, out") != std::string::npos);
    b.reply("beastmaster.dismiss all");
    b.spawnable(1);
    b.seconds(11);
    CHECK(b.reply("beastmaster.call 1").find("comes") != std::string::npos);
    REQUIRE(b.r.fake.spawns.size() == 1);
    CHECK(guid_text(b.r.fake.spawns[0].guid) == kTrollTemplate);
    auto& h = component<ecs::Health>(b.r.fake, 1234);
    h.dataStorage[2] = 40;
    b.seconds(1);
    CHECK(h.dataStorage[3] == 80);  // 40 + 80 = 120 = 3 x 40
    CHECK(b.reply("beastmaster.give troll").find("troll") != std::string::npos);
}

TEST_CASE("beastmaster: a species_extra.lua beast is fed the bait its entry names; one without bait takes none") {
    BeastRig b;
    b.r.fake.item_names[kDogBait] = "Food_T2_Dog_Bait";
    extra_species(b, "return {\n" + entry("troll", "Troll", kTrollTemplate, kTrollGraph, ", bait = \"Food_T2_Dog_Bait\"") +
                         entry("ogre", "Ogre", kOgreTemplate, kOgreGraph) + "}\n");
    b.beast(50, kOgreGraph, 0, 5, kVukah, 100, 100);   // ahead (+z)
    b.beast(51, kTrollGraph, 5, 0, kVukah, 100, 100);  // to the right (+x)
    b.look(1, 0, 10.5, 0, 0);
    CHECK(b.reply("beastmaster.tame") == "The ogre is too strong: bring it under 25 % health.");
    CHECK(b.reply("beastmaster.feed") == "The ogre takes no bait: bring it under 25 % health to tame it.");
    CHECK(b.r.fake.takes.empty());
    b.look(1, 0, 10.5, 0, 90);
    CHECK(b.reply("beastmaster.tame").find("feed it its bait (Food_T2_Dog_Bait)") != std::string::npos);
    CHECK(b.reply("beastmaster.feed") == "You offer the troll some Food_T2_Dog_Bait.");
    REQUIRE(b.r.fake.takes.size() == 1);
    CHECK(b.r.fake.takes.back().item == kDogBait);
}

TEST_CASE("beastmaster: a species_extra.lua beast whose bait the game does not know points at its entry") {
    BeastRig b;
    extra_species(b, "return {\n" + entry("ogre", "Ogre", kOgreTemplate, kOgreGraph, ", bait = \"No_Such_Item\"") + "}\n");
    b.beast(50, kOgreGraph, 0, 5, kVukah, 100, 100);
    b.look(1, 0, 10.5, 0, 0);
    CHECK(b.reply("beastmaster.feed") == "The game has no item named No_Such_Item: check its bait in species_extra.lua.");
}

TEST_CASE("beastmaster: species_extra.lua entries without id, name, template or graph, or with a taken id, are skipped with a log line") {
    BeastRig b;
    CHECK(logged(b.r.fake, "species_extra") == 0);  // no file: nothing to say
    extra_species(b, "return {\n" + entry("wolf", "Grey wolf", kTrollTemplate, kTrollGraph) +
                         "\t{ name = \"Nameless\", template = \"" + kOgreTemplate + "\", graph = \"" + kOgreGraph + "\" },\n" +
                         "\t{ id = \"ghost\", name = \"Ghost\", template = \"" + kOgreTemplate + "\" },\n" +
                         "\t\"ogre\",\n" +
                         entry("troll", "Troll", kTrollTemplate, "7E0D1C2B-3A4F-4B5C-8D6E-0F1A2B3C4D05") + "}\n");
    CHECK(logged(b.r.fake, "species_extra.lua: entry 1 (wolf) skipped: the id wolf is taken") == 1);
    CHECK(logged(b.r.fake, "species_extra.lua: entry 2 skipped: it has no id") == 1);
    CHECK(logged(b.r.fake, "species_extra.lua: entry 3 (ghost) skipped: it has no graph") == 1);
    CHECK(logged(b.r.fake, "species_extra.lua: entry 4 skipped: not a table") == 1);
    CHECK(logged(b.r.fake, "species_extra.lua: 1 beast added (troll)") == 1);
    CHECK(b.reply(std::string("beastmaster.species ") + kTrollGraph + " 3") == "troll");  // its graph in any case
    CHECK(b.reply(std::string("beastmaster.species ") + kWolfGraph + " 6") == "wolf");
    CHECK(b.reply("beastmaster.give ghost").find("no species ghost") != std::string::npos);
}

TEST_CASE("beastmaster: a species_extra.lua that does not load or gives no list is logged; the mod runs on with its own beasts") {
    BeastRig b;
    extra_species(b, "return {\n\t{ id = \"troll\",\n");  // cut short
    CHECK(logged(b.r.fake, "species_extra.lua did not load:") == 1);
    CHECK(b.reply(std::string("beastmaster.species ") + kWolfGraph + " 6") == "wolf");
    extra_species(b, "return 5\n", 2);
    CHECK(logged(b.r.fake, "species_extra.lua gave no list of beasts") == 1);
    CHECK(b.reply("beastmaster.give wolf").find("added") != std::string::npos);
}

TEST_CASE("wild beasts: a species_extra.lua beast with a pack comes as a wild beast too") {
    BeastRig b({{"wild_bear_minutes", "1"}});
    for (const char* t : {kBearTemplate, kAurochsTemplate, kSnakeTemplate}) b.r.fake.missing_templates.insert(guid_bytes(t));
    no_rare_snakes(b.r.fake);
    extra_species(b, "return {\n" + entry("troll", "Troll", kTrollTemplate, kTrollGraph, ", pack = \"a private pack\"") + "}\n");
    b.spawnable(2);
    b.seconds(61);
    REQUIRE(b.r.fake.spawns.size() == 1);
    CHECK(guid_text(b.r.fake.spawns.back().guid) == kTrollTemplate);
}

// ---- talents: points from loyalty, spent at a stable, kept with the world ----
namespace {
bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
// Player 2's wolves Grey and Wolf 2 at loyalty 4 (60 points: 3 talent points each) from a save; a built stable 3 m from
// player 2 (at 30, 10, 0).
void guest_wolves(BeastRig& b) {
    write(b.data, "b 0x00000000000000bb wolf 0 0 Grey\nb 0x00000000000000bb wolf 0 0 Wolf 2\n"
                  "y 0x00000000000000bb 60 Grey\ny 0x00000000000000bb 60 Wolf 2\ns 1\n");
    b.r.fake.session = 2;
    fire_load(1);
    flame_altar(b, 900, 30, 0);
    stable_prop(b, 961, 33, 10, 0);
    b.seconds(32);  // the next search of the world finds it
}
}  // namespace

TEST_CASE("talent: a beast gets a point for each loyalty above 1; the new loyalty's note says so") {
    BeastRig b;
    b.reply("beastmaster.give wolf");
    next_beast(b, 1234);
    REQUIRE(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    CHECK(has(b.reply("beastmaster.talents Wolf"), "Wolf's talents: no points yet (one for each loyalty above 1)"));
    for (uint32_t i = 0; i < 10; ++i) {
        creature(b, 100 + i, 0, 10, kSkeletonFoe);
        deliver(b.r.fake, "HitEvent", hit_event(1234, 100 + i));
        deliver(b.r.fake, "EntityDiedEvent", died_event(100 + i));
        b.seconds(1);
    }
    CHECK(b.noted("Wolf reached loyalty 2: a talent point to spend at a stable."));
    const std::string list = b.reply("beastmaster talents Wolf");
    CAPTURE(list);
    CHECK(has(list, "Wolf's talents: 1 point (1 left)"));
    for (const char* line : {"Strength 0/5: +4 % damage a point", "Toughness 0/5: +5 % health a point",
                             "Swiftness 0/3: +4 % speed a point",
                             "Second Wind 0/1: a killing blow leaves it at 20 % health, then 5 min before it works again",
                             "Ambush 0/2: +50 % damage a point on its first hit in a fight",
                             "Guard 0/2: no damage for the first 3 s of a fight (5 s with 2 points), once every 30 s",
                             "Menace 0/3: draws 10 % more of the monsters a point",
                             "Taunt 0/2: roars when a monster near it hits you: the monsters within 10 m turn on it for 6 s, "
                             "every 45 s (30 s with 2 points)"})
        CHECK(has(list, line));
    b.reply("beastmaster.loyalty Wolf 10");
    CHECK(has(b.reply("beastmaster.talents 1"), "Wolf's talents: 9 points (9 left)"));
}

TEST_CASE("talent: spending and reset at a stable; refused away from one, when full or when no point is left") {
    BeastRig b;
    guest_wolves(b);
    auto say = [&](const std::string& c) { return b.reply(c, 2, 0xBB); };
    CHECK(say("beastmaster talent Grey strength") == "Grey: Strength 1/5, +4 % damage (2 points left).");
    CHECK(say("beastmaster.talent Grey Strength") == "Grey: Strength 2/5, +8 % damage (1 point left).");
    CHECK(say("beastmaster talent 1 STRENGTH") == "Grey: Strength 3/5, +12 % damage (0 points left).");
    CHECK(say("beastmaster talent Grey toughness") == "Grey has no talent point left (3 of 3 spent).");
    CHECK(has(say("beastmaster.talents Grey"), "Strength 3/5: +12 % damage"));
    CHECK(say("beastmaster talents Grey reset") == "Grey's talents are reset: 3 points to spend.");
    CHECK(has(say("beastmaster.talents Grey"), "Grey's talents: 3 points (3 left)"));
    CHECK(say("beastmaster talent Wolf 2 second wind") ==
          "Wolf 2: Second Wind 1/1, a killing blow leaves it at 20 % health, then 5 min before it works again "
          "(2 points left).");
    CHECK(say("beastmaster talent Wolf 2 Second_Wind") == "Wolf 2's Second Wind is full (1/1).");
    CHECK(say("beastmaster talent Wolf 2 guard") ==
          "Wolf 2: Guard 1/2, no damage for the first 3 s of a fight, once every 30 s (1 point left).");
    const std::string which = "A talent is Strength, Toughness, Swiftness, Second Wind, Ambush, Guard, Menace or Taunt: "
                              "/beastmaster talent "
                              "<name or number> <talent>";
    CHECK(say("beastmaster talent Grey shield") == which);
    CHECK(say("beastmaster talent Grey") == which);
    CHECK(say("beastmaster talent Nobody strength") == "No such beast (beastmaster.list shows yours).");
    CHECK(say("beastmaster talents Nobody") == "No such beast (beastmaster.list shows yours).");
    place(b.r.fake, 2, 60, 10, 0);  // 27 m from the stable
    CHECK(say("beastmaster talent Grey ambush") == "Go to a stable (an animal bed in your base) within 10 m to do that.");
    CHECK(say("beastmaster talents Grey reset") == "Go to a stable (an animal bed in your base) within 10 m to do that.");
    CHECK(has(say("beastmaster talents Grey"), "Grey's talents: 3 points (3 left)"));  // a list anywhere
}

TEST_CASE("talent: spent points are kept with the world; a release forgets them, a rename keeps them; 1.6.0 and 1.2.0 saves load with none spent") {
    BeastRig b;
    // a Beastmaster 1.6.0 save: modes, beasts, loyalty, active flags, stabled, a mark
    write(b.data, "m 0x00000000000000aa passive\nb 0x00000000000000aa wolf 0 0 Fang\nb 0x00000000000000aa boar 0 0 Tusk\n"
                  "y 0x00000000000000aa 60 Fang\ny 0x00000000000000aa 30 Tusk\na 0x00000000000000aa Fang\ns 1\n"
                  "k 50.00 10.00 50.00 -\n");
    b.r.fake.session = 2;
    fire_load(1);
    b.seconds(1);
    const std::string list = b.reply("beastmaster.list");
    CHECK(has(list, "1. Fang - wolf, loyalty 4, at home, active"));
    CHECK(has(list, "2. Tusk - boar, loyalty 3, at home"));
    CHECK(has(b.reply("beastmaster.talents Fang"), "Fang's talents: 3 points (3 left)"));
    at_stable(b);
    b.reply("beastmaster.talent Fang strength");
    b.reply("beastmaster.talent Fang strength");
    CHECK(b.reply("beastmaster.talent Fang guard") ==
          "Fang: Guard 1/2, no damage for the first 3 s of a fight, once every 30 s (0 points left).");
    b.seconds(1);
    const std::string saved = b.saved();
    CAPTURE(saved);
    for (const char* line : {"t 0x00000000000000aa strength:2,guard:1 Fang", "m 0x00000000000000aa passive",
                             "y 0x00000000000000aa 60 Fang", "y 0x00000000000000aa 30 Tusk", "a 0x00000000000000aa Fang",
                             "\ns 1", "k 50.00 10.00 50.00 -"})
        CHECK(has(saved, line));
    b.reply("beastmaster.rename Fang Rex");
    CHECK(has(b.saved(), "t 0x00000000000000aa strength:2,guard:1 Rex"));
    b.r.fake.session = 3;  // the world loads again
    fire_load(1);
    b.seconds(1);
    const std::string again = b.reply("beastmaster.talents Rex");
    CAPTURE(again);
    CHECK(has(again, "Rex's talents: 3 points (0 left)"));
    CHECK(has(again, "Strength 2/5: +8 % damage"));
    CHECK(has(again, "Guard 1/2: no damage for the first 3 s of a fight, once every 30 s"));
    b.reply("beastmaster.release Rex");
    CHECK(b.reply("beastmaster.release Rex") == "Rex is gone: you let it go.");
    CHECK_FALSE(has(b.saved(), "\nt "));
    // a Beastmaster 1.2.0 save: beasts only
    write(b.data, "b 0x00000000000000aa wolf 0 0 Fang\n");
    b.r.fake.session = 4;
    fire_load(1);
    b.seconds(1);
    CHECK(has(b.reply("beastmaster.talents Fang"), "Fang's talents: no points yet"));
    b.reply("beastmaster.mode aggressive");
    b.seconds(1);
    CHECK(has(b.saved(), "b 0x00000000000000aa wolf 0 0 Fang"));
    CHECK_FALSE(has(b.saved(), "\nt "));
}

TEST_CASE("talent: points spent beyond loyalty stay; none can be spent; the list says how many too many") {
    BeastRig b;
    b.reply("beastmaster.give wolf");
    b.reply("beastmaster.loyalty Wolf 4");
    at_stable(b);
    for (int i = 0; i < 3; ++i) b.reply("beastmaster.talent Wolf strength");
    CHECK(b.reply("beastmaster.loyalty Wolf 2") ==
          "Wolf's loyalty is now 2. Talents: 1 point (2 too many: reset to choose again).");
    const std::string list = b.reply("beastmaster.talents Wolf");
    CHECK(has(list, "Wolf's talents: 1 point (2 too many: reset to choose again)"));
    CHECK(has(list, "Strength 3/5: +12 % damage"));
    CHECK(b.reply("beastmaster.talent Wolf guard") == "Wolf has 2 talent points spent too many: reset to choose again.");
    b.seconds(1);
    CHECK(has(b.saved(), "t 0x00000000000000aa strength:3 Wolf"));
    CHECK(b.reply("beastmaster.talents Wolf reset") == "Wolf's talents are reset: 1 point to spend.");
}

TEST_CASE("talent: t lines from another version: an unknown talent is skipped, a rank over the most is cut to it, a name with no beast is ignored") {
    BeastRig b;
    write(b.data, "b 0x00000000000000aa wolf 0 0 Fang\ny 0x00000000000000aa 450 Fang\n"
                  "t 0x00000000000000aa strength:9,shield:2,guard:1 Fang\nt 0x00000000000000aa ambush:1 Nobody\n"
                  "t 0x00000000000000aa nonsense Fang\n");
    b.r.fake.session = 2;
    fire_load(1);
    b.seconds(1);
    const std::string list = b.reply("beastmaster.talents Fang");
    CAPTURE(list);
    CHECK(has(list, "Fang's talents: 9 points (3 left)"));
    CHECK(has(list, "Strength 5/5: +20 % damage"));
    CHECK(has(list, "Guard 1/2"));
    b.reply("beastmaster.mode passive");  // a change: the world's data is written
    b.seconds(1);
    const std::string saved = b.saved();
    CAPTURE(saved);
    CHECK(has(saved, "t 0x00000000000000aa strength:5,guard:1 Fang"));
    CHECK_FALSE(has(saved, "Nobody"));
    CHECK_FALSE(has(saved, "shield"));
}

// ---- talents on a beast out: Strength, Toughness, Swiftness ----
namespace {
// Movement's storage [1] (C index): its multiplier, which Swiftness multiplies (mods/beastmaster/talents.lua
// SPEED_ENTRIES).
float speed_of(BeastRig& b, uint32_t id) { return now_of<ecs::Movement>(b, id).dataStorage[1]; }
void one_step(BeastRig& b) {
    b.r.fake.tick += 1;
    b.host->tick(kServer);
}
}  // namespace

TEST_CASE("talent: Strength and Toughness go on top of role and loyalty, at once on a beast out and on the next call") {
    BeastRig b({{"beast_health", "100"}, {"beast_damage", "100"}});
    b.reply("beastmaster.give wolf");
    b.reply("beastmaster.loyalty Wolf 3");
    next_beast(b, 1234);
    REQUIRE(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    at_stable(b);
    b.seconds(1);
    const double f = 1 + 2.0 / 9 * 0.2;  // loyalty 3
    REQUIRE(now_of<ecs::DamageMod>(b, 1234).dataStorage[2] == doctest::Approx(1.15 * f));
    REQUIRE(now_of<ecs::Health>(b, 1234).dataStorage[3] == 25);  // 100 x 1.2 x f - 100, rounded
    b.reply("beastmaster.talent Wolf strength");
    b.reply("beastmaster.talent Wolf strength");
    CHECK(now_of<ecs::DamageMod>(b, 1234).dataStorage[2] == doctest::Approx(1.15 * f * 1.08));  // at once
    b.reply("beastmaster.talents Wolf reset");
    CHECK(now_of<ecs::DamageMod>(b, 1234).dataStorage[2] == doctest::Approx(1.15 * f));
    b.reply("beastmaster.talent Wolf toughness");
    b.reply("beastmaster.talent Wolf strength");
    one_step(b);
    CHECK(now_of<ecs::Health>(b, 1234).dataStorage[3] == 32);  // 100 x 1.2 x f x 1.05 - 100, rounded
    CHECK(now_of<ecs::DamageMod>(b, 1234).dataStorage[2] == doctest::Approx(1.15 * f * 1.04));
    b.reply("beastmaster.dismiss Wolf");
    next_beast(b, 1235);
    b.seconds(11);
    REQUIRE(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    b.seconds(1);
    CHECK(now_of<ecs::Health>(b, 1235).dataStorage[3] == 32);
    CHECK(now_of<ecs::DamageMod>(b, 1235).dataStorage[2] == doctest::Approx(1.15 * f * 1.04));
}

TEST_CASE("talent: Swiftness multiplies the Movement multiplier, kept when the game writes it again; reset gives the game's value back; no Swiftness leaves Movement alone") {
    BeastRig b;
    b.reply("beastmaster.give wolf");
    b.reply("beastmaster.give boar");
    b.reply("beastmaster.loyalty Wolf 4");
    next_beast(b, 1234);
    component<ecs::Movement>(b.r.fake, 1234).dataStorage[1] = 1.0f;
    REQUIRE(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    at_stable(b);
    b.seconds(1);
    CHECK(speed_of(b, 1234) == 1.0f);  // no Swiftness yet: as the game made it
    for (int i = 0; i < 3; ++i) b.reply("beastmaster.talent Wolf swiftness");
    CHECK(speed_of(b, 1234) == doctest::Approx(1.12));  // at once
    live_of<ecs::Movement>(b, 1234).dataStorage[1] = 0.5f;  // the game writes it anew
    one_step(b);
    CHECK(speed_of(b, 1234) == doctest::Approx(0.56));
    b.seconds(2);
    CHECK(speed_of(b, 1234) == doctest::Approx(0.56));  // not again on top of itself
    b.reply("beastmaster.talents Wolf reset");
    CHECK(speed_of(b, 1234) == doctest::Approx(0.5));
    live_of<ecs::Movement>(b, 1234).dataStorage[1] = 0.8f;  // the game's own from now on
    b.seconds(2);
    CHECK(speed_of(b, 1234) == doctest::Approx(0.8));
    next_beast(b, 1235);
    component<ecs::Movement>(b.r.fake, 1235).dataStorage[1] = 0.7f;
    b.seconds(11);
    REQUIRE(b.reply("beastmaster.call Boar").find("comes") != std::string::npos);
    b.seconds(2);
    CHECK(speed_of(b, 1235) == doctest::Approx(0.7));
}

// ---- talents in fights: Guard, Second Wind, Ambush ----
namespace {
// A hit from `source` on `target` that changed its health by `change` (negative: damage).
std::vector<uint8_t> hit(uint32_t source, uint32_t target, int32_t change) {
    auto e = hit_event(source, target);
    std::memcpy(&e[offsetof(ecs::HitEvent, healthChange)], &change, 4);
    return e;
}
// The game's minimum health of `id` (Health storage [1]): the floor Guard and Second Wind hold.
int32_t floor_of(BeastRig& b, uint32_t id) { return now_of<ecs::Health>(b, id).dataStorage[1]; }
// Player 1's wolf (loyalty 10: 9 points) out as entity 1234 with 100 of 100 health, at a stable, with the talents
// `spend` spent; foe 70 (a skeleton, 100 of 100) beside it.
void wolf_with(BeastRig& b, const std::vector<std::string>& spend) {
    b.reply("beastmaster.give wolf");
    b.reply("beastmaster.loyalty Wolf 10");
    next_beast(b, 1234);
    component<ecs::Movement>(b.r.fake, 1234).dataStorage[1] = 1.0f;
    REQUIRE(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    component<ecs::DynamicEnemy01BehaviorState>(b.r.fake, 1234).guardEntityId.id = 1;  // it guards its owner
    at_stable(b);
    for (const auto& t : spend) REQUIRE(b.reply("beastmaster.talent Wolf " + t).rfind("Wolf: ", 0) == 0);
    creature(b, 70, 0, 10, kSkeletonFoe);
    b.seconds(1);
    b.r.fake.attr_adds.clear();
    b.r.fake.damages.clear();
}
}  // namespace

TEST_CASE("talent: Guard: the first seconds of a fight take no damage, the opening hit healed back; a hit after 10 s quiet starts a new fight") {
    BeastRig b;
    wolf_with(b, {"guard"});
    REQUIRE(floor_of(b, 1234) == 0);
    b.r.fake.attrs[{1234, EMF_ATTR_HEALTH}] = 70;  // the foe's opening hit: 30
    deliver(b.r.fake, "HitEvent", hit(70, 1234, -30));
    CHECK(b.added(1234, EMF_ATTR_HEALTH, 30));  // healed back
    CHECK(floor_of(b, 1234) == 100);            // its health when the fight started
    b.seconds(2);
    b.r.fake.attr_adds.clear();
    deliver(b.r.fake, "HitEvent", hit(70, 1234, -10));  // in the fight: the floor holds, nothing healed back
    CHECK(b.r.fake.attr_adds.empty());
    CHECK(floor_of(b, 1234) == 100);
    b.seconds(2);  // 4 s after the fight started: 3 s of Guard are over
    CHECK(floor_of(b, 1234) == 0);
    deliver(b.r.fake, "HitEvent", hit(1234, 70, -10));  // still the same fight (2 s after the last hit)
    CHECK(floor_of(b, 1234) == 0);
    b.reply("beastmaster.talent Wolf guard");  // 2 points: 5 s, from the next fight
    b.seconds(27);  // the fight is over (10 s quiet), and Guard worked 31 s ago (at most once every 30 s)
    b.r.fake.attrs[{1234, EMF_ATTR_HEALTH}] = 60;
    b.r.fake.attr_adds.clear();
    deliver(b.r.fake, "HitEvent", hit(1234, 70, -10));  // its own hit starts the next fight
    CHECK(b.r.fake.attr_adds.empty());
    CHECK(floor_of(b, 1234) == 60);
    b.seconds(4);
    CHECK(floor_of(b, 1234) == 60);
    b.seconds(2);
    CHECK(floor_of(b, 1234) == 0);
}

TEST_CASE("talent: Second Wind: a killing blow leaves it at 20 %; then 5 minutes before it works again") {
    BeastRig b;
    wolf_with(b, {"second wind"});
    CHECK(floor_of(b, 1234) == 1);  // ready: a killing blow leaves it at 1
    b.r.fake.attrs[{1234, EMF_ATTR_HEALTH}] = 1;
    one_step(b);
    CHECK(b.added(1234, EMF_ATTR_HEALTH, 19));  // up to 20 of 100
    CHECK(notes_with(b.r.fake, "Wolf gets up again: Second Wind (5 min before it works again).") == 1);
    CHECK(floor_of(b, 1234) == 0);
    b.r.fake.attrs[{1234, EMF_ATTR_HEALTH}] = 40;
    b.seconds(290);
    CHECK(floor_of(b, 1234) == 0);
    b.r.fake.attrs[{1234, EMF_ATTR_HEALTH}] = 1;  // within the 5 minutes: nothing
    one_step(b);
    CHECK(notes_with(b.r.fake, "Wolf gets up again") == 1);
    b.r.fake.attrs[{1234, EMF_ATTR_HEALTH}] = 40;
    b.seconds(11);
    CHECK(floor_of(b, 1234) == 1);  // ready again
}

TEST_CASE("talent: Ambush: the first damaging hit in a fight deals 50 % more a point; a bonus that kills is its owner's") {
    BeastRig b;
    wolf_with(b, {"ambush"});
    using Hit = std::tuple<uint32_t, uint32_t, int32_t>;
    b.r.fake.attrs[{70, EMF_ATTR_HEALTH}] = 60;
    deliver(b.r.fake, "HitEvent", hit(1234, 70, -40));
    REQUIRE(b.r.fake.damages.size() == 1u);
    CHECK(b.r.fake.damages[0] == Hit{1234, 70, 20});
    deliver(b.r.fake, "HitEvent", hit(1234, 70, -10));  // its next hit: nothing more
    CHECK(b.r.fake.damages.size() == 1u);
    b.reply("beastmaster.talent Wolf ambush");
    b.seconds(11);  // 10 s quiet: the next hit starts a new fight
    deliver(b.r.fake, "HitEvent", hit(70, 1234, -5));   // the foe hits first: the fight starts, Ambush waits
    deliver(b.r.fake, "HitEvent", hit(1234, 70, -0));   // a hit that deals nothing: still waits
    CHECK(b.r.fake.damages.size() == 1u);
    deliver(b.r.fake, "HitEvent", hit(1234, 70, -20));
    REQUIRE(b.r.fake.damages.size() == 2u);
    CHECK(b.r.fake.damages[1] == Hit{1234, 70, 20});  // 2 points: +100 %
    b.seconds(11);
    creature(b, 71, 0, 12, kSkeletonFoe);
    b.r.fake.attrs[{71, EMF_ATTR_HEALTH}] = 26;  // 26 of 100 after the hit: the bonus kills it
    deliver(b.r.fake, "HitEvent", hit(1234, 71, -26));
    REQUIRE(b.r.fake.damages.size() == 3u);
    CHECK(b.r.fake.damages[2] == Hit{1, 71, 26});  // the owner's: the XP and loot are theirs
}

TEST_CASE("talent: Ambush never hits a player or a player's ally, nor a foe its hit already handed to the owner") {
    BeastRig b;
    wolf_with(b, {"ambush"});
    using Hit = std::tuple<uint32_t, uint32_t, int32_t>;
    creature(b, 80, 5, 5, kPlayerAlly);  // another player's beast
    deliver(b.r.fake, "HitEvent", hit(1234, 2, -10));
    deliver(b.r.fake, "HitEvent", hit(1234, 80, -10));
    CHECK(b.r.fake.damages.empty());
    b.r.fake.attrs[{70, EMF_ATTR_HEALTH}] = 20;  // 20 of 100: the XP credit hands the killing blow to the owner
    deliver(b.r.fake, "HitEvent", hit(1234, 70, -30));
    REQUIRE(b.r.fake.damages.size() == 1u);
    CHECK(b.r.fake.damages[0] == Hit{1, 70, 20});
    deliver(b.r.fake, "HitEvent", hit(1234, 70, -10));  // its first damaging hit in this fight was spent
    CHECK(b.r.fake.damages.size() == 1u);
}

TEST_CASE("talent: guard and second wind together: the higher floor while Guard lasts, then Second Wind's, then the game's") {
    BeastRig b;
    wolf_with(b, {"guard", "second wind"});
    CHECK(floor_of(b, 1234) == 1);
    b.r.fake.attrs[{1234, EMF_ATTR_HEALTH}] = 80;
    deliver(b.r.fake, "HitEvent", hit(70, 1234, -20));
    CHECK(floor_of(b, 1234) == 100);
    b.seconds(4);
    CHECK(floor_of(b, 1234) == 1);  // Guard is over; Second Wind is ready
    b.r.fake.attrs[{1234, EMF_ATTR_HEALTH}] = 1;
    one_step(b);
    CHECK(b.added(1234, EMF_ATTR_HEALTH, 19));
    CHECK(floor_of(b, 1234) == 0);  // neither: the game's minimum
}

TEST_CASE("talent: a beast sent home mid-fight leaves nothing behind; its next call starts a fresh fight") {
    BeastRig b;
    wolf_with(b, {"guard", "second wind", "swiftness"});
    b.r.fake.attrs[{1234, EMF_ATTR_HEALTH}] = 90;
    deliver(b.r.fake, "HitEvent", hit(70, 1234, -10));
    REQUIRE(floor_of(b, 1234) == 100);
    REQUIRE(speed_of(b, 1234) == doctest::Approx(1.04));
    b.reply("beastmaster.dismiss Wolf");
    // the game gives the id to another creature: nothing of the wolf's is written on it
    component<ecs::Health>(b.r.fake, 1234);
    component<ecs::Movement>(b.r.fake, 1234).dataStorage[1] = 1.0f;
    b.seconds(5);
    CHECK(floor_of(b, 1234) == 0);
    CHECK(speed_of(b, 1234) == 1.0f);
    next_beast(b, 1235);
    b.seconds(6);
    REQUIRE(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    b.seconds(1);
    CHECK(floor_of(b, 1235) == 1);  // Second Wind ready
    b.seconds(19);                  // Guard worked 31 s ago (once every 30 s, also across a call)
    b.r.fake.attrs[{1235, EMF_ATTR_HEALTH}] = 90;
    b.r.fake.attr_adds.clear();
    deliver(b.r.fake, "HitEvent", hit(70, 1235, -10));  // the opening hit of its own fight
    CHECK(b.added(1235, EMF_ATTR_HEALTH, 10));
    CHECK(floor_of(b, 1235) == 100);
}

TEST_CASE("talent: a reset while out takes Second Wind's floor off at the next step") {
    BeastRig b;
    wolf_with(b, {"second wind"});
    REQUIRE(floor_of(b, 1234) == 1);
    b.reply("beastmaster.talents Wolf reset");
    one_step(b);
    CHECK(floor_of(b, 1234) == 0);
}

// ---- the beast's tab of the Beasts window ----
namespace {
// three_beasts with Fang at loyalty 3 (2 talent points) out as entity 1234, the host at a stable with the window open.
void fang_window(BeastRig& b) {
    three_beasts(b);
    b.reply("beastmaster.loyalty Fang 3");
    next_beast(b, 1234);
    REQUIRE(b.reply("beastmaster.call Fang").find("comes") != std::string::npos);
    at_stable(b);
    b.seconds(1);
    press(b, kE);
    REQUIRE(b.r.fake.windows.size() == 1u);
}
bool lit(const Fake::Win& w, const std::string& key) { return (flags_at(w, key) & EMF_UI_SELECTED) != 0u; }
bool off(const Fake::Win& w, const std::string& key) { return (flags_at(w, key) & EMF_UI_DISABLED) != 0u; }
// The window's picture row, or -1.
int image_row(const Fake::Win& w) {
    for (size_t i = 0; i < w.widgets.size(); ++i)
        if (w.widgets[i].type == EMF_UI_IMAGE) return static_cast<int>(i);
    return -1;
}
// The icon of the window's picture row as last drawn (0: none).
uint32_t picture(BeastRig& b) {
    const int i = image_row(shown(b));
    return i < 0 ? 0u : shown(b).widgets[static_cast<size_t>(i)].icon;
}
}  // namespace

TEST_CASE("beast tab: the selected beast's portrait turns under its name (EMF's picture of its kind, loaded once)") {
    BeastRig b;
    with_images(b.r.api);  // a v26 core
    fang_window(b);
    CHECK(image_row(shown(b)) == -1);  // the Stable tab has none
    CHECK(b.r.fake.images_loaded.empty());
    click(b, "beast.Fang");
    {
        const Fake::Win& w = shown(b);
        const int pic = image_row(w);
        REQUIRE(pic > 0);
        CHECK(w.texts[static_cast<size_t>(pic - 1)] == "Fang");  // right under its name
        CHECK(w.widgets[static_cast<size_t>(pic)].icon == 50u);
        CHECK(w.widgets[static_cast<size_t>(pic)].fps == 8u);
        // 6 lines: with the picture the tab's last row (Make active, Store, Release) still fits a window of 85 % of a
        // 995 px high game window, as the tab with every talent is
        CHECK(w.widgets[static_cast<size_t>(pic)].height == doctest::Approx(6.0));
    }
    CHECK(b.r.fake.images_loaded == std::vector<std::string>{"portrait:beastmaster.wolf"});
    click(b, "tab.stable");
    CHECK(picture(b) == 0u);
    click(b, "tab.beast");
    CHECK(picture(b) == 50u);
    click(b, "beast.Tusk");  // the boar
    CHECK(picture(b) == 51u);
    click(b, "beast.Fang");
    CHECK(picture(b) == 50u);
    CHECK(b.r.fake.images_loaded == std::vector<std::string>{"portrait:beastmaster.wolf", "portrait:beastmaster.boar"});
}

TEST_CASE("beast tab: without its picture (not drawn, or a core before v26) the tab has no picture row and no gap") {
    for (bool v26 : {true, false}) {
        CAPTURE(v26);
        BeastRig b;
        if (v26) {
            with_images(b.r.api);
            b.r.fake.image_ok = false;  // the cache has no picture of it
        }
        fang_window(b);
        click(b, "beast.Fang");
        CHECK(image_row(shown(b)) == -1);
        CHECK(shows(shown(b), "Wolf, level 10"));
        CHECK(b.host->status(b.id) == "running");
        click(b, "tab.stable");
        click(b, "tab.beast");
        CHECK(b.r.fake.images_loaded.size() == (v26 ? 1u : 0u));  // a refused picture is not asked for again
    }
}

TEST_CASE("beastmaster: mod.json asks EMF for a portrait of every kind, the wilds pack's too (beastmaster.<id>, its template)") {
    const std::string dir = std::string(EMF_SOURCE_DIR) + "/mods/beastmaster/";
    const std::string species = slurp(std::filesystem::path(dir + "species.lua").wstring());
    std::map<std::string, std::string> kinds;  // id -> template, from species.lua's list
    const std::regex entry(R"re(\{ id = "([a-z_]+)",[^\n]*template = "([0-9a-f-]{36})")re");
    for (auto it = std::sregex_iterator(species.begin(), species.end(), entry); it != std::sregex_iterator(); ++it)
        kinds[(*it)[1]] = (*it)[2];
    REQUIRE(kinds.size() == 12u);  // wolf, boar, sabretooth, hound, rat, bear, aurochs, four snakes and the pack llama
    // and the wilds pack's fighting animals (species_wilds.lua: an entry spans lines)
    const std::string wilds_file = slurp(std::filesystem::path(dir + "species_wilds.lua").wstring());
    const size_t from = wilds_file.find("M.beasts = {"), to = wilds_file.find("M.kinds = {");
    REQUIRE(from != std::string::npos);
    REQUIRE(to != std::string::npos);
    const std::string wilds = wilds_file.substr(from, to - from);
    const std::regex wild_entry(R"re(\{ id = "([a-z_]+)",[^}]*?template = "([0-9a-f-]{36})")re");
    for (auto it = std::sregex_iterator(wilds.begin(), wilds.end(), wild_entry); it != std::sregex_iterator(); ++it)
        kinds[(*it)[1]] = (*it)[2];
    REQUIRE(kinds.size() == 32u);
    std::vector<std::string> problems;
    const auto items = emf::loader::read_portrait_lists(
        {{"beastmaster", slurp(std::filesystem::path(dir + "mod.json").wstring())}}, &problems);
    CHECK(problems.empty());
    CHECK(items.size() == kinds.size());
    for (const auto& [id, tpl] : kinds) {
        CAPTURE(id);
        const auto it = std::find_if(items.begin(), items.end(),
                                     [&](const emf::loader::PortraitItem& p) { return p.key == "beastmaster." + id; });
        REQUIRE(it != items.end());
        CHECK(it->template_guid == tpl);
        CHECK(it->model_guid.empty());
        CHECK(it->frames == 24);
        CHECK(it->size == 192);
    }
}

TEST_CASE("talent (1.20): a beast's tab shows its health and damage with the Beast health and Beast damage settings") {
    BeastRig b;  // the defaults: 150 % and 125 %
    fang_window(b);
    click(b, "beast.Fang");
    CHECK(shows(shown(b), "Health x1.88, damage x1.50, speed x1.00"));  // 1.25 x 1.5 and 1.20 x 1.25
}

TEST_CASE("talent: selecting a beast opens its tab with its stats and talents; Stable goes back") {
    BeastRig b({{"beast_health", "100"}, {"beast_damage", "100"}});  // level 10
    fang_window(b);
    CHECK(lit(shown(b), "tab.stable"));
    CHECK(row_at(shown(b), "tab.beast") == -1);  // nothing selected: the Stable tab alone
    click(b, "beast.Fang");
    const Fake::Win& w = shown(b);
    CHECK_FALSE(lit(w, "tab.stable"));
    CHECK(text_at(w, "tab.beast") == "Fang");
    CHECK(lit(w, "tab.beast"));
    CHECK(row_at(w, "active.1") == -1);  // the slots and the list are on the Stable tab
    CHECK(row_at(w, "beast.Tusk") == -1);
    for (const char* line : {"Wolf, level 10", "Loyalty 3: 30 kills to loyalty 4", "Health x1.25, damage x1.20, speed x1.00",
                             "Out, active", "Talents: 2 points (2 left)", "Strength 0/5: +4 % damage a point",
                             "Guard 0/2: no damage for the first 3 s of a fight (5 s with 2 points), once every 30 s"})
        CHECK_MESSAGE(shows(w, line), line);
    for (const char* t : {"talent.strength", "talent.toughness", "talent.swiftness", "talent.second_wind", "talent.ambush",
                          "talent.guard"})
        CHECK_MESSAGE(!off(w, t), t);
    CHECK(off(w, "reset_talents"));  // nothing spent
    CHECK(off(w, "make_active"));    // active already
    CHECK_FALSE(off(w, "store"));
    CHECK_FALSE(off(w, "release"));
    click(b, "tab.stable");
    CHECK(lit(shown(b), "tab.stable"));
    CHECK(shows(shown(b), "Active 1/2"));
    CHECK(lit(shown(b), "beast.Fang"));  // still selected
    CHECK(text_at(shown(b), "tab.beast") == "Fang");
    click(b, "tab.beast");
    CHECK(shows(shown(b), "Out, active"));
    click(b, "tab.stable");
    click(b, "beast.Tusk");  // another beast: its tab
    CHECK(text_at(shown(b), "tab.beast") == "Tusk");
    CHECK(shows(shown(b), "Boar, level 10"));
    CHECK(shows(shown(b), "Loyalty 1: 10 kills to loyalty 2"));
    CHECK(shows(shown(b), "At home, in the stable"));
    CHECK(shows(shown(b), "Talents: no points yet"));
    CHECK(off(shown(b), "talent.strength"));
}

TEST_CASE("talent: + spends a point at once on a beast out; full or no point left turns + off; Reset talents gives every point back") {
    BeastRig b({{"beast_health", "100"}, {"beast_damage", "100"}});
    fang_window(b);
    const double f = 1 + 2.0 / 9 * 0.2;  // loyalty 3
    click(b, "beast.Fang");
    click(b, "talent.strength");
    CHECK(shows(shown(b), "Strength 1/5: +4 % damage"));
    CHECK(shows(shown(b), "Fang: Strength 1/5, +4 % damage (1 point left)."));
    CHECK(shows(shown(b), "Talents: 2 points (1 left)"));
    CHECK(now_of<ecs::DamageMod>(b, 1234).dataStorage[2] == doctest::Approx(1.15 * f * 1.04));
    click(b, "talent.second_wind");
    CHECK(shows(shown(b), "Second Wind 1/1: a killing blow leaves it at 20 % health, then 5 min before it works again (ready)"));
    CHECK(shows(shown(b), "Talents: 2 points (0 left)"));
    for (const char* t : {"talent.strength", "talent.second_wind", "talent.guard"}) CHECK_MESSAGE(off(shown(b), t), t);
    CHECK(b.saved().find("t 0x00000000000000aa strength:1,second_wind:1 Fang") != std::string::npos);
    CHECK_FALSE(off(shown(b), "reset_talents"));
    click(b, "reset_talents");
    CHECK(shows(shown(b), "Fang's talents are reset: 2 points to spend."));
    CHECK(shows(shown(b), "Talents: 2 points (2 left)"));
    CHECK(now_of<ecs::DamageMod>(b, 1234).dataStorage[2] == doctest::Approx(1.15 * f));
    CHECK(off(shown(b), "reset_talents"));
    b.reply("beastmaster.loyalty Fang 10");
    b.seconds(1);
    click(b, "talent.second_wind");
    CHECK(off(shown(b), "talent.second_wind"));  // full (1/1)
    CHECK_FALSE(off(shown(b), "talent.strength"));
}

TEST_CASE("talent: a beast released on its tab, or by chat while its tab is open, leaves the Stable tab showing") {
    BeastRig b;
    fang_window(b);
    click(b, "beast.Fang");
    click(b, "release");
    CHECK(shows(shown(b), "Click Release again within 5 s to let Fang go for good."));
    click(b, "release");
    CHECK(shows(shown(b), "Fang is gone: you let it go."));
    CHECK(lit(shown(b), "tab.stable"));
    CHECK(row_at(shown(b), "tab.beast") == -1);
    CHECK(shows(shown(b), "Stable 2/8"));
    click(b, "beast.Tusk");
    REQUIRE(text_at(shown(b), "tab.beast") == "Tusk");
    b.reply("beastmaster.release Tusk");  // the host's console, at the stable
    REQUIRE(b.reply("beastmaster.release Tusk") == "Tusk is gone: you let it go.");
    b.seconds(1);  // the window is drawn again
    CHECK(lit(shown(b), "tab.stable"));
    CHECK(row_at(shown(b), "tab.beast") == -1);
    CHECK(shows(shown(b), "Stable 1/8"));
}

// ---- talents: fixes after review ----
TEST_CASE("talent: an id that stops being our beast (its faction changes) keeps no floor or speed of ours") {
    BeastRig b;
    wolf_with(b, {"second wind", "swiftness"});
    REQUIRE(floor_of(b, 1234) == 1);
    REQUIRE(speed_of(b, 1234) == doctest::Approx(1.04));
    component<ecs::Faction>(b.r.fake, 1234).faction = static_cast<emf::gen::FactionType>(kSkeletonFoe);
    one_step(b);  // never written on a creature that is not the players' ally, and given back at once
    CHECK(floor_of(b, 1234) == 0);
    CHECK(speed_of(b, 1234) == doctest::Approx(1.0));
    b.seconds(2);  // the check that drops it
    CHECK(has(b.reply("beastmaster.list"), "1. Wolf - wolf, loyalty 10, at home"));
    CHECK(floor_of(b, 1234) == 0);
    CHECK(speed_of(b, 1234) == doctest::Approx(1.0));
}

TEST_CASE("talent: an id dropped because it guards someone else gets the game's minimum and speed back") {
    BeastRig b;
    wolf_with(b, {"second wind", "swiftness"});
    REQUIRE(floor_of(b, 1234) == 1);
    component<ecs::DynamicEnemy01BehaviorState>(b.r.fake, 1234).guardEntityId.id = 999;  // another entity's guard now
    b.seconds(4);
    REQUIRE(has(b.reply("beastmaster.list"), "1. Wolf - wolf, loyalty 10, at home"));
    CHECK(floor_of(b, 1234) == 0);
    CHECK(speed_of(b, 1234) == doctest::Approx(1.0));
}

TEST_CASE("talent: a beast sent home that is no longer ours (not destroyed) gets the game's minimum and speed back") {
    BeastRig b;
    wolf_with(b, {"second wind", "swiftness"});
    component<ecs::Faction>(b.r.fake, 1234).faction = static_cast<emf::gen::FactionType>(kSkeletonFoe);
    b.reply("beastmaster.dismiss Wolf");  // before any step: it is left alive
    REQUIRE_FALSE(contains(b.r.fake.destroyed, 1234));
    b.seconds(2);
    CHECK(floor_of(b, 1234) == 0);
    CHECK(speed_of(b, 1234) == doctest::Approx(1.0));
}

TEST_CASE("talent: a floor or speed the game changed meanwhile is left as the game has it when the id leaves") {
    BeastRig b;
    wolf_with(b, {"second wind", "swiftness"});
    component<ecs::Faction>(b.r.fake, 1234).faction = static_cast<emf::gen::FactionType>(kSkeletonFoe);
    live_of<ecs::Health>(b, 1234).dataStorage[1] = 7;       // the game's own values now
    live_of<ecs::Movement>(b, 1234).dataStorage[1] = 0.6f;
    b.seconds(2);
    CHECK(floor_of(b, 1234) == 7);
    CHECK(speed_of(b, 1234) == doctest::Approx(0.6));
}

TEST_CASE("talent: Ambush is used up but strikes nothing when the first hit already killed the foe") {
    BeastRig b;
    wolf_with(b, {"ambush"});
    b.r.fake.attrs[{70, EMF_ATTR_HEALTH}] = 0;  // the bite killed it
    deliver(b.r.fake, "HitEvent", hit(1234, 70, -40));
    CHECK(b.r.fake.damages.empty());
    creature(b, 71, 0, 12, kSkeletonFoe);
    deliver(b.r.fake, "HitEvent", hit(1234, 71, -40));  // the same fight: it was used up
    CHECK(b.r.fake.damages.empty());
}

TEST_CASE("talent: Guard works at most once every 30 s") {
    BeastRig b;
    wolf_with(b, {"guard"});
    auto opening = [&]() {
        b.r.fake.attrs[{1234, EMF_ATTR_HEALTH}] = 70;
        b.r.fake.attr_adds.clear();
        deliver(b.r.fake, "HitEvent", hit(70, 1234, -30));
    };
    opening();
    CHECK(b.added(1234, EMF_ATTR_HEALTH, 30));
    CHECK(floor_of(b, 1234) == 100);
    b.seconds(12);  // a new fight (10 s quiet), 12 s after Guard last worked
    opening();
    CHECK_FALSE(b.added(1234, EMF_ATTR_HEALTH, 30));
    CHECK(floor_of(b, 1234) == 0);
    b.seconds(19);  // 31 s after Guard last worked, 19 s quiet
    opening();
    CHECK(b.added(1234, EMF_ATTR_HEALTH, 30));
    CHECK(floor_of(b, 1234) == 100);
}

// ---- threat: how much of the monsters in a fight the beasts draw ----
TEST_CASE("threat: the beast tab says how much of the monsters a beast draws") {
    BeastRig b;
    fang_window(b);  // Fang, a wolf at loyalty 3 (2 points), out
    click(b, "beast.Fang");
    CHECK(shows(shown(b), "Draws 20 % of the monsters in a fight"));
    click(b, "talent.menace");
    CHECK(shows(shown(b), "Menace 1/3: draws 10 % more of the monsters"));
    CHECK(shows(shown(b), "Draws 30 % of the monsters in a fight"));
    click(b, "talent.taunt");
    CHECK(shows(shown(b), "Taunt 1/2: roars when a monster near it hits you: the monsters within 10 m turn on it for 6 s, "
                          "every 45 s"));
    click(b, "tab.stable");
    click(b, "beast.Tusk");  // a boar: a tank
    CHECK(shows(shown(b), "Draws 50 % of the monsters in a fight"));
    b.reply("beastmaster.mode passive");
    b.seconds(1);
    CHECK(shows(shown(b), "Draws no monsters (passive mode)"));
}

namespace {
// What monster `id` goes for now (SelectedTargets.targets[0]), and whether the game may switch its target.
uint32_t pick_of(BeastRig& b, uint32_t id) { return now_of<ecs::SelectedTargets>(b, id).targets[0].targetEntityId.id; }
uint32_t root_of(BeastRig& b, uint32_t id) { return now_of<ecs::SelectedTargets>(b, id).targets[0].targetRootEntityId.id; }
bool switches(BeastRig& b, uint32_t id) { return now_of<ecs::DynamicAggro>(b, id).allowTargetSwitches; }
// The game picks `pick` for monster `id` (0: nothing).
void game_picks(BeastRig& b, uint32_t id, uint32_t pick) {
    auto& t = component<ecs::SelectedTargets>(b.r.fake, id).targets[0];
    t.targetEntityId.id = pick;
    t.targetRootEntityId.id = pick;
}
// A hostile monster (a skeleton unless `faction`) at (x, 10, z), 100 of 100, that goes for `pick` (0: nothing), its
// target switches on.
void monster(BeastRig& b, uint32_t id, double x, double z, uint32_t pick, int faction = kSkeletonFoe) {
    creature(b, id, x, z, faction);
    component<ecs::DynamicAggro>(b.r.fake, id).allowTargetSwitches = true;
    game_picks(b, id, pick);
}
// Calls player 1's beast `name` (in the collection already) as entity `id` and puts it at (x, 10, z), guarding them.
void out_at(BeastRig& b, const std::string& name, uint32_t id, double x, double z) {
    next_beast(b, id);
    REQUIRE(b.reply("beastmaster.call " + name).find("comes") != std::string::npos);
    component<ecs::DynamicEnemy01BehaviorState>(b.r.fake, id).guardEntityId.id = 1;
    place(b.r.fake, id, x, 10, z);
}
size_t on(BeastRig& b, std::initializer_list<uint32_t> monsters, uint32_t target) {
    size_t n = 0;
    for (uint32_t m : monsters) n += pick_of(b, m) == target;
    return n;
}
}  // namespace

TEST_CASE("threat: a monster that goes for you joins the fight and is held on a beast by its share for at least 8 s, then picks again; one left to you is never written on") {
    RollRig b;
    b.reply("beastmaster.give boar");
    out_at(b, "Boar", 1234, 0, 4);
    monster(b, 70, 10, 2, 1);  // as far from you as from the boar, going for you
    b.roll(0.6);               // over your half: the boar
    b.seconds(1);              // the next look at the fights
    CHECK(pick_of(b, 70) == 1234);
    CHECK(root_of(b, 70) == 1234);
    CHECK_FALSE(switches(b, 70));
    game_picks(b, 70, 1);  // the game picks you again
    one_step(b);
    CHECK(pick_of(b, 70) == 1234);  // held, every step
    b.roll(0.4);
    b.seconds(6);
    CHECK(pick_of(b, 70) == 1234);
    b.seconds(2);  // 8 s: it picks again: you; the game's own pick, switches on
    CHECK(switches(b, 70));
    CHECK(has(b.reply("beastmaster.threat"), "70 on you"));
    game_picks(b, 70, 0);
    b.seconds(2);
    CHECK(pick_of(b, 70) == 0);  // nothing written
    CHECK(switches(b, 70));
}

TEST_CASE("pack beast: a monster is never held on the pack llama (it draws none)") {
    RollRig b;
    b.reply("beastmaster.give packllama");
    out_at(b, "Pack llama", 1234, 0, 4);
    monster(b, 70, 10, 2, 1);  // going for you, as far from you as from the llama
    b.roll(0.95);              // a roll that would give it to a beast
    b.seconds(1);
    CHECK(pick_of(b, 70) != 1234u);
    b.seconds(4);
    CHECK(pick_of(b, 70) != 1234u);
}

TEST_CASE("threat: a monster that hits your beast, or that you or your beast hit, joins the fight") {
    RollRig b;
    b.reply("beastmaster.give boar");
    out_at(b, "Boar", 1234, 0, 4);
    b.roll(0.99);  // the boar
    for (uint32_t m : {71u, 72u, 73u, 74u}) monster(b, m, 6, 4, 0);
    deliver(b.r.fake, "HitEvent", hit(1234, 71, -5));
    deliver(b.r.fake, "HitEvent", hit(1, 72, -5));
    deliver(b.r.fake, "HitEvent", hit(73, 1234, -5));
    deliver(b.r.fake, "HitEvent", hit(74, 2, -5));  // player 2 has no beast out
    one_step(b);
    const std::string held = b.reply("beastmaster.threat");
    CAPTURE(held);
    CHECK(has(held, "71 on Boar"));
    CHECK(has(held, "72 on you"));  // the second of a fight: 1 in 4 from 2, rounded up
    CHECK(has(held, "73 on Boar"));
    CHECK_FALSE(has(held, "74 on"));
    for (uint32_t m : {71u, 73u}) {
        CAPTURE(m);
        CHECK(pick_of(b, m) == 1234);
        CHECK_FALSE(switches(b, m));
    }
    CHECK(pick_of(b, 74) == 0);
    CHECK(switches(b, 74));
}

TEST_CASE("threat: shares: a tank 50 %, another beast 20 %, Menace 10 % a point; your beasts at most 90 %, of a boss at most 35 %") {
    RollRig b({{"call_cooldown_s", "0"}});
    b.reply("beastmaster.give boar");
    b.reply("beastmaster.give wolf");
    b.reply("beastmaster.loyalty Boar 10");
    b.reply("beastmaster.loyalty Wolf 10");
    at_stable(b);
    out_at(b, "Boar", 1234, 10, 20);  // you at (0, 10, 10), the monsters at (0, 10, 20): all three 10 m from them
    out_at(b, "Wolf", 1235, -10, 20);
    place(b.r.fake, 1, 0, 10, 10);
    uint32_t next = 70;
    auto pick_at = [&](double roll, bool boss = false) {
        const uint32_t m = next++;
        monster(b, m, 0, 20, 1);
        if (boss) component<ecs::Boss>(b.r.fake, m);
        b.roll(roll);
        b.seconds(1);
        const uint32_t got = pick_of(b, m);
        b.r.fake.attrs[{m, EMF_ATTR_HEALTH}] = 0;  // it falls: the next one fights alone
        b.seconds(1);
        return got;
    };
    // the boar 50 %, the wolf 20 %: you 30 %
    CHECK(pick_at(0.29) == 1u);
    CHECK(pick_at(0.31) == 1234u);
    CHECK(pick_at(0.79) == 1234u);
    CHECK(pick_at(0.81) == 1235u);
    place(b.r.fake, 1, 0, 10, 0);  // to the stable: Menace 3 on both (80 % and 50 %: 130 %, at most 90 %)
    for (int i = 0; i < 3; ++i) {
        REQUIRE(b.reply("beastmaster.talent Boar menace").rfind("Boar: ", 0) == 0);
        REQUIRE(b.reply("beastmaster.talent Wolf menace").rfind("Wolf: ", 0) == 0);
    }
    place(b.r.fake, 1, 0, 10, 10);
    CHECK(pick_at(0.09) == 1u);
    CHECK(pick_at(0.11) == 1234u);  // 90 % x 80/130 = 55.4 %
    CHECK(pick_at(0.64) == 1234u);
    CHECK(pick_at(0.66) == 1235u);
    // a boss: your beasts 35 % at most
    CHECK(pick_at(0.64, true) == 1u);
    CHECK(pick_at(0.66, true) == 1234u);  // 35 % x 80/130 = 21.5 %
    CHECK(pick_at(0.87, true) == 1235u);
}

TEST_CASE("threat: at least 1 in 4 monsters of a fight stays on you, rounded up from 2 monsters; a single one can go to a beast") {
    RollRig b({{"tank_share_pct", "90"}});
    b.reply("beastmaster.give boar");
    out_at(b, "Boar", 1234, 0, 4);
    b.roll(0.99);  // every pick by share: the boar
    monster(b, 70, 6, 4, 1);
    b.seconds(1);
    CHECK(pick_of(b, 70) == 1234);  // alone: by share
    monster(b, 71, 7, 4, 1);
    b.seconds(1);
    CHECK(on(b, {70, 71}, 1) == 1u);  // 2 monsters: 1 on you
    for (int i = 0; i < 2; ++i) monster(b, 72 + i, 8.0 + i, 4, 1);
    b.seconds(1);
    CHECK(on(b, {70, 71, 72, 73}, 1) == 1u);
    CHECK(on(b, {70, 71, 72, 73}, 1234) == 3u);
    monster(b, 74, -6, 4, 1);
    b.seconds(1);
    CHECK(on(b, {70, 71, 72, 73, 74}, 1) == 2u);  // 5: 2 on you
    for (int i = 0; i < 3; ++i) monster(b, 75 + i, -7.0 - i, 4, 1);
    b.seconds(1);
    CHECK(on(b, {70, 71, 72, 73, 74, 75, 76, 77}, 1) == 2u);
    monster(b, 80, 6, -4, 1);
    b.seconds(1);
    CHECK(on(b, {70, 71, 72, 73, 74, 75, 76, 77, 80}, 1) == 3u);  // 9: 3 on you
}

TEST_CASE("threat: a monster picks again at once when its beast is sent home, falls or is out of its reach; with no beast left it gets its own pick back") {
    RollRig b({{"call_cooldown_s", "0"}});
    b.reply("beastmaster.give boar");
    b.reply("beastmaster.give wolf");
    out_at(b, "Boar", 1234, 0, 4);
    out_at(b, "Wolf", 1235, 0, -4);
    monster(b, 70, 10, 0, 1);
    b.roll(0.99);  // the last: the wolf, then the boar
    b.seconds(1);
    REQUIRE(pick_of(b, 70) == 1235);
    b.reply("beastmaster.dismiss Wolf");  // sent home
    one_step(b);
    CHECK(pick_of(b, 70) == 1234);
    place(b.r.fake, 1234, 0, 10, 40);  // the boar 40 m from the monster: out of its reach
    one_step(b);
    CHECK(switches(b, 70));  // no beast in reach: the game's own pick
    game_picks(b, 70, 1);
    b.seconds(2);
    CHECK(pick_of(b, 70) == 1);
    CHECK(switches(b, 70));
    place(b.r.fake, 1234, 0, 10, 4);  // back
    b.seconds(1);
    REQUIRE(pick_of(b, 70) == 1234);
    deliver(b.r.fake, "EntityDiedEvent", died_event(1234));  // it falls
    one_step(b);
    CHECK(switches(b, 70));
}

TEST_CASE("threat: never on a monster not in the fight, a player-side creature, in passive mode or with the setting off; a monster tamed meanwhile gets its own pick back") {
    RollRig b;
    b.reply("beastmaster.give boar");
    out_at(b, "Boar", 1234, 0, 4);
    b.roll(0.99);  // a pick would be the boar
    monster(b, 80, 8, 0, 0);               // not fighting
    monster(b, 81, 6, 0, 1, kPlayerAlly);  // on the players' side
    monster(b, 82, 25, 0, 2);              // fighting player 2, who has no beast out
    b.seconds(2);
    for (uint32_t m : {80u, 81u, 82u}) {
        CAPTURE(m);
        CHECK(switches(b, m));
    }
    CHECK(pick_of(b, 80) == 0);
    CHECK(pick_of(b, 81) == 1);
    CHECK(pick_of(b, 82) == 2);
    monster(b, 70, 6, 4, 1);
    b.seconds(1);
    REQUIRE(pick_of(b, 70) == 1234);
    component<ecs::Faction>(b.r.fake, 70).faction = static_cast<emf::gen::FactionType>(kPlayerAlly);  // tamed
    one_step(b);
    CHECK(switches(b, 70));
    game_picks(b, 70, 0);
    b.seconds(2);
    CHECK(pick_of(b, 70) == 0);
    monster(b, 71, 6, 4, 1);
    b.seconds(1);
    REQUIRE(pick_of(b, 71) == 1234);
    b.reply("beastmaster.mode passive");
    one_step(b);
    CHECK(switches(b, 71));
    monster(b, 72, 6, 4, 1);
    b.seconds(2);
    CHECK(pick_of(b, 72) == 1);
    CHECK(switches(b, 72));
    b.reply("beastmaster.mode defending");
    monster(b, 73, 6, 4, 1);
    b.seconds(1);
    REQUIRE(pick_of(b, 73) == 1234);
    b.r.fake.settings["beastmaster.threat"] = "false";
    one_step(b);
    CHECK(switches(b, 73));
    monster(b, 74, 6, 4, 1);
    b.seconds(2);
    CHECK(pick_of(b, 74) == 1);
    CHECK(switches(b, 74));
}

// A creature the game keeps pacified (the game's Beastmaster skill on Vukah) is no foe: the beasts never draw it into
// the fight, not even when it hits their player, and one held that becomes pacified is let go.
TEST_CASE("threat (1.20): a pacified monster is never drawn to the beasts, even after hitting their player; one held that becomes pacified gets its own pick back") {
    RollRig b;
    b.reply("beastmaster.give boar");
    out_at(b, "Boar", 1234, 0, 4);
    b.roll(0.99);  // a pick would be the boar
    monster(b, 75, 6, 4, 1);
    pacify(b, 75, true);
    b.seconds(2);
    CHECK(pick_of(b, 75) == 1);  // left to the game
    CHECK(switches(b, 75));
    deliver(b.r.fake, "HitEvent", hit_event(75, 1));  // it hits the player beside the boar anyway
    one_step(b);
    CHECK(pick_of(b, 75) == 1);
    CHECK(switches(b, 75));
    monster(b, 76, 6, 4, 1);
    b.seconds(1);
    REQUIRE(pick_of(b, 76) == 1234);
    REQUIRE_FALSE(switches(b, 76));
    pacify(b, 76, true);  // the game makes its peace
    one_step(b);
    CHECK(switches(b, 76));  // let go
    game_picks(b, 76, 1);
    b.seconds(2);
    CHECK(pick_of(b, 76) == 1);
}

// ---- Taunt ----
namespace {
// Player 1's beasts `names` (given now) at loyalty 10 with Taunt spent `points` times, at a stable.
void taunters(BeastRig& b, std::initializer_list<const char*> kinds, int points = 1) {
    for (const char* k : kinds) b.reply(std::string("beastmaster.give ") + k);
    for (const char* name : {"Wolf", "Boar"}) b.reply(std::string("beastmaster.loyalty ") + name + " 10");
    at_stable(b);
    for (const char* k : kinds) {
        std::string name = k;
        name[0] = static_cast<char>(name[0] - 'a' + 'A');
        for (int i = 0; i < points; ++i) REQUIRE(b.reply("beastmaster.talent " + name + " taunt").rfind(name + ": ", 0) == 0);
    }
}
}  // namespace

TEST_CASE("threat: Taunt: a monster hitting you within 10 m of the beast makes it roar: the fight's monsters within 10 m turn on it for 6 s; 45 s before it roars again, 30 s with 2 points") {
    RollRig b;
    taunters(b, {"wolf"});
    out_at(b, "Wolf", 1234, 0, 4);
    b.roll(0.0);                // every pick by share: you
    monster(b, 70, 3, 4, 1);    // 3 m from the wolf
    monster(b, 71, -4, 4, 1);   // 4 m from it
    monster(b, 72, 0, 10, 0);   // 6 m from it, not in the fight
    monster(b, 73, 0, -16, 1);  // 20 m from it
    b.seconds(1);
    REQUIRE(pick_of(b, 70) == 1);
    REQUIRE(pick_of(b, 71) == 1);
    deliver(b.r.fake, "HitEvent", hit(70, 1, -10));
    CHECK(notes_with(b.r.fake, "Wolf roars: the monsters turn on it.") == 1);
    one_step(b);
    CHECK(pick_of(b, 70) == 1234);
    CHECK(pick_of(b, 71) == 1234);
    CHECK(pick_of(b, 72) == 0);
    CHECK(switches(b, 72));
    CHECK(pick_of(b, 73) == 1);
    b.seconds(5);
    CHECK(pick_of(b, 70) == 1234);  // over the shares (they pick you)
    CHECK_FALSE(switches(b, 70));
    b.seconds(2);                   // 6 s: they pick as usual: you, the game's own pick
    CHECK(switches(b, 70));
    b.seconds(3);
    deliver(b.r.fake, "HitEvent", hit(70, 1, -10));  // 10 s after the roar
    CHECK(notes_with(b.r.fake, "Wolf roars") == 1);
    b.seconds(36);
    deliver(b.r.fake, "HitEvent", hit(70, 1, -10));  // 46 s after it
    CHECK(notes_with(b.r.fake, "Wolf roars") == 2);
    REQUIRE(b.reply("beastmaster.talent Wolf taunt").rfind("Wolf: Taunt 2/2", 0) == 0);
    b.seconds(20);
    deliver(b.r.fake, "HitEvent", hit(70, 1, -10));
    CHECK(notes_with(b.r.fake, "Wolf roars") == 2);
    b.seconds(11);  // 31 s after the last roar
    deliver(b.r.fake, "HitEvent", hit(70, 1, -10));
    CHECK(notes_with(b.r.fake, "Wolf roars") == 3);
}

TEST_CASE("threat: only the nearest ready beast roars; a beast too far does not and keeps its cooldown; none in passive mode") {
    RollRig b({{"call_cooldown_s", "0"}});
    taunters(b, {"wolf", "boar"});
    out_at(b, "Wolf", 1234, 0, 4);
    out_at(b, "Boar", 1235, 0, -8);
    b.roll(0.0);
    monster(b, 69, 25, 0, 1);  // more than 10 m from both
    monster(b, 70, 3, 6, 1);   // 4 m from the wolf, 14 m from the boar
    monster(b, 71, 0, -1, 1);  // 5 m from the wolf, 7 m from the boar
    monster(b, 72, 0, -5, 1);  // 9 m from the wolf, 3 m from the boar
    b.seconds(1);
    deliver(b.r.fake, "HitEvent", hit(69, 1, -10));
    CHECK(notes_with(b.r.fake, "roars") == 0);       // too far: neither roars, both stay ready
    deliver(b.r.fake, "HitEvent", hit(71, 1, -10));  // both ready and in reach: the nearer
    CHECK(notes_with(b.r.fake, "Wolf roars") == 1);
    CHECK(notes_with(b.r.fake, "Boar roars") == 0);
    deliver(b.r.fake, "HitEvent", hit(72, 1, -10));  // the wolf waits: the boar is the nearest ready one
    CHECK(notes_with(b.r.fake, "Wolf roars") == 1);
    CHECK(notes_with(b.r.fake, "Boar roars") == 1);
    one_step(b);
    CHECK(pick_of(b, 70) == 1234);  // the wolf's roar only
    CHECK(pick_of(b, 71) == 1235);  // the boar's roar came later
    CHECK(pick_of(b, 72) == 1235);
    b.seconds(46);
    b.reply("beastmaster.mode passive");
    deliver(b.r.fake, "HitEvent", hit(70, 1, -10));
    CHECK(notes_with(b.r.fake, "roars") == 2);
}

TEST_CASE("threat: a boss in the middle of an attack keeps its target until the attack ends (a roar, a join), then turns") {
    RollRig b;
    taunters(b, {"wolf"});
    out_at(b, "Wolf", 1234, 0, 4);
    b.roll(0.0);
    monster(b, 75, 2, 4, 1);
    component<ecs::Boss>(b.r.fake, 75);
    auto& attacking = component<ecs::DynamicEnemy01BehaviorState>(b.r.fake, 75);
    attacking.state = emf::gen::enemy01::State::ControlledByAttack;
    monster(b, 76, -2, 4, 1);
    component<ecs::Boss>(b.r.fake, 76);
    component<ecs::DynamicEnemy01BehaviorState>(b.r.fake, 76).state = emf::gen::enemy01::State::Combat;
    b.seconds(1);
    deliver(b.r.fake, "HitEvent", hit(75, 1, -10));
    CHECK(notes_with(b.r.fake, "Wolf roars") == 1);
    one_step(b);
    CHECK(pick_of(b, 75) == 1);     // mid-attack: it keeps you for now
    CHECK(pick_of(b, 76) == 1234);  // the other boss turns
    b.seconds(2);
    CHECK(pick_of(b, 75) == 1);
    live_of<ecs::DynamicEnemy01BehaviorState>(b, 75).state = emf::gen::enemy01::State::Combat;  // the attack ends
    one_step(b);
    CHECK(pick_of(b, 75) == 1234);  // within the roar's 6 s
    b.r.fake.attrs[{75, EMF_ATTR_HEALTH}] = 0;
    b.r.fake.attrs[{76, EMF_ATTR_HEALTH}] = 0;
    b.seconds(1);
    monster(b, 77, 0, 8, 1);  // a boss joins mid-attack, picked for the wolf
    component<ecs::Boss>(b.r.fake, 77);
    component<ecs::DynamicEnemy01BehaviorState>(b.r.fake, 77).state = emf::gen::enemy01::State::ControlledByAttack;
    b.roll(0.99);
    b.seconds(1);
    CHECK(has(b.reply("beastmaster.threat"), "77 on Wolf"));
    CHECK(pick_of(b, 77) == 1);
    CHECK(switches(b, 77));
    live_of<ecs::DynamicEnemy01BehaviorState>(b, 77).state = emf::gen::enemy01::State::Combat;
    one_step(b);
    CHECK(pick_of(b, 77) == 1234);
}

TEST_CASE("threat: a roar that turns no monster says nothing and keeps the cooldown") {
    RollRig b;
    taunters(b, {"wolf"});
    out_at(b, "Wolf", 1234, 0, 4);
    b.roll(0.0);
    creature(b, 79, 2, 4, kSkeletonFoe);  // a monster whose target cannot be held
    b.seconds(1);
    deliver(b.r.fake, "HitEvent", hit(79, 1, -10));
    CHECK(notes_with(b.r.fake, "roars") == 0);
    monster(b, 70, 2, 5, 1);
    b.seconds(1);
    deliver(b.r.fake, "HitEvent", hit(70, 1, -10));
    CHECK(notes_with(b.r.fake, "Wolf roars: the monsters turn on it.") == 1);
}

TEST_CASE("threat: monsters of your fight are let go as soon as your last beast is out of the world") {
    RollRig b;
    b.reply("beastmaster.give boar");
    out_at(b, "Boar", 1234, 0, 4);
    b.roll(0.0);  // you
    monster(b, 70, 10, 2, 1);
    b.seconds(1);
    REQUIRE(has(b.reply("beastmaster.threat"), "70 on you"));
    CHECK(switches(b, 70));  // left to you: never written on
    b.reply("beastmaster.dismiss Boar");
    one_step(b);
    CHECK(b.reply("beastmaster.threat") == "No monster is held.");
}

// ---- threat: after review ----
TEST_CASE("threat: a monster with no hit to or from your side for twice the hold (16 s) is let go; a fight that goes on keeps it") {
    RollRig b;
    b.reply("beastmaster.give boar");
    out_at(b, "Boar", 1234, 0, 4);
    b.roll(0.99);
    monster(b, 70, 6, 4, 1);
    monster(b, 71, 7, 4, 1);
    b.roll(0.99);
    b.seconds(1);
    REQUIRE(has(b.reply("beastmaster.threat"), "70 on Boar"));
    REQUIRE_FALSE(switches(b, 70));
    for (int i = 0; i < 3; ++i) {
        b.seconds(5);
        deliver(b.r.fake, "HitEvent", hit(1234, 71, -5));  // the boar keeps biting 71
    }
    b.seconds(1);   // 16 s
    CHECK_FALSE(switches(b, 70));
    one_step(b);    // over 16 s without a hit
    CHECK(switches(b, 70));
    CHECK_FALSE(has(b.reply("beastmaster.threat"), "70 on"));
    CHECK(has(b.reply("beastmaster.threat"), "71 on"));
    game_picks(b, 70, 0);  // the game picks something else
    b.seconds(2);
    CHECK(pick_of(b, 70) == 0);
}

TEST_CASE("threat: a monster that goes home, flees or idles is let go at once, and taken again only when it fights again") {
    RollRig b;
    b.reply("beastmaster.give boar");
    out_at(b, "Boar", 1234, 0, 4);
    b.roll(0.99);
    monster(b, 70, 6, 4, 1);
    auto& ai = component<ecs::DynamicEnemy01BehaviorState>(b.r.fake, 70);
    ai.state = emf::gen::enemy01::State::Combat;
    b.seconds(1);
    REQUIRE_FALSE(switches(b, 70));
    using S = emf::gen::enemy01::State;
    for (S st : {S::Idle, S::ReturnHome, S::FleeHome, S::WaitAtHome, S::Flee}) {
        CAPTURE(static_cast<int>(st));
        live_of<ecs::DynamicEnemy01BehaviorState>(b, 70).state = st;
        one_step(b);
        CHECK(switches(b, 70));
        b.seconds(2);  // its pick is still the boar, but it is not fighting
        CHECK(switches(b, 70));
        live_of<ecs::DynamicEnemy01BehaviorState>(b, 70).state = S::Combat;
        b.seconds(1);
        CHECK_FALSE(switches(b, 70));
    }
}

TEST_CASE("threat: the cap holds after closeness: a boss with your bear 2 m from it and you 15 m away goes to the beasts 35 % of the time") {
    RollRig b;
    b.reply("beastmaster.give bear");
    out_at(b, "Bear", 1234, 0, 13);
    uint32_t next = 70;
    auto pick_at = [&](double roll) {
        const uint32_t m = next++;
        monster(b, m, 0, 15, 1);
        component<ecs::Boss>(b.r.fake, m);
        b.roll(roll);
        b.seconds(1);
        const uint32_t got = pick_of(b, m);
        b.r.fake.attrs[{m, EMF_ATTR_HEALTH}] = 0;
        b.seconds(1);
        return got;
    };
    CHECK(pick_at(0.64) == 1u);
    CHECK(pick_at(0.66) == 1234u);
}

TEST_CASE("threat: a monster that goes for another player or their beast is not taken into your fight") {
    RollRig b({{"call_cooldown_s", "0"}});
    write(b.data, "b 0x00000000000000bb wolf 0 1 Grey\n");  // player 2's wolf, out
    b.r.fake.session = 2;
    fire_load(1);
    next_beast(b, 1234);
    b.seconds(2);
    component<ecs::DynamicEnemy01BehaviorState>(b.r.fake, 1234).guardEntityId.id = 2;
    REQUIRE(has(b.reply("beastmaster.list", 2, 0xBB), "1. Grey - wolf, loyalty 1, out"));
    b.reply("beastmaster.give boar");
    out_at(b, "Boar", 1235, 0, 4);
    b.roll(0.99);
    monster(b, 70, -6, 4, 1234);  // going for player 2's wolf
    monster(b, 71, -6, 5, 2);     // going for player 2
    monster(b, 72, -6, 6, 0);     // going for nothing
    for (uint32_t m : {70u, 71u, 72u}) deliver(b.r.fake, "HitEvent", hit(1235, m, -5));  // your boar bites them all
    one_step(b);
    const std::string held = b.reply("beastmaster.threat");
    CAPTURE(held);
    CHECK_FALSE(has(held, "70 on"));
    CHECK_FALSE(has(held, "71 on"));
    CHECK(has(held, "72 on Boar"));
    CHECK(pick_of(b, 70) != 1235u);
    CHECK(pick_of(b, 71) != 1235u);
}

TEST_CASE("threat: with no share for the beasts (both share settings 0) Beastmaster picks nothing") {
    RollRig b({{"tank_share_pct", "0"}, {"damage_share_pct", "0"}});
    b.reply("beastmaster.give boar");
    out_at(b, "Boar", 1234, 0, 4);
    b.roll(0.99);
    monster(b, 70, 6, 4, 1);
    b.seconds(2);
    CHECK(b.reply("beastmaster.threat") == "No monster is held.");
    CHECK(switches(b, 70));
}

// ---- the stable's E prompt (EMF API v25, Lua 1.14) ----
namespace {
size_t offers_on(BeastRig& b, uint32_t id) {
    size_t n = 0;
    for (const auto& o : b.r.fake.offers) n += o.entity == id;
    return n;
}
const Fake::Off* last_offer(BeastRig& b, uint32_t id) {
    for (auto it = b.r.fake.offers.rbegin(); it != b.r.fake.offers.rend(); ++it)
        if (it->entity == id) return &*it;
    return nullptr;
}
}  // namespace

TEST_CASE("stable: every stable gets the game's E Open prompt on its server entity, again at every search; one taken down loses it") {
    BeastRig b;
    with_offers(b.r.api);  // a v25 core
    flame_altar(b, 900, 0, 0);
    stable_prop(b, 950, 5, 10, 0);
    b.seconds(32);  // the next search
    REQUIRE(offers_on(b, 950) == 1u);
    const Fake::Off* o = last_offer(b, 950);
    CHECK(o->o.verb == EMF_VERB_OPEN);
    CHECK(o->text.empty());
    CHECK(o->world == kServer);
    CHECK(offers_on(b, 900) == 0u);
    b.seconds(30);
    CHECK(offers_on(b, 950) == 2u);  // again at the next search (the game may have dropped it)
    CHECK(b.r.fake.offers_removed.empty());
    remove_entity(b.r.fake, 950);  // taken down
    b.seconds(31);
    CHECK(contains(b.r.fake.offers_removed, 950));
}

TEST_CASE("stable: a bed only the host's client world shows gets its prompt on the server entity at its place") {
    BeastRig b;
    with_offers(b.r.api);
    flame_altar(b, 900, 0, 0);
    stable_prop(b, 951, 10, 10, 0, nullptr);  // the bed on the server world: no render model there
    const auto model = guid_bytes(kStableModel);
    auto step_both = [&](int secs) {
        for (int i = 0; i < secs * 60; ++i) {
            place(b.r.fake, 950, 10, 10, 0);  // the client world's entity of the same bed, with its model
            b.r.fake.templates[{ecs::ModelResource::kHash, 950}] = model;
            add_entity(b.r.fake, 950);
            b.host->tick(kClient);
            remove_entity(b.r.fake, 950);
            b.r.fake.templates.erase({ecs::ModelResource::kHash, 950});
            b.r.fake.tick += 1;
            b.host->tick(kServer);
        }
    };
    step_both(40);
    REQUIRE(has(b.reply("beastmaster.stable list"), "1. Animal bed at 10, 10, 0"));
    CHECK(offers_on(b, 951) >= 1u);
    CHECK(offers_on(b, 950) == 0u);  // never the client world's id
}

TEST_CASE("stable: a marked prop gets its prompt at once, loses it when unmarked, and gets it back after a reload") {
    BeastRig b;
    with_offers(b.r.api);
    at_stable(b);  // prop 960 marked
    CHECK(offers_on(b, 960) == 1u);
    b.r.fake.session = 2;  // the world loads again: the mark is found by its place
    fire_load(1);
    b.seconds(1);
    CHECK(offers_on(b, 960) == 2u);
    REQUIRE(b.reply("beastmaster.stable unmark").rfind("Unmarked", 0) == 0);
    CHECK(contains(b.r.fake.offers_removed, 960));
}

namespace {
bool told(BeastRig& b, uint32_t player, const std::string& part) {
    for (const auto& [p, text] : b.r.fake.tells)
        if (p == player && text.find(part) != std::string::npos) return true;
    return false;
}
}  // namespace

TEST_CASE("stable: E Open at a stable opens the Beasts window for the host and tells a player who joined the stable's commands") {
    BeastRig b;
    with_offers(b.r.api);
    three_beasts(b);
    at_stable(b);
    Fake::Off o = *last_offer(b, 960);
    o.cb(kServer, 1, 960, o.user);  // the host presses E there
    REQUIRE(b.r.fake.windows.size() == 1u);
    CHECK(b.r.fake.windows.back().id == "beastmaster.stable");
    CHECK(shows(b.r.fake.windows.back(), "Stable 2/8"));
    o.cb(kServer, 2, 960, o.user);  // a player who joined
    CHECK(b.r.fake.windows.size() == 1u);
    for (const char* part : {"/beastmaster active <name or number>", "/beastmaster store", "/beastmaster release",
                             "/beastmaster talent <name or number> <talent>"})
        CHECK_MESSAGE(told(b, 2, part), part);
    b.r.api.local_player = []() -> uint32_t { return 0; };  // a dedicated server: nobody has the window
    o.cb(kServer, 1, 960, o.user);
    CHECK(b.r.fake.windows.size() == 1u);
    CHECK(told(b, 1, "/beastmaster active"));
}

TEST_CASE("stable: the E key opens the Beasts window only at a stable without the prompt") {
    BeastRig b;
    with_offers(b.r.api);
    three_beasts(b);
    at_stable(b);
    REQUIRE(offers_on(b, 960) == 1u);
    component<ecs::InteractionOfferRuntime>(b.r.fake, 960);  // the game's prompt components, as the core adds them
    press(b, kE);
    CHECK(b.r.fake.windows.empty());  // the prompt opens it (once)
    BeastRig c;  // the game refused the prompt there (the prop has an E use of its own)
    with_offers(c.r.api);
    c.r.fake.offer_ok = false;
    three_beasts(c);
    at_stable(c);
    press(c, kE);
    CHECK(c.r.fake.windows.size() == 1u);
}

// ---- the stable's prompt: after review ----
TEST_CASE("stable: the E key opens the window at a stable whose prompt the game dropped (streamed out and back), and a new search starts") {
    BeastRig b;
    with_offers(b.r.api);
    three_beasts(b);
    at_stable(b);
    component<ecs::InteractionOfferRuntime>(b.r.fake, 960);
    REQUIRE(offers_on(b, 960) == 1u);
    b.r.fake.components.erase({ecs::InteractionOfferRuntime::kHash, 960});  // the game dropped the prompt
    press(b, kE);
    CHECK(b.r.fake.windows.size() == 1u);
    CHECK(offers_on(b, 960) == 2u);  // searched again at once: offered again
    BeastRig c;  // the prompt's id names something elsewhere now
    with_offers(c.r.api);
    three_beasts(c);
    at_stable(c);
    component<ecs::InteractionOfferRuntime>(c.r.fake, 960);
    place(c.r.fake, 960, 20, 10, 20);
    press(c, kE);
    CHECK(c.r.fake.windows.size() == 1u);
}

TEST_CASE("stable: the E key opens the window at a stable whose offer EMF dropped (the prompt's parts left on it), and it is offered again") {
    BeastRig b;
    with_offers(b.r.api);
    three_beasts(b);
    at_stable(b);
    component<ecs::InteractionOfferRuntime>(b.r.fake, 960);  // the game's prompt is on it
    REQUIRE(offers_on(b, 960) == 1u);
    press(b, kE);
    REQUIRE(b.r.fake.windows.empty());  // the prompt opens it, not the key
    b.r.fake.offer_active = false;  // EMF dropped the offer; its parts stay on the entity
    press(b, kE);
    CHECK(b.r.fake.windows.size() == 1u);
    CHECK(offers_on(b, 960) == 2u);  // searched again at once: offered again (EMF takes its dropped offer back)
}

TEST_CASE("stable: an id that names something elsewhere now is not offered again, and E Open from it does nothing") {
    BeastRig b;
    with_offers(b.r.api);
    three_beasts(b);
    at_stable(b);  // prop 960 marked and offered
    REQUIRE(offers_on(b, 960) == 1u);
    const Fake::Off old = *last_offer(b, 960);
    place(b.r.fake, 960, 20, 10, 20);  // the prop streamed out; the game gave its id to something else
    old.cb(kServer, 1, 960, old.user);  // E there before the stables are offered again
    CHECK(b.r.fake.windows.empty());
    stable_prop(b, 962, 3, 10, 0, nullptr);
    b.look(1, 0, 11.5, 0, 90);  // a second mark: the stables are offered again
    REQUIRE(b.reply("beastmaster.stable mark").rfind("Marked: ", 0) == 0);
    CHECK(offers_on(b, 962) == 1u);
    CHECK(offers_on(b, 960) == 1u);  // not again on the old id
}

TEST_CASE("stable: a stable whose entity has no StaticCollider gets no prompt; the E key opens its window") {
    BeastRig b;
    with_offers(b.r.api);
    flame_altar(b, 900, 0, 0);
    stable_prop(b, 950, 5, 10, 0);  // a bed the server world shows, without a collider
    b.r.fake.components.erase({ecs::StaticCollider::kHash, 950});
    b.seconds(32);
    REQUIRE(has(b.reply("beastmaster.stable list"), "1. Animal bed at 5, 10, 0"));
    CHECK(offers_on(b, 950) == 0u);
    BeastRig c;  // a marked prop that has a model and no collider
    with_offers(c.r.api);
    three_beasts(c);
    stable_prop(c, 963, 0, 10, 3, kStableModel);
    c.r.fake.components.erase({ecs::StaticCollider::kHash, 963});
    c.look(1, 0, 11.5, 0, 0);
    REQUIRE(c.reply("beastmaster.stable mark").rfind("Marked: ", 0) == 0);
    CHECK(offers_on(c, 963) == 0u);
    press(c, kE);
    CHECK(c.r.fake.windows.size() == 1u);
}

TEST_CASE("stable: one E press that reaches both the prompt and the key opens the window once") {
    BeastRig b;
    with_offers(b.r.api);
    three_beasts(b);
    at_stable(b);  // offered; the game's prompt not seen there yet: the key path takes it as a stable without one
    Fake::Off o = *last_offer(b, 960);
    o.cb(kServer, 1, 960, o.user);  // the prompt opens the window
    REQUIRE(b.r.fake.windows.size() == 1u);
    press(b, kE);  // the same press, on the key path
    CHECK(b.r.fake.windows.size() == 1u);
}

TEST_CASE("stable: a window from before a world load that closes late does not close the one opened since") {
    BeastRig b;
    three_beasts(b);
    at_stable(b);
    press(b, kE);
    REQUIRE(b.r.fake.windows.size() == 1u);
    b.r.fake.session = 3;  // the world loads again
    fire_load(1);
    b.seconds(1);
    press(b, kE);
    REQUIRE(b.r.fake.windows.size() == 2u);
    const Fake::Win first = b.r.fake.windows[0];
    EmfUiEvent e{};
    e.size = sizeof e;
    e.kind = EMF_UI_CLOSED;
    e.window = first.id.c_str();
    e.key = "";
    e.from_key = "";
    e.reason = EMF_UI_BY_CORE;
    first.cb(kServer, &e, first.user);  // the first window's close arrives now
    click(b, "beast.Tusk");             // the open window still answers
    CHECK(text_at(shown(b), "tab.beast") == "Tusk");
}

// ---- idle: while their owner stands still the beasts pass the time; they follow the moment the owner moves ----
namespace {
constexpr uint32_t kSleepBit = 1u << 21;  // EnemyFeature Sleep (DynamicEnemy01BehaviorState.enabledFeatures)
constexpr uint32_t kRotate = 1u << 5;      // EnemyFeature Rotate
constexpr uint32_t kRotateBits = kRotate | (1u << 6) | (1u << 17);  // Rotate, RotateWhileStanding, IdleTurn
using AiState = emf::gen::enemy01::State;
const ecs::DynamicEnemy01BehaviorState& ai_now(BeastRig& b, uint32_t id) {
    return now_of<ecs::DynamicEnemy01BehaviorState>(b, id);
}
// Held by the game's sleep: the Sleep state, its feature bit on and an end time (its lease; a creature whose graph has
// sleep states lies down).
bool asleep(BeastRig& b, uint32_t id) {
    const auto& ai = ai_now(b, id);
    return ai.state == AiState::Sleep && (ai.enabledFeatures & kSleepBit) != 0 && ai.sleepEndTime.value > 0;
}
// Up and free: not in the Sleep state, the bit off, no end time.
bool awake(BeastRig& b, uint32_t id) {
    const auto& ai = ai_now(b, id);
    return ai.state != AiState::Sleep && (ai.enabledFeatures & kSleepBit) == 0 && ai.sleepEndTime.value == 0;
}
// Player 1's beast `name` (in the collection) called as `id` at (x, 10, z), guarding them, in the game's Roaming.
void roaming(BeastRig& b, const std::string& name, uint32_t id, double x, double z) {
    out_at(b, name, id, x, z);
    auto& ai = live_of<ecs::DynamicEnemy01BehaviorState>(b, id);
    ai.state = AiState::Roaming;
    ai.stateChangeTime.value = 1000000000;
}
}  // namespace

TEST_CASE("idle: a wolf out lies down after its owner stands still 10 s and gets up at once when the owner moves 2 m") {
    RollRig b;
    b.roll(0.0);
    b.reply("beastmaster.give wolf");
    roaming(b, "Wolf", 1234, 0, 4);
    live_of<ecs::DynamicEnemy01BehaviorState>(b, 1234).disabledFeatures = kSleepBit | 1u;  // the game's own off-list
    b.seconds(9);
    CHECK(awake(b, 1234));
    CHECK(ai_now(b, 1234).state == AiState::Roaming);
    b.seconds(2);
    REQUIRE(asleep(b, 1234));
    CHECK(ai_now(b, 1234).disabledFeatures == 1u);  // not off while it sleeps
    place(b.r.fake, 1, 0.5, 10, 0);                 // a step: under 2 m from where idling began
    b.seconds(2);
    CHECK(asleep(b, 1234));
    place(b.r.fake, 1, 2.1, 10, 0);  // 2 m from it
    one_step(b);
    CHECK(awake(b, 1234));
    CHECK(ai_now(b, 1234).state == AiState::Idle);
    CHECK(ai_now(b, 1234).disabledFeatures == (kSleepBit | 1u));  // as it was
    b.seconds(9);
    CHECK(awake(b, 1234));  // 10 s of stillness again
    b.seconds(2);
    CHECK(asleep(b, 1234));
}

TEST_CASE("idle: a hound lies down like a wolf; a sabretooth (no sleep states) with nothing to look at is left to roam, and roaming off past 15 m it drops out alone") {
    RollRig b({{"call_cooldown_s", "0"}});
    b.roll(0.0);
    b.reply("beastmaster.give hound");
    b.reply("beastmaster.give sabretooth");
    roaming(b, "Hound", 1234, 0, 4);
    roaming(b, "Sabretooth", 1235, 0, -4);
    b.seconds(11);
    CHECK(asleep(b, 1234));
    b.seconds(10);
    CHECK(awake(b, 1235));
    CHECK(ai_now(b, 1235).state == AiState::Roaming);
    place(b.r.fake, 1235, 0, 10, -17);  // it roams off to 17 m: nothing written, the hound sleeps on
    one_step(b);
    CHECK(asleep(b, 1234));
    CHECK(ai_now(b, 1235).state == AiState::Roaming);
}

TEST_CASE("idle: a bear lies down too (its pack's graph is a copy of the wolf's)") {
    RollRig b;
    b.roll(0.0);
    b.reply("beastmaster.give bear");
    roaming(b, "Bear", 1234, 0, 4);
    b.seconds(11);
    CHECK(asleep(b, 1234));
}

TEST_CASE("idle: each player's beasts follow only their own stillness") {
    RollRig b({{"call_cooldown_s", "0"}});
    b.roll(0.0);
    write(b.data, "b 0x00000000000000bb wolf 0 1 Grey\n");  // player 2's wolf, out
    b.r.fake.session = 2;
    fire_load(1);
    next_beast(b, 1234);
    b.seconds(1);
    REQUIRE(has(b.reply("beastmaster.list", 2, 0xBB), "1. Grey - wolf, loyalty 1, out"));
    auto& grey = component<ecs::DynamicEnemy01BehaviorState>(b.r.fake, 1234);
    grey.guardEntityId.id = 2;
    grey.state = AiState::Roaming;
    place(b.r.fake, 1234, 30, 10, 4);
    b.reply("beastmaster.give wolf");
    roaming(b, "Wolf", 1235, 0, 4);
    b.seconds(11);
    REQUIRE(asleep(b, 1234));
    REQUIRE(asleep(b, 1235));
    place(b.r.fake, 2, 33, 10, 0);  // player 2 moves on
    one_step(b);
    CHECK(awake(b, 1234));
    CHECK(asleep(b, 1235));
    b.seconds(11);
    REQUIRE(asleep(b, 1234));
    remove_entity(b.r.fake, 2);  // player 2 leaves
    b.seconds(2);
    CHECK(contains(b.r.fake.destroyed, 1234));  // out of the world while away
    CHECK(asleep(b, 1235));
}

TEST_CASE("idle: a hit wakes the beasts at once; idling waits 10 s after the last hit to or from the owner or a beast, and while the threat share holds a monster for them") {
    RollRig b({{"idle_s", "5"}});
    b.roll(0.0);
    b.reply("beastmaster.give wolf");
    roaming(b, "Wolf", 1234, 0, 4);
    b.seconds(7);
    REQUIRE(asleep(b, 1234));
    creature(b, 70, 0, 20, kSkeletonFoe);
    deliver(b.r.fake, "HitEvent", hit(70, 1234, -5));  // a monster bites the wolf
    CHECK(awake(b, 1234));                             // in the event, before any step
    b.seconds(8);
    CHECK(awake(b, 1234));  // 10 s quiet first (idle_s is 5)
    b.seconds(3);
    REQUIRE(asleep(b, 1234));
    deliver(b.r.fake, "HitEvent", hit(1, 70, -5));  // the owner hits a monster
    CHECK(awake(b, 1234));
    b.seconds(11);
    REQUIRE(asleep(b, 1234));
    deliver(b.r.fake, "HitEvent", hit(0, 1, -5));        // a fall: no one's hit
    deliver(b.r.fake, "HitEvent", hit(1234, 1234, -5));  // its own
    CHECK(asleep(b, 1234));
    monster(b, 71, 10, 2, 1);  // a monster goes for the owner: the threat share holds it (on the owner, roll 0)
    b.seconds(2);
    REQUIRE(has(b.reply("beastmaster.threat"), "71 on you"));
    CHECK(awake(b, 1234));
    b.seconds(12);
    CHECK(awake(b, 1234));  // held still
    b.r.fake.attrs[{71, EMF_ATTR_HEALTH}] = 0;  // it falls: let go
    b.seconds(2);
    CHECK(asleep(b, 1234));
}

TEST_CASE("idle: Attack, dismiss and call wake every beast of that owner at once; idling starts anew after idle_s") {
    RollRig b({{"call_cooldown_s", "0"}});
    b.roll(0.0);
    b.reply("beastmaster.give wolf");
    b.reply("beastmaster.give wolf");
    roaming(b, "Wolf", 1234, 0, 4);
    roaming(b, "Wolf 2", 1235, 0, -4);
    b.seconds(11);
    REQUIRE(asleep(b, 1234));
    REQUIRE(asleep(b, 1235));
    b.reply("beastmaster.dismiss Wolf 2");
    CHECK(awake(b, 1234));
    b.seconds(9);
    CHECK(awake(b, 1234));
    b.seconds(2);
    REQUIRE(asleep(b, 1234));
    roaming(b, "Wolf 2", 1236, 0, -4);  // called again
    CHECK(awake(b, 1234));
    b.seconds(11);
    REQUIRE(asleep(b, 1234));
    REQUIRE(asleep(b, 1236));
    creature(b, 70, 0, 8, kSkeletonFoe);
    b.look(1, 0, 10.5, 0, 0);  // the owner looks at it
    REQUIRE(has(b.reply("beastmaster.attack"), "Your beasts attack (2)"));
    CHECK(awake(b, 1234));
    CHECK(awake(b, 1236));
    b.seconds(15);
    CHECK(awake(b, 1234));  // Attack's order holds 20 s
    CHECK(awake(b, 1236));
}

TEST_CASE("idle: building or the idle setting off end it; a beast not guarding its owner or farther than 15 m takes no part, and one that goes farther drops out alone") {
    RollRig b({{"call_cooldown_s", "0"}, {"make_room", "false"}});
    b.roll(0.0);
    b.reply("beastmaster.give wolf");
    b.reply("beastmaster.give wolf");
    roaming(b, "Wolf", 1234, 0, 4);
    live_of<ecs::DynamicEnemy01BehaviorState>(b, 1234).guardEntityId.id = 0;  // not following its owner yet
    b.seconds(11);
    CHECK(awake(b, 1234));
    live_of<ecs::DynamicEnemy01BehaviorState>(b, 1234).guardEntityId.id = 1;
    b.seconds(1);
    REQUIRE(asleep(b, 1234));
    building(b.r.fake, 1, true);  // the owner holds something to build (make_room off: the beasts stay)
    one_step(b);
    CHECK(awake(b, 1234));
    b.seconds(15);
    CHECK(awake(b, 1234));
    building(b.r.fake, 1, false);
    b.r.fake.settings["beastmaster.idle"] = "false";
    b.seconds(15);
    CHECK(awake(b, 1234));
    b.r.fake.settings["beastmaster.idle"] = "1";
    b.seconds(1);
    REQUIRE(asleep(b, 1234));
    b.r.fake.settings["beastmaster.idle"] = "false";
    one_step(b);
    CHECK(awake(b, 1234));
    b.r.fake.settings["beastmaster.idle"] = "1";
    roaming(b, "Wolf 2", 1235, 0, -16);  // 16 m from the owner
    b.seconds(11);
    REQUIRE(asleep(b, 1234));
    CHECK(awake(b, 1235));
    CHECK(ai_now(b, 1235).state == AiState::Roaming);
    place(b.r.fake, 1235, 0, 10, -4);  // it comes within 15 m
    one_step(b);
    REQUIRE(asleep(b, 1235));
    place(b.r.fake, 1234, 0, 10, 16);  // the first wolf is 16 m away now: it alone gets up
    one_step(b);
    CHECK(awake(b, 1234));
    CHECK(asleep(b, 1235));
    place(b.r.fake, 1234, 0, 10, 5);  // back within 15 m: it takes part again
    one_step(b);
    CHECK(asleep(b, 1234));
}

namespace {
// A campfire or a fireplace at (x, 10, z): a ComfortProvider of category Hearth.
void fire_at(BeastRig& b, uint32_t id, double x, double z) {
    place(b.r.fake, id, x, 10, z);
    component<ecs::ComfortProvider>(b.r.fake, id).category = emf::gen::ecs::ComfortCategory::Hearth;
    add_entity(b.r.fake, id);
}
// A villager at (x, 10, z): the ActiveNpcState template half.
void villager_at(BeastRig& b, uint32_t id, double x, double z) {
    place(b.r.fake, id, x, 10, z);
    b.r.fake.templates[{ecs::ActiveNpcState::kHash, id}] = std::vector<uint8_t>(ecs::ActiveNpcState::kSize, 0);
    add_entity(b.r.fake, id);
}
// The game's teleports asked for entity `id` (a turn in place is one with a yaw).
std::vector<EmfTeleport> turns_of(BeastRig& b, uint32_t id) {
    std::vector<EmfTeleport> out;
    for (const auto& t : b.r.fake.teleports)
        if (t.entity == id) out.push_back(t);
    return out;
}
}  // namespace

TEST_CASE("idle: by the fire: a beast turns to face a fire within 15 m (a teleport in place with a yaw) and stays put; a wolf lies down facing it") {
    RollRig b({{"call_cooldown_s", "0"}});
    b.roll(0.6);  // by the fire
    b.reply("beastmaster.give boar");
    b.reply("beastmaster.give wolf");
    fire_at(b, 900, 4, 4);        // a campfire 4 m east of the boar
    flame_altar(b, 901, 0, -8);   // a flame altar 4 m south of the wolf
    roaming(b, "Boar", 1234, 0, 4);
    roaming(b, "Wolf", 1235, 0, -4);
    b.seconds(11);
    const auto boar = turns_of(b, 1234);
    REQUIRE(boar.size() == 1u);
    CHECK(boar[0].x == doctest::Approx(0.0));  // where it stands
    CHECK(boar[0].y == doctest::Approx(10.0));
    CHECK(boar[0].z == doctest::Approx(4.0));
    CHECK((boar[0].flags & EMF_TELEPORT_FACE_YAW) != 0u);
    CHECK((boar[0].flags & EMF_TELEPORT_FIND_GROUND) == 0u);
    CHECK(boar[0].yaw_deg == doctest::Approx(90.0));  // +x
    CHECK(asleep(b, 1234));  // held in the Sleep state (a boar's graph has no sleep states: it stands there)
    const auto wolf = turns_of(b, 1235);
    REQUIRE(wolf.size() == 1u);
    CHECK(std::abs(wolf[0].yaw_deg) == doctest::Approx(180.0));  // -z
    CHECK(asleep(b, 1235));
}

TEST_CASE("idle: watching: a beast turns to a villager within 15 m and stays put; a sleeper lies down facing them") {
    RollRig b({{"call_cooldown_s", "0"}});
    b.roll(0.6);  // watching
    b.reply("beastmaster.give boar");
    b.reply("beastmaster.give wolf");
    villager_at(b, 902, -3, 4);  // 3 m west of the boar
    roaming(b, "Boar", 1234, 0, 4);
    roaming(b, "Wolf", 1235, 0, -4);
    b.seconds(11);
    const auto boar = turns_of(b, 1234);
    REQUIRE(boar.size() == 1u);
    CHECK(boar[0].yaw_deg == doctest::Approx(-90.0));  // -x
    CHECK(asleep(b, 1234));
    const auto wolf = turns_of(b, 1235);
    REQUIRE(wolf.size() == 1u);
    CHECK(wolf[0].yaw_deg == doctest::Approx(std::atan2(-3.0, 8.0) * 180.0 / 3.14159265358979));
    CHECK(asleep(b, 1235));
}

TEST_CASE("idle: a fire or a villager farther than 15 m from the beast is not looked at") {
    RollRig b;
    b.roll(0.0);  // the first scene it can have
    b.reply("beastmaster.give boar");
    fire_at(b, 900, 0, 24);       // 20 m from the boar
    villager_at(b, 902, 20, 4);   // 20 m from it
    roaming(b, "Boar", 1234, 0, 4);
    b.seconds(11);
    CHECK(turns_of(b, 1234).empty());
    CHECK(awake(b, 1234));
    CHECK(ai_now(b, 1234).state == AiState::Roaming);
}

TEST_CASE("idle: a new scene every 30 to 60 s; the same scene is kept without a second turn") {
    RollRig b;
    b.roll(0.0);  // settle; the next pick 30 s later
    b.reply("beastmaster.give wolf");
    fire_at(b, 900, 4, 4);
    roaming(b, "Wolf", 1234, 0, 4);
    b.seconds(11);
    REQUIRE(asleep(b, 1234));
    CHECK(turns_of(b, 1234).empty());
    b.roll(0.6);  // by the fire; the next pick 60 s later
    b.seconds(29);
    CHECK(turns_of(b, 1234).empty());
    b.seconds(1);
    REQUIRE(turns_of(b, 1234).size() == 1u);
    CHECK(turns_of(b, 1234)[0].yaw_deg == doctest::Approx(90.0));
    CHECK(asleep(b, 1234));
    b.seconds(60);  // by the fire again
    CHECK(turns_of(b, 1234).size() == 1u);
    CHECK(asleep(b, 1234));
    b.roll(0.99);  // sniffing about
    b.seconds(60);
    CHECK(awake(b, 1234));
}

TEST_CASE("idle: at night one beast keeps watch facing away from its owner while the sleepers settle; a lone wolf settles") {
    RollRig b({{"call_cooldown_s", "0"}});
    b.roll(0.99);  // by day this is sniffing about
    set_daytime(b.r.fake, 23.0);
    b.reply("beastmaster.give wolf");
    b.reply("beastmaster.give boar");
    roaming(b, "Wolf", 1234, 0, 4);
    roaming(b, "Boar", 1235, 0, -4);
    b.seconds(11);
    CHECK(asleep(b, 1234));  // the wolf settles
    const auto watch = turns_of(b, 1235);
    REQUIRE(watch.size() == 1u);
    CHECK(std::abs(watch[0].yaw_deg) == doctest::Approx(180.0));  // away from its owner at (0, 10, 0)
    CHECK(asleep(b, 1235));                                      // a boar on watch stands still
    CHECK(turns_of(b, 1234).empty());
    set_daytime(b.r.fake, 12.0);
    b.seconds(60);  // the next picks are the day's
    CHECK(awake(b, 1234));
    CHECK(awake(b, 1235));
    b.reply("beastmaster.dismiss Boar");
    set_daytime(b.r.fake, 23.0);
    b.seconds(11);
    CHECK(asleep(b, 1234));  // alone it settles
}

TEST_CASE("idle: a turn the game refuses is skipped; the beast still stays put") {
    RollRig b;
    b.roll(0.0);
    b.reply("beastmaster.give boar");
    fire_at(b, 900, 4, 4);
    roaming(b, "Boar", 1234, 0, 4);
    b.r.fake.teleport_ok = false;  // e.g. a beast spawned a moment ago reads as dead
    b.seconds(11);
    CHECK(turns_of(b, 1234).size() == 1u);
    CHECK(asleep(b, 1234));
    CHECK(b.host->status("beastmaster") == "running");
}

TEST_CASE("idle: a beast that comes within 15 m of its owner while they idle gets a scene too") {
    RollRig b({{"call_cooldown_s", "0"}});
    b.roll(0.0);
    b.reply("beastmaster.give wolf");
    b.reply("beastmaster.give wolf");
    roaming(b, "Wolf", 1234, 0, 4);
    roaming(b, "Wolf 2", 1235, 0, -16);  // 16 m away: not in the idling
    b.seconds(11);
    REQUIRE(asleep(b, 1234));
    REQUIRE(awake(b, 1235));
    place(b.r.fake, 1235, 0, 10, -5);  // it comes back to its owner
    one_step(b);
    CHECK(asleep(b, 1235));
    CHECK(asleep(b, 1234));
}

TEST_CASE("idle: at the mod's start or a reload every player-side creature left with the Sleep bit is woken; a wild one is left alone") {
    BeastRig b;
    for (uint32_t id : {1300u, 1301u}) {
        creature(b, id, 5, 5, id == 1300u ? kPlayerAlly : kWildBeast);
        auto& ai = component<ecs::DynamicEnemy01BehaviorState>(b.r.fake, id);
        ai.enabledFeatures = kSleepBit | 4u;
        ai.state = AiState::Sleep;
        ai.sleepEndTime.value = 1000000000000000000LL;
    }
    b.edit_main("local wild_mod = require(\"wild\")\n", "-- reloaded\n");  // the script reloads; a second is played
    CHECK(awake(b, 1300));
    CHECK(ai_now(b, 1300).enabledFeatures == 4u);
    CHECK(ai_now(b, 1300).state == AiState::Idle);
    CHECK(asleep(b, 1301));
}

TEST_CASE("idle: an error in the idle code wakes every beast of the idling and is logged; the mod runs on") {
    RollRig b;
    b.edit_main("local wild_mod = require(\"wild\")\n", "emf.teleport = function() error(\"teleport broke\") end\n");
    b.roll(0.0);  // settle; the next pick 30 s later
    b.reply("beastmaster.give wolf");
    fire_at(b, 900, 4, 4);
    roaming(b, "Wolf", 1234, 0, 4);
    b.seconds(11);
    REQUIRE(asleep(b, 1234));
    b.roll(0.6);  // by the fire: the turn fails with an error
    b.seconds(30);
    CHECK(awake(b, 1234));
    auto logged = [&] {
        size_t n = 0;
        for (const auto& l : b.r.fake.log) n += has(l, "idle: ") && has(l, "teleport broke");
        return n;
    };
    CHECK(logged() == 1u);
    CHECK(b.host->status("beastmaster") == "running");
    b.seconds(11);  // idling again, by the fire again: the same error, not logged again
    CHECK(awake(b, 1234));
    CHECK(logged() == 1u);
}

TEST_CASE("idle: beastmaster.idle says what each beast does") {
    RollRig b({{"call_cooldown_s", "0"}});
    b.roll(0.0);
    b.reply("beastmaster.give wolf");
    b.reply("beastmaster.give boar");
    fire_at(b, 900, 4, -4);  // 4 m east of the boar
    roaming(b, "Wolf", 1234, 0, 4);
    roaming(b, "Boar", 1235, 0, -4);
    b.seconds(5);
    CHECK(has(b.reply("beastmaster.idle"), "Your beasts are not idling"));
    b.seconds(6);
    const std::string idle = b.reply("beastmaster.idle");
    CAPTURE(idle);
    CHECK(has(idle, "Wolf settles (lying down"));
    CHECK(has(idle, "Boar sits by the fire (standing still, turned"));
}

TEST_CASE("idle: at night with only wolves and hounds out none keeps watch: they all settle") {
    RollRig b({{"call_cooldown_s", "0"}});
    b.roll(0.99);  // by day this is sniffing about
    set_daytime(b.r.fake, 23.0);
    b.reply("beastmaster.give wolf");
    b.reply("beastmaster.give hound");
    roaming(b, "Wolf", 1234, 0, 4);
    roaming(b, "Hound", 1235, 0, -4);
    b.seconds(11);
    CHECK(asleep(b, 1234));
    CHECK(asleep(b, 1235));
    CHECK(turns_of(b, 1234).empty());
    CHECK(turns_of(b, 1235).empty());
}

TEST_CASE("idle: a turned beast keeps facing: the game's turning (Rotate, RotateWhileStanding, IdleTurn) is off while it is held, and back as it was when it gets up") {
    RollRig b({{"call_cooldown_s", "0"}});
    b.roll(0.0);
    b.reply("beastmaster.give boar");
    b.reply("beastmaster.give wolf");
    fire_at(b, 900, 4, 4);
    roaming(b, "Boar", 1234, 0, 4);
    roaming(b, "Wolf", 1235, 0, -4);
    live_of<ecs::DynamicEnemy01BehaviorState>(b, 1234).disabledFeatures = kRotate | 1u;  // Rotate off already
    b.seconds(11);
    REQUIRE(asleep(b, 1234));
    REQUIRE(turns_of(b, 1234).size() == 1u);
    CHECK(ai_now(b, 1234).disabledFeatures == (kRotateBits | 1u));
    REQUIRE(asleep(b, 1235));  // settled, not turned: its turning is left alone
    CHECK(ai_now(b, 1235).disabledFeatures == 0u);
    place(b.r.fake, 1, 2.5, 10, 0);
    one_step(b);
    CHECK(awake(b, 1234));
    CHECK(ai_now(b, 1234).disabledFeatures == (kRotate | 1u));  // only what was on before stays off
}

TEST_CASE("idle: the Sleep state is a lease: the game's time plus 120 s, renewed every 5 s while held; the game's own sleep check is pushed far ahead") {
    RollRig b;
    b.roll(0.0);
    b.reply("beastmaster.give wolf");
    const double called = b.r.fake.tick / 60.0;
    roaming(b, "Wolf", 1234, 0, 4);  // spawned at the game's time kSpawnNs
    auto lease = [&] { return kSpawnNs + static_cast<int64_t>((b.r.fake.tick / 60.0 - called) * 1e9) + 120000000000LL; };
    b.seconds(11);
    REQUIRE(asleep(b, 1234));
    CHECK(ai_now(b, 1234).sleepEndTime.value == lease());
    CHECK(ai_now(b, 1234).nextSleepCheckTime.value > kSpawnNs + 900000000000000000LL);
    const int64_t first = ai_now(b, 1234).sleepEndTime.value;
    b.seconds(4);
    CHECK(ai_now(b, 1234).sleepEndTime.value == first);
    b.seconds(1);
    CHECK(ai_now(b, 1234).sleepEndTime.value == lease());  // renewed
    CHECK(ai_now(b, 1234).sleepEndTime.value == first + 5000000000LL);
}

TEST_CASE("idle: without a call's spawn time the game's time is taken from the AI's own state change; until then nothing is held") {
    RollRig b;
    b.roll(0.0);
    b.look(1, 0, 10.5, 0, 0);
    b.beast(50, kWolfGraph, 0, 5, kWildBeast, 20, 100);
    REQUIRE(has(b.reply("beastmaster.tame"), "Taming"));
    b.seconds(4);
    REQUIRE(has(b.reply("beastmaster.list"), "1. Wolf - wolf, loyalty 1, out"));  // tamed, never called
    auto& ai = component<ecs::DynamicEnemy01BehaviorState>(b.r.fake, 50);
    ai.guardEntityId.id = 1;
    ai.state = AiState::Roaming;
    ai.stateChangeTime.value = 1000000000;
    b.seconds(12);
    CHECK(awake(b, 50));  // idling, but the game's time is not known yet
    live_of<ecs::DynamicEnemy01BehaviorState>(b, 50).stateChangeTime.value = 3000000000000LL;  // its state changes now
    one_step(b);
    CHECK(asleep(b, 50));
    CHECK(ai_now(b, 50).sleepEndTime.value == 3000000000000LL + 120000000000LL);
}

TEST_CASE("idle: a beast's Sleep bit is off once it gets up, also when the bit was on before") {
    RollRig b;
    b.roll(0.0);
    b.reply("beastmaster.give wolf");
    roaming(b, "Wolf", 1234, 0, 4);
    b.seconds(8);  // after the search at half of idle_s
    live_of<ecs::DynamicEnemy01BehaviorState>(b, 1234).enabledFeatures = kSleepBit | 2u;
    b.seconds(3);
    REQUIRE(asleep(b, 1234));
    place(b.r.fake, 1, 2.5, 10, 0);
    one_step(b);
    CHECK(awake(b, 1234));
    CHECK(ai_now(b, 1234).enabledFeatures == 2u);
}

TEST_CASE("idle: a beast takes part only while its AI idles, roams or guards; one the game moves on (a fall, water) drops out alone") {
    RollRig b;
    b.roll(0.0);
    b.reply("beastmaster.give wolf");
    roaming(b, "Wolf", 1234, 0, 4);
    live_of<ecs::DynamicEnemy01BehaviorState>(b, 1234).state = AiState::AvoidWater;
    b.seconds(11);
    CHECK(awake(b, 1234));
    CHECK(ai_now(b, 1234).state == AiState::AvoidWater);  // nothing written
    live_of<ecs::DynamicEnemy01BehaviorState>(b, 1234).state = AiState::Roaming;
    one_step(b);
    REQUIRE(asleep(b, 1234));
    live_of<ecs::DynamicEnemy01BehaviorState>(b, 1234).state = AiState::Falling;  // the game moves it on
    one_step(b);
    CHECK(awake(b, 1234));
    CHECK(ai_now(b, 1234).state == AiState::Falling);
    live_of<ecs::DynamicEnemy01BehaviorState>(b, 1234).state = AiState::Guard;
    one_step(b);
    CHECK(asleep(b, 1234));
}

TEST_CASE("idle: an error drops the search under way; the next waits 30 s") {
    RollRig b({{"idle_s", "5"}});
    b.edit_main("local wild_mod = require(\"wild\")\n", "emf.teleport = function() error(\"teleport broke\") end\n");
    b.roll(0.0);  // settle; the next pick 30 s later
    b.reply("beastmaster.give wolf");
    fire_at(b, 900, 4, 4);
    for (uint32_t id = 5000; id < 6000; ++id) add_entity(b.r.fake, id);  // a search takes 5 steps
    roaming(b, "Wolf", 1234, 0, 4);
    b.seconds(10);  // searched from 3 s of stillness on (5 steps); idling from 5 s
    REQUIRE(asleep(b, 1234));
    creature(b, 6100, 30, 30, kPlayerAlly);  // a player-side creature left asleep, the last entity
    auto& stray = component<ecs::DynamicEnemy01BehaviorState>(b.r.fake, 6100);
    stray.enabledFeatures = kSleepBit;
    stray.state = AiState::Sleep;
    stray.sleepEndTime.value = 1000000000000000000LL;
    b.seconds(25);  // 35 s: the next search has begun (30 s after the first)
    REQUIRE(asleep(b, 6100));
    b.roll(0.6);  // the pick at 36 s: by the fire, whose turn fails with an error
    b.seconds(1);
    REQUIRE(awake(b, 1234));
    b.roll(0.0);
    b.seconds(5);
    CHECK(asleep(b, 6100));  // the search under way was dropped
    b.seconds(30);
    CHECK(awake(b, 6100));  // the next one, 30 s after the error
}

// edit_main's reload check compares modification times, so two edits in a row must never get the same one: the system
// clock moves in steps of about 15 ms, and on a fast machine (CI) an edit made right after another fell in the same step,
// was not seen as a change, and the test ran on the old script.
TEST_CASE("rig: write_later gives every write a newer modification time, however fast they follow each other") {
    BeastRig b;
    const std::wstring path = b.folder + L"\\scratch.lua";
    auto stamp = [&] {
        WIN32_FILE_ATTRIBUTE_DATA d{};
        REQUIRE(GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d));
        return (static_cast<uint64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
    };
    write_later(path, "0", 1);
    uint64_t last = stamp();
    for (int i = 1; i < 300; ++i) {
        write_later(path, std::to_string(i), 1);
        const uint64_t now = stamp();
        REQUIRE(now > last);
        last = now;
    }
}

// ---- home: the beasts a player left at home live in the Large Stable near them ----
namespace {
constexpr double kPi = 3.14159265358979;
constexpr const char* kHomeBoar = "255fd8d9-4447-445d-8ffc-4d9dc0afdd16";
constexpr const char* kHomeHound = "87575915-bc75-4611-ab71-0cd4e79535b1";
constexpr const char* kHomeSabretooth = "50b96064-3ffc-47a2-8c7d-1d592a3c459c";
constexpr const char* kHomeAurochs = "d5f2b8e3-7c4a-4e9f-8b21-6a3c8d4e0f02";
// A Large Stable (the beasts pack's) `id` at (x, 10, z) in the base of the flame altar 900 at (0, 0), turned `yaw_deg`
// (0: its open side +z, 90: +x).
void large_stable(BeastRig& b, uint32_t id, double x, double z, double yaw_deg = 0) {
    flame_altar(b, 900, 0, 0);
    stable_prop(b, id, x, 10, z, kLargeOwnStableModel);
    auto& q = live_of<ecs::CurrentTransform>(b, id).transform.orientation;
    const double h = yaw_deg * kPi / 360.0;
    q.x = 0;
    q.y = static_cast<float>(std::sin(h));
    q.z = 0;
    q.w = static_cast<float>(std::cos(h));
}
// The creatures the next `n` spawns make: wild, level 1, 100 health, a SpawnTime (the game's clock), an AI state.
void homes_ready(BeastRig& b, int n) {
    const uint32_t first = b.r.fake.next_spawn_id;
    b.spawnable(n);
    for (int i = 0; i < n; ++i) {
        const uint32_t id = first + static_cast<uint32_t>(i);
        component<ecs::SpawnTime>(b.r.fake, id).spawnTime.value = kSpawnNs;
        component<ecs::DynamicEnemy01BehaviorState>(b.r.fake, id);
    }
}
bool spawned_as(const Fake::Spawn& s, const char* guid) {
    const auto g = guid_bytes(guid);
    return std::memcmp(s.guid, g.data(), 16) == 0;
}
// A world point (x, z) in the frame of a stable at (sx, sz) turned yaw_deg: across it (first) and out of its open side.
std::pair<double, double> local_of(double x, double z, double sx, double sz, double yaw_deg) {
    const double a = yaw_deg * kPi / 180.0, dx = x - sx, dz = z - sz;
    return {dx * std::cos(a) - dz * std::sin(a), dx * std::sin(a) + dz * std::cos(a)};
}
// On the Large Stable's plank floor, between its walls (the Stable's inner floor times 1.6).
bool on_floor(const std::pair<double, double>& l) {
    return l.first > -2.34 && l.first < 2.49 && l.second > -1.44 && l.second < 2.27;
}
// The yaw of the last turn (a teleport with a yaw) asked for entity `id`, in [-180, 180); nan when none.
double last_yaw(BeastRig& b, uint32_t id) {
    double yaw = std::nan("");
    for (const auto& t : b.r.fake.teleports)
        if (t.entity == id && (t.flags & EMF_TELEPORT_FACE_YAW)) yaw = t.yaw_deg;
    return std::isnan(yaw) ? yaw : std::fmod(std::fmod(yaw + 180.0, 360.0) + 360.0, 360.0) - 180.0;
}
// Teleports with a ground search (a put-back, not a turn in place) asked for entity `id` to (x, z).
size_t moves_to(BeastRig& b, uint32_t id, double x, double z) {
    size_t n = 0;
    for (const auto& t : b.r.fake.teleports)
        n += t.entity == id && (t.flags & EMF_TELEPORT_FIND_GROUND) && std::abs(t.x - x) < 0.01 && std::abs(t.z - z) < 0.01;
    return n;
}
bool destroyed(BeastRig& b, uint32_t id) {
    return std::count(b.r.fake.destroyed.begin(), b.r.fake.destroyed.end(), id) > 0;
}
// A save of player 1's beasts (`lines`: "b ..." lines) in a world that has had a stable, loaded.
void home_save(BeastRig& b, const std::string& lines) {
    write(b.data, lines + "s 1\n");
    b.r.fake.session = 2;
    fire_load(1);
    b.seconds(1);
}
// Player 1's three beasts at home (three_beasts: Fang the wolf, Tusk the boar, Rex the hound) and a Large Stable at
// (10, 10, 0), found by the next search: they show up in it as 1234, 1235, 1236.
void home_three(BeastRig& b, double yaw_deg = 0) {
    three_beasts(b);
    large_stable(b, 950, 10, 0, yaw_deg);
    homes_ready(b, 3);
    b.seconds(32);
}
}  // namespace

TEST_CASE("home: the beasts a player left at home show up in the Large Stable near them, each on a spot of its own on "
          "its floor, as their allies keeping to the stable") {
    BeastRig b;
    home_three(b);
    REQUIRE(b.r.fake.spawns.size() == 3);
    CHECK(spawned_as(b.r.fake.spawns[0], kWolfTemplate));
    CHECK(spawned_as(b.r.fake.spawns[1], kHomeBoar));
    CHECK(spawned_as(b.r.fake.spawns[2], kHomeHound));
    const auto guards = b.guards();
    for (uint32_t i = 0; i < 3; ++i) {
        const auto& s = b.r.fake.spawns[i];
        CHECK(on_floor(local_of(s.x, s.z, 10, 0, 0)));
        CHECK(s.y > 10.0);
        CHECK(s.y < 11.0);
        CHECK(b.faction(1234 + i) == kPlayerAlly);
        CHECK(std::count(guards.begin(), guards.end(), std::make_pair(1234 + i, 950u)) >= 1);
        for (uint32_t j = 0; j < i; ++j)
            CHECK(std::hypot(s.x - b.r.fake.spawns[j].x, s.z - b.r.fake.spawns[j].z) >= 1.0);
    }
    b.seconds(5);
    CHECK(b.r.fake.spawns.size() == 3);  // once
    CHECK(b.r.fake.destroyed.empty());
}

TEST_CASE("home: a Large Stable turned 90 degrees has its spots turned with it; the beasts turn to face out of its open "
          "side") {
    BeastRig b;
    home_three(b, 90);
    REQUIRE(b.r.fake.spawns.size() == 3);
    // the first spot, 1.75 m to the left and 0.45 m out, turned with the stable: 0.45 m along +x, 1.75 m along +z
    CHECK(b.r.fake.spawns[0].x == doctest::Approx(10.45));
    CHECK(b.r.fake.spawns[0].z == doctest::Approx(1.75));
    for (const auto& s : b.r.fake.spawns) CHECK(on_floor(local_of(s.x, s.z, 10, 0, 90)));
    b.seconds(2);
    for (uint32_t id : {1234u, 1235u, 1236u}) {
        const double yaw = last_yaw(b, id);
        REQUIRE_FALSE(std::isnan(yaw));
        CHECK(std::abs(yaw - 90) <= 20.5);
    }
}

TEST_CASE("home: a beast at home strolls first, then rests in the game's Sleep state (a wolf lies down, a boar stands "
          "still), then strolls again near the stable") {
    RollRig b;
    b.roll(0.0);  // the shortest scenes: a first stroll of 5 s, a rest of 40 s, a stroll of 15 s
    home_three(b);
    CHECK(awake(b, 1234));
    b.seconds(6);
    CHECK(asleep(b, 1234));
    CHECK(asleep(b, 1235));
    const size_t guarded = b.guards().size();
    b.seconds(40);
    CHECK(awake(b, 1234));
    CHECK(b.guards().size() > guarded);  // told to keep to the stable again
    b.seconds(15);
    CHECK(asleep(b, 1234));
}

TEST_CASE("home: a beast off the floor when its stroll ends is put back on its spot before it rests; one 6 m or more "
          "from the middle for 3 s at once") {
    RollRig b;
    b.roll(0.0);
    home_three(b);
    REQUIRE(b.r.fake.spawns.size() == 3);
    const Fake::Spawn spot = b.r.fake.spawns[0];
    place(b.r.fake, 1234, 10, 10, 4);  // it strolled out of the open side, 4 m from the middle
    b.seconds(6);
    REQUIRE(asleep(b, 1234));
    CHECK(moves_to(b, 1234, spot.x, spot.z) == 1);
    place(b.r.fake, 1234, spot.x, spot.y, spot.z);
    place(b.r.fake, 1235, 10, 10, 9);  // 9 m out
    const size_t before = moves_to(b, 1235, b.r.fake.spawns[1].x, b.r.fake.spawns[1].z);
    b.seconds(2);
    CHECK(moves_to(b, 1235, b.r.fake.spawns[1].x, b.r.fake.spawns[1].z) == before);
    b.seconds(3);
    CHECK(moves_to(b, 1235, b.r.fake.spawns[1].x, b.r.fake.spawns[1].z) == before + 1);
}

// As for a beast out: another mod's move order (EMF 0.6.8) walks a beast at home somewhere; Beastmaster put it back on
// its spot, told it to guard the stable and held it asleep meanwhile, so the move never got anywhere.
TEST_CASE("home: a beast at home on another mod's move order is left alone (not put back, turned, told to guard the "
          "stable or held asleep); after the move it is put back") {
    RollRig b;
    b.roll(0.0);
    home_three(b);
    REQUIRE(b.r.fake.spawns.size() == 3);
    const Fake::Spawn spot = b.r.fake.spawns[0];
    b.seconds(6);
    REQUIRE(asleep(b, 1234));
    auto stable_guards = [&b] {
        const auto sent = b.guards();
        return std::count(sent.begin(), sent.end(), std::make_pair(1234u, 950u));
    };
    const auto guards_before = stable_guards();
    const size_t teleports_before = teleports_of(b.r.fake, 1234);
    b.r.fake.moving[1234] = 7;
    b.seconds(1);
    CHECK(awake(b, 1234));  // got up: the move can go
    place(b.r.fake, 1234, 10, 10, 9);  // 9 m out of the open side, walking
    b.seconds(40);                     // past its rest and the next stroll's end
    CHECK(teleports_of(b.r.fake, 1234) == teleports_before);
    CHECK(stable_guards() == guards_before);
    CHECK(awake(b, 1234));
    CHECK_FALSE(destroyed(b, 1234));
    CHECK(b.r.fake.spawns.size() == 3);
    b.r.fake.moving.erase(1234);  // the move ended (EMF gives it back to the stable); still 9 m out
    b.seconds(5);
    CHECK(moves_to(b, 1234, spot.x, spot.z) >= 1);
}

TEST_CASE("home: a beast at home never fights (its aggro and chosen targets are cleared every step) and is healed to "
          "full every second") {
    BeastRig b;
    home_three(b);
    creature(b, 70, 20, 10, kSkeletonFoe);
    fighting(b, 1234, 70);
    one_step(b);
    CHECK_FALSE(still_fighting(b, 1234));
    b.r.fake.attrs[{1235, EMF_ATTR_HEALTH}] = 40;
    b.seconds(1);
    CHECK(b.added(1235, EMF_ATTR_HEALTH, 60));
}

TEST_CASE("home: a beast called leaves the stable at once and comes beside its owner; sent home, it shows up in the "
          "stable again") {
    BeastRig b;
    home_three(b);
    next_beast(b, 1237);
    CHECK(b.reply("beastmaster.call Fang").find("comes") != std::string::npos);
    CHECK(destroyed(b, 1234));
    REQUIRE(b.r.fake.spawns.size() == 4);
    CHECK(b.r.fake.spawns[3].x == doctest::Approx(2));  // beside its owner
    b.seconds(3);
    CHECK(b.r.fake.spawns.size() == 4);
    homes_ready(b, 1);
    b.reply("beastmaster.dismiss Fang");
    b.seconds(2);
    REQUIRE(b.r.fake.spawns.size() == 5);
    CHECK(spawned_as(b.r.fake.spawns[4], kWolfTemplate));
    CHECK(on_floor(local_of(b.r.fake.spawns[4].x, b.r.fake.spawns[4].z, 10, 0, 0)));
}

TEST_CASE("home: the beasts leave when their owner is more than 70 m from the stable, and show up again within 50 m") {
    BeastRig b;
    home_three(b);
    place(b.r.fake, 1, 70, 10, 0);  // 60 m: they stay
    b.seconds(2);
    CHECK(b.r.fake.destroyed.empty());
    place(b.r.fake, 1, 90, 10, 0);  // 80 m
    b.seconds(2);
    for (uint32_t id : {1234u, 1235u, 1236u}) CHECK(destroyed(b, id));
    homes_ready(b, 3);
    place(b.r.fake, 1, 65, 10, 0);  // 55 m: not yet
    b.seconds(2);
    CHECK(b.r.fake.spawns.size() == 3);
    place(b.r.fake, 1, 40, 10, 0);  // 30 m
    b.seconds(2);
    CHECK(b.r.fake.spawns.size() == 6);
}

TEST_CASE("home: a Large Stable has room for 4 beasts: with 6 at home, the first 4 in list order show") {
    BeastRig b;
    home_save(b, "b 0x00000000000000aa wolf 0 0 A\nb 0x00000000000000aa boar 0 0 B\nb 0x00000000000000aa hound 0 0 C\n"
                 "b 0x00000000000000aa sabretooth 0 0 D\nb 0x00000000000000aa wolf 0 0 E\nb 0x00000000000000aa boar 0 0 F\n");
    large_stable(b, 950, 10, 0);
    homes_ready(b, 6);
    b.seconds(32);
    REQUIRE(b.r.fake.spawns.size() == 4);
    CHECK(spawned_as(b.r.fake.spawns[3], kHomeSabretooth));
}

TEST_CASE("home: the Stable and the animal beds show no beasts; a Large Stable more than 50 m from the player shows none") {
    BeastRig b;
    three_beasts(b);
    flame_altar(b, 900, 0, 0);
    stable_prop(b, 951, 10, 10, 0, kOwnStableModel);
    stable_prop(b, 952, -10, 10, 0, kLargeStableModel);  // the large animal bed
    large_stable(b, 950, 60, 0);
    homes_ready(b, 3);
    b.seconds(32);
    CHECK(b.reply("beastmaster.stable list").find("3. ") != std::string::npos);  // all three are stables
    CHECK(b.r.fake.spawns.empty());
    place(b.r.fake, 1, 15, 10, 0);  // 45 m from the Large Stable
    b.seconds(2);
    CHECK(b.r.fake.spawns.size() == 3);
}

TEST_CASE("home: a beast at home that dies anyway is removed and shows up again 60 s later; its owner loses nothing") {
    BeastRig b;
    home_three(b);
    b.r.fake.attrs[{1234, EMF_ATTR_HEALTH}] = 0;
    deliver(b.r.fake, "EntityDiedEvent", died_event(1234));
    b.seconds(2);
    CHECK(destroyed(b, 1234));  // its body
    homes_ready(b, 1);
    b.seconds(50);
    CHECK(b.r.fake.spawns.size() == 3);
    b.seconds(10);
    REQUIRE(b.r.fake.spawns.size() == 4);
    CHECK(spawned_as(b.r.fake.spawns[3], kWolfTemplate));
    next_beast(b, 1238);
    CHECK(b.reply("beastmaster.call Fang").find("comes") != std::string::npos);  // no rest
}

TEST_CASE("home: a player building within 20 m of the Large Stable sends its beasts away; they come back 5 s after the "
          "building stops") {
    BeastRig b;
    home_three(b);
    building(b.r.fake, 1, true);
    b.seconds(2);
    for (uint32_t id : {1234u, 1235u, 1236u}) CHECK(destroyed(b, id));
    homes_ready(b, 3);
    b.seconds(5);
    CHECK(b.r.fake.spawns.size() == 3);
    building(b.r.fake, 1, false);
    b.seconds(3);
    CHECK(b.r.fake.spawns.size() == 3);
    b.seconds(4);
    CHECK(b.r.fake.spawns.size() == 6);
}

TEST_CASE("home: with the setting home_beasts off no beast shows; turned off while they show, they leave") {
    BeastRig off({{"home_beasts", "false"}});
    home_three(off);
    CHECK(off.r.fake.spawns.empty());
    BeastRig b;
    home_three(b);
    REQUIRE(b.r.fake.spawns.size() == 3);
    b.r.fake.settings["beastmaster.home_beasts"] = "false";
    b.seconds(2);
    for (uint32_t id : {1234u, 1235u, 1236u}) CHECK(destroyed(b, id));
    b.seconds(5);
    CHECK(b.r.fake.spawns.size() == 3);
}

TEST_CASE("home: a beast whose pack is not installed does not show (the others do) and is tried again after 5 minutes") {
    BeastRig b;
    home_save(b, "b 0x00000000000000aa aurochs 0 0 Horns\nb 0x00000000000000aa wolf 0 0 Fang\n");
    b.r.fake.missing_templates.insert(guid_bytes(kHomeAurochs));
    large_stable(b, 950, 10, 0);
    homes_ready(b, 1);
    b.seconds(32);
    REQUIRE(b.r.fake.spawns.size() == 1);
    CHECK(spawned_as(b.r.fake.spawns[0], kWolfTemplate));
    b.r.fake.missing_templates.clear();
    homes_ready(b, 1);
    b.seconds(60);
    CHECK(b.r.fake.spawns.size() == 1);
    b.seconds(250);
    REQUIRE(b.r.fake.spawns.size() == 2);
    CHECK(spawned_as(b.r.fake.spawns[1], kHomeAurochs));
}

TEST_CASE("home: idle's search for player-side creatures left asleep leaves the beasts resting at home asleep") {
    RollRig b;
    b.roll(0.0);
    home_three(b);
    b.seconds(6);
    REQUIRE(asleep(b, 1236));
    out_at(b, "Fang", 1237, 0, 4);  // a beast out: its owner stands still, idle searches the world
    b.seconds(8);
    CHECK(asleep(b, 1236));
    CHECK(asleep(b, 1235));
}

TEST_CASE("home: after a reload of the script the beasts it had at home are taken out of the world before new ones "
          "show up; a player's beast out that stands in the stable stays") {
    BeastRig b;
    home_three(b);
    creature(b, 1300, 10.5, 0.5, kPlayerAlly);  // someone's beast out, on the floor: it guards its owner
    component<ecs::DynamicEnemy01BehaviorState>(b.r.fake, 1300).guardEntityId.id = 1;
    creature(b, 1301, 9.5, 0.5, kPlayerAlly);  // a player-side creature that is no beast (another graph): a villager
    for (uint32_t id : {1234u, 1235u, 1236u, 1300u}) {  // beasts: they run a beast's animation graph
        auto g = guid_bytes(kWolfGraph);
        g.resize(ecs::AnimationGraphResourceComponent2::kSize, 0);
        b.r.fake.templates[{ecs::AnimationGraphResourceComponent2::kHash, id}] = g;
    }
    homes_ready(b, 3);
    b.edit_main("local wild_mod = require(\"wild\")\n", "-- reloaded\n");
    b.seconds(33);
    for (uint32_t id : {1234u, 1235u, 1236u}) CHECK(destroyed(b, id));
    CHECK_FALSE(destroyed(b, 1300));
    CHECK_FALSE(destroyed(b, 1301));
    CHECK(b.r.fake.spawns.size() == 6);
}

TEST_CASE("home: an entity id the game gave to another creature is not ours any more: it is left alone, and the beast "
          "shows up again on a new one") {
    BeastRig b;
    home_three(b);
    b.seconds(2);                                                                 // its SpawnTime is known
    live_of<ecs::SpawnTime>(b, 1234).spawnTime.value = kSpawnNs + 5000000000LL;  // another creature on the id now
    homes_ready(b, 1);
    b.seconds(2);
    CHECK_FALSE(destroyed(b, 1234));
    REQUIRE(b.r.fake.spawns.size() == 4);
    CHECK(spawned_as(b.r.fake.spawns[3], kWolfTemplate));
}

TEST_CASE("home: a creature that does not become a player-side one is taken out at once, and its beast is not tried "
          "again for 5 minutes") {
    BeastRig b;
    home_save(b, "b 0x00000000000000aa wolf 0 0 Fang\n");
    large_stable(b, 950, 10, 0);
    homes_ready(b, 3);
    b.r.fake.components.erase({ecs::Faction::kHash, 1234});  // nothing to write its faction into
    b.seconds(32);
    REQUIRE(b.r.fake.spawns.size() == 1);
    CHECK(destroyed(b, 1234));
    b.seconds(10);
    CHECK(b.r.fake.spawns.size() == 1);
}

TEST_CASE("home: the game's time is taken again from each new beast at home, so its Sleep lease follows the game's "
          "clock") {
    RollRig b;
    b.roll(0.0);
    home_three(b);
    b.r.fake.attrs[{1234, EMF_ATTR_HEALTH}] = 0;
    deliver(b.r.fake, "EntityDiedEvent", died_event(1234));
    b.seconds(2);
    homes_ready(b, 1);  // 1237: the game's clock runs 1000 s ahead of the first beast's by now
    live_of<ecs::SpawnTime>(b, 1237).spawnTime.value = kSpawnNs + 1000000000000LL;
    b.seconds(60);
    REQUIRE(b.r.fake.spawns.size() == 4);
    b.seconds(8);
    REQUIRE(asleep(b, 1237));
    const int64_t ahead = ai_now(b, 1237).sleepEndTime.value - (kSpawnNs + 1000000000000LL);
    CHECK(ahead >= 120000000000LL);  // the lease: the game's time plus 120 s
    CHECK(ahead <= 140000000000LL);
}

TEST_CASE("home: a beast the game's Sleep state cannot hold strolls on, without being turned again every second") {
    RollRig b;
    b.roll(0.0);
    home_three(b);
    b.r.fake.components.erase({ecs::DynamicEnemy01BehaviorState::kHash, 1235});
    const size_t before = turns_of(b, 1235).size();
    b.seconds(12);
    CHECK(turns_of(b, 1235).size() <= before + 2);
}

TEST_CASE("home: with make_room off the beasts at home stay while someone builds near the stable") {
    BeastRig b({{"make_room", "false"}});
    home_three(b);
    building(b.r.fake, 1, true);
    b.seconds(3);
    CHECK(b.r.fake.destroyed.empty());
}

TEST_CASE("home: beastmaster.home says where your beasts at home are and what they do") {
    BeastRig b;
    three_beasts(b);
    CHECK(b.reply("beastmaster.home").find("None of your beasts is in a Large Stable now") != std::string::npos);
    large_stable(b, 950, 10, 0);
    homes_ready(b, 3);
    b.seconds(32);
    const std::string text = b.reply("beastmaster.home");
    CHECK(text.find("Fang: in the Large Stable at 10, 10, 0, strolling") != std::string::npos);
    CHECK(text.find("Rex: in the Large Stable at 10, 10, 0") != std::string::npos);
}

TEST_CASE("home: a beast at home has the health it has when called (its role, loyalty and talents)") {
    BeastRig b({{"beast_health", "100"}, {"beast_damage", "100"}});
    three_beasts(b);
    large_stable(b, 950, 10, 0);
    homes_ready(b, 3);
    for (uint32_t id : {1234u, 1235u, 1236u}) component<ecs::Health>(b.r.fake, id).dataStorage[2] = 100;
    b.seconds(33);
    REQUIRE(b.r.fake.spawns.size() == 3);
    CHECK(now_of<ecs::Health>(b, 1234).dataStorage[3] == 20);  // the wolf's role: 1.2x
    CHECK(now_of<ecs::Health>(b, 1235).dataStorage[3] == 70);  // the boar's: 1.7x
}

TEST_CASE("home: a change of Beast health reaches the beasts at home at once (not only when next shown)") {
    BeastRig b({{"beast_health", "100"}, {"beast_damage", "100"}});
    three_beasts(b);
    large_stable(b, 950, 10, 0);
    homes_ready(b, 3);
    for (uint32_t id : {1234u, 1235u, 1236u}) component<ecs::Health>(b.r.fake, id).dataStorage[2] = 100;
    b.seconds(33);
    REQUIRE(b.r.fake.spawns.size() == 3);
    REQUIRE(now_of<ecs::Health>(b, 1234).dataStorage[3] == 20);
    b.r.fake.settings["beastmaster.beast_health"] = "200";
    for (auto& [mod, cb] : b.r.fake.setting_watchers)  // the Mods tab changed it
        if (mod == "beastmaster") cb.first(kServer, "beastmaster", "beast_health", cb.second);
    b.seconds(2);
    CHECK(b.r.fake.spawns.size() == 3);  // the same creatures: none shown again
    CHECK(now_of<ecs::Health>(b, 1234).dataStorage[3] == 140);  // 100 x 1.2 x 2 - 100
    CHECK(now_of<ecs::Health>(b, 1235).dataStorage[3] == 240);  // 100 x 1.7 x 2 - 100
}

TEST_CASE("home: a monster's hit on a beast at home sends that stable's beasts inside (out of the world); they come "
          "back 30 s after the last hit") {
    BeastRig b;
    home_three(b);
    creature(b, 70, 14, 4, kSkeletonFoe);
    deliver(b.r.fake, "HitEvent", hit_event(70, 1235));
    b.seconds(1);
    for (uint32_t id : {1234u, 1235u, 1236u}) CHECK(destroyed(b, id));
    homes_ready(b, 3);
    b.seconds(20);
    deliver(b.r.fake, "HitEvent", hit_event(70, 1));  // the monster hits the player at the stable: still inside
    b.seconds(25);
    CHECK(b.r.fake.spawns.size() == 3);
    b.seconds(7);
    CHECK(b.r.fake.spawns.size() == 6);
}

TEST_CASE("home: a monster's hit on a player at a Large Stable sends its beasts inside; a hit on a player far from it, "
          "or a fall, does not") {
    BeastRig b;
    home_three(b);
    creature(b, 70, 40, 40, kSkeletonFoe);
    place(b.r.fake, 2, 60, 10, 60);  // player 2, 78 m from the stable
    deliver(b.r.fake, "HitEvent", hit_event(70, 2));
    deliver(b.r.fake, "HitEvent", hit_event(0, 1));  // a fall: no source
    b.seconds(2);
    CHECK(b.r.fake.destroyed.empty());
    deliver(b.r.fake, "HitEvent", hit_event(70, 1));  // player 1, 10 m from the stable
    b.seconds(1);
    for (uint32_t id : {1234u, 1235u, 1236u}) CHECK(destroyed(b, id));
}

namespace {
// The game's last answer to teleport `t` (its callback): moved, or failed (given up after it was taken).
void answer(const EmfTeleport& t, bool moved) {
    EmfTeleportResult r{};
    r.size = sizeof r;
    r.entity = t.entity;
    r.status = moved ? EMF_TELEPORT_MOVED : EMF_TELEPORT_FAILED;
    r.flags = EMF_TELEPORT_LAST;
    r.reason = moved ? "" : "given up";
    if (t.cb) t.cb(kServer, &r, t.user);
}
}  // namespace

TEST_CASE("idle: a turn the game took but did not carry out (its teleport failed) is asked again, once") {
    RollRig b({{"call_cooldown_s", "0"}});
    b.roll(0.6);  // by the fire
    b.reply("beastmaster.give boar");
    fire_at(b, 900, 4, 4);
    roaming(b, "Boar", 1234, 0, 4);
    b.seconds(11);
    auto turns = turns_of(b, 1234);
    REQUIRE(turns.size() == 1u);
    answer(turns[0], false);
    one_step(b);
    turns = turns_of(b, 1234);
    REQUIRE(turns.size() == 2u);
    CHECK(turns[1].yaw_deg == doctest::Approx(90.0));
    answer(turns[1], false);  // failed again: not a third time
    b.seconds(2);
    CHECK(turns_of(b, 1234).size() == 2u);
}

TEST_CASE("home: a beast's turn to face out that the game did not carry out is asked again, at most 3 times") {
    RollRig b;
    b.roll(0.6);  // the longest first stroll (12 s): no turn of a rest meanwhile
    home_three(b);
    size_t answered = 0;
    for (int i = 0; i < 6; ++i) {
        const auto turns = turns_of(b, 1234);
        if (turns.size() > answered) answer(turns.back(), false);  // each turn answered once (the answer is its last)
        answered = turns.size();
        b.seconds(1);
    }
    CHECK(turns_of(b, 1234).size() == 3u);
}

// ---- home: petting the beasts at home, and their night in the Large Stable ----
namespace {
// The last E Pet prompt EMF was asked to put on entity `id` (nullptr when none).
const Fake::Off* pet_prompt(BeastRig& b, uint32_t id) {
    const Fake::Off* last = nullptr;
    for (const auto& o : b.r.fake.offers)
        if (o.entity == id && o.o.verb == EMF_VERB_PET) last = &o;
    return last;
}
// `player` presses E at beast `id`'s Pet prompt.
void pet(BeastRig& b, uint32_t player, uint32_t id) {
    const Fake::Off* o = pet_prompt(b, id);
    REQUIRE(o != nullptr);
    o->cb(kServer, player, id, o->user);
}
size_t pet_prompts(BeastRig& b) {
    size_t n = 0;
    for (const auto& o : b.r.fake.offers) n += o.o.verb == EMF_VERB_PET;
    return n;
}
}  // namespace

TEST_CASE("home: each beast at home carries the game's E Pet prompt; one EMF no longer holds is offered again") {
    BeastRig b;
    with_offers(b.r.api);
    home_three(b);
    b.seconds(2);
    for (uint32_t id : {1234u, 1235u, 1236u}) CHECK(pet_prompt(b, id) != nullptr);
    const size_t before = pet_prompts(b);
    b.seconds(5);
    CHECK(pet_prompts(b) == before);  // held: not asked again
    b.r.fake.offer_active = false;    // EMF dropped them (the beasts streamed out and back)
    b.seconds(11);
    CHECK(pet_prompts(b) >= before + 3);
}

TEST_CASE("home: its owner pets a beast at home: it turns to them, settles for 20 s and earns a loyalty point, once a "
          "game day") {
    BeastRig b;
    with_offers(b.r.api);
    home_three(b);
    b.seconds(2);
    pet(b, 1, 1234);
    CHECK(asleep(b, 1234));
    const auto& s = b.r.fake.spawns[0];
    const double toward = std::atan2(0.0 - s.x, 0.0 - s.z) * 180.0 / kPi;  // player 1 stands at (0, 10, 0)
    REQUIRE_FALSE(std::isnan(last_yaw(b, 1234)));
    CHECK(std::abs(last_yaw(b, 1234) - toward) < 1.0);
    CHECK(noted(b, "Fang enjoys it"));
    CHECK(b.saved().find("y 0x00000000000000aa 1 Fang") != std::string::npos);
    pet(b, 1, 1234);  // again today: a note, no point
    CHECK(noted(b, "again tomorrow"));
    CHECK(b.saved().find("y 0x00000000000000aa 1 Fang") != std::string::npos);
    b.seconds(21);
    CHECK(awake(b, 1234));  // the 20 s are over: it strolls again
}

TEST_CASE("home: a new game day (the dawn) lets a beast earn a point from a pet again") {
    BeastRig b;
    with_offers(b.r.api);
    set_daytime(b.r.fake, 20.0);
    home_three(b);
    b.seconds(2);
    pet(b, 1, 1235);
    CHECK(b.saved().find("y 0x00000000000000aa 1 Tusk") != std::string::npos);
    set_daytime(b.r.fake, 2.0);  // past midnight: the same game day until the dawn
    b.seconds(2);
    pet(b, 1, 1235);
    CHECK(b.saved().find("y 0x00000000000000aa 1 Tusk") != std::string::npos);
    set_daytime(b.r.fake, 7.0);  // the dawn
    b.seconds(2);
    pet(b, 1, 1235);
    CHECK(b.saved().find("y 0x00000000000000aa 2 Tusk") != std::string::npos);
}

TEST_CASE("home: another player's pet: the beast turns to them and settles, but earns no point") {
    BeastRig b;
    with_offers(b.r.api);
    home_three(b);
    b.seconds(2);
    pet(b, 2, 1236);
    CHECK(asleep(b, 1236));
    CHECK(b.saved().find("Rex") != std::string::npos);
    CHECK(b.saved().find("y 0x00000000000000aa 1 Rex") == std::string::npos);
    bool told = false;
    for (const auto& [player, text] : b.r.fake.tells) told = told || (player == 2 && text.find("Rex enjoys it") != std::string::npos);
    CHECK(told);
}

TEST_CASE("home: at night the beasts at home rest the whole night, without strolls; in the morning they stroll again") {
    RollRig b;
    b.roll(0.0);  // the shortest scenes
    set_daytime(b.r.fake, 22.0);
    home_three(b);
    b.seconds(2);
    for (int i = 0; i < 6; ++i) {  // two minutes of night
        for (uint32_t id : {1234u, 1235u, 1236u}) CHECK(asleep(b, id));
        b.seconds(20);
    }
    CHECK(b.reply("beastmaster.home").find("Fang: in the Large Stable at 10, 10, 0, sleeping") != std::string::npos);
    set_daytime(b.r.fake, 8.0);
    b.seconds(41);  // the rest under way ends (40 s at the most with these rolls): a stroll
    CHECK(awake(b, 1234));
}

// ---- Beastmaster for players who joined (mod messages, Lua 1.19: the beastmaster/ui channel) ----
namespace {
const Fake::NetCh& ui_channel(BeastRig& b) {
    const Fake::NetCh* found = nullptr;
    for (const auto& c : b.r.fake.net_channels)
        if (c.name == "beastmaster/ui") found = &c;
    REQUIRE(found != nullptr);
    return *found;
}
EmfNetFrom net_from(uint32_t player, uint64_t account) {
    EmfNetFrom f{};
    f.size = sizeof f;
    f.player = player;
    f.account = account;
    f.peer_version = 1;
    return f;
}
// A message from player `p`'s game to the server (the core's call, on the server world).
void from_player(BeastRig& b, uint32_t p, uint64_t account, const std::string& json) {
    const Fake::NetCh ch = ui_channel(b);
    const EmfNetFrom f = net_from(p, account);
    ch.def.on_server(kServer, &f, json.data(), static_cast<uint32_t>(json.size()), ch.def.user);
}
// A message from the server to this game (the core's call, on the client world).
void from_server(BeastRig& b, const std::string& json) {
    const Fake::NetCh ch = ui_channel(b);
    const EmfNetFrom f = net_from(0, 0);
    ch.def.on_player(kClient, &f, json.data(), static_cast<uint32_t>(json.size()), ch.def.user);
}
// The data of the messages sent to player `p` (0: to the server).
std::vector<std::string> sent_to(BeastRig& b, uint32_t p) {
    std::vector<std::string> out;
    for (const auto& m : b.r.fake.net_sent)
        if (m.channel == "beastmaster/ui" && m.player == p) out.push_back(m.data);
    return out;
}
std::vector<std::string> sent_up(BeastRig& b) { return sent_to(b, 0); }
// Takes the messages to player `p` (0: to the server) out of what was sent.
std::vector<std::string> take_sent(BeastRig& b, uint32_t p) {
    std::vector<std::string> out;
    auto& v = b.r.fake.net_sent;
    for (auto it = v.begin(); it != v.end();) {
        if (it->channel == "beastmaster/ui" && it->player == p) {
            out.push_back(it->data);
            it = v.erase(it);
        } else {
            ++it;
        }
    }
    return out;
}
// What the server sent player `p` arrives at this game (the rig is both).
void relay_down(BeastRig& b, uint32_t p) {
    for (const auto& m : take_sent(b, p)) from_server(b, m);
}
// What this game sent the server arrives there, from player `p`.
void relay_up(BeastRig& b, uint32_t p, uint64_t account) {
    for (const auto& m : take_sent(b, 0)) from_player(b, p, account, m);
}
// `n` steps of the client world alone (someone else's game: after 180 of them Beastmaster knows it joined one).
void client_steps(BeastRig& b, int n) {
    for (int i = 0; i < n; ++i) {
        b.r.fake.tick += 1;
        b.host->tick(kClient);
    }
}
// Presses `vk` on the client world: no server world stepped in the last second (the host sees a joined game).
// `keep`: still held afterwards (the test lets go: g_held[vk] = false).
void press_client(BeastRig& b, uint32_t vk, bool keep = false) {
    b.host->set_key_reader([](uint32_t k) { return g_held[k]; });
    Sleep(1100);
    g_held[vk] = true;
    b.r.fake.tick += 1;
    b.host->tick(kClient);
    if (keep) return;
    g_held[vk] = false;
    b.r.fake.tick += 1;
    b.host->tick(kClient);
}
bool has_part(const std::vector<std::string>& messages, const std::string& a, const std::string& c = "") {
    for (const auto& m : messages)
        if (m.find(a) != std::string::npos && m.find(c) != std::string::npos) return true;
    return false;
}
// Player 2 (at 30, 10, 0, looking along +z) tames a beaten wolf 5 m in front of them: "You tamed a wolf: Wolf".
void tame_for_player2(BeastRig& b) {
    b.look(2, 30, 10.5, 0, 0);
    b.beast(50, kWolfGraph, 30, 5, kWildBeast, 20, 100);
    REQUIRE(b.reply("beastmaster.tame", 2, 0xBB).find("Taming") != std::string::npos);
    b.seconds(4);
}
// Player 2's beasts (0xBB): Grey (wolf, active), Tusk (boar); player 1's Fang (wolf, active); the world has a stable.
void joined_beasts(BeastRig& b) {
    write(b.data, "b 0x00000000000000aa wolf 0 0 Fang\nb 0x00000000000000bb wolf 0 0 Grey\n"
                  "b 0x00000000000000bb boar 0 0 Tusk\na 0x00000000000000aa Fang\na 0x00000000000000bb Grey\ns 1\n");
    b.r.fake.session = 2;
    fire_load(1);
    b.seconds(1);
}
}  // namespace

TEST_CASE("beastmaster net: the ui channel is beastmaster/ui, version 1, both ways, up to 16384 bytes") {
    BeastRig b({}, true);
    const auto& ch = ui_channel(b);
    CHECK(ch.def.version == 1u);
    CHECK(ch.def.flags == (EMF_NET_TO_SERVER | EMF_NET_TO_PLAYERS));
    CHECK(ch.def.max_size == 16384u);
    CHECK(ch.def.on_server != nullptr);
    CHECK(ch.def.on_player != nullptr);
}

TEST_CASE("beastmaster net: news for a joined player whose game has the channel goes as a note message, not a chat line; another version gets the chat line") {
    for (uint32_t version : {1u, 4u}) {
        CAPTURE(version);
        BeastRig b({}, true);
        b.r.fake.net_peer_version = version;
        tame_for_player2(b);
        CHECK(has_part(sent_to(b, 2), "\"t\":\"note\"", "You tamed a wolf: Wolf") == (version == 1));
        CHECK(told(b, 2, "You tamed a wolf") == (version != 1));
    }
}

TEST_CASE("beastmaster net: the joined player's game shows the server's note on screen") {
    BeastRig b({}, true);
    from_server(b, R"({"t":"note","text":"Grey reached loyalty 3: a talent point to spend at a stable."})");
    CHECK(b.noted("Grey reached loyalty 3"));
}

TEST_CASE("beastmaster net: messages from a player's game that are not Beastmaster's are dropped, logged once, and the mod runs on") {
    BeastRig b({}, true);
    b.r.fake.net_peer_version = 1;
    for (const char* bad : {"not json", "[1,2]", R"({"t":5})", R"({"t":"dance"})", R"({"t":"click","n":"x","key":{}})",
                            R"({"t":"pick","seq":[],"i":"one"})", R"({"t":"stables","list":"none"})"})
        from_player(b, 2, 0xBB, bad);
    from_player(b, 2, 0xAA, R"({"t":"wheel"})");  // another account than player 2's: ignored
    CHECK(b.host->status("beastmaster") == "running");
    CHECK(logged("beastmaster/ui"));
    CHECK(sent_to(b, 2).empty());
}

namespace {
// An event on the window this game opened for the server's Beasts window (its events on the client world).
void client_click(BeastRig& b, const std::string& key, uint32_t kind = EMF_UI_CLICK) {
    REQUIRE(!b.r.fake.windows.empty());
    const Fake::Win w = b.r.fake.windows.back();
    EmfUiEvent e{};
    e.size = sizeof e;
    e.kind = kind;
    e.window = w.id.c_str();
    e.key = key.c_str();
    e.from_key = "";
    e.reason = EMF_UI_BY_PLAYER;
    w.cb(kClient, &e, w.user);
}
// Player 2 presses E at the marked stable 960 (at_stable): the server's answer is in sent_to(b, 2).
Fake::Off joined_at_stable(BeastRig& b) {
    b.r.fake.net_peer_version = 1;
    with_offers(b.r.api);
    joined_beasts(b);
    at_stable(b);
    Fake::Off o = *last_offer(b, 960);
    o.cb(kServer, 2, 960, o.user);
    return o;
}
}  // namespace

TEST_CASE("beastmaster net: E at a stable opens the Beasts window on a joined player's game; their clicks act on the server and it is drawn again") {
    BeastRig b({}, true);
    joined_at_stable(b);
    CHECK(b.r.fake.windows.empty());  // not the host's window
    CHECK_FALSE(told(b, 2, "/beastmaster active"));
    REQUIRE(sent_to(b, 2).size() == 1u);
    const std::string win = sent_to(b, 2)[0];
    CHECK(win.find("\"t\":\"win\"") != std::string::npos);
    CHECK(win.find("\"open\":true") != std::string::npos);
    CHECK(win.find("Stable 1/8") != std::string::npos);  // Tusk: the active Grey is in its slot
    relay_down(b, 2);
    REQUIRE(b.r.fake.windows.size() == 1u);
    const Fake::Win w = b.r.fake.windows.back();
    CHECK(w.id == "beastmaster.stable");
    CHECK(w.kind == EMF_WORLD_CLIENT);
    CHECK(w.close_key == kE);
    CHECK(text_at(w, "active.1") == "Grey");
    CHECK(text_at(w, "beast.Tusk") == "Tusk (boar, loyalty 1, at home)");
    client_click(b, "beast.Tusk");
    relay_up(b, 2, 0xBB);
    relay_down(b, 2);
    REQUIRE(!b.r.fake.window_updates.empty());
    CHECK(shows(b.r.fake.window_updates.back(), "Tusk"));
    CHECK(shows(b.r.fake.window_updates.back(), "Talents: "));
    client_click(b, "make_active");
    relay_up(b, 2, 0xBB);
    relay_down(b, 2);
    CHECK(shows(b.r.fake.window_updates.back(), "Tusk is active."));
    CHECK(b.reply("beastmaster.list", 2, 0xBB).find("Tusk - boar, loyalty 1, at home, active") != std::string::npos);
}

TEST_CASE("beastmaster net: the picture on a joined player's beast tab comes from their own game, by name") {
    BeastRig b({}, true);
    with_images(b.r.api);
    joined_at_stable(b);
    relay_down(b, 2);
    client_click(b, "beast.Grey");
    relay_up(b, 2, 0xBB);
    REQUIRE(sent_to(b, 2).size() == 1u);
    CHECK(sent_to(b, 2)[0].find("\"portrait\":\"wolf\"") != std::string::npos);
    CHECK(sent_to(b, 2)[0].find("\"image\"") == std::string::npos);
    CHECK(b.r.fake.images_loaded.empty());  // the server draws no picture
    relay_down(b, 2);
    CHECK(image_row(b.r.fake.window_updates.back()) >= 0);
    CHECK(b.r.fake.images_loaded == std::vector<std::string>{"portrait:beastmaster.wolf"});
    from_server(b, R"({"t":"win","n":1,"title":"Beasts","rows":[{"heading":"X"},{"portrait":"../x","height":6,"fps":8}]})");
    CHECK(image_row(b.r.fake.window_updates.back()) < 0);
    CHECK(shows(b.r.fake.window_updates.back(), "X"));
    CHECK(b.r.fake.images_loaded.size() == 1u);
}

TEST_CASE("beastmaster net: the server draws a joined player's window again only when what it shows changed; a closed one is forgotten") {
    BeastRig b({}, true);
    Fake::Off o = joined_at_stable(b);
    relay_down(b, 2);
    b.seconds(3);
    CHECK(sent_to(b, 2).empty());
    REQUIRE(b.reply("beastmaster.rename Grey Ash", 2, 0xBB).find("now called Ash") != std::string::npos);
    b.seconds(1);
    REQUIRE(sent_to(b, 2).size() == 1u);
    CHECK(sent_to(b, 2)[0].find("\"Ash\"") != std::string::npos);  // its slot
    CHECK(sent_to(b, 2)[0].find("Grey") == std::string::npos);
    CHECK(sent_to(b, 2)[0].find("\"open\"") == std::string::npos);
    relay_down(b, 2);
    client_click(b, "", EMF_UI_CLOSED);
    REQUIRE(has_part(sent_up(b), "\"t\":\"closed\""));
    relay_up(b, 2, 0xBB);
    b.reply("beastmaster.rename Ash Grey", 2, 0xBB);
    b.seconds(2);
    CHECK(sent_to(b, 2).empty());  // forgotten: nothing more drawn
    o.cb(kServer, 2, 960, o.user);
    REQUIRE(sent_to(b, 2).size() == 1u);
    CHECK(sent_to(b, 2)[0].find("\"open\":true") != std::string::npos);
    CHECK(sent_to(b, 2)[0].find("\"n\":2") != std::string::npos);
}

TEST_CASE("beastmaster net: a click from a window opened before the player left or the world loaded again does nothing") {
    BeastRig b({}, true);
    Fake::Off o = joined_at_stable(b);
    relay_down(b, 2);
    remove_entity(b.r.fake, 2);  // player 2 leaves ...
    b.seconds(2);
    add_player(b.r.fake, 2, 0xBB, 30, 10, 0);  // ... and comes back
    add_entity(b.r.fake, 2);
    b.seconds(1);
    client_click(b, "beast.Tusk");
    relay_up(b, 2, 0xBB);
    CHECK(sent_to(b, 2).empty());
    o.cb(kServer, 2, 960, o.user);  // a window again; Tusk's tab; then the world loads again under it
    relay_down(b, 2);
    client_click(b, "beast.Tusk");
    relay_up(b, 2, 0xBB);
    relay_down(b, 2);
    REQUIRE(shows(b.r.fake.window_updates.back(), "Talents: "));
    b.r.fake.session = 3;
    fire_load(1);
    b.seconds(1);
    client_click(b, "make_active");
    relay_up(b, 2, 0xBB);
    CHECK(sent_to(b, 2).empty());
    CHECK(b.reply("beastmaster.list", 2, 0xBB).find("Tusk - boar, loyalty 1, at home, active") == std::string::npos);
}

TEST_CASE("beastmaster net: when the window cannot be sent, the joined player gets the stable's chat commands") {
    BeastRig b({}, true);
    b.r.fake.net_ok = false;  // the core refuses (too large, over the rate, the game said bye)
    joined_at_stable(b);
    CHECK(told(b, 2, "/beastmaster active"));
    b.r.fake.net_sent.clear();
    b.r.fake.net_ok = true;
    b.seconds(2);
    CHECK(sent_to(b, 2).empty());  // no window is held for them
}

namespace {
// A stables report from a player's game: one entry of `model` at (x, y, z), not turned.
std::string report_of(const char* model, double x, double y, double z) {
    char buf[320];
    std::snprintf(buf, sizeof buf,
                  R"({"t":"stables","list":[{"m":"%s","x":%.2f,"y":%.2f,"z":%.2f,"q":{"x":0.0,"y":0.0,"z":0.0,"w":1.0}}]})",
                  model, x, y, z);
    return buf;
}
bool listed(BeastRig& b, const std::string& part) { return b.reply("beastmaster.stable list").find(part) != std::string::npos; }
}  // namespace

TEST_CASE("beastmaster net: a joined player's game reports the stables it sees after its first 30 s, and again only when they change") {
    BeastRig b({}, true);
    b.r.fake.net_peer_version = 1;
    stable_prop(b, 950, 10, 10, 0);  // the client world shows the bed's model
    client_steps(b, 181 + 25 * 60);
    CHECK(sent_up(b).empty());  // still settling
    client_steps(b, 10 * 60);
    REQUIRE(sent_up(b).size() == 1u);
    CHECK(sent_up(b)[0].find("\"t\":\"stables\"") != std::string::npos);
    CHECK(sent_up(b)[0].find("\"m\":\"daeb81bb-8398-4128-9107-c75b2e3e7d5b\"") != std::string::npos);
    client_steps(b, 20 * 60);
    CHECK(sent_up(b).size() == 1u);  // nothing changed
    remove_entity(b.r.fake, 950);
    client_steps(b, 10 * 60);
    REQUIRE(sent_up(b).size() == 2u);  // the bed is gone: an empty list
    CHECK(sent_up(b)[1].find("daeb81bb") == std::string::npos);
}

TEST_CASE("beastmaster net: a stable a joined player reports near a flame altar counts, gets its prompt, is kept with the world, and makes the world one with stables") {
    BeastRig b({}, true);
    b.r.fake.net_peer_version = 1;
    with_offers(b.r.api);
    flame_altar(b, 900, 0, 0);
    stable_prop(b, 960, 10, 10, 0, nullptr);  // a dedicated server: the entity, no render model
    b.seconds(31);                             // a search found the altar
    from_player(b, 2, 0xBB, report_of(kStableModel, 10, 10, 0));
    CHECK(listed(b, "1. Animal bed at 10, 10, 0, seen by a player's game"));
    b.seconds(31);
    CHECK(offers_on(b, 960) >= 1u);
    CHECK(b.saved().find("r 10.00 10.00 0.00 daeb81bb-8398-4128-9107-c75b2e3e7d5b") != std::string::npos);
    CHECK(b.saved().find("s 1") != std::string::npos);
    CHECK(logged("stables reported by 0x00000000000000bb: 1 seen, 1 added, 0 gone"));
    b.r.fake.session = 2;
    fire_load(1);
    b.seconds(1);
    CHECK(listed(b, "1. Animal bed at 10, 10, 0, seen by a player's game"));
}

TEST_CASE("beastmaster net: a Large Stable a joined player's game reports is a home: their beasts at home show up in it") {
    BeastRig b({}, true);
    b.r.fake.net_peer_version = 1;
    joined_beasts(b);
    flame_altar(b, 900, 0, 0);
    stable_prop(b, 960, 35, 10, 0, nullptr);
    homes_ready(b, 3);
    b.seconds(31);
    from_player(b, 2, 0xBB, report_of(kLargeOwnStableModel, 35, 10, 0));
    b.seconds(5);
    CHECK(b.reply("beastmaster.home", 2, 0xBB).find("Tusk: in the Large Stable at 35, 10, 0") != std::string::npos);
}

TEST_CASE("beastmaster net: a reported stable far from every flame altar waits until a search finds one") {
    BeastRig b({}, true);
    stable_prop(b, 960, 10, 10, 0, nullptr);
    from_player(b, 2, 0xBB, report_of(kStableModel, 10, 10, 0));
    CHECK(b.reply("beastmaster.stable list").rfind("No stable in this world yet", 0) == 0);
    flame_altar(b, 900, 0, 0);
    b.seconds(31);
    CHECK(listed(b, "1. Animal bed at 10, 10, 0, seen by a player's game"));
}

TEST_CASE("beastmaster net: a reported stable is gone only when a game within 40 m of it no longer reports it") {
    BeastRig b({}, true);
    flame_altar(b, 900, 0, 0);
    stable_prop(b, 960, 10, 10, 0, nullptr);
    b.seconds(31);
    from_player(b, 2, 0xBB, report_of(kStableModel, 10, 10, 0));  // player 2 at (30, 10, 0): 20 m from it
    REQUIRE(listed(b, "seen by a player's game"));
    place(b.r.fake, 2, 300, 10, 0);
    b.seconds(3);  // (a player's reports count at most one every 2 s)
    from_player(b, 2, 0xBB, R"({"t":"stables","list":{}})");
    CHECK(listed(b, "seen by a player's game"));  // too far to see it
    place(b.r.fake, 2, 30, 10, 0);
    b.seconds(3);
    from_player(b, 2, 0xBB, R"({"t":"stables","list":[]})");
    CHECK(b.reply("beastmaster.stable list").rfind("No stable in this world now", 0) == 0);
    b.seconds(1);
    CHECK(b.saved().find("r 10.00") == std::string::npos);
}

TEST_CASE("beastmaster net: of a report, at most 32 entries are looked at and unknown models or bad numbers are skipped") {
    BeastRig b({}, true);
    flame_altar(b, 900, 0, 0);
    b.seconds(31);
    std::string list;
    for (int i = 0; i < 40; ++i) {
        const bool bad_model = i == 1 || i == 3 || i == 5 || i == 7 || i == 9;
        char e[200];
        if (i == 11)
            std::snprintf(e, sizeof e, R"({"m":"%s","x":"ten","y":10.0,"z":0.0})", kStableModel);
        else
            std::snprintf(e, sizeof e, R"({"m":"%s","x":%d.0,"y":10.0,"z":0.0})", bad_model ? "not-a-model" : kStableModel, 2 * i);
        list += (list.empty() ? "" : ",") + std::string(e);
    }
    from_player(b, 2, 0xBB, R"({"t":"stables","list":[)" + list + "]}");
    b.seconds(1);
    const std::string saved = b.saved();  // (the list command's reply holds at most 1023 bytes: count the saved ones)
    size_t kept = 0;
    for (size_t at = saved.find("r "); at != std::string::npos; at = saved.find("\nr ", at + 1)) ++kept;
    CHECK(kept == 26u);
    CHECK(saved.find("not-a-model") == std::string::npos);
    CHECK(saved.find("r 64.00 ") == std::string::npos);  // entry 32: not looked at
}

TEST_CASE("beastmaster net: on the host, a reported stable the host's own game no longer shows within 40 m is gone, but not in its first 30 s") {
    BeastRig b({}, true);
    flame_altar(b, 900, 0, 0);
    b.seconds(31);
    from_player(b, 2, 0xBB, report_of(kStableModel, 10, 10, 0));
    REQUIRE(listed(b, "seen by a player's game"));
    auto step_both = [&](int secs) {  // the host's client world shows no bed there
        for (int i = 0; i < secs * 60; ++i) {
            b.host->tick(kClient);
            b.r.fake.tick += 1;
            b.host->tick(kServer);
        }
    };
    step_both(20);
    CHECK(listed(b, "seen by a player's game"));
    step_both(20);
    CHECK(b.reply("beastmaster.stable list").rfind("No stable in this world now", 0) == 0);
}

TEST_CASE("beastmaster net: the E key on a joined player's game asks the server only near a stable it sees; the server opens the window only at one without the prompt") {
    BeastRig b({}, true);
    b.r.fake.net_peer_version = 1;
    stable_prop(b, 950, 0, 10, 3);  // the client world shows a bed 3 m from this game's player (1, at the origin)
    client_steps(b, 181 + 6 * 60);
    press_client(b, kE);
    CHECK(has_part(sent_up(b), "\"t\":\"open\""));
    take_sent(b, 0);
    place(b.r.fake, 950, 0, 10, 30);
    client_steps(b, 6 * 60);
    press_client(b, kE);
    CHECK_FALSE(has_part(sent_up(b), "\"t\":\"open\""));

    BeastRig c({}, true);  // the server: player 2 looks at a marked prop 3 m in front of them
    c.r.fake.net_peer_version = 1;
    joined_beasts(c);
    stable_prop(c, 960, 30, 10, 3, nullptr);
    c.look(2, 30, 11.5, 0, 0);
    REQUIRE(c.reply("beastmaster.stable mark", 2, 0xBB).rfind("Marked: ", 0) == 0);
    from_player(c, 2, 0xBB, R"({"t":"open"})");
    REQUIRE(sent_to(c, 2).size() == 1u);
    CHECK(sent_to(c, 2)[0].find("\"open\":true") != std::string::npos);

    BeastRig d({}, true);  // the game's prompt is there: the prompt opens it, the E key does nothing
    d.r.fake.net_peer_version = 1;
    with_offers(d.r.api);
    joined_beasts(d);
    stable_prop(d, 960, 30, 10, 3, nullptr);
    d.look(2, 30, 11.5, 0, 0);
    REQUIRE(d.reply("beastmaster.stable mark", 2, 0xBB).rfind("Marked: ", 0) == 0);
    component<ecs::InteractionOfferRuntime>(d.r.fake, 960);
    from_player(d, 2, 0xBB, R"({"t":"open"})");
    CHECK(sent_to(d, 2).empty());
}

namespace {
// The 1-based place of `label` in a wheel message's "labels" list, or -1.
int label_index(const std::string& msg, const std::string& label) {
    const size_t start = msg.find("\"labels\":[");
    if (start == std::string::npos) return -1;
    const size_t end = msg.find(']', start);
    const std::string list = msg.substr(start + 10, end - start - 10);
    int i = 0;
    for (size_t at = 0; at < list.size();) {
        const size_t q1 = list.find('"', at);
        if (q1 == std::string::npos) break;
        const size_t q2 = list.find('"', q1 + 1);
        ++i;
        if (list.substr(q1 + 1, q2 - q1 - 1) == label) return i;
        at = q2 + 1;
    }
    return -1;
}
std::string pick_json(int seq, int i, const std::string& label) {
    return "{\"t\":\"pick\",\"seq\":" + std::to_string(seq) + ",\"i\":" + std::to_string(i) + ",\"label\":\"" + label + "\"}";
}
}  // namespace

TEST_CASE("beastmaster net: the wheel key on a joined player's game asks the server for the items and opens the wheel with them; a pick runs on the server for that player") {
    BeastRig b({}, true);
    b.r.fake.net_peer_version = 1;
    joined_beasts(b);
    b.spawnable(1);
    press_client(b, VK_F3, /*keep=*/true);
    REQUIRE(sent_up(b).size() == 1u);
    CHECK(sent_up(b)[0].find("\"t\":\"wheel\"") != std::string::npos);
    relay_up(b, 2, 0xBB);
    relay_down(b, 2);
    REQUIRE(b.r.fake.wheels.size() == 1u);
    const auto w = b.r.fake.wheels[0];
    CHECK(w.hold == VK_F3);
    const int grey = index_of(w.items, "Call Grey");
    REQUIRE(grey >= 0);
    CHECK(index_of(w.items, "Call Fang") < 0);  // the host's beast is not on player 2's wheel
    CHECK(index_of(w.items, "Call Tusk") < 0);  // stored: not on the wheel
    w.cb(kClient, grey, w.user);
    g_held[VK_F3] = false;
    relay_up(b, 2, 0xBB);
    REQUIRE(b.r.fake.spawns.size() == 1u);
    CHECK(has_part(sent_to(b, 2), "\"t\":\"note\"", "Grey the wolf comes"));
}

TEST_CASE("beastmaster net: wheel items that arrive after the key was let go, or a second time, open no wheel") {
    BeastRig b({}, true);
    b.r.fake.net_peer_version = 1;
    joined_beasts(b);
    press_client(b, VK_F3);  // let go at once
    relay_up(b, 2, 0xBB);
    relay_down(b, 2);
    CHECK(b.r.fake.wheels.empty());
    press_client(b, VK_F3, /*keep=*/true);
    relay_up(b, 2, 0xBB);
    const auto items = take_sent(b, 2);
    REQUIRE(items.size() == 1u);
    from_server(b, items[0]);
    from_server(b, items[0]);  // the same answer again
    CHECK(b.r.fake.wheels.size() == 1u);
    g_held[VK_F3] = false;
}

TEST_CASE("beastmaster net: a pick for a wheel the server did not send, or with another label, does nothing") {
    BeastRig b({}, true);
    b.r.fake.net_peer_version = 1;
    joined_beasts(b);
    b.spawnable(1);
    from_player(b, 2, 0xBB, R"({"t":"wheel"})");
    const auto items = take_sent(b, 2);
    REQUIRE(items.size() == 1u);
    const int grey = label_index(items[0], "Call Grey (wolf)");
    REQUIRE(grey > 1);
    from_player(b, 2, 0xBB, pick_json(99, grey, "Call Grey (wolf)"));  // another wheel
    from_player(b, 2, 0xBB, pick_json(1, 1, "Call Grey (wolf)"));      // the label is not the one shown there
    from_player(b, 1, 0xAA, pick_json(1, grey, "Call Grey (wolf)"));   // another player
    CHECK(b.r.fake.spawns.empty());
    from_player(b, 2, 0xBB, pick_json(1, grey, "Call Grey (wolf)"));
    CHECK(b.r.fake.spawns.size() == 1u);
    from_player(b, 2, 0xBB, pick_json(1, grey, "Call Grey (wolf)"));  // used up
    CHECK(b.r.fake.spawns.size() == 1u);
}

TEST_CASE("beastmaster net: without Beastmaster on the server the wheel key says to use the chat and sends nothing") {
    BeastRig b({}, true);
    b.r.fake.net_peer_version = 0;
    press_client(b, VK_F3);
    CHECK(b.noted("type /beastmaster in the chat"));
    CHECK(sent_up(b).empty());
}

TEST_CASE("beastmaster net: a joined player's game shows their beast line and counts rests down itself; the server sends it only when it changes") {
    BeastRig b({}, true);
    b.r.fake.net_peer_version = 1;
    write(b.data, "b 0x00000000000000bb wolf 80 0 Grey\n");  // resting 80 s
    b.r.fake.session = 2;
    fire_load(1);
    b.seconds(1);
    REQUIRE(sent_to(b, 2).size() == 1u);
    CHECK(sent_to(b, 2)[0].find("\"t\":\"line\"") != std::string::npos);
    CHECK(sent_to(b, 2)[0].find("\"state\":\"rest\"") != std::string::npos);
    relay_down(b, 2);  // the client keeps rest 80 from now
    b.seconds(5);
    CHECK(sent_to(b, 2).empty());  // the countdown alone sends nothing
    client_steps(b, 181 + 60);     // 300 + 241 ticks since it came: 80 - 9.02 s
    CHECK(b.hud("out") == "Beasts (defending): Grey (rests 1:10)");
    client_steps(b, 10 * 60);  // 80 - 19.02 s
    CHECK(b.hud("out") == "Beasts (defending): Grey (rests 1:00)");
}

TEST_CASE("beastmaster net: the line goes away on a joined player's game when the server no longer answers") {
    BeastRig b({}, true);
    b.r.fake.net_peer_version = 1;
    write(b.data, "b 0x00000000000000bb wolf 80 0 Grey\n");
    b.r.fake.session = 2;
    fire_load(1);
    b.seconds(1);
    relay_down(b, 2);
    client_steps(b, 181 + 60);
    REQUIRE(b.hud("out").find("Grey (rests") != std::string::npos);
    b.r.fake.net_peer_version = 0;  // another server, or it left
    client_steps(b, 60);
    CHECK(b.hud("out") == "");
}

TEST_CASE("beastmaster: beastmaster.as runs a player's word as that player from the console; from the chat it is refused") {
    BeastRig b;
    CHECK(b.reply("beastmaster.as #2 give wolf") == "added Wolf (wolf) to your collection");
    CHECK(b.reply("beastmaster.list", 2, 0xBB).find("1. Wolf - wolf") != std::string::npos);
    CHECK(b.reply("beastmaster.list").find("no beasts") != std::string::npos);  // not the host's
    CHECK(b.reply("beastmaster.as #2 list").find("1. Wolf - wolf") != std::string::npos);
    CHECK(b.reply("beastmaster.as 2 list").find("1. Wolf - wolf") != std::string::npos);
    CHECK(b.reply("beastmaster.list").find("no beasts") != std::string::npos);  // the console is the host's again
    CHECK(b.reply("beastmaster.as #3 list") == "No player #3 in the world.");
    CHECK(b.reply("beastmaster.as 77 list") == "No player with entity 77.");
    CHECK(b.reply("beastmaster.as #2 give wolf", 2, 0xBB) == "beastmaster.as is for the host's or the server's console");
    CHECK(b.reply("beastmaster.list", 2, 0xBB).find("2. ") == std::string::npos);
}

TEST_CASE("beastmaster net: beastmaster.as #k open opens a joined player's Beasts window on their game") {
    BeastRig b({}, true);
    b.r.fake.net_peer_version = 1;
    joined_beasts(b);
    CHECK(b.reply("beastmaster.as #2 open Grey") == "Opened the Beasts window on that player's game.");
    REQUIRE(sent_to(b, 2).size() == 1u);  // opened on Grey's tab (one message)
    CHECK(has_part(sent_to(b, 2), "\"open\":true", "Talents: "));
    b.r.fake.net_peer_version = 0;
    CHECK(b.reply("beastmaster.as #2 open").rfind("That player's game has no Beastmaster window", 0) == 0);
}

// ---- after the final review ----
namespace {
// The reported stables kept with the world ("r" lines in the saved data).
size_t r_lines(const std::string& saved) {
    size_t n = saved.rfind("r ", 0) == 0 ? 1 : 0;
    for (size_t at = saved.find("\nr "); at != std::string::npos; at = saved.find("\nr ", at + 1)) ++n;
    return n;
}
size_t log_count(BeastRig& b, const std::string& part) {
    size_t n = 0;
    for (const auto& l : b.r.fake.log) n += l.find(part) != std::string::npos;
    return n;
}
// A stables report of the beds at `points` (x, z; y 10).
std::string report_points(const std::vector<std::pair<double, double>>& points) {
    std::string list;
    for (const auto& [x, z] : points) {
        char e[160];
        std::snprintf(e, sizeof e, R"({"m":"%s","x":%.2f,"y":10.0,"z":%.2f})", kStableModel, x, z);
        list += (list.empty() ? "" : ",") + std::string(e);
    }
    return R"({"t":"stables","list":[)" + list + "]}";
}
}  // namespace

TEST_CASE("beastmaster net: a player's game cannot grow the reported stables without end: at most 64 are kept") {
    BeastRig b({}, true);
    flame_altar(b, 900, 100, 0);  // a base 70 m from player 2 (at 30, 10, 0): nothing there is within their 40 m
    b.seconds(31);
    for (int r = 0; r < 10; ++r) {  // 10 reports of 32 new beds each
        std::vector<std::pair<double, double>> points;
        for (int i = 0; i < 32; ++i) points.push_back({90.0 + 2.0 * (i % 8), -40.0 + 2.0 * (r * 4 + i / 8)});
        from_player(b, 2, 0xBB, report_points(points));
        b.seconds(3);
    }
    CHECK(r_lines(b.saved()) == 64u);
    CHECK(log_count(b, "64") >= 1u);
}

TEST_CASE("beastmaster net: a stable reported far from the player whose game reports it is not taken") {
    BeastRig b({}, true);
    flame_altar(b, 900, 200, 0);
    b.seconds(31);
    from_player(b, 2, 0xBB, report_of(kStableModel, 210, 10, 0));  // 180 m from player 2 at (30, 10, 0)
    b.seconds(1);
    CHECK(r_lines(b.saved()) == 0u);
    place(b.r.fake, 2, 200, 10, 0);
    b.seconds(3);
    from_player(b, 2, 0xBB, report_of(kStableModel, 210, 10, 0));
    b.seconds(1);
    CHECK(r_lines(b.saved()) == 1u);
}

TEST_CASE("beastmaster net: a player's stables reports count at most one every 2 s") {
    BeastRig b({}, true);
    flame_altar(b, 900, 0, 0);
    b.seconds(31);
    from_player(b, 2, 0xBB, report_of(kStableModel, 10, 10, 0));
    from_player(b, 2, 0xBB, report_points({{10, 0}, {14, 0}}));  // at once: not taken
    b.seconds(1);
    CHECK(r_lines(b.saved()) == 1u);
    b.seconds(2);
    from_player(b, 2, 0xBB, report_points({{10, 0}, {14, 0}}));
    b.seconds(1);
    CHECK(r_lines(b.saved()) == 2u);
}

TEST_CASE("beastmaster net: a full report (its 32 nearest) removes no stable beyond the farthest one it lists") {
    BeastRig b({}, true);
    flame_altar(b, 900, 5, 4);
    b.seconds(31);
    std::vector<std::pair<double, double>> beds;  // 40 beds, 2 m apart
    for (int i = 0; i < 40; ++i) beds.push_back({2.0 * (i % 8), 2.0 * (i / 8)});
    auto nearest = [&](double px, double pz) {
        auto v = beds;
        std::sort(v.begin(), v.end(), [&](const auto& a, const auto& c) {
            return std::hypot(a.first - px, a.second - pz) < std::hypot(c.first - px, c.second - pz);
        });
        v.resize(32);
        return v;
    };
    place(b.r.fake, 2, -10, 10, 4);
    from_player(b, 2, 0xBB, report_points(nearest(-10, 4)));
    b.seconds(3);
    place(b.r.fake, 2, 24, 10, 4);
    from_player(b, 2, 0xBB, report_points(nearest(24, 4)));
    b.seconds(3);
    REQUIRE(r_lines(b.saved()) == 40u);
    place(b.r.fake, 2, -10, 10, 4);
    from_player(b, 2, 0xBB, report_points(nearest(-10, 4)));  // the same 32 again: the other 8 are farther
    b.seconds(1);
    CHECK(r_lines(b.saved()) == 40u);
}

TEST_CASE("beastmaster net: unknown kinds of messages write one log line however many come") {
    BeastRig b({}, true);
    b.r.fake.net_peer_version = 1;
    for (int i = 0; i < 50; ++i) from_player(b, 2, 0xBB, "{\"t\":\"kind" + std::to_string(i) + "\"}");
    CHECK(log_count(b, "an unknown kind") == 1u);
}

TEST_CASE("beastmaster net: after a session ends and the clock goes back (a restarted server), the game searches and reports again") {
    BeastRig b({}, true);
    b.r.fake.net_peer_version = 1;
    stable_prop(b, 950, 10, 10, 0);
    client_steps(b, 181 + 35 * 60);
    REQUIRE(sent_up(b).size() == 1u);
    b.r.fake.net_peer_version = 0;  // the session ends ...
    client_steps(b, 60);
    b.r.fake.tick -= 2000;          // ... and the next server's clock is behind
    b.r.fake.net_peer_version = 1;
    client_steps(b, 36 * 60);
    CHECK(sent_up(b).size() == 2u);
}

TEST_CASE("beastmaster net: after a jump of 100 m or more (fast travel) the game waits 30 s again before it reports") {
    BeastRig b({}, true);
    b.r.fake.net_peer_version = 1;
    place(b.r.fake, 1, 500, 10, 3);
    stable_prop(b, 950, 500, 10, 0);  // a bed in another base
    client_steps(b, 181 + 35 * 60);
    REQUIRE(sent_up(b).size() == 1u);
    place(b.r.fake, 1, 0, 10, 0);     // fast travel home: that base streamed out, home not streamed in yet
    remove_entity(b.r.fake, 950);
    client_steps(b, 15 * 60);
    CHECK(sent_up(b).size() == 1u);   // no report of "nothing here" while home streams in
    stable_prop(b, 951, 10, 10, 0);   // home's bed shows up
    client_steps(b, 25 * 60);
    REQUIRE(sent_up(b).size() == 2u);
    CHECK(sent_up(b)[1].find("\"x\":10.0") != std::string::npos);
}

// ---- the 0.6.2 review's smaller findings ----
namespace {
// The seconds of the first "rests M:SS" (or a beast tab's "Rests M:SS") a window shows, or -1.
int rest_shown(const Fake::Win& w) {
    for (const auto& t : w.texts) {
        const size_t at = t.find("ests ");
        if (at == std::string::npos) continue;
        int m = 0, s = 0;
        if (std::sscanf(t.c_str() + at, "ests %d:%d", &m, &s) == 2) return m * 60 + s;
    }
    return -1;
}
// Only the "win" messages of `messages`.
std::vector<std::string> wins(const std::vector<std::string>& messages) {
    std::vector<std::string> out;
    for (const auto& m : messages)
        if (m.find("\"t\":\"win\"") != std::string::npos) out.push_back(m);
    return out;
}
// Player 2 (0xBB) with Grey resting 80 s presses E at the stable: the window's message is in sent_to(b, 2).
void joined_resting_at_stable(BeastRig& b) {
    b.r.fake.net_peer_version = 1;
    with_offers(b.r.api);
    write(b.data, "b 0x00000000000000aa wolf 0 0 Fang\nb 0x00000000000000bb wolf 80 0 Grey\n"
                  "b 0x00000000000000bb boar 0 0 Tusk\na 0x00000000000000aa Fang\na 0x00000000000000bb Grey\ns 1\n");
    b.r.fake.session = 2;
    fire_load(1);
    b.seconds(1);
    at_stable(b);
    Fake::Off o = *last_offer(b, 960);
    o.cb(kServer, 2, 960, o.user);
}
// The server sends window `n` to this game, to open it.
void server_opens(BeastRig& b, int n) {
    from_server(b, "{\"t\":\"win\",\"n\":" + std::to_string(n) + ",\"open\":true,\"title\":\"Beasts\",\"rows\":[{\"text\":\"x\"}]}");
}
}  // namespace

TEST_CASE("beastmaster net: once a window was closed on a joined player's game, the server opens another there only within 5 s after a press of E or the stable key") {
    BeastRig b({}, true);
    joined_at_stable(b);
    relay_down(b, 2);
    REQUIRE(b.r.fake.windows.size() == 1u);  // the first opens (beastmaster.as open works for a first look)
    client_click(b, "", EMF_UI_CLOSED);
    take_sent(b, 0);
    for (int n = 7; n < 10; ++n) server_opens(b, n);  // again and again, unasked
    CHECK(b.r.fake.windows.size() == 1u);
    CHECK(has_part(sent_up(b), "\"t\":\"closed\"", "\"n\":9"));  // the server hears it is closed
    take_sent(b, 0);
    press_client(b, kE);
    server_opens(b, 10);
    REQUIRE(b.r.fake.windows.size() == 2u);  // asked for: shown
    client_click(b, "", EMF_UI_CLOSED);
    server_opens(b, 11);
    CHECK(b.r.fake.windows.size() == 2u);  // one press, one window
    press_client(b, kE);
    b.r.fake.tick += 6 * 60;
    server_opens(b, 12);
    CHECK(b.r.fake.windows.size() == 2u);  // too late after the press

    BeastRig c({{"stable_key", "R"}}, true);  // another stable key: E (the game's prompt) still counts, and so does R
    joined_at_stable(c);
    relay_down(c, 2);
    client_click(c, "", EMF_UI_CLOSED);
    press_client(c, kE);
    server_opens(c, 7);
    REQUIRE(c.r.fake.windows.size() == 2u);
    client_click(c, "", EMF_UI_CLOSED);
    press_client(c, 'R');
    server_opens(c, 8);
    CHECK(c.r.fake.windows.size() == 3u);
}

TEST_CASE("beastmaster net: a window whose update did not go is closed on the joined player's game") {
    BeastRig b({}, true);
    joined_at_stable(b);
    relay_down(b, 2);
    b.r.fake.net_ok = false;  // the core takes no message for a while
    REQUIRE(b.reply("beastmaster.rename Grey Ash", 2, 0xBB).find("now called Ash") != std::string::npos);
    b.seconds(3);
    take_sent(b, 2);
    b.r.fake.net_ok = true;
    b.seconds(1);
    REQUIRE(has_part(sent_to(b, 2), "\"t\":\"close\"", "\"n\":1"));
    relay_down(b, 2);
    CHECK(b.r.fake.windows_closed == std::vector<std::string>{"beastmaster.stable"});
    b.seconds(3);
    CHECK(sent_to(b, 2).empty());  // closed once
}

TEST_CASE("beastmaster net: a window the joined player's game cannot draw again is closed there and the server hears it") {
    BeastRig b({}, true);
    joined_at_stable(b);
    relay_down(b, 2);
    take_sent(b, 0);
    b.r.fake.window_update_ok = false;
    from_server(b, R"({"t":"win","n":1,"title":"Beasts","rows":[{"text":"x"}]})");
    CHECK(b.r.fake.windows_closed == std::vector<std::string>{"beastmaster.stable"});
    CHECK(has_part(sent_up(b), "\"t\":\"closed\"", "\"n\":1"));
}

TEST_CASE("beastmaster net: a late close of an earlier window leaves the joined player's window open now alone") {
    BeastRig b({}, true);
    joined_at_stable(b);
    relay_down(b, 2);
    const Fake::Win first = b.r.fake.windows.back();
    from_server(b, R"({"t":"close","n":1})");
    press_client(b, kE);
    server_opens(b, 2);
    REQUIRE(b.r.fake.windows.size() == 2u);
    EmfUiEvent e{};  // the core's "closed" for the first window comes only now
    e.size = sizeof e;
    e.kind = EMF_UI_CLOSED;
    e.window = first.id.c_str();
    e.key = "";
    e.from_key = "";
    e.reason = EMF_UI_BY_MOD;
    take_sent(b, 0);
    first.cb(kClient, &e, first.user);
    CHECK_FALSE(has_part(sent_up(b), "\"t\":\"closed\"", "\"n\":2"));
    client_click(b, "beast.Tusk");
    CHECK(has_part(sent_up(b), "\"t\":\"click\"", "\"n\":2"));
}

TEST_CASE("beastmaster net: a joined player's window counts its rests down itself; the server does not send it again for that") {
    BeastRig b({}, true);
    joined_resting_at_stable(b);
    REQUIRE(wins(sent_to(b, 2)).size() == 1u);
    relay_down(b, 2);
    REQUIRE(b.r.fake.windows.size() == 1u);
    const int first = rest_shown(b.r.fake.windows.back());
    REQUIRE(first > 60);
    b.seconds(5);
    CHECK(wins(sent_to(b, 2)).empty());  // the countdown alone sends nothing
    client_steps(b, 181 + 60);           // 300 + 241 ticks since it came: 9 s
    REQUIRE(!b.r.fake.window_updates.empty());
    CHECK(first - rest_shown(b.r.fake.window_updates.back()) == 9);
    b.seconds(11);  // a game that does not count them (Beastmaster 1.15.0) gets them every 15 s
    CHECK(wins(sent_to(b, 2)).size() == 1u);
}

TEST_CASE("beastmaster net: on a resting beast's own tab the joined player's game counts the rest down too") {
    BeastRig b({}, true);
    joined_resting_at_stable(b);
    relay_down(b, 2);
    client_click(b, "beast.Grey");
    relay_up(b, 2, 0xBB);
    REQUIRE(wins(sent_to(b, 2)).size() == 1u);
    relay_down(b, 2);
    const int first = rest_shown(b.r.fake.window_updates.back());
    REQUIRE(first > 60);
    b.seconds(5);
    CHECK(wins(sent_to(b, 2)).empty());
    client_steps(b, 181 + 60);
    CHECK(first - rest_shown(b.r.fake.window_updates.back()) == 9);
}

TEST_CASE("beastmaster net: a countdown with absurd numbers from the server is left as it is") {
    BeastRig b({}, true);
    b.r.fake.net_peer_version = 1;
    joined_beasts(b);
    from_server(b, R"({"t":"win","n":1,"open":true,"title":"Beasts","rows":[{"text":"Grey rests 99999999999999999999:00"}]})");
    REQUIRE(b.r.fake.windows.size() == 1u);
    client_steps(b, 181 + 120);
    CHECK(b.host->status("beastmaster") == "running");
}

TEST_CASE("beastmaster net: at a stable this game shows, a window the server opens shows without a key press (a controller, another interact key), but not within 3 s of a close") {
    BeastRig b({}, true);
    b.r.fake.net_peer_version = 1;
    stable_prop(b, 950, 0, 10, 40);  // 40 m from this game's player (1, at the origin)
    client_steps(b, 181 + 6 * 60);   // this game has searched
    server_opens(b, 1);              // the free one
    REQUIRE(b.r.fake.windows.size() == 1u);
    client_click(b, "", EMF_UI_CLOSED);
    CHECK_FALSE(b.noted("open it at a stable"));
    server_opens(b, 2);  // far from the stable, no key: not
    server_opens(b, 3);
    CHECK(b.r.fake.windows.size() == 1u);
    size_t notes = 0;
    for (const auto& n : b.r.fake.notes) notes += n.find("open it at a stable") != std::string::npos;
    CHECK(notes == 1u);  // said once
    place(b.r.fake, 950, 0, 10, 3);  // the player walks up to it: 3 m
    client_steps(b, 10 * 60);
    server_opens(b, 4);
    REQUIRE(b.r.fake.windows.size() == 2u);  // at the stable: shown
    client_click(b, "", EMF_UI_CLOSED);
    server_opens(b, 5);  // at once after the close: not
    CHECK(b.r.fake.windows.size() == 2u);
    client_steps(b, 4 * 60);
    server_opens(b, 6);
    CHECK(b.r.fake.windows.size() == 3u);
}

TEST_CASE("beastmaster net: a second press of the wheel key while the server's answer is on its way asks once; the pick from that wheel runs") {
    BeastRig b({}, true);
    b.r.fake.net_peer_version = 1;
    joined_beasts(b);
    b.spawnable(1);
    press_client(b, VK_F3);                 // a tap ...
    press_client(b, VK_F3, /*keep=*/true);  // ... and at once again, held
    REQUIRE(sent_up(b).size() == 1u);
    relay_up(b, 2, 0xBB);
    relay_down(b, 2);
    REQUIRE(b.r.fake.wheels.size() == 1u);
    const auto w = b.r.fake.wheels[0];
    const int grey = index_of(w.items, "Call Grey");
    REQUIRE(grey >= 0);
    w.cb(kClient, grey, w.user);
    g_held[VK_F3] = false;
    relay_up(b, 2, 0xBB);
    CHECK(b.r.fake.spawns.size() == 1u);
}

TEST_CASE("beastmaster net: on the host, the first 30 s in which its own game removes no reported stable start when the host is in the world") {
    BeastRig b({}, true);
    flame_altar(b, 900, 0, 0);
    auto step_both = [&](int secs) {
        for (int i = 0; i < secs * 60; ++i) {
            b.host->tick(kClient);
            b.r.fake.tick += 1;
            b.host->tick(kServer);
        }
    };
    remove_entity(b.r.fake, 1);  // the host's player is not in its world yet: its loading screen
    step_both(31);
    from_player(b, 2, 0xBB, report_of(kStableModel, 10, 10, 0));
    REQUIRE(r_lines(b.saved()) == 1u);
    step_both(10);
    add_player(b.r.fake, 1, 0xAA, 0, 10, 0);  // the host is in; its game shows no bed there yet (still streaming in)
    add_entity(b.r.fake, 1);
    step_both(5);
    CHECK(r_lines(b.saved()) == 1u);
    CHECK(listed(b, "seen by a player's game"));
}

TEST_CASE("beastmaster net: messages of a known kind with bad fields write one log line per kind") {
    BeastRig b({}, true);
    b.r.fake.net_peer_version = 1;
    joined_beasts(b);
    for (int i = 0; i < 5; ++i) {
        from_player(b, 2, 0xBB, R"({"t":"click","n":"one","key":5})");
        from_player(b, 2, 0xBB, R"({"t":"pick","seq":1,"i":"x"})");
        from_player(b, 2, 0xBB, R"({"t":"stables","list":7})");
        from_server(b, R"({"t":"line","head":7})");
    }
    CHECK(log_count(b, "bad fields in a click") == 1u);
    CHECK(log_count(b, "bad fields in a pick") == 1u);
    CHECK(log_count(b, "bad fields in a stables") == 1u);
    CHECK(log_count(b, "bad fields in a line") == 1u);
}

TEST_CASE("beastmaster net: a joined player's game does not search for stables while the server's Beastmaster does not answer") {
    BeastRig b({}, true);
    b.r.fake.net_peer_version = 0;  // no Beastmaster 1.15 on the server
    stable_prop(b, 950, 10, 10, 0);
    client_steps(b, 181);
    const size_t walks = b.r.fake.for_each_calls;
    client_steps(b, 40 * 60);
    CHECK(b.r.fake.for_each_calls == walks);
    b.r.fake.net_peer_version = 1;  // it answers now: the game searches at once
    client_steps(b, 60);
    CHECK(b.r.fake.for_each_calls > walks);
}

TEST_CASE("beastmaster net: a stables report that comes before the world's data is taken once the data is here") {
    BeastRig b({}, true, /*load=*/false);
    b.r.fake.net_peer_version = 1;
    flame_altar(b, 900, 0, 0);
    b.seconds(31);
    from_player(b, 2, 0xBB, report_of(kStableModel, 10, 10, 0));
    fire_load(1);
    b.seconds(35);
    CHECK(listed(b, "seen by a player's game"));
}

// ---- 1.16: beast and stable markers (EMF's markers, lua 1.25) ----
namespace {
const Fake::MarkerCopy* sent_to(const Fake& f, uint32_t player, const std::string& id) {
    const Fake::MarkerCopy* last = nullptr;
    for (const auto& [p, m] : f.markers_sent)
        if (p == player && m.id == id) last = &m;
    return last;
}
bool unsent_to(const Fake& f, uint32_t player, const std::string& id) {
    for (const auto& [p, i] : f.markers_unsent)
        if (p == player && i == id) return true;
    return false;
}
}  // namespace

TEST_CASE("beastmaster 1.16: markers are off by default: a called wolf sends no marker") {
    BeastRig b;
    b.reply("beastmaster.give wolf");
    b.spawnable(1);
    REQUIRE(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    b.seconds(3);
    CHECK(b.r.fake.markers_sent.empty());
}

TEST_CASE("beastmaster 1.16: beast markers on: the owner gets a marker over each beast out, with its name; a dismissed one goes") {
    BeastRig b({{"beast_markers", "1"}});
    b.reply("beastmaster.give wolf");
    b.spawnable(1);
    REQUIRE(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    b.seconds(2);
    const auto* m = sent_to(b.r.fake, 1, "beastmaster:beast.1234");
    REQUIRE(m != nullptr);
    CHECK(m->text == "Wolf");
    CHECK(m->m.entity == 1234u);
    CHECK(m->m.height == doctest::Approx(1.7f));
    CHECK(m->m.flags ==
          (EMF_MARKER_WORLD | EMF_MARKER_DISTANCE | EMF_MARKER_GAME_MAP | EMF_MARKER_EDGE_ARROW | EMF_MARKER_BAR));
    CHECK(sent_to(b.r.fake, 2, "beastmaster:beast.1234") == nullptr);  // only its owner
    const size_t n = b.r.fake.markers_sent.size();
    b.seconds(3);
    CHECK(b.r.fake.markers_sent.size() == n);  // not sent again while nothing changed
    b.reply("beastmaster.dismiss Wolf");
    b.seconds(2);
    CHECK(unsent_to(b.r.fake, 1, "beastmaster:beast.1234"));
}

TEST_CASE("beastmaster 1.19: a beast's marker has a health bar in 5 % steps, sent again when it moves a step") {
    BeastRig b({{"beast_markers", "1"}});
    b.reply("beastmaster.give wolf");
    b.spawnable(1);
    REQUIRE(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    b.seconds(2);
    const auto* m = sent_to(b.r.fake, 1, "beastmaster:beast.1234");
    REQUIRE(m != nullptr);
    REQUIRE((m->m.flags & EMF_MARKER_BAR) != 0u);
    CHECK(m->m.bar == doctest::Approx(1.0f));
    const int32_t max = b.r.fake.attrs[{1234u, EMF_ATTR_MAX_HEALTH}];
    REQUIRE(max > 0);
    b.r.fake.attrs[{1234u, EMF_ATTR_HEALTH}] = max * 42 / 100;
    b.seconds(1);
    m = sent_to(b.r.fake, 1, "beastmaster:beast.1234");
    REQUIRE(m != nullptr);
    CHECK(m->m.bar < 0.6f);  // 42 % (it may heal a step meanwhile)
    CHECK(m->m.bar >= 0.4f);
    const float steps = m->m.bar / 0.05f;
    CHECK(steps == doctest::Approx(std::round(steps)));
}

TEST_CASE("beastmaster 1.19 (review): on a core older than v47 the beast's marker goes without the bar, and keeps going") {
    BeastRig b({{"beast_markers", "1"}});
    b.r.api.size = static_cast<uint32_t>(offsetof(EmfApi, find_wall_ex));
    b.r.api.version = 46;
    b.reply("beastmaster.give wolf");
    b.spawnable(1);
    REQUIRE(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    b.seconds(2);
    const auto* m = sent_to(b.r.fake, 1, "beastmaster:beast.1234");
    REQUIRE(m != nullptr);
    CHECK((m->m.flags & EMF_MARKER_BAR) == 0u);
    CHECK(m->text == "Wolf");
}

// A beast with no position (not placed yet, or out of the world) was sent at (0, 0, 0): a marker at the world's origin.
TEST_CASE("beastmaster: a beast with no position gets no marker; it gets one where it is once it has a place") {
    BeastRig b;
    wolf_out(b);  // markers off (the default)
    b.seconds(1);
    b.r.fake.components.erase({ecs::CurrentTransform::kHash, 1234u});  // no place now
    REQUIRE(b.reply("beastmaster markers on").find("on") != std::string::npos);
    b.host->tick(kServer);  // the same second: the markers' step runs at once, the beasts' check is not due yet
    CHECK(sent_to(b.r.fake, 1, "beastmaster:beast.1234") == nullptr);
    place(b.r.fake, 1234, 3, 10, 1);
    b.seconds(1);
    const auto* m = sent_to(b.r.fake, 1, "beastmaster:beast.1234");
    REQUIRE(m != nullptr);
    CHECK(m->m.x == doctest::Approx(3));
    CHECK(m->m.z == doctest::Approx(1));
}

TEST_CASE("beastmaster 1.16: /beastmaster markers on|off is the player's own choice over the setting") {
    BeastRig b;
    b.reply("beastmaster.give wolf");
    b.spawnable(1);
    b.reply("beastmaster.call Wolf");
    b.seconds(2);
    CHECK(b.r.fake.markers_sent.empty());
    CHECK(b.reply("beastmaster markers on").find("on") != std::string::npos);
    b.seconds(2);
    CHECK(sent_to(b.r.fake, 1, "beastmaster:beast.1234") != nullptr);
    CHECK(b.reply("beastmaster markers off").find("off") != std::string::npos);
    b.seconds(2);
    CHECK(unsent_to(b.r.fake, 1, "beastmaster:beast.1234"));
    CHECK(b.reply("beastmaster markers maybe").find("on|off") != std::string::npos);
}

TEST_CASE("beastmaster 1.16: stable markers on: the world's stables, square, on the map and in the world within 200 m") {
    BeastRig b({{"stable_markers", "1"}});
    flame_altar(b, 900, 0, 0);
    stable_prop(b, 950, 5, 10, 0);
    b.seconds(33);
    const Fake::MarkerCopy* m = nullptr;
    for (const auto& [p, c] : b.r.fake.markers_sent)
        if (p == 1 && c.id.rfind("beastmaster:stable.", 0) == 0) m = &c;
    REQUIRE(m != nullptr);
    CHECK(m->m.shape == EMF_MARKER_SQUARE);
    CHECK(m->m.x == doctest::Approx(5));
    CHECK(m->m.max_distance == doctest::Approx(200.0f));
    CHECK((m->m.flags & EMF_MARKER_GAME_MAP) != 0u);
    CHECK(m->text == "Animal bed");
    bool to_guest = false;
    for (const auto& [p, c] : b.r.fake.markers_sent) to_guest |= p == 2 && c.id == m->id;
    CHECK(to_guest);  // the world's stables are everyone's
}

TEST_CASE("beastmaster: beastmaster.spawn puts a wild beast of a species in front of you, to try taming") {
    BeastRig b;
    const std::string r = b.reply("beastmaster.spawn wolf");
    CHECK(r.find("a wild Wolf") != std::string::npos);
    REQUIRE(b.r.fake.spawns.size() == 1);
    CHECK(guid_text(b.r.fake.spawns[0].guid) == kWolfTemplate);
    const double d = std::sqrt(b.r.fake.spawns[0].x * b.r.fake.spawns[0].x + b.r.fake.spawns[0].z * b.r.fake.spawns[0].z);
    CHECK(d == doctest::Approx(5.0).epsilon(0.05));  // 5 m out (the player stands at the origin)
    CHECK(b.reply("beastmaster.spawn dragon").find("no species dragon") != std::string::npos);
    CHECK(b.r.fake.spawns.size() == 1);
}

TEST_CASE("beastmaster: a beast being tamed stops fighting you, and a tamed one loses its debuffs (a bleed no longer kills it)") {
    BeastRig b;
    b.look(1, 0, 10.5, 0, 0);
    b.beast(50, kWolfGraph, 0, 5, kWildBeast, 20, 100);
    fighting(b, 50, 1);  // it fights you
    EmfBuff bleed{};
    bleed.size = sizeof bleed;
    bleed.type = 0x777;
    bleed.entity = 900;
    bleed.slot = EMF_BUFF_DEBUFF;
    EmfBuff food{};
    food.size = sizeof food;
    food.type = 0x111;
    food.entity = 901;
    food.slot = EMF_BUFF_FOOD;
    b.r.fake.buffs[50] = {bleed, food};
    CHECK(b.reply("beastmaster.tame").find("Taming") != std::string::npos);
    CHECK_FALSE(still_fighting(b, 50));  // calm from the channel's start
    fighting(b, 50, 1);                  // its aggro list still had you
    b.seconds(4);
    CHECK(b.noted("You tamed a wolf: Wolf"));
    CHECK_FALSE(still_fighting(b, 50));  // calm at the tame, and through its protection
    bool bleed_removed = false, food_removed = false;
    for (const auto& a : b.r.fake.buff_asks) {
        bleed_removed |= a.remove && a.target == 50 && a.type == 0x777;
        food_removed |= a.remove && a.target == 50 && a.type == 0x111;
    }
    CHECK(bleed_removed);
    CHECK_FALSE(food_removed);  // only debuffs
}

TEST_CASE("beastmaster: a beast teleported back from far away drops its chase and guards its owner again (it follows)") {
    BeastRig b;
    wolf_out(b);
    b.seconds(1);
    const size_t guards_before = b.guards().size();
    fighting(b, 1234, 777);              // it chased a hare far away
    place(b.r.fake, 1234, 130, 10, 0);   // too far at once: it is brought back
    b.seconds(1);
    REQUIRE(teleports_of(b.r.fake, 1234) >= 1);
    CHECK_FALSE(still_fighting(b, 1234));  // the chase is dropped
    const auto g = b.guards();
    REQUIRE(g.size() > guards_before);
    CHECK(g.back() == std::make_pair(1234u, 1u));  // guarding its owner again
    fighting(b, 1234, 777);              // the game still had the hare
    b.seconds(2);
    CHECK_FALSE(still_fighting(b, 1234));  // kept calm for a moment after the move
}

// Seen live (enemy01 state read every second): a tamed wolf, calmed, stayed in Combat (6) with no target and no aggro
// and stood 15 m from its owner; set to Idle (2) it came to them at once.
TEST_CASE("beastmaster: a calmed beast left in the game's Combat state goes back to Idle (so it follows); one that follows is left alone") {
    using S = emf::gen::enemy01::State;
    BeastRig b;
    b.look(1, 0, 10.5, 0, 0);
    b.beast(50, kWolfGraph, 0, 5, kWildBeast, 20, 100);
    fighting(b, 50, 1);
    component<ecs::DynamicEnemy01BehaviorState>(b.r.fake, 50).state = S::Combat;
    REQUIRE(b.reply("beastmaster.tame").find("Taming") != std::string::npos);
    b.seconds(4);
    REQUIRE(b.noted("You tamed a wolf: Wolf"));
    CHECK(now_of<ecs::DynamicEnemy01BehaviorState>(b, 50).state == S::Idle);
    live_of<ecs::DynamicEnemy01BehaviorState>(b, 50).state = S::Guard;  // it follows its owner
    b.seconds(1);  // still in its protection: calmed every step
    CHECK(now_of<ecs::DynamicEnemy01BehaviorState>(b, 50).state == S::Guard);

    BeastRig chase;  // brought back from a far chase, in Combat
    wolf_out(chase);
    chase.seconds(1);
    fighting(chase, 1234, 777);
    component<ecs::DynamicEnemy01BehaviorState>(chase.r.fake, 1234).state = S::Combat;
    place(chase.r.fake, 1234, 130, 10, 0);
    chase.seconds(1);
    REQUIRE(teleports_of(chase.r.fake, 1234) >= 1);
    CHECK(now_of<ecs::DynamicEnemy01BehaviorState>(chase, 1234).state == S::Idle);
}

// Seen live: a wild wolf bit a just-tamed wolf through its protection, which kept it from fighting anything; it fell
// 8 s after the tame.
TEST_CASE("beastmaster: through its protection a tamed beast stops fighting players only; it may fight a monster") {
    using S = emf::gen::enemy01::State;
    BeastRig b;
    b.look(1, 0, 10.5, 0, 0);
    b.beast(50, kWolfGraph, 0, 5, kWildBeast, 20, 100);
    component<ecs::Health>(b.r.fake, 50);  // its protection holds the game's Health minimum
    REQUIRE(b.reply("beastmaster.tame").find("Taming") != std::string::npos);
    b.seconds(4);
    REQUIRE(b.noted("You tamed a wolf: Wolf"));
    creature(b, 70, 0, 8, kWildBeast);  // a wild wolf bites it
    fighting(b, 50, 70);
    component<ecs::DynamicEnemy01BehaviorState>(b.r.fake, 50).state = S::Combat;
    b.seconds(1);  // still in its protection
    CHECK(still_fighting(b, 50));
    CHECK(now_of<ecs::DynamicEnemy01BehaviorState>(b, 50).state == S::Combat);
    fighting(b, 50, 1);  // the game still had its tamer
    b.seconds(1);
    CHECK_FALSE(still_fighting(b, 50));
    CHECK(now_of<ecs::DynamicEnemy01BehaviorState>(b, 50).state == S::Idle);
}

// ---- training (1.21): beasts sent to a Training Grounds (the beasts pack's) earn loyalty by the game's clock ----
namespace {
constexpr const char* kTrainingModel = "bb3549a0-5088-4c3b-8057-3a7e48a59ad6";
// A Training Grounds `id` at (x, 10, z) in the base of the flame altar 900 at (0, 0): found by the next search.
void training_grounds(BeastRig& b, uint32_t id, double x, double z) {
    flame_altar(b, 900, 0, 0);
    stable_prop(b, id, x, 10, z, kTrainingModel);
}
bool says(const std::string& reply, const std::string& part) { return reply.find(part) != std::string::npos; }
}  // namespace

TEST_CASE("training: a beast goes to the nearest Training Grounds with room; one more than its room is refused; untrain brings it back") {
    BeastRig b;
    three_beasts(b);
    CHECK(says(b.reply("beastmaster.train Fang"), "No Training Grounds in this world yet"));
    training_grounds(b, 960, 10, 0);
    b.seconds(32);
    CHECK(says(b.reply("beastmaster.train Fang"), "Fang goes to the Training Grounds"));
    CHECK(says(b.reply("beastmaster.train Fang"), "training already"));
    CHECK(says(b.reply("beastmaster.train Tusk"), "Tusk goes to the Training Grounds"));
    CHECK(says(b.reply("beastmaster.train Rex"), "full (2 of 2)"));  // training_room: 2
    CHECK(says(b.reply("beastmaster.train"), "Fang trains at the Training Grounds at 10, 10, 0 (loyalty 1, 0 % of a point)"));
    CHECK(says(b.reply("beastmaster.untrain Fang"), "Fang is back in the stable"));
    CHECK(says(b.reply("beastmaster.untrain Fang"), "Fang is not training"));
    CHECK(says(b.reply("beastmaster.train Rex"), "Rex goes to the Training Grounds"));
    CHECK(says(b.saved(), "g 0x00000000000000aa 0.0000 10.0,10.0,0.0 Rex"));
    CHECK(says(b.saved(), "q 10.0 10.0 0.0 "));
    CHECK_FALSE(says(b.saved(), "10.0,10.0,0.0 Fang"));  // brought back: no g line
}

TEST_CASE("training: a beast that is out is not sent; calling a beast in training ends its training") {
    BeastRig b;
    three_beasts(b);
    training_grounds(b, 960, 10, 0);
    b.seconds(32);
    REQUIRE(says(b.reply("beastmaster.train Fang"), "goes to the Training Grounds"));
    b.spawnable(1);
    REQUIRE(says(b.reply("beastmaster.call Fang"), "comes"));
    CHECK(says(b.reply("beastmaster.train"), "None of your beasts is training"));
    CHECK(says(b.reply("beastmaster.train Fang"), "Fang is out: send it home first"));
}

TEST_CASE("training: 3 loyalty points a game day by the game's clock while its owner is in the world; a night slept through counts, a jump over 12 h not") {
    BeastRig b;
    three_beasts(b);
    training_grounds(b, 960, 10, 0);
    b.seconds(32);
    set_daytime(b.r.fake, 6.0);
    REQUIRE(says(b.reply("beastmaster.train Fang"), "goes to the Training Grounds"));
    b.seconds(2);
    set_daytime(b.r.fake, 14.0);  // 8 game hours: one point
    b.seconds(2);
    CHECK(says(b.saved(), "y 0x00000000000000aa 1 Fang"));
    set_daytime(b.r.fake, 13.0);  // round the clock (23 h): a clock set, nothing counts
    b.seconds(2);
    CHECK(says(b.reply("beastmaster.train"), "Fang trains at the Training Grounds at 10, 10, 0 (loyalty 1, 0 % of a point)"));
    set_daytime(b.r.fake, 22.0);  // 9 game hours, then a night slept through (22 to 6: 8 h): one and a half points more
    b.seconds(2);
    set_daytime(b.r.fake, 6.0);
    b.seconds(2);
    CHECK(says(b.saved(), "y 0x00000000000000aa 3 Fang"));
    CHECK(says(b.reply("beastmaster.train"), "(loyalty 1, 12 % of a point)"));  // 17 h: 2.125 points
    // its owner leaves the world: nothing counts
    b.r.fake.components.erase({ecs::PlayerState::kHash, 1});
    b.seconds(2);
    set_daytime(b.r.fake, 14.0);
    b.seconds(2);
    CHECK(says(b.saved(), "y 0x00000000000000aa 3 Fang"));
    // 0 a day: training only looks
    component<ecs::PlayerState>(b.r.fake, 1);
    b.r.fake.settings["beastmaster.training_points"] = "0";
    b.seconds(2);
    set_daytime(b.r.fake, 22.0);
    b.seconds(2);
    CHECK(says(b.saved(), "y 0x00000000000000aa 3 Fang"));
}

TEST_CASE("training: a save's g and q lines load back: a beast trains on at a Training Grounds nobody is near") {
    BeastRig b;
    write(b.data, "b 0x00000000000000aa wolf 0 0 Fang\ng 0x00000000000000aa 0.5000 300.0,10.0,0.0 Fang\n"
                  "q 300.0 10.0 0.0 -\ns 1\n");
    b.r.fake.session = 2;
    fire_load(1);
    b.seconds(1);
    CHECK(says(b.reply("beastmaster.train"), "Fang trains at the Training Grounds at 300, 10, 0 (loyalty 1, 50 % of a point)"));
    set_daytime(b.r.fake, 6.0);
    b.seconds(2);
    set_daytime(b.r.fake, 10.0);  // 4 game hours: half a point, the first whole one
    b.seconds(2);
    CHECK(says(b.saved(), "y 0x00000000000000aa 1 Fang"));
    CHECK(says(b.saved(), "q 300.0 10.0 0.0 -"));  // still known: nobody near it saw it gone
}

TEST_CASE("training: a Training Grounds a player near it no longer sees for 30 s is gone: its beasts go back to the stable") {
    BeastRig b;
    three_beasts(b);
    training_grounds(b, 960, 10, 0);
    b.seconds(32);
    REQUIRE(says(b.reply("beastmaster.train Fang"), "goes to the Training Grounds"));
    remove_entity(b.r.fake, 960);  // taken down, the player 10 m from it
    b.seconds(65);                 // the next search (every 30 s), then 30 s unseen
    CHECK(says(b.reply("beastmaster.train"), "None of your beasts is training"));
    CHECK_FALSE(says(b.saved(), "q 10.0 10.0 0.0"));
    bool heard = false;
    for (const auto& n : b.r.fake.notes) heard = heard || n.find("Fang's Training Grounds is gone") != std::string::npos;
    for (const auto& [p, t] : b.r.fake.tells) heard = heard || t.find("Fang's Training Grounds is gone") != std::string::npos;
    CHECK(heard);
}

TEST_CASE("home: a beast in training does not show up in the Large Stable; it shows up at its Training Grounds") {
    BeastRig b;
    three_beasts(b);
    large_stable(b, 950, 10, 0);
    training_grounds(b, 960, -10, 0);
    homes_ready(b, 4);
    b.seconds(32);
    REQUIRE(b.r.fake.spawns.size() == 3);
    REQUIRE(says(b.reply("beastmaster.train Rex"), "goes to the Training Grounds"));
    b.seconds(2);
    CHECK(destroyed(b, 1236));  // Rex left the Large Stable
    b.seconds(5);
    REQUIRE(b.r.fake.spawns.size() >= 4);  // shown once more: on the Training Grounds (then maybe the dummy's target)
    CHECK(spawned_as(b.r.fake.spawns[3], kHomeHound));
    CHECK(std::hypot(b.r.fake.spawns[3].x + 10, b.r.fake.spawns[3].z) < 1.6);
}

namespace {
constexpr const char* kDummyTarget = "217bfc46-d4c4-4732-b097-0e8564cb57bc";  // Base_Door_Collision
// The orders on `enemy` so far.
std::vector<EmfEnemyOrder> orders_of(BeastRig& b, uint32_t enemy) {
    std::vector<EmfEnemyOrder> out;
    for (const auto& o : b.r.fake.orders)
        if (o.enemy == enemy) out.push_back(o);
    return out;
}
// Player 1's Fang and Tusk in training at a Training Grounds at (10, 10, 0): shown as 1234 and 1235 (1236 is ready for
// the dummy's target).
void two_training(BeastRig& b) {
    three_beasts(b);
    training_grounds(b, 960, 10, 0);
    homes_ready(b, 3);
    b.seconds(32);
    REQUIRE(says(b.reply("beastmaster.train Fang"), "goes to the Training Grounds"));
    REQUIRE(says(b.reply("beastmaster.train Tusk"), "goes to the Training Grounds"));
    b.seconds(2);
}
}  // namespace

TEST_CASE("training: the beasts in training show up on their Training Grounds, on spots round the dummy, as player-side creatures") {
    BeastRig b;
    two_training(b);
    REQUIRE(b.r.fake.spawns.size() == 2);
    for (uint32_t i = 0; i < 2; ++i) {
        const auto& s = b.r.fake.spawns[i];
        const double r = std::hypot(s.x - 10, s.z - 0);
        CHECK(r > 1.0);
        CHECK(r < 1.6);
        CHECK(s.y > 10.3);
        CHECK(b.faction(1234 + i) == kPlayerAlly);
    }
    CHECK(says(b.reply("beastmaster.home"), "None of your beasts is in a Large Stable"));
}

TEST_CASE("training: the first scene is an attack: an enemy order (allies) on the dummy's invisible target, renewed every 3 s") {
    RollRig b;
    b.roll(0.99);  // the longest times: the first scene 12 s after showing up, a 15 s attack
    two_training(b);
    b.seconds(13);  // the first scene's start
    REQUIRE(b.r.fake.spawns.size() == 3);
    CHECK(spawned_as(b.r.fake.spawns[2], kDummyTarget));
    CHECK(std::hypot(b.r.fake.spawns[2].x - 10, b.r.fake.spawns[2].z) < 0.01);  // at the dummy
    for (uint32_t id : {1234u, 1235u}) {
        CAPTURE(id);
        const auto o = orders_of(b, id);
        REQUIRE_FALSE(o.empty());
        CHECK(o.back().target == 1236);
        CHECK((o.back().flags & EMF_ORDER_ALLIES) != 0);
        CHECK(o.back().seconds == doctest::Approx(4.0));
    }
    const size_t before = orders_of(b, 1234).size();
    b.seconds(4);
    CHECK(orders_of(b, 1234).size() > before);  // renewed while the attack lasts
    CHECK(b.r.fake.spawns.size() == 3);         // one target for the Training Grounds
}

TEST_CASE("training: an attack is followed by another scene, then attacks again; the dummy's target goes when no beast of it is shown") {
    RollRig b;
    b.roll(0.0);  // the shortest times, and the first scene in the list: 5 s, an 8 s attack, a 10 s rest, an attack
    two_training(b);
    b.seconds(5);
    REQUIRE_FALSE(orders_of(b, 1234).empty());  // the attack began
    b.seconds(10);                              // it ended (8 s): a rest
    const size_t n = orders_of(b, 1234).size();
    b.seconds(4);
    CHECK(orders_of(b, 1234).size() == n);  // no orders while it rests
    b.seconds(6);
    CHECK(orders_of(b, 1234).size() > n);   // attacking again
    REQUIRE(says(b.reply("beastmaster.untrain Fang"), "back in the stable"));
    REQUIRE(says(b.reply("beastmaster.untrain Tusk"), "back in the stable"));
    b.seconds(2);
    CHECK(destroyed(b, 1234));
    CHECK(destroyed(b, 1235));
    CHECK(destroyed(b, 1236));  // the target too
}

TEST_CASE("training: a beast is shown only at its own Training Grounds, and only while its owner is within 50 m of it") {
    BeastRig b;
    three_beasts(b);
    training_grounds(b, 960, 10, 0);
    stable_prop(b, 961, -10, 10, 0, kTrainingModel);
    homes_ready(b, 3);
    b.seconds(32);
    REQUIRE(says(b.reply("beastmaster.train Fang"), "goes to the Training Grounds"));  // the nearest: both 10 m away
    b.seconds(2);
    REQUIRE(b.r.fake.spawns.size() == 1);
    const double gx = b.r.fake.spawns[0].x > 0 ? 10 : -10;
    CHECK(says(b.reply("beastmaster.train"), gx > 0 ? "at 10, 10, 0" : "at -10, 10, 0"));
    place(b.r.fake, 1, 0, 10, -100);  // the owner goes 100 m away: it leaves the platform (and trains on)
    b.seconds(2);
    CHECK(destroyed(b, 1234));
}

TEST_CASE("training: the Beasts window sends the selected beast to training and brings it back; no such button without a Training Grounds") {
    BeastRig b;
    three_beasts(b);
    at_stable(b);
    press(b, kE);
    REQUIRE(b.r.fake.windows.size() == 1u);
    click(b, "beast.Tusk");
    CHECK(row_at(shown(b), "train") == -1);  // no Training Grounds in the world
    CHECK(row_at(shown(b), "untrain") == -1);
    training_grounds(b, 970, 10, 0);
    b.seconds(32);
    click(b, "beast.Tusk");
    REQUIRE(row_at(shown(b), "train") != -1);
    CHECK(text_at(shown(b), "train") == "Send to training");
    CHECK((flags_at(shown(b), "train") & EMF_UI_DISABLED) == 0u);
    click(b, "train");
    CHECK(shows(shown(b), "Tusk goes to the Training Grounds"));
    CHECK(text_at(shown(b), "untrain") == "Back to the stable");
    click(b, "tab.stable");
    CHECK(text_at(shown(b), "beast.Tusk") == "Tusk (boar, loyalty 1, training)");
    click(b, "beast.Tusk");
    click(b, "untrain");
    CHECK(shows(shown(b), "Tusk is back in the stable"));
    CHECK(text_at(shown(b), "train") == "Send to training");
}

// Review (2026-11-23): a Training Grounds' place comes rounded differently from a joined player's report than from the
// host's world; it is the same Training Grounds within 1 m.
TEST_CASE("training: a Training Grounds found a few centimetres from a known one is that one: its beasts show and keep training") {
    BeastRig b;
    write(b.data, "b 0x00000000000000aa wolf 0 0 Fang\na 0x00000000000000aa Fang\ng 0x00000000000000aa 0.0000 10.0,10.0,0.0 Fang\n"
                  "q 10.0 10.0 0.0 -\ns 1\n");
    b.r.fake.session = 2;
    fire_load(1);
    b.seconds(1);
    flame_altar(b, 900, 0, 0);
    stable_prop(b, 960, 10.06, 10, 0, kTrainingModel);  // "10.1,10.0,0.0" by its own place
    homes_ready(b, 2);
    b.seconds(70);  // found, and long past the 30 s a missing one is given
    CHECK(says(b.reply("beastmaster.train"), "Fang trains at the Training Grounds at 10, 10, 0"));
    REQUIRE_FALSE(b.r.fake.spawns.empty());  // shown on it
    CHECK(std::hypot(b.r.fake.spawns[0].x - 10, b.r.fake.spawns[0].z) < 1.6);
    CHECK_FALSE(says(b.saved(), "q 10.1"));  // no second Training Grounds
}

TEST_CASE("training: a beast goes only to a Training Grounds within 50 m of its owner, and the reply names it") {
    BeastRig b;
    write(b.data, "b 0x00000000000000aa wolf 0 0 Fang\nb 0x00000000000000aa boar 0 0 Tusk\n"
                  "b 0x00000000000000aa hound 0 0 Rex\na 0x00000000000000aa Fang\ns 1\n"
                  "q 300.0 10.0 0.0 -\n");  // a Training Grounds in another base, known from before
    b.r.fake.session = 2;
    fire_load(1);
    b.seconds(1);
    training_grounds(b, 960, 10, 0);
    b.seconds(32);
    CHECK(says(b.reply("beastmaster.train Fang"), "Fang goes to the Training Grounds at 10, 10, 0"));
    CHECK(says(b.reply("beastmaster.train Tusk"), "Tusk goes to the Training Grounds at 10, 10, 0"));
    CHECK(says(b.reply("beastmaster.train Rex"), "full (2 of 2)"));  // not sent to one far away
    place(b.r.fake, 1, 0, 10, -100);
    CHECK(says(b.reply("beastmaster.untrain Fang"), "back in the stable"));
    CHECK(says(b.reply("beastmaster.train Fang"), "No Training Grounds within 50 m of you"));
}

TEST_CASE("training: a clock set back (or forward more than 12 h) counts nothing; a renamed beast trains on") {
    BeastRig b;
    three_beasts(b);
    training_grounds(b, 960, 10, 0);
    b.seconds(32);
    set_daytime(b.r.fake, 12.0);
    REQUIRE(says(b.reply("beastmaster.train Fang"), "goes to the Training Grounds"));
    b.seconds(2);
    set_daytime(b.r.fake, 11.0);  // set back an hour: not 23 game hours of training
    b.seconds(2);
    CHECK(says(b.reply("beastmaster.train"), "(loyalty 1, 0 % of a point)"));
    REQUIRE(says(b.reply("beastmaster.rename Fang Grey Fang"), "is now called Grey Fang"));
    CHECK(says(b.saved(), "g 0x00000000000000aa 0.0000 10.0,10.0,0.0 Grey Fang"));
    set_daytime(b.r.fake, 19.0);  // 8 game hours: a point
    b.seconds(2);
    CHECK(says(b.saved(), "y 0x00000000000000aa 1 Grey Fang"));
}

// ---- critters (1.21, GitHub #47): beasts leave small critters (rats) alone, unless one attacks ----
namespace {
constexpr const char* kRatGraph = "57a2f080-1f1e-4d18-8170-2d80b756fe56";  // Enemy_Wildbeast_Rat_hook's
}  // namespace

// A player's idea: their wolf killed every rat in an old house; they liked the rats running around.
TEST_CASE("critters: an aggressive beast leaves a rat alone; a rat that bites its owner is hunted; 30 s later the hunt ends") {
    BeastRig b;
    wolf_out(b);
    b.beast(72, kRatGraph, 5, 20, kWildBeast, 30, 30);
    armed(b, 1234);
    b.reply("beastmaster.mode aggressive");
    b.seconds(3);
    CHECK(ordered(b, 1234) == 0u);
    deliver(b.r.fake, "HitEvent", hit_event(72, 1));  // it bites the player
    b.seconds(3);
    CHECK(ordered(b, 1234) == 72u);
    b.seconds(29);  // 30 s after the bite it is spared again: the hunt ends and no new one picks it
    CHECK(ordered(b, 1234) == 0u);
}

TEST_CASE("critters: Attack on a rat is the player's call: the order holds and its fight is not cleared") {
    BeastRig b;
    wolf_out(b);
    b.beast(72, kRatGraph, 0, 10, kWildBeast, 30, 30);
    armed(b, 1234);
    b.look(1, 0, 10.5, 0, 0);  // right at the rat
    CHECK(b.reply("beastmaster.attack") == "Your beasts attack (1).");
    fighting(b, 1234, 72);
    b.seconds(2);
    CHECK(ordered(b, 1234) == 72u);
    CHECK(still_fighting(b, 1234));
}

TEST_CASE("critters: a beast's own fight against a rat is cleared every step; one that bit it is fought for 30 s; with the setting off a rat is a foe") {
    BeastRig b;
    wolf_out(b);
    b.beast(72, kRatGraph, 0, 3, kWildBeast, 30, 30);
    fighting(b, 1234, 72);  // its own senses picked the rat
    b.seconds(1);
    CHECK_FALSE(still_fighting(b, 1234));
    deliver(b.r.fake, "HitEvent", hit_event(72, 1234));  // the rat bites the beast
    fighting(b, 1234, 72);
    b.seconds(1);
    CHECK(still_fighting(b, 1234));
    b.seconds(31);  // 30 s after the bite it is a critter again
    fighting(b, 1234, 72);
    b.seconds(1);
    CHECK_FALSE(still_fighting(b, 1234));
    b.r.fake.settings["beastmaster.spare_critters"] = "false";
    fighting(b, 1234, 72);
    b.seconds(1);
    CHECK(still_fighting(b, 1234));
}

// ---- our animals in the wild (replace.lua, the wilds pack) ----
namespace {
constexpr const char* kSabreGraph = "9c1b120e-4901-495c-9cc0-5de134fbe140";
constexpr const char* kGoatGraph = "1fc6e01e-f25b-4aeb-a5a0-2d3fb2c727f5";
constexpr const char* kFoxTemplate = "2e377d42-ff1e-4e6e-b77a-1596e9404afd";          // wilds_fox
constexpr const char* kArcticFoxTemplate = "9c54a6f2-8075-4688-8668-533b44fddd8c";    // wilds_fox_arctic
constexpr const char* kBlackBearTemplate = "797eac81-cf61-413b-8ae4-81297aac0ae6";    // wilds_bear_black
constexpr const char* kSnowBearTemplate = "8027e2cc-db55-4242-9977-a0dcf5fabbb6";     // wilds_bear_snow
constexpr const char* kWildsGoatTemplate = "2d1e9269-b64b-44b5-94c5-7543f5be2ae6";    // wilds_goat
constexpr const char* kSnowSabretoothModel = "3d555869-f7ef-4462-b341-06cad1b2d7e6";  // the game's render model
constexpr int kWildlife = 5, kScavenger = 4;

// A rig past the first walk of the world (what it found stays the game's), with the next spawn ready. The players have
// the game's Faction (7, Player) as in game.
RollRig& walked(RollRig& b) {
    for (uint32_t p : {1u, 2u}) component<ecs::Faction>(b.r.fake, p).faction = static_cast<emf::gen::FactionType>(7);
    b.seconds(11);
    b.spawnable(2);
    return b;
}
// Player 3 (account 0xCC) joins at (x, 10, z), with the game's Faction.
void join_player(RollRig& b, double x, double z) {
    add_player(b.r.fake, 3, 0xCC, x, 10, z);
    component<ecs::Faction>(b.r.fake, 3).faction = static_cast<emf::gen::FactionType>(7);
    add_entity(b.r.fake, 3);
}
// A game wolf (no ScaleComponent) at level 17 that turns up at (x, 10, z), facing +x.
void game_wolf(RollRig& b, uint32_t id, double x, double z) {
    b.beast(id, kWolfGraph, x, z, kWildBeast, 100, 100);
    component<ecs::Level>(b.r.fake, id).dataStorage[0] = 17;
    auto& t = live_of<ecs::CurrentTransform>(b, id);
    t.transform.orientation.y = 0.70710678f;  // a quarter turn about the up axis: facing +x (yaw 90)
    t.transform.orientation.w = 0.70710678f;
}
bool swapped(RollRig& b, uint32_t id) { return contains(b.r.fake.destroyed, id); }
}  // namespace

TEST_CASE("wilds swap: a game wolf that turns up 40 m away is now and then a fox in its place: its level, facing its way") {
    RollRig b;
    walked(b);
    b.roll(0.10);  // over the 4 % of the colours, under 4 + 15: one of the wolf's animals (the fox)
    game_wolf(b, 50, 0, -40);
    b.seconds(12);  // the next walk finds it; no client world answers for its model within a second: the plain wolf
    REQUIRE(b.r.fake.spawns.size() == 1);
    CHECK(guid_text(b.r.fake.spawns[0].guid) == kFoxTemplate);
    CHECK(std::abs(b.r.fake.spawns[0].x - 0.0) + std::abs(b.r.fake.spawns[0].z + 40.0) < 0.01);
    CHECK(b.r.fake.spawns[0].facing);
    CHECK(std::abs(b.r.fake.spawns[0].yaw - 90.0f) < 0.5f);
    CHECK(swapped(b, 50));
    CHECK(now_of<ecs::Level>(b, 1234).dataStorage[0] == 17);
    b.seconds(30);
    CHECK(b.r.fake.spawns.size() == 1);  // once
}

TEST_CASE("wilds swap: a creature rolled once stays the game's for good; settings at 0 swap nothing") {
    RollRig b;
    walked(b);
    b.roll(0.5);
    game_wolf(b, 50, 0, -40);
    b.seconds(12);
    b.roll(0.01);
    b.seconds(30);
    CHECK(b.r.fake.spawns.empty());
    CHECK(!swapped(b, 50));

    RollRig off({{"wild_replace_pct", "0"}, {"wild_coat_pct", "0"}});
    walked(off);
    off.roll(0.0);
    game_wolf(off, 50, 0, -40);
    off.seconds(30);
    CHECK(off.r.fake.spawns.empty());
    CHECK(!swapped(off, 50));
}

TEST_CASE("wilds swap: a colour version now and then (4 %): stronger by wild_coat_strength (125 %), health and damage") {
    RollRig b;
    walked(b);
    b.roll(0.01);
    game_wolf(b, 50, 0, -40);
    auto& dm = component<ecs::DamageMod>(b.r.fake, 1234);
    dm.dataStorage[2] = 2.0f;
    component<ecs::ScaleComponent>(b.r.fake, 1234).uniformScale = 0.715f;  // its template's size
    b.seconds(12);
    REQUIRE(b.r.fake.spawns.size() == 1);
    CHECK(guid_text(b.r.fake.spawns[0].guid) == kArcticFoxTemplate);
    auto& h = component<ecs::Health>(b.r.fake, 1234);
    h.dataStorage[2] = 40;
    b.seconds(2);
    CHECK(h.dataStorage[3] == 10);  // 40 + 10 = 1.25 x 40
    CHECK(std::abs(dm.dataStorage[2] - 2.5f) < 0.001f);
    b.seconds(5);
    CHECK(std::abs(dm.dataStorage[2] - 2.5f) < 0.001f);  // once
    dm.dataStorage[2] = 2.2f;  // the game's rescale to a level writes its own value: the bonus goes on top again
    b.seconds(1);
    CHECK(std::abs(dm.dataStorage[2] - 2.75f) < 0.001f);
}

TEST_CASE("wilds swap: a colour version tamed is stronger by its role numbers, not by its wild bonus too") {
    RollRig b;
    walked(b);
    b.roll(0.01);
    game_wolf(b, 50, 0, -40);
    component<ecs::DamageMod>(b.r.fake, 1234).dataStorage[2] = 2.0f;
    b.seconds(12);
    REQUIRE(guid_text(b.r.fake.spawns.at(0).guid) == kArcticFoxTemplate);
    REQUIRE(now_of<ecs::DamageMod>(b, 1234).dataStorage[2] == doctest::Approx(2.5));
    b.beast(1234, kWolfGraph, 0, 5, kWildBeast, 20, 100);  // beaten down in front of player 1
    component<ecs::ScaleComponent>(b.r.fake, 1234).uniformScale = 0.715f;
    b.look(1, 0, 10.5, 0, 0);
    REQUIRE(b.reply("beastmaster.tame").find("Taming") != std::string::npos);
    b.seconds(4);
    REQUIRE(b.reply("beastmaster.list").find("1. Arctic fox - arctic_fox") != std::string::npos);
    // its template's 2.0, times its role 1.35 and Beast damage 125 %
    CHECK(now_of<ecs::DamageMod>(b, 1234).dataStorage[2] == doctest::Approx(2.0 * 1.35 * 1.25));
}

TEST_CASE("wilds swap: never in view (25 m of a player), near a base, hurt, one of our packs' (a size), or not wild") {
    RollRig b;
    flame_altar(b, 900, -100, 0);
    b.seconds(32);  // the stables' search of the world (every 30 s) knows the altar
    walked(b);
    b.roll(0.01);
    game_wolf(b, 50, 0, -20);  // 20 m from player 1
    game_wolf(b, 51, -100, -30);  // 30 m from the altar
    game_wolf(b, 52, 0, -60);
    b.r.fake.attrs[{52, EMF_ATTR_HEALTH}] = 60;  // hurt (in a fight)
    game_wolf(b, 53, 0, -80);
    component<ecs::ScaleComponent>(b.r.fake, 53).uniformScale = 1.4f;  // the bear pack's bear runs the wolf's graph
    b.beast(54, kWolfGraph, 0, -100, kScavenger, 100, 100);  // a scavengers' hound
    b.seconds(25);
    CHECK(b.r.fake.spawns.empty());
    CHECK(b.r.fake.destroyed.empty());
}

TEST_CASE("wilds swap: creatures about the host as they arrive stay the game's, also when the client names them at once") {
    RollRig b;
    for (uint32_t p : {1u, 2u}) remove_entity(b.r.fake, p);  // loading: the players are not in the world yet
    b.r.fake.session = 2;
    fire_load(1);
    b.seconds(2);  // the first walk finds nobody
    add_player(b.r.fake, 1, 0xAA, 0, 10, 0);  // the players arrive
    add_player(b.r.fake, 2, 0xBB, 30, 10, 0);
    for (uint32_t p : {1u, 2u}) {
        component<ecs::Faction>(b.r.fake, p).faction = static_cast<emf::gen::FactionType>(7);
        add_entity(b.r.fake, p);
    }
    b.roll(0.01);
    b.spawnable(1);
    game_wolf(b, 50, 0, -40);  // streams in with the host
    const auto model = guid_bytes("91e2d4cc-01c9-4203-a92e-b08769364522");
    for (int i = 0; i < 25 * 60; ++i) {
        b.r.fake.templates[{ecs::ModelResource::kHash, 50}] = model;
        b.host->tick(kClient);
        b.r.fake.templates.erase({ecs::ModelResource::kHash, 50});
        b.r.fake.tick += 1;
        b.host->tick(kServer);
    }
    CHECK(!swapped(b, 50));
}

TEST_CASE("wilds swap: a player who joined while a walk runs counts for the 25 m rule (no swap in view)") {
    RollRig b;
    walked(b);
    b.roll(0.01);
    game_wolf(b, 50, 200, 200);  // first in the walk's list
    for (uint32_t i = 0; i < 1100; ++i) add_entity(b.r.fake, 20000 + i);  // props: the walk takes three steps
    join_player(b, 200, 195);  // last in the list, 5 m from the wolf
    const auto model = guid_bytes("91e2d4cc-01c9-4203-a92e-b08769364522");
    for (int i = 0; i < 25 * 60; ++i) {
        b.r.fake.templates[{ecs::ModelResource::kHash, 50}] = model;
        b.host->tick(kClient);
        b.r.fake.templates.erase({ecs::ModelResource::kHash, 50});
        b.r.fake.tick += 1;
        b.host->tick(kServer);
    }
    CHECK(!swapped(b, 50));
}

TEST_CASE("wilds swap: what the world had when it loaded stays the game's") {
    RollRig b;
    for (uint32_t p : {1u, 2u}) component<ecs::Faction>(b.r.fake, p).faction = static_cast<emf::gen::FactionType>(7);
    b.roll(0.01);
    game_wolf(b, 50, 0, -40);  // there when the world loads
    b.r.fake.session = 2;
    fire_load(1);
    b.spawnable(1);
    b.seconds(30);
    CHECK(b.r.fake.spawns.empty());
}

TEST_CASE("wilds swap: a creature on the id when its roll comes is checked again (the game gives ids out again)") {
    // the frame its swap comes in a plain run; then the same run with the wolf's id taken by a called beast meanwhile
    auto run = [](int change_at) {
        RollRig b;
        walked(b);
        b.roll(0.10);
        game_wolf(b, 50, 0, -40);
        for (int i = 0; i < 15 * 60; ++i) {
            if (i == change_at) component<ecs::Faction>(b.r.fake, 50).faction = static_cast<emf::gen::FactionType>(11);
            b.r.fake.tick += 1;
            b.host->tick(kServer);
            if (swapped(b, 50)) return i;
        }
        return -1;
    };
    const int at = run(-1);
    REQUIRE(at > 60);
    CHECK(run(at - 30) == -1);  // between the walk that found it and its roll (a second later)
}

TEST_CASE("wilds swap: without the wilds pack nothing is taken out, and it stops trying (one log line)") {
    RollRig b;
    walked(b);
    b.roll(0.10);
    b.r.fake.missing_templates.insert(guid_bytes(kFoxTemplate));
    game_wolf(b, 50, 0, -40);
    b.seconds(12);
    CHECK(!swapped(b, 50));
    game_wolf(b, 51, 0, -60);
    b.seconds(12);
    CHECK(!swapped(b, 51));
    CHECK(logged(b.r.fake, "wilds pack") == 1);
}

TEST_CASE("wilds swap: the fox in a wolf's place is tamed as a fox; a creature at a fox's size it did not put there is not") {
    RollRig b;
    walked(b);
    b.roll(0.10);
    game_wolf(b, 50, 0, -40);
    b.seconds(12);
    REQUIRE(swapped(b, 50));
    b.beast(1234, kWolfGraph, 0, 5, kWildBeast, 20, 100);  // beaten down, in front of player 1 (it runs the wolf's graph)
    component<ecs::ScaleComponent>(b.r.fake, 1234).uniformScale = 0.65f;
    b.look(1, 0, 10.5, 0, 0);
    REQUIRE(b.reply("beastmaster.tame").find("Taming") != std::string::npos);
    b.seconds(4);
    CHECK(b.reply("beastmaster.list").find("1. Fox - fox, loyalty 1, out") != std::string::npos);

    BeastRig c;  // the same creature that no swap made (a script reload forgot the list): nothing to tame
    c.beast(60, kWolfGraph, 0, 5, kWildBeast, 20, 100);
    component<ecs::ScaleComponent>(c.r.fake, 60).uniformScale = 0.65f;
    c.look(1, 0, 10.5, 0, 0);
    CHECK(c.reply("beastmaster.tame").find("Taming") == std::string::npos);

    // the brown bear is drawn at 1.0, but with a ScaleComponent, which the game's sabretooths do not have
    BeastRig d;
    d.beast(61, kSabreGraph, 0, 5, kWildBeast, 20, 100);
    component<ecs::ScaleComponent>(d.r.fake, 61).uniformScale = 1.0f;
    d.look(1, 0, 10.5, 0, 0);
    CHECK(d.reply("beastmaster.tame").find("Taming") == std::string::npos);
    BeastRig e;  // a game sabretooth (no ScaleComponent) is one
    e.beast(62, kSabreGraph, 0, 5, kWildBeast, 20, 100);
    e.look(1, 0, 10.5, 0, 0);
    CHECK(e.reply("beastmaster.tame").find("Taming the sabretooth") != std::string::npos);
}

TEST_CASE("wilds swap: a fox from beastmaster.spawn can be tamed too (the list knows it)") {
    BeastRig b;
    b.spawnable(1);
    REQUIRE(b.reply("beastmaster.spawn fox").find("entity 1234") != std::string::npos);
    CHECK(guid_text(b.r.fake.spawns[0].guid) == kFoxTemplate);
    b.beast(1234, kWolfGraph, 0, 5, kWildBeast, 20, 100);
    component<ecs::ScaleComponent>(b.r.fake, 1234).uniformScale = 0.65f;
    b.look(1, 0, 10.5, 0, 0);
    REQUIRE(b.reply("beastmaster.tame").find("Taming") != std::string::npos);
    b.seconds(4);
    CHECK(b.reply("beastmaster.list").find("1. Fox - fox, loyalty 1, out") != std::string::npos);
}

TEST_CASE("wilds swap: a game goat is now and then our goat, which is never tamed (it cannot fight)") {
    RollRig b;
    walked(b);
    b.roll(0.10);
    b.beast(50, kGoatGraph, 0, -40, kWildlife, 100, 100);
    b.seconds(12);
    REQUIRE(b.r.fake.spawns.size() == 1);
    CHECK(guid_text(b.r.fake.spawns[0].guid) == kWildsGoatTemplate);
    b.beast(1234, kGoatGraph, 0, 5, kWildlife, 20, 100);
    component<ecs::ScaleComponent>(b.r.fake, 1234).uniformScale = 1.0f;
    b.look(1, 0, 10.5, 0, 0);
    CHECK(b.reply("beastmaster.tame").find("Taming") == std::string::npos);
}

TEST_CASE("wilds swap: on the host the client world's model tells a snow sabretooth from a sabretooth") {
    // the server world has no render models for creatures; the host's client world has, on the same entity ids
    auto run = [](bool snow) {
        RollRig b;
        walked(b);
        b.roll(0.01);
        b.beast(50, kSabreGraph, 0, -40, kWildBeast, 100, 100);
        const auto model = guid_bytes(kSnowSabretoothModel);
        for (int i = 0; i < 15 * 60; ++i) {
            if (snow) b.r.fake.templates[{ecs::ModelResource::kHash, 50}] = model;
            b.host->tick(kClient);
            b.r.fake.templates.erase({ecs::ModelResource::kHash, 50});
            b.r.fake.tick += 1;
            b.host->tick(kServer);
        }
        REQUIRE(b.r.fake.spawns.size() == 1);
        return guid_text(b.r.fake.spawns[0].guid);
    };
    CHECK(run(true) == kSnowBearTemplate);    // the snow sabretooth's colours: snow bear, snow leopard, snow lynx
    CHECK(run(false) == kBlackBearTemplate);  // no model known: the sabretooth's (black bear first)
}
