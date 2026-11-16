// Necromancer (mods/necromancer), run by the Lua host from a copy of the shipped folder, with fake engine events,
// components and commands (tests/lua_mod_rig.h).
#include "lua_mod_rig.h"

#include "core/overlay/wheel_ui.h"
#include "emf/keys.hpp"

namespace {
constexpr const char* kSkeleton = "b3e66f3b-7b13-4fa0-a68c-d2a41d8c84d9";  // Enemy_Skeleton_Melee01
constexpr const char* kSpirit = "442ac9a3-ea47-4129-878d-73659f94f9d4";    // Player_Companion_FlyingSkull
constexpr int kAlly = 11;

std::string guid_of(const uint8_t g[16]) {
    char b[40];
    std::snprintf(b, sizeof b, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", g[3], g[2], g[1],
                  g[0], g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    return b;
}
bool has(const std::vector<uint32_t>& v, uint32_t x) { return std::find(v.begin(), v.end(), x) != v.end(); }

// Player 1 (account 0xAA, the host, level 10) at (0, 10, 0); player 2 (0xBB) at (30, 10, 0); 100 mana each.
struct NecroRig : ModRig {
    explicit NecroRig(const std::map<std::string, std::string>& settings = {}, const std::vector<std::string>& also = {})
        : ModRig("necromancer", L"necromancer", settings, [](Fake& f) {
              add_player(f, 1, 0xAA, 0, 10, 0);
              add_player(f, 2, 0xBB, 30, 10, 0);
              for (uint32_t p : {1u, 2u}) {
                  component<ecs::Level>(f, p).dataStorage[0] = 10;
                  add_entity(f, p);
                  f.attrs[{p, EMF_ATTR_MANA}] = 100;
                  f.attrs[{p, EMF_ATTR_MAX_MANA}] = 100;
              }
          }, also) {
        with_attributes(r.api);
        with_teleport(r.api);
    }
    std::string saved() { return slurp(data); }
    // The creatures the game makes for the next `n` spawns (ids 1234, 1235, ...): hostile skeletons, level 1, 50 health.
    void spawnable(int n) {
        for (int i = 0; i < n; ++i) {
            const uint32_t id = r.fake.next_spawn_id + static_cast<uint32_t>(i);
            component<ecs::Faction>(r.fake, id).faction = static_cast<emf::gen::FactionType>(10);
            component<ecs::Level>(r.fake, id).dataStorage[0] = 1;
            r.fake.attrs[{id, EMF_ATTR_HEALTH}] = 50;
            r.fake.attrs[{id, EMF_ATTR_MAX_HEALTH}] = 50;
            add_entity(r.fake, id);
        }
    }
    int faction(uint32_t id) { return r.fake.components[{ecs::Faction::kHash, id}][0]; }
    bool added(uint32_t entity, uint32_t attr, int32_t delta) {
        for (const auto& [e, a, d] : r.fake.attr_adds)
            if (e == entity && a == attr && d == delta) return true;
        return false;
    }
    std::vector<std::pair<uint32_t, uint32_t>> events(const char* name, size_t a_off, size_t b_off) {
        send_events();
        std::vector<std::pair<uint32_t, uint32_t>> out;
        for (const auto& [n, b] : r.fake.emitted) {
            if (n != name) continue;
            uint32_t x = 0, y = 0;
            std::memcpy(&x, b.data() + a_off, 4);
            std::memcpy(&y, b.data() + b_off, 4);
            out.push_back({x, y});
        }
        return out;
    }
    std::vector<std::pair<uint32_t, uint32_t>> guards() {
        return events("StartGuardingEvent", offsetof(ecs::StartGuardingEvent, guardingEntityId),
                      offsetof(ecs::StartGuardingEvent, guardedEntityId));
    }
    std::vector<std::pair<uint32_t, uint32_t>> attacks() {
        return events("AddThreatTargetEvent", offsetof(ecs::AddThreatTargetEvent, attackerId),
                      offsetof(ecs::AddThreatTargetEvent, targetId));
    }
    size_t spawned(const char* guid) {
        size_t n = 0;
        for (const auto& s : r.fake.spawns) n += guid_of(s.guid) == guid;
        return n;
    }
    // Player `p`'s camera at (x, y, z) looking along `yaw_deg` (0 = +z, 90 = +x), level.
    void look(uint32_t p, double x, double y, double z, double yaw_deg) {
        auto& cam = component<ecs::PlayerInput>(r.fake, p).fromClient.camera;
        cam.position.x = static_cast<int64_t>(x * 4294967296.0);
        cam.position.y = static_cast<int64_t>(y * 4294967296.0);
        cam.position.z = static_cast<int64_t>(z * 4294967296.0);
        const double h = yaw_deg * 3.14159265358979 / 360.0;
        cam.orientation.x = 0;
        cam.orientation.y = static_cast<float>(std::sin(h));
        cam.orientation.z = 0;
        cam.orientation.w = static_cast<float>(std::cos(h));
    }
    // A creature at (x, 10, z) of `faction` with health.
    void creature(uint32_t id, double x, double z, int faction, int32_t cur, int32_t max) {
        place(r.fake, id, x, 10, z);
        component<ecs::Faction>(r.fake, id).faction = static_cast<emf::gen::FactionType>(faction);
        r.fake.attrs[{id, EMF_ATTR_HEALTH}] = cur;
        r.fake.attrs[{id, EMF_ATTR_MAX_HEALTH}] = max;
        add_entity(r.fake, id);
    }
};
}  // namespace

TEST_CASE("necromancer: raise costs 25 % mana, spawns a skeleton warrior beside you as your ally at your level; the 4th is refused (cap 3); 1 s cooldown") {
    NecroRig n;
    n.spawnable(4);
    CHECK(n.reply("necromancer.raise").find("rises (1/3)") != std::string::npos);
    REQUIRE(n.r.fake.spawns.size() == 1);
    CHECK(guid_of(n.r.fake.spawns[0].guid) == kSkeleton);
    CHECK(std::abs(n.r.fake.spawns[0].x) + std::abs(n.r.fake.spawns[0].z) <= 3.0);  // beside you
    CHECK(n.added(1, EMF_ATTR_MANA, -25));
    CHECK(n.faction(1234) == kAlly);
    CHECK(n.r.fake.components[{ecs::Level::kHash, 1234}][8] == 10);
    const auto g = n.guards();
    CHECK(std::find(g.begin(), g.end(), std::make_pair(1234u, 1u)) != g.end());
    CHECK(n.reply("necromancer.raise").find("wait") != std::string::npos);  // the cooldown
    CHECK(n.r.fake.spawns.size() == 1);
    n.seconds(1);
    CHECK(n.reply("necromancer.raise").find("rises (2/3)") != std::string::npos);
    n.seconds(1);
    CHECK(n.reply("necromancer.raise").find("rises (3/3)") != std::string::npos);
    n.seconds(1);
    CHECK(n.reply("necromancer.raise").find("at most 3") != std::string::npos);
    CHECK(n.r.fake.spawns.size() == 3);
    CHECK(n.reply("necromancer.dismiss").find("3") != std::string::npos);
    CHECK(has(n.r.fake.destroyed, 1234));
    n.r.fake.attrs[{1, EMF_ATTR_MANA}] = 20;
    CHECK(n.reply("necromancer.raise").find("Not enough mana") != std::string::npos);
    CHECK(n.r.fake.spawns.size() == 3);
}

TEST_CASE("necromancer: a skeleton that dies is gone (count -1) and its body removed while dead; no loot") {
    NecroRig n;
    n.spawnable(1);
    n.reply("necromancer.raise");
    deliver(n.r.fake, "EntityDiedEvent", died_event(1234));
    n.r.fake.attrs[{1234, EMF_ATTR_HEALTH}] = 0;
    n.seconds(3);
    CHECK(has(n.r.fake.destroyed, 1234));
    CHECK(n.reply("necromancer.list").find("0/3") != std::string::npos);
    CHECK(n.noted("A skeleton fell"));
}

TEST_CASE("necromancer: skeletons come back after a reload (the saved count, once) and leave with their owner") {
    NecroRig n;
    n.spawnable(2);
    n.reply("necromancer.raise");
    n.seconds(1);
    n.reply("necromancer.raise");
    n.seconds(2);
    CHECK(n.saved().find("s 0x00000000000000aa 2") != std::string::npos);
    const size_t mana_calls = n.r.fake.attr_adds.size();
    // the same world loads again: the old bodies are gone (not saved by the game)
    n.r.fake.session = 2;
    fire_load(1);
    n.spawnable(4);
    n.seconds(3);
    CHECK(n.spawned(kSkeleton) == 4);  // 2 raised + 2 back after the load
    n.seconds(5);
    CHECK(n.spawned(kSkeleton) == 4);  // never twice
    CHECK(n.r.fake.attr_adds.size() == mana_calls);  // no mana for coming back
    // the owner leaves the world: the skeletons go (the count stays); back when they return
    n.r.fake.destroyed.clear();
    remove_entity(n.r.fake, 1);
    n.seconds(2);
    CHECK(n.r.fake.destroyed.size() == 2);
    CHECK(n.saved().find("s 0x00000000000000aa 2") != std::string::npos);
    add_player(n.r.fake, 1, 0xAA, 0, 10, 0);
    add_entity(n.r.fake, 1);
    n.seconds(2);
    CHECK(n.spawned(kSkeleton) == 6);
}

TEST_CASE("necromancer: a skeleton guarding someone else for 3 checks is not ours; leash 40 m / 3 s, 80 m at once") {
    NecroRig n;
    n.spawnable(2);
    n.reply("necromancer.raise");
    n.seconds(1);
    place(n.r.fake, 1234, 50, 10, 0);
    n.seconds(1);
    CHECK(n.r.fake.teleports.empty());  // not before 3 s
    n.seconds(3);
    REQUIRE(!n.r.fake.teleports.empty());
    CHECK(n.r.fake.teleports.back().entity == 1234);
    place(n.r.fake, 1234, 1, 10, 0);
    n.seconds(4);
    const size_t before = n.r.fake.teleports.size();
    place(n.r.fake, 1234, 90, 10, 0);
    n.seconds(1);
    CHECK(n.r.fake.teleports.size() == before + 1);  // at once
    component<ecs::DynamicEnemy01BehaviorState>(n.r.fake, 1234).guardEntityId.id = 999;
    n.seconds(4);
    CHECK(n.reply("necromancer.list").find("0/3") != std::string::npos);
    n.r.fake.destroyed.clear();
    n.reply("necromancer.dismiss");
    CHECK(!has(n.r.fake.destroyed, 1234));
}

TEST_CASE("necromancer: a skeleton's hit that leaves a foe at 20 % hands the killing blow to its owner, never on bosses or the player side") {
    NecroRig n;
    n.spawnable(1);
    n.reply("necromancer.raise");
    for (uint32_t id : {60u, 61u, 62u}) {
        place(n.r.fake, id, 0, 10, 5);
        add_entity(n.r.fake, id);
        n.r.fake.attrs[{id, EMF_ATTR_HEALTH}] = 20;
        n.r.fake.attrs[{id, EMF_ATTR_MAX_HEALTH}] = 100;
    }
    component<ecs::Faction>(n.r.fake, 60).faction = static_cast<emf::gen::FactionType>(6);
    component<ecs::Faction>(n.r.fake, 61).faction = static_cast<emf::gen::FactionType>(6);
    component<ecs::Boss>(n.r.fake, 61);
    component<ecs::Faction>(n.r.fake, 62).faction = static_cast<emf::gen::FactionType>(7);
    for (uint32_t id : {60u, 61u, 62u}) deliver(n.r.fake, "HitEvent", hit_event(1234, id));
    n.seconds(1);
    REQUIRE(n.r.fake.damages.size() == 1);
    CHECK(std::get<0>(n.r.fake.damages[0]) == 1u);   // the owner lands it
    CHECK(std::get<1>(n.r.fake.damages[0]) == 60u);  // on the plain foe
}

TEST_CASE("necromancer: spirits cost 8 % mana, two per cast, each sent at the target; they are gone after 6 s") {
    NecroRig n;
    n.look(1, 0, 10.5, 0, 0);
    n.creature(60, 0, 6, 10 /* Skeleton */, 50, 50);  // a foe under the crosshair
    n.spawnable(2);
    CHECK(n.reply("necromancer.spirits").find("Raging spirits (2)") != std::string::npos);
    CHECK(n.spawned(kSpirit) == 2);
    CHECK(n.added(1, EMF_ATTR_MANA, -8));
    CHECK(n.faction(1234) == kAlly);
    const auto a = n.attacks();
    CHECK(std::find(a.begin(), a.end(), std::make_pair(1234u, 60u)) != a.end());
    CHECK(std::find(a.begin(), a.end(), std::make_pair(1235u, 60u)) != a.end());
    n.r.fake.attrs[{60, EMF_ATTR_HEALTH}] = 10;  // a spirit's hit leaves it at 20 %: the killing blow is the caster's
    deliver(n.r.fake, "HitEvent", hit_event(1234, 60));
    n.seconds(1);
    CHECK(n.r.fake.damages.size() == 1);
    n.seconds(4);
    CHECK(!has(n.r.fake.destroyed, 1234));
    n.seconds(2);
    CHECK(has(n.r.fake.destroyed, 1234));
    CHECK(has(n.r.fake.destroyed, 1235));
}

TEST_CASE("necromancer: at most 10 spirits: the oldest go first; never saved; no target = guard the owner; never a player, ally or farm animal") {
    NecroRig n;
    n.look(1, 0, 10.5, 0, 180);                 // looking along -z
    n.creature(70, 0, -5, 12 /* FarmAnimal */, 30, 30);  // under the crosshair, but a farm animal
    n.creature(71, 2, -3, 7 /* Player */, 30, 30);
    n.spawnable(12);
    CHECK(n.reply("necromancer.spirits").find("Raging spirits (2)") != std::string::npos);
    CHECK(n.attacks().empty());                 // no foe: they guard you
    const auto g = n.guards();
    CHECK(std::find(g.begin(), g.end(), std::make_pair(1234u, 1u)) != g.end());
    for (int i = 0; i < 5; ++i) n.reply("necromancer.spirits");  // 12 cast, 10 at most
    CHECK(n.spawned(kSpirit) == 12);
    CHECK(has(n.r.fake.destroyed, 1234));
    CHECK(has(n.r.fake.destroyed, 1235));
    CHECK(!has(n.r.fake.destroyed, 1236));
    n.seconds(7);
    for (uint32_t id = 1234; id < 1246; ++id) CHECK(has(n.r.fake.destroyed, id));
    CHECK(n.saved().empty());  // only skeletons are saved; spirits never
    CHECK(n.reply("necromancer.list").find("0/3") != std::string::npos);  // spirits are not skeletons
}

namespace {
std::map<uint32_t, bool> g_keys;  // the fake keyboard for these tests: vk -> held
// Presses `vk` once on the server world (this game hosts) and lets go.
void press_key(ModRig& m, uint32_t vk) {
    m.host->set_key_reader([](uint32_t k) { return g_keys[k]; });
    g_keys[vk] = true;
    m.host->tick(kServer);
    g_keys[vk] = false;
    m.host->tick(kServer);
}
int item_at(const std::vector<std::string>& items, const std::string& start) {
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].rfind(start, 0) == 0) return static_cast<int>(i);
    return -1;
}
constexpr const char* kWolfTemplate = "478381c5-585f-4264-b373-dbb1ea596eca";
}  // namespace

