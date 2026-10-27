// Beastmaster (mods/beastmaster), run by the Lua host from a copy of the shipped folder, with fake engine events,
// components and commands (tests/lua_mod_rig.h).
#include "lua_mod_rig.h"

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
struct BeastRig : ModRig {
    explicit BeastRig(const std::map<std::string, std::string>& settings = {})
        : ModRig("beastmaster", L"beastmaster", settings, [](Fake& f) {
              add_player(f, 1, 0xAA, 0, 10, 0);
              add_player(f, 2, 0xBB, 30, 10, 0);
              for (uint32_t p : {1u, 2u}) {
                  component<ecs::Level>(f, p).dataStorage[0] = 10;
                  add_entity(f, p);
                  f.attrs[{p, EMF_ATTR_MANA}] = 100;
                  f.attrs[{p, EMF_ATTR_MAX_MANA}] = 100;
              }
          }) {
        with_attributes(r.api);  // a v18+ core: health, mana and damage through the game's attributes
        with_teleport(r.api);    // a v20 core
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
}  // namespace

TEST_CASE("beastmaster: the roster names beasts uniquely, holds 8 per player, and comes back from the world's data") {
    BeastRig b;
    for (int i = 1; i <= 8; ++i) CHECK(b.reply("beastmaster.give wolf").find("added") != std::string::npos);
    const std::string ninth = b.reply("beastmaster.give wolf");
    CHECK(ninth.find("8") != std::string::npos);
    CHECK(ninth.find("added") == std::string::npos);
    const std::string list = b.reply("beastmaster.list");
    CHECK(list.find("1. Wolf - wolf, at home") != std::string::npos);
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
    CHECK(b.reply("beastmaster.list").find("1. Wolf - wolf, out") != std::string::npos);
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

TEST_CASE("beastmaster: a healthy beast is not tamed by defeat; with 3 baits it is tamed by feeding") {
    BeastRig b;
    b.r.fake.item_names[kDogBait] = "Food_T2_Dog_Bait";
    b.look(1, 0, 10.5, 0, 0);
    b.beast(50, kWolfGraph, 0, 5, kWildBeast, 100, 100);
    CHECK(b.reply("beastmaster.tame").find("too strong") != std::string::npos);
    for (int i = 0; i < 3; ++i) {
        b.reply("beastmaster.feed");
        REQUIRE(b.r.fake.takes.size() == static_cast<size_t>(i + 1));
        const EmfItemsRequest t = b.r.fake.takes.back();
        CHECK(t.item == kDogBait);
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

TEST_CASE("beastmaster: calling a wolf costs 15 % mana, spawns it beside you as your ally, and a second call of a wolf is refused") {
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
    CHECK(b.reply("beastmaster.list").find("1. Wolf - wolf, out") != std::string::npos);
    CHECK(b.reply("beastmaster.call Wolf 2").find("out already") != std::string::npos);
    CHECK(b.r.fake.spawns.size() == 1);
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

TEST_CASE("beastmaster: a beast that dies rests 120 s, its body is removed, and it can be called again after") {
    BeastRig b;
    b.reply("beastmaster.give wolf");
    b.spawnable(2);
    b.reply("beastmaster.call Wolf");
    deliver(b.r.fake, "EntityDiedEvent", died_event(1234));
    b.r.fake.attrs[{1234, EMF_ATTR_HEALTH}] = 0;
    b.seconds(2);
    CHECK(contains(b.r.fake.destroyed, 1234));  // no loot from our own beasts
    CHECK(b.reply("beastmaster.list").find("rests 1:5") != std::string::npos);
    CHECK(b.reply("beastmaster.call Wolf").find("rests") != std::string::npos);
    b.seconds(120);
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
    CHECK(b.reply("beastmaster.list").find("1. Wolf - wolf, at home") != std::string::npos);
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
std::vector<std::pair<uint32_t, uint32_t>> attacks(BeastRig& b) {
    b.send_events();
    std::vector<std::pair<uint32_t, uint32_t>> out;
    for (const auto& [name, bytes] : b.r.fake.emitted) {
        if (name != "AddThreatTargetEvent") continue;
        const auto* e = reinterpret_cast<const ecs::AddThreatTargetEvent*>(bytes.data());
        out.push_back({e->attackerId.id, e->targetId.id});
    }
    return out;
}
// A wolf called out (entity 1234) for player 1.
void wolf_out(BeastRig& b) {
    b.reply("beastmaster.give wolf");
    b.spawnable(1);
    REQUIRE(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
}
}  // namespace

TEST_CASE("beastmaster: a beast 50 m away for 3 s, or 90 m away at once, is teleported beside its owner") {
    BeastRig b;
    wolf_out(b);
    place(b.r.fake, 1234, 50, 10, 0);
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
    place(b.r.fake, 1234, 92, 10, 0);  // far at once
    b.seconds(1);
    CHECK(b.r.fake.teleports.size() == before + 1);
}

TEST_CASE("beastmaster: attack sends every beast out at the creature you look at") {
    BeastRig b;
    wolf_out(b);
    b.look(1, 0, 10.5, 0, 0);
    b.beast(60, "00000000-0000-0000-0000-000000000099", 0, 6, 4 /* Scavenger */, 100, 100);
    CHECK(b.reply("beastmaster.attack").find("attack") != std::string::npos);
    const auto a = attacks(b);
    REQUIRE(a.size() == 1);
    CHECK(a[0] == std::make_pair(1234u, 60u));
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
    press(b, 'B');
    REQUIRE(b.r.fake.wheels.size() == 1);
    const auto& w = b.r.fake.wheels[0];
    CHECK(w.hold == 'B');
    for (const char* a : {"Tame", "Feed", "Attack", "Dismiss all"}) CHECK(index_of(w.items, a) >= 0);
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
constexpr const char* kPantherTemplate = "f3c9a1d7-5b2e-4f8a-9c64-2e7b1d9a5f02";  // the beasts pack's
size_t logged(const Fake& f, const std::string& needle) {
    size_t n = 0;
    for (const auto& l : f.log) n += l.find(needle) != std::string::npos;
    return n;
}
}  // namespace

TEST_CASE("wild bears: one spawns 50-70 m from a player when none exists; none when the pack is missing") {
    BeastRig b({{"wild_bear_minutes", "1"}});
    b.r.fake.missing_templates.insert(guid_bytes(kAurochsTemplate));  // only the bear pack: the bear every time
    b.r.fake.missing_templates.insert(guid_bytes(kPantherTemplate));
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
TEST_CASE("beastmaster: a bear's health adder keeps its maximum at 2.5x the game's base, also after a level change; a wolf keeps its own") {
    BeastRig b;
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
    CHECK(bear.dataStorage[3] == 51);  // 34 + 51 = 85 = 2.5 x 34
    bear.dataStorage[2] = 65;          // the game rescaled it to a new level
    b.seconds(2);
    CHECK(bear.dataStorage[3] == 98);  // 65 x 1.5 = 97.5
    wolf.dataStorage[2] = 34;
    b.seconds(11);
    CHECK(b.reply("beastmaster.call Wolf").find("comes") != std::string::npos);
    b.seconds(2);
    CHECK(wolf.dataStorage[3] == 0);
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
    b.r.fake.item_names[kDogBait] = "Food_T2_Dog_Bait";
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
    CHECK(b.reply("beastmaster.attack").find("Look at a creature") != std::string::npos);  // nothing hit you yet
    deliver(b.r.fake, "HitEvent", hit_event(60, 1));
    b.seconds(1);
    CHECK(b.reply("beastmaster.attack").find("Your beasts attack") != std::string::npos);
    const auto a = attacks(b);
    REQUIRE(a.size() == 1);
    CHECK(a[0] == std::make_pair(1234u, 60u));
    b.seconds(11);
    CHECK(b.reply("beastmaster.attack").find("Look at a creature") != std::string::npos);  // forgotten after 10 s
}

TEST_CASE("beastmaster: a bear is the wolf's graph at 1.4x scale (the pack's ScaleComponent); a wolf, or a reused id, is a wolf") {
    BeastRig b;
    b.beast(1234, kWolfGraph, 2, 2, kWildBeast, 20, 100);  // a wild bear: it came to you, and you beat it down
    component<ecs::ScaleComponent>(b.r.fake, 1234).uniformScale = 1.4f;
    b.look(1, 0, 10.5, 0, 45);
    CHECK(b.reply("beastmaster.tame").find("Taming the bear") != std::string::npos);
    b.seconds(4);
    CHECK(b.reply("beastmaster.list").find("1. Bear - bear, out") != std::string::npos);
    b.beast(70, kWolfGraph, -2, 2, kWildBeast, 20, 100);  // a game wolf: still a wolf
    CHECK(b.reply("beastmaster.tame").find("Taming the wolf") != std::string::npos);
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
    b.r.fake.item_names[kDogBait] = "Food_T2_Dog_Bait";
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
    CHECK(b.reply("beastmaster.list").find("Wolf - wolf, out") != std::string::npos);  // not at the first look
    b.seconds(2);
    CHECK(b.reply("beastmaster.list").find("Wolf - wolf, at home") != std::string::npos);
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
        press(b, 'B');
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
    BeastRig b;
    CHECK(b.reply(std::string("beastmaster.species ") + kBoarGraph + " 6 1.3") == "aurochs");
    CHECK(b.reply(std::string("beastmaster.species ") + kBoarGraph + " 6") == "boar");
    CHECK(b.reply(std::string("beastmaster.species ") + kBoarGraph + " 3 1.3") == "unknown");  // a Vukah's big boar
    CHECK(b.reply(std::string("beastmaster.species ") + kWolfGraph + " 4 1.25") == "hound");     // the scavengers' pet
    b.beast(50, kBoarGraph, 0, 5, kWildBeast, 20, 100);
    component<ecs::ScaleComponent>(b.r.fake, 50).uniformScale = 1.3f;
    b.look(1, 0, 10.5, 0, 0);
    CHECK(b.reply("beastmaster.tame").find("Taming the aurochs") != std::string::npos);
    b.seconds(4);
    CHECK(b.reply("beastmaster.list").find("1. Aurochs - aurochs, out") != std::string::npos);
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
    CHECK(h.dataStorage[3] == 80);  // 40 + 80 = 120 = 3 x 40
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
    CHECK(b.reply("beastmaster.list").find("Aurochs - aurochs, at home") != std::string::npos);
    CHECK(b.reply("beastmaster.call Aurochs").find("pack") != std::string::npos);
}

TEST_CASE("wild beasts: with only the beasts pack installed, wild aurochs and panthers come (the missing bear is skipped)") {
    BeastRig b({{"wild_bear_minutes", "1"}});
    b.r.fake.missing_templates.insert(guid_bytes(kBearTemplate));
    b.spawnable(8);
    std::set<std::string> seen;
    for (int round = 0; round < 6; ++round) {
        const size_t before = b.r.fake.spawns.size();
        b.seconds(61);
        REQUIRE(b.r.fake.spawns.size() == before + 1);
        const std::string t = guid_text(b.r.fake.spawns.back().guid);
        CHECK((t == kAurochsTemplate || t == kPantherTemplate));
        seen.insert(t);
        const uint32_t id = 1234 + static_cast<uint32_t>(before);
        // the pack's size: the aurochs 1.3x, the panther 0.85x
        component<ecs::ScaleComponent>(b.r.fake, id).uniformScale = t == kAurochsTemplate ? 1.3f : 0.85f;
        CHECK(b.faction(id) == kWildBeast);
        b.seconds(60);
        CHECK(b.r.fake.spawns.size() == before + 1);  // one at a time: it is still about
        b.r.fake.attrs[{id, EMF_ATTR_HEALTH}] = 0;     // killed
    }
    CHECK(logged(b.r.fake, "wild bears") <= 1);
}

// ---- new beasts (the beasts pack): the Panther ----
namespace {
constexpr const char* kSabretoothGraph = "9c1b120e-4901-495c-9cc0-5de134fbe140";
}

TEST_CASE("beastmaster: a panther is the sabretooth graph at 0.92x or less (WildBeast); a game sabretooth is not") {
    BeastRig b;
    CHECK(b.reply(std::string("beastmaster.species ") + kSabretoothGraph + " 6 0.85") == "panther");
    CHECK(b.reply(std::string("beastmaster.species ") + kSabretoothGraph + " 6") == "sabretooth");
    CHECK(b.reply(std::string("beastmaster.species ") + kSabretoothGraph + " 6 0.95") == "sabretooth");
    CHECK(b.reply(std::string("beastmaster.species ") + kSabretoothGraph + " 4 0.85") == "sabretooth");  // a tamed one
    b.beast(50, kSabretoothGraph, 0, 5, kWildBeast, 20, 100);
    component<ecs::ScaleComponent>(b.r.fake, 50).uniformScale = 0.85f;
    b.look(1, 0, 10.5, 0, 0);
    CHECK(b.reply("beastmaster.tame").find("Taming the panther") != std::string::npos);
    b.seconds(4);
    CHECK(b.reply("beastmaster.list").find("1. Panther - panther, out") != std::string::npos);
    b.reply("beastmaster.dismiss all");
    b.spawnable(1);
    b.seconds(11);
    CHECK(b.reply("beastmaster.call Panther").find("comes") != std::string::npos);
    REQUIRE(b.r.fake.spawns.size() == 1);
    CHECK(guid_text(b.r.fake.spawns[0].guid) == kPantherTemplate);
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
    CHECK(b.reply("beastmaster.list").find("Aurochs - aurochs, out") != std::string::npos);
    size_t notes = 0;
    for (const auto& x : b.r.fake.notes) notes += x.find("Aurochs") != std::string::npos;
    CHECK(notes == 0);
}

// ---- holding a beast during the taming channel (a beaten panther ran out of range in game) ----
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
    b.r.fake.item_names[kDogBait] = "Food_T2_Dog_Bait";
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
    b.r.fake.item_names[kDogBait] = "Food_T2_Dog_Bait";
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