TEST_CASE("necromancer: F2 opens the summons wheel with the Undead section; Raise Skeleton picked raises one; the HUD counts them") {
    NecroRig n;
    n.spawnable(1);
    press_key(n, VK_F2);
    REQUIRE(n.r.fake.wheels.size() == 1);
    const auto w = n.r.fake.wheels[0];
    CHECK(w.hold == VK_F2);
    for (const char* a : {"Raise Skeleton (0/3)", "Raging Spirits", "Gather", "Attack"})
        CHECK_MESSAGE(item_at(w.items, a) >= 0, a);
    CHECK(item_at(w.items, "Dismiss") < 0);  // no undead: nothing to dismiss
    w.cb(kClient, item_at(w.items, "Raise Skeleton"), w.user);  // the wheel answers on the client world
    n.seconds(1);                                               // the raise runs on the server's next step
    CHECK(n.spawned(kSkeleton) == 1);
    CHECK(n.noted("A skeleton rises (1/3)"));
    n.seconds(1);
    CHECK(n.hud("out").find("Skeletons 1/3") != std::string::npos);
}

TEST_CASE("beastmaster + necromancer: one wheel with both sections; a pick in either runs that mod; shared_wheel off keeps a mod's own") {
    NecroRig n({}, {"beastmaster"});
    REQUIRE(n.host->status("beastmaster") == "running");
    n.reply("beastmaster.give wolf");
    n.spawnable(2);
    n.seconds(2);
    press_key(n, VK_F2);
    REQUIRE(n.r.fake.wheels.size() == 1);
    const auto w = n.r.fake.wheels[0];
    CHECK(item_at(w.items, "Undead: Raise Skeleton") >= 0);
    const int wolf = item_at(w.items, "Beasts: Call Wolf");
    REQUIRE(wolf >= 0);
    w.cb(kClient, wolf, w.user);
    n.seconds(1);
    REQUIRE(n.r.fake.spawns.size() == 1);
    CHECK(guid_of(n.r.fake.spawns[0].guid) == kWolfTemplate);  // Beastmaster ran it
    press_key(n, VK_F3);
    REQUIRE(n.r.fake.wheels.size() == 2);
    CHECK(n.r.fake.wheels[1].hold == VK_F3);
    CHECK(item_at(n.r.fake.wheels[1].items, "Undead: Raise Skeleton") >= 0);
    n.r.fake.wheels[1].cb(kClient, item_at(n.r.fake.wheels[1].items, "Undead: Raise Skeleton"), n.r.fake.wheels[1].user);
    n.seconds(1);
    CHECK(n.spawned(kSkeleton) == 1);  // Necromancer ran it
    // Necromancer's own wheel: its key shows only its section, and Beastmaster's wheel no longer shows it
    n.r.fake.settings["necromancer.shared_wheel"] = "false";
    n.seconds(2);
    press_key(n, VK_F2);
    REQUIRE(n.r.fake.wheels.size() == 3);
    CHECK(item_at(n.r.fake.wheels[2].items, "Raise Skeleton") >= 0);
    CHECK(item_at(n.r.fake.wheels[2].items, "Beasts:") < 0);
    press_key(n, VK_F3);
    REQUIRE(n.r.fake.wheels.size() == 4);
    CHECK(item_at(n.r.fake.wheels[3].items, "Undead:") < 0);
    CHECK(item_at(n.r.fake.wheels[3].items, "Tame") >= 0);
}

TEST_CASE("beastmaster + necromancer: the wheel's labels are short and every line fits its slice (a page of 8, the overlay's font)") {
    NecroRig n({}, {"beastmaster"});
    for (const char* s : {"wolf", "boar", "sabretooth", "sabretooth", "hound", "aurochs"})
        n.reply(std::string("beastmaster.give ") + s);
    n.spawnable(3);
    const std::string wolf = n.reply("beastmaster.call Wolf");
    CHECK_MESSAGE(wolf.find("comes") != std::string::npos, wolf);
    n.seconds(11);
    CHECK(n.reply("beastmaster.call Sabretooth 2").find("comes") != std::string::npos);
    deliver(n.r.fake, "EntityDiedEvent", died_event(1234));  // the wolf falls: it rests, and comes back after
    CHECK(n.reply("necromancer.raise").find("rises") != std::string::npos);
    n.seconds(2);
    press_key(n, VK_F2);
    REQUIRE(n.r.fake.wheels.size() == 1);
    CHECK(item_at(n.r.fake.wheels[0].items, "Beasts: Dismiss Wolf (rests") >= 0);
    CHECK(item_at(n.r.fake.wheels[0].items, "Beasts: Dismiss Sabretooth 2") >= 0);
    for (std::string label : n.r.fake.wheels[0].items) {
        const size_t colon = label.find(": ");
        if (colon != std::string::npos && colon <= 6) label = label.substr(colon + 2);  // "Beasts: ", "Undead: "
        const size_t cut = label.find(" (");
        const std::string name = label.substr(0, cut);
        const std::string detail = cut == std::string::npos ? "" : label.substr(cut + 1);
        CHECK_MESSAGE(name.size() <= 20, label);
        CHECK_MESSAGE(detail.size() <= 13, label);
    }
    // as the overlay draws them: Consolas at height / 54 pixels (0.55 of that a character), wrapped by the wheel's code
    for (float h : {1080.0f, 2160.0f}) {
        const float width = emf::core::wheel_label_width(ImVec2(h * 16.0f / 9.0f, h), 8);
        const auto measure = [h](const std::string& s) { return 0.55f * (h / 54.0f) * static_cast<float>(s.size()); };
        for (const auto& label : n.r.fake.wheels[0].items) {
            const auto lines = emf::core::wheel_label_layout(label, width, measure);
            CHECK_MESSAGE(lines.size() <= 4, label);
            for (const auto& l : lines) CHECK_MESSAGE(measure(l.text) <= width, label << ": " << l.text);
        }
    }
}

namespace {
// Enshrouded's own keyboard keys (Preset A, game 1.0): every letter (WASD, C sneak, E, F, Q, R, T, G, X, Y, Z, U, L, P,
// and I, V crafting, B backpack, N, M, H, J, O, K for its screens), the digits (action bar 1-8, building shapes 9 and
// 0), Tab, Space, Shift, Ctrl, Alt, Enter, Esc, F1, PageUp, PageDown and Delete; plus the key under Esc (EMF's console).
bool game_key(uint32_t vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return true;
    for (uint32_t k : {VK_TAB, VK_SPACE, VK_SHIFT, VK_CONTROL, VK_MENU, VK_RETURN, VK_ESCAPE, VK_F1, VK_PRIOR, VK_NEXT,
                       VK_DELETE, VK_OEM_3})
        if (vk == k) return true;
    return false;
}
}  // namespace

TEST_CASE("beastmaster + necromancer: the wheel keys are not the game's own keys (B was its backpack, V its crafting)") {
    std::vector<uint32_t> keys;
    for (const char* mod : {"beastmaster", "necromancer"}) {
        const auto m = emf::core::parse_manifest(
            slurp((std::filesystem::path(EMF_SOURCE_DIR) / "mods" / mod / "mod.json").wstring()));
        REQUIRE_MESSAGE(m.manifest, m.error);
        std::string def;
        for (const auto& d : m.manifest->settings)
            if (d.key == "wheel_key") def = d.default_value;
        const uint32_t vk = emf::keys::parse_key(std::wstring(def.begin(), def.end()));
        CHECK_MESSAGE(vk != 0, mod);
        CHECK_MESSAGE(!game_key(vk), mod << ": " << def);
        keys.push_back(vk);
    }
    CHECK(keys[0] != keys[1]);  // each mod's key (one wheel or two)
    // the player docs (the zip's README.txt too) name the new keys, and that EMF builds the packs (no EML needed)
    for (const char* doc : {"README_EMF.md", "package/README.txt", "mods/beastmaster/README.md", "mods/necromancer/README.md"}) {
        std::string text = slurp((std::filesystem::path(EMF_SOURCE_DIR) / doc).wstring());
        std::transform(text.begin(), text.end(), text.begin(), [](char c) { return static_cast<char>(std::tolower(c)); });
        for (const char* old : {"hold b ", "hold v ", "hold b)", "hold v)", "hold **b**", "hold **v**", "b and v", "v and b"})
            CHECK_MESSAGE(text.find(old) == std::string::npos, (std::string(doc) + ": " + old));
        CHECK_MESSAGE(text.find("f3") != std::string::npos, std::string(doc));
        CHECK_MESSAGE(text.find("f2") != std::string::npos, std::string(doc));
        CHECK_MESSAGE(text.find("emf builds") != std::string::npos, std::string(doc));
        for (const char* old : {"install eml or shroudforge first", "install eml (or shroudforge) first", "which need eml",
                                "eml (or shroudforge) installed first", "install eml first"})
            CHECK_MESSAGE(text.find(old) == std::string::npos, (std::string(doc) + ": " + old));
    }
    // the old keys open nothing now
    NecroRig n({}, {"beastmaster"});
    press_key(n, 'B');
    press_key(n, 'V');
    CHECK(n.r.fake.wheels.empty());
}

TEST_CASE("necromancer: /necromancer <word> for guests acts on their own skeletons") {
    NecroRig n;
    n.spawnable(1);
    CHECK(n.reply("necromancer raise", 2, 0xBB).find("rises (1/3)") != std::string::npos);
    REQUIRE(n.r.fake.spawns.size() == 1);
    CHECK(std::abs(n.r.fake.spawns[0].x - 30.0) <= 3.0);  // beside the guest
    CHECK(n.added(2, EMF_ATTR_MANA, -25));
    CHECK(n.reply("necromancer list", 2, 0xBB).find("1/3") != std::string::npos);
    CHECK(n.reply("necromancer list").find("0/3") != std::string::npos);  // the host's own
    const std::string help = n.reply("necromancer", 2, 0xBB);
    for (const char* word : {"raise", "spirits", "gather", "attack", "dismiss", "list"})
        CHECK_MESSAGE(help.find(word) != std::string::npos, word);
}

TEST_CASE("necromancer: dismiss one kind or all: /necromancer dismiss skeleton sends the newest skeleton back, no word or all every one; the wheel has both") {
    NecroRig n;
    n.spawnable(3);
    CHECK(n.reply("necromancer.raise").find("rises (1/3)") != std::string::npos);
    n.seconds(1);
    CHECK(n.reply("necromancer.raise").find("rises (2/3)") != std::string::npos);
    n.seconds(1);
    CHECK(n.reply("necromancer.raise lich").find("rises (1/1)") != std::string::npos);
    n.seconds(1);
    press_key(n, VK_F2);
    REQUIRE(n.r.fake.wheels.size() == 1);
    const auto w = n.r.fake.wheels[0];
    for (const char* a : {"Dismiss Skeleton", "Dismiss Lich", "Dismiss all"}) CHECK_MESSAGE(item_at(w.items, a) >= 0, a);
    CHECK(item_at(w.items, "Dismiss Bone Golem") < 0);  // none to dismiss
    CHECK(n.reply("necromancer.dismiss skeleton").find("A skeleton returns to the ground (1/3)") != std::string::npos);
    CHECK(has(n.r.fake.destroyed, 1235));  // the newest skeleton
    CHECK(!has(n.r.fake.destroyed, 1234));
    CHECK(n.reply("necromancer.list").find("Skeletons 1/3, Liches 1/1") != std::string::npos);
    CHECK(n.reply("necromancer.dismiss golem").find("You have no bone golems") != std::string::npos);
    CHECK(n.reply("necromancer.dismiss dragon").find("Dismiss what?") != std::string::npos);
    w.cb(kClient, item_at(w.items, "Dismiss Lich"), w.user);
    n.seconds(1);
    CHECK(n.noted("A lich returns to the ground (0/1)"));
    CHECK(has(n.r.fake.destroyed, 1236));
    n.seconds(2);
    CHECK(n.reply("necromancer.list").find("Skeletons 1/3, Liches 0/1") != std::string::npos);  // not raised again
    CHECK(n.reply("necromancer.dismiss").find("1 undead returns to the ground") != std::string::npos);  // no word: all
    CHECK(n.reply("necromancer.dismiss all").find("You have no undead") != std::string::npos);
    bool told = false;
    for (const auto& c : n.r.fake.commands)
        if (c.name == "necromancer.dismiss") told = c.help.find("all") != std::string::npos && c.help.find("skeleton") != std::string::npos;
    CHECK(told);
    // a guest dismisses their own
    n.spawnable(1);
    CHECK(n.reply("necromancer raise", 2, 0xBB).find("rises (1/3)") != std::string::npos);
    CHECK(n.reply("necromancer dismiss skeleton", 2, 0xBB).find("returns to the ground (0/3)") != std::string::npos);
}

TEST_CASE("necromancer: a wheel pick dismisses what was shown, though new undead moved the items meanwhile (shared and own wheel)") {
    for (const char* shared : {"true", "false"}) {
        NecroRig n({{"shared_wheel", shared}});
        n.spawnable(2);
        CHECK(n.reply("necromancer.raise lich").find("rises (1/1)") != std::string::npos);
        n.seconds(2);
        press_key(n, VK_F2);
        REQUIRE(n.r.fake.wheels.size() == 1);
        const auto w = n.r.fake.wheels[0];
        const int lich = item_at(w.items, "Dismiss Lich");
        REQUIRE(lich >= 0);
        CHECK(n.reply("necromancer.raise").find("rises (1/3)") != std::string::npos);  // "Dismiss Skeleton" comes first now
        n.seconds(2);
        w.cb(kClient, lich, w.user);
        n.seconds(1);
        const std::string list = n.reply("necromancer.list");
        CHECK_MESSAGE(list.find("Skeletons 1/3, Liches 0/1") != std::string::npos, shared << ": " << list);
    }
}

TEST_CASE("necromancer: a wheel pick runs the item that was shown, though its label changed meanwhile") {
    NecroRig n;
    n.spawnable(2);
    press_key(n, VK_F2);
    REQUIRE(n.r.fake.wheels.size() == 1);
    const auto w = n.r.fake.wheels[0];
    const int raise = item_at(w.items, "Raise Skeleton (0/3)");
    REQUIRE(raise >= 0);
    CHECK(n.reply("necromancer.raise").find("rises (1/3)") != std::string::npos);  // the label is "(1/3)" now
    n.seconds(2);
    w.cb(kClient, raise, w.user);
    n.seconds(1);
    CHECK(n.spawned(kSkeleton) == 2);
}

// ---- edge cases ----

TEST_CASE("necromancer: a spirit whose id the game gave to another entity before its 6 s are up is not destroyed") {
    NecroRig n;
    n.look(1, 0, 10.5, 0, 0);
    n.creature(60, 0, 6, 10, 50, 50);
    n.spawnable(2);
    n.reply("necromancer.spirits");
    // spirit 1234 left the world without a death; the game gave its id to a player-side NPC
    component<ecs::Faction>(n.r.fake, 1234).faction = static_cast<emf::gen::FactionType>(7);
    n.seconds(7);
    CHECK(!has(n.r.fake.destroyed, 1234));
    CHECK(has(n.r.fake.destroyed, 1235));
}

TEST_CASE("necromancer: spirits and attack orders go only for monsters: never base objects (Environment, HitAll, HitOthers), allies or players") {
    NecroRig n;
    n.look(1, 0, 10.5, 0, 180);  // nothing under the crosshair
    n.creature(80, 0, 4, 1 /* HitAll: a proximity mine */, 20, 20);
    n.creature(81, 3, 0, 0 /* Environment: a training dummy */, 50, 50);
    n.creature(82, -2, 0, 9 /* HitOthers */, 50, 50);
    n.creature(83, 0, 2, kAlly, 50, 50);
    n.spawnable(4);
    CHECK(n.reply("necromancer.spirits").find("they guard you") != std::string::npos);
    CHECK(n.attacks().empty());
    n.creature(84, 0, 10, 6 /* WildBeast */, 50, 50);  // a monster 10 m away
    CHECK(n.reply("necromancer.spirits").find("they guard you") == std::string::npos);
    const auto a = n.attacks();
    REQUIRE(!a.empty());
    for (const auto& [from, to] : a) CHECK(to == 84u);
    // the attack order's fallback (what hit you) also refuses a base object
    deliver(n.r.fake, "HitEvent", hit_event(80, 1));  // the mine hit you
    n.seconds(1);
    CHECK(n.reply("necromancer.attack").find("Look at a creature") != std::string::npos);
}

TEST_CASE("necromancer: spirits are gone after 6 s when their target dies or their owner leaves; a new world keeps no old spirit") {
    NecroRig n;
    n.look(1, 0, 10.5, 0, 0);
    n.creature(60, 0, 6, 10, 50, 50);
    n.spawnable(4);
    n.reply("necromancer.spirits");  // 1234, 1235
    n.r.fake.attrs[{60, EMF_ATTR_HEALTH}] = 0;  // the target dies
    deliver(n.r.fake, "EntityDiedEvent", died_event(60));
    remove_entity(n.r.fake, 1);  // and the owner leaves
    n.seconds(7);
    CHECK(has(n.r.fake.destroyed, 1234));
    CHECK(has(n.r.fake.destroyed, 1235));
    add_player(n.r.fake, 1, 0xAA, 0, 10, 0);
    add_entity(n.r.fake, 1);
    n.r.fake.attrs[{1, EMF_ATTR_MANA}] = 100;
    n.r.fake.attrs[{1, EMF_ATTR_MAX_MANA}] = 100;
    n.reply("necromancer.spirits");  // 1236, 1237
    n.r.fake.session = 2;            // another world: its ids are not ours, nothing of the old one is touched
    fire_load(1);
    n.r.fake.destroyed.clear();
    n.seconds(8);
    CHECK(!has(n.r.fake.destroyed, 1236));
    CHECK(!has(n.r.fake.destroyed, 1237));
}

TEST_CASE("necromancer: two casts in the same step cannot spend mana that is not there") {
    NecroRig n;
    n.r.fake.attrs[{1, EMF_ATTR_MANA}] = 10;
    n.spawnable(4);
    CHECK(n.reply("necromancer.spirits").find("Raging spirits") != std::string::npos);
    CHECK(n.reply("necromancer.spirits").find("Not enough mana") != std::string::npos);
    CHECK(n.spawned(kSpirit) == 2);
    CHECK(n.reply("necromancer.raise").find("Not enough mana") != std::string::npos);
}

// ---- kinds (the undead pack): the Lich ----
namespace {
constexpr const char* kLich = "c4e1a7d2-6b3f-4d8e-9a10-5f2b7c3d9e02";
}

TEST_CASE("necromancer: a lich is raised as its own kind (cap 1, 40 % mana, 1.5x health); skeleton warriors keep their 3") {
    NecroRig n;
    n.spawnable(5);
    CHECK(n.reply("necromancer.raise lich").find("A lich rises (1/1)") != std::string::npos);
    REQUIRE(n.r.fake.spawns.size() == 1);
    CHECK(guid_of(n.r.fake.spawns[0].guid) == kLich);
    CHECK(n.added(1, EMF_ATTR_MANA, -40));
    auto& h = component<ecs::Health>(n.r.fake, 1234);
    h.dataStorage[2] = 60;
    n.seconds(1);
    CHECK(h.dataStorage[3] == 30);  // 60 + 30 = 1.5 x 60
    CHECK(n.reply("necromancer raise lich").find("at most 1") != std::string::npos);
    n.r.fake.attrs[{1, EMF_ATTR_MANA}] = 100;
    for (int i = 0; i < 3; ++i) {
        n.seconds(1);
        CHECK(n.reply("necromancer.raise").find("A skeleton rises") != std::string::npos);
    }
    CHECK(n.reply("necromancer.list").find("Skeletons 3/3, Liches 1/1") != std::string::npos);
    n.seconds(1);
    CHECK(n.hud("out").find("Skeletons 3/3, Liches 1/1") != std::string::npos);
}

TEST_CASE("necromancer: old saves (s lines) load as skeleton warriors; kinds are saved and come back") {
    NecroRig n;
    write(n.data, "s 0x00000000000000aa 2\n");
    n.r.fake.session = 2;
    fire_load(1);
    n.spawnable(4);
    n.seconds(2);
    CHECK(n.spawned(kSkeleton) == 2);
    CHECK(n.reply("necromancer.list").find("Skeletons 2/3") != std::string::npos);
    n.seconds(1);
    CHECK(n.reply("necromancer.raise lich").find("rises") != std::string::npos);
    n.seconds(2);
    const std::string saved = n.saved();
    CHECK(saved.find("s 0x00000000000000aa 2") != std::string::npos);
    CHECK(saved.find("k 0x00000000000000aa lich 1") != std::string::npos);
    n.r.fake.session = 3;
    fire_load(1);
    n.spawnable(4);
    n.seconds(2);
    CHECK(n.spawned(kLich) == 2);  // raised once, back once after the load
}

TEST_CASE("necromancer: without the undead pack the lich is refused with a note, and a saved lich does not loop") {
    NecroRig n;
    n.r.fake.spawn_fails = true;
    CHECK(n.reply("necromancer.raise lich").find("creature packs") != std::string::npos);
    CHECK(n.reply("necromancer.list").find("Liches 0/1") != std::string::npos);
    CHECK(!n.added(1, EMF_ATTR_MANA, -40));
    write(n.data, "s 0x00000000000000aa 0\nk 0x00000000000000aa lich 1\n");
    n.r.fake.session = 2;
    fire_load(1);
    n.r.fake.log.clear();
    n.seconds(5);
    size_t notes = 0;
    for (const auto& x : n.r.fake.notes) notes += x.find("lich") != std::string::npos;
    CHECK(notes == 1);
    CHECK(n.reply("necromancer.list").find("Liches 1/1") != std::string::npos);  // kept: it comes back with the pack
}

TEST_CASE("necromancer: a lich or golem that cannot rise says it comes with the EMF creature packs and how to install them") {
    NecroRig n;
    n.r.fake.spawn_fails = true;  // no lich in this game: no pack, or its content was not built
    for (const char* kind : {"lich", "golem"}) {
        const std::string raise = n.reply(std::string("necromancer.raise ") + kind);
        CHECK_MESSAGE(raise.find("EMF creature packs") != std::string::npos, raise);
        CHECK_MESSAGE(raise.find("Install them with EMF.exe") != std::string::npos, raise);
        CHECK_MESSAGE(raise.find("need EML") == std::string::npos, raise);  // EMF builds the packs itself (0.4.0)
        CHECK_MESSAGE(raise.find("Install EML") == std::string::npos, raise);
        CHECK_MESSAGE(raise.find("emf_necromancer") == std::string::npos, raise);  // no folder names
    }
    write(n.data, "k 0x00000000000000aa lich 1\n");
    n.r.fake.session = 2;
    fire_load(1);
    n.seconds(5);
    std::string note;
    for (const auto& x : n.r.fake.notes)
        if (x.find("ich") != std::string::npos) note = x;
    CHECK(note.find("stays saved") != std::string::npos);
    CHECK(note.find("EMF creature packs") != std::string::npos);
    CHECK(note.find("EML") == std::string::npos);
}

// ---- kinds (the undead pack): the Bone Golem ----
namespace {
constexpr const char* kGolem = "e7b3c9d1-2a4f-4e6b-8c15-9d2f4a6b8e02";
}

TEST_CASE("necromancer: a bone golem is its own kind (cap 1, 60 % mana), raised by name, saved and on the wheel") {
    NecroRig n;
    n.spawnable(4);
    CHECK(n.reply("necromancer.raise golem").find("A bone golem rises (1/1)") != std::string::npos);
    REQUIRE(n.r.fake.spawns.size() == 1);
    CHECK(guid_of(n.r.fake.spawns[0].guid) == kGolem);
    CHECK(n.added(1, EMF_ATTR_MANA, -60));
    CHECK(n.faction(1234) == kAlly);
    n.seconds(1);
    CHECK(n.reply("necromancer raise bone golem").find("at most 1") != std::string::npos);  // its two-word name
    CHECK(n.reply("necromancer.list").find("Bone Golems 1/1") != std::string::npos);
    n.seconds(2);
    CHECK(n.saved().find("k 0x00000000000000aa golem 1") != std::string::npos);
    press_key(n, VK_F2);
    REQUIRE(n.r.fake.wheels.size() == 1);
    CHECK(item_at(n.r.fake.wheels[0].items, "Raise Bone Golem (1/1)") >= 0);  // its own section only: no "Undead: " prefix
    CHECK(n.reply("necromancer.raise dragon").find("Raise what?") != std::string::npos);
}

TEST_CASE("necromancer: a saved lich whose first restores fail for a moment (no position yet, one refused spawn) comes back, with no note") {
    NecroRig n;
    write(n.data, "k 0x00000000000000aa lich 1\n");
    n.r.fake.session = 2;
    n.r.fake.components.erase(std::make_pair(ecs::CurrentTransform::kHash, 1u));  // the player is not placed yet
    fire_load(1);
    n.seconds(2);
    place(n.r.fake, 1, 0, 10, 0);
    n.r.fake.spawn_fails = true;  // then the game refuses one spawn
    n.seconds(1);
    n.r.fake.spawn_fails = false;
    n.spawnable(2);
    n.seconds(2);
    CHECK(n.spawned(kLich) == 1);
    size_t notes = 0;
    for (const auto& x : n.r.fake.notes) notes += x.find("lich") != std::string::npos;
    CHECK(notes == 0);
    CHECK(n.reply("necromancer.list").find("Liches 1/1") != std::string::npos);
}

TEST_CASE("necromancer: a bone golem keeps 2x the game's base health") {
    NecroRig n;
    n.spawnable(1);
    CHECK(n.reply("necromancer.raise golem").find("rises") != std::string::npos);
    auto& h = component<ecs::Health>(n.r.fake, 1234);
    h.dataStorage[2] = 120;
    n.seconds(1);
    CHECK(h.dataStorage[3] == 120);  // 120 + 120 = 2 x 120
}

// ---- stepping aside: the game keeps a no-build zone around every creature with the enemy AI (add_no_buildzones_enemy),
// so a skeleton at your side blocks building and placing there ----
TEST_CASE("necromancer: the undead step aside while their owner builds and come back after, free; a raise meanwhile waits") {
    NecroRig n;
    n.spawnable(6);
    CHECK(n.reply("necromancer.raise").find("rises (1/3)") != std::string::npos);
    n.seconds(1);
    CHECK(n.reply("necromancer.raise").find("rises (2/3)") != std::string::npos);
    CHECK(n.reply("necromancer raise", 2, 0xBB).find("rises (1/3)") != std::string::npos);  // a guest's, 1236
    n.seconds(2);
    component<ecs::NetworkCursor>(n.r.fake, 1).currentBuildingItemId.value = 0x77;  // the host builds (the replicated copy)
    n.seconds(2);
    CHECK(has(n.r.fake.destroyed, 1234));
    CHECK(has(n.r.fake.destroyed, 1235));
    CHECK(!has(n.r.fake.destroyed, 1236));  // the guest does not build
    size_t told = 0;
    for (const auto& x : n.r.fake.notes) told += x.find("step aside while you build") != std::string::npos;
    CHECK(told == 1);
    CHECK(n.reply("necromancer.list").find("Skeletons 2/3") != std::string::npos);  // kept: they come back
    CHECK(n.hud("out").find("aside") != std::string::npos);
    CHECK(n.reply("necromancer.raise").find("while you build") != std::string::npos);
    CHECK(n.reply("necromancer.gather").find("while you build") != std::string::npos);
    n.seconds(5);
    CHECK(n.spawned(kSkeleton) == 3);
    component<ecs::NetworkCursor>(n.r.fake, 1).currentBuildingItemId.value = 0;
    n.seconds(5);
    CHECK(n.spawned(kSkeleton) == 5);  // both back, free
    size_t paid = 0;
    for (const auto& [e, a, d] : n.r.fake.attr_adds) paid += e == 1 && a == EMF_ATTR_MANA && d == -25;
    CHECK(paid == 2);
    CHECK(n.hud("out").find("aside") == std::string::npos);
}

TEST_CASE("necromancer: /necromancer aside sends the undead away until typed again; Gather or a raise brings them back") {
    NecroRig n;
    n.spawnable(6);
    CHECK(n.reply("necromancer.aside").find("You have no undead") != std::string::npos);
    CHECK(n.reply("necromancer.raise").find("rises (1/3)") != std::string::npos);
    n.seconds(2);
    CHECK(n.reply("necromancer.aside").find("step aside") != std::string::npos);
    n.seconds(2);
    CHECK(has(n.r.fake.destroyed, 1234));
    n.seconds(5);
    CHECK(n.spawned(kSkeleton) == 1);
    CHECK(n.reply("necromancer.gather").find("come back (1)") != std::string::npos);
    n.seconds(2);
    CHECK(n.spawned(kSkeleton) == 2);
    CHECK(n.reply("necromancer.aside").find("step aside") != std::string::npos);
    n.seconds(2);
    CHECK(n.reply("necromancer.aside").find("come back") != std::string::npos);
    n.seconds(2);
    CHECK(n.spawned(kSkeleton) == 3);
    CHECK(n.reply("necromancer.aside").find("step aside") != std::string::npos);
    n.seconds(2);
    CHECK(n.reply("necromancer.raise").find("rises (2/3)") != std::string::npos);  // a raise ends it
    n.seconds(2);
    CHECK(n.spawned(kSkeleton) == 5);
    CHECK(n.reply("necromancer").find("aside") != std::string::npos);
}

TEST_CASE("necromancer: while aside by the command the wheel's Gather says Come back and the list says how; dismissing all ends the aside") {
    NecroRig n;
    n.spawnable(4);
    CHECK(n.reply("necromancer.raise").find("rises (1/3)") != std::string::npos);
    n.seconds(2);
    CHECK(n.reply("necromancer.aside").find("step aside") != std::string::npos);
    n.seconds(2);
    CHECK(n.hud("out").rfind("Undead (aside): Skeletons 1/3", 0) == 0);  // "(aside)" goes with "Undead"
    CHECK(n.reply("necromancer.list").find("Gather") != std::string::npos);
    press_key(n, VK_F2);
    REQUIRE(n.r.fake.wheels.size() == 1);
    CHECK(item_at(n.r.fake.wheels[0].items, "Come back") >= 0);
    CHECK(item_at(n.r.fake.wheels[0].items, "Gather") < 0);
    CHECK(n.reply("necromancer.dismiss").find("returns to the ground") != std::string::npos);
    n.seconds(1);
    CHECK(n.reply("necromancer.raise").find("rises (1/3)") != std::string::npos);
    n.seconds(2);
    CHECK(!has(n.r.fake.destroyed, 1235));  // the new one stays: no aside left over
    CHECK(n.hud("out").rfind("Undead: Skeletons 1/3", 0) == 0);
}

TEST_CASE("necromancer: the undead also step aside while another player builds within 20 m of them; aside off while building says when") {
    NecroRig n;
    n.spawnable(4);
    CHECK(n.reply("necromancer.raise").find("rises (1/3)") != std::string::npos);
    n.seconds(2);
    component<ecs::ServerCursor>(n.r.fake, 2).currentBuildingItemInstance.itemId.value = 0x55;  // the guest, 30 m away
    n.seconds(3);
    CHECK(!has(n.r.fake.destroyed, 1234));
    component<ecs::ServerCursor>(n.r.fake, 2).currentBuildingItemInstance.itemId.value = 0;
    n.seconds(4);
    place(n.r.fake, 2, 10, 10, 0);  // 10 m away, and builds again
    component<ecs::ServerCursor>(n.r.fake, 2).currentBuildingItemInstance.itemId.value = 0x55;
    n.r.fake.tick += 1;  // noticed in the next game step
    n.host->tick(kServer);
    CHECK(has(n.r.fake.destroyed, 1234));
    size_t told = 0;
    for (const auto& x : n.r.fake.notes) told += x.find("a player builds near them") != std::string::npos;
    CHECK(told == 1);
    CHECK(n.reply("necromancer.raise").find("while a player builds near you") != std::string::npos);
    CHECK(n.reply("necromancer.aside").find("step aside") != std::string::npos);
    const std::string off = n.reply("necromancer.aside");
    CHECK(off.find("a few seconds after") != std::string::npos);
    CHECK(off != "Your undead come back.");
    component<ecs::ServerCursor>(n.r.fake, 2).currentBuildingItemInstance.itemId.value = 0;
    n.seconds(5);
    CHECK(n.spawned(kSkeleton) == 2);  // back
}

// ---- in the air: an undead far from its owner is brought beside them with the game's ground search (10 m at most), so
// beside an owner on the glider it stood in the air and fell (a player's report, of a Beastmaster wolf) ----
TEST_CASE("necromancer: the undead step aside while their owner glides and come back beside them a moment after landing, free; a raise meanwhile waits") {
    NecroRig n;
    n.spawnable(4);
    CHECK(n.reply("necromancer.raise").find("rises (1/3)") != std::string::npos);
    n.seconds(2);
    component<ecs::DynamicLocomotion>(n.r.fake, 1).state = static_cast<emf::gen::ecs::LocomotionState>(4);  // the glider
    place(n.r.fake, 1, 0, 60, 0);
    n.seconds(1);
    CHECK(has(n.r.fake.destroyed, 1234));
    size_t told = 0;
    for (const auto& x : n.r.fake.notes) told += x.find("step aside while you are in the air") != std::string::npos;
    CHECK(told == 1);
    CHECK(n.reply("necromancer.list").find("Skeletons 1/3") != std::string::npos);  // kept: it comes back
    CHECK(n.reply("necromancer.raise").find("once you land") != std::string::npos);
    CHECK(n.reply("necromancer.gather").find("once you land") != std::string::npos);
    for (int i = 1; i <= 5; ++i) {
        place(n.r.fake, 1, 8.0 * i, 60.0 - i, 0);
        n.seconds(1);
    }
    CHECK(n.spawned(kSkeleton) == 1);
    component<ecs::DynamicLocomotion>(n.r.fake, 1).state = static_cast<emf::gen::ecs::LocomotionState>(0);  // landed
    place(n.r.fake, 1, 50, 10, 0);
    n.seconds(2);
    CHECK(n.spawned(kSkeleton) == 2);  // back, free
    size_t paid = 0;
    for (const auto& [e, a, d] : n.r.fake.attr_adds) paid += e == 1 && a == EMF_ATTR_MANA && d == -25;
    CHECK(paid == 1);
}

// Seen live with Beastmaster: the game's EntityDiedEvent comes a step or two after the health reads 0; a check in between
// took the dead minion for gone, so its body (with loot) stayed and its owner heard nothing.
TEST_CASE("necromancer: a minion whose health reads 0 before the game's death event has fallen: its body goes and its owner hears it once") {
    NecroRig n;
    n.spawnable(2);
    CHECK(n.reply("necromancer.raise").find("rises (1/3)") != std::string::npos);
    n.seconds(2);
    n.r.fake.attrs[{1234, EMF_ATTR_HEALTH}] = 0;  // dead, the event not here yet
    n.seconds(3);
    CHECK(has(n.r.fake.destroyed, 1234));  // its body: no loot from our own undead
    deliver(n.r.fake, "EntityDiedEvent", died_event(1234));  // late
    n.seconds(2);
    size_t told = 0;
    for (const auto& x : n.r.fake.notes) told += x.find("fell") != std::string::npos;
    CHECK(told == 1);
    CHECK(n.reply("necromancer.list").find("Skeletons 0/3") != std::string::npos);
}
