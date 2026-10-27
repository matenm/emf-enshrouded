// Waypoint Teleports' game side (mods/teleports/tp_glue.h) over a fake API: the shroud as vanilla's fast-travel ban sees
// it, death, the player's spawn point (SelectedTargets slot 9) or the nearest flame altar, EMF's teleport and the cost.
#include <doctest.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../mods/teleports/tp_glue.h"

namespace gen = emf::gen::ecs;
using namespace tp;

namespace {
EmfWorld* const kW = reinterpret_cast<EmfWorld*>(0x7000);

struct Fake {
    std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> comps;
    std::vector<uint32_t> entities;
    std::vector<EmfTeleport> teleports;
    std::vector<EmfItemsRequest> takes, puts;
    std::vector<std::string> notes, log;
    uint32_t local = 1;
    std::map<uint32_t, int32_t> health;  // the health attribute (attribute_get), by entity
    struct Give {
        uint32_t entity, item, count;
    };
    std::vector<Give> gives;
    std::set<uint32_t> gone;  // give_item at these fails (no transform any more)
    std::vector<std::pair<uint32_t, std::string>> tells;
    std::map<uint32_t, std::string> names;
    uint32_t session = 1;
};
Fake* f = nullptr;

void* f_get(EmfWorld*, uint32_t hash, uint32_t entity, uint64_t* size) {
    auto it = f->comps.find({hash, entity});
    if (it == f->comps.end()) return nullptr;
    if (size) *size = it->second.size();
    return it->second.data();
}
uint8_t f_kind(EmfWorld*) { return EMF_WORLD_SERVER; }
void f_each(EmfWorld*, EmfEntityCallback cb, void* user) {
    for (uint32_t e : f->entities) cb(e, user);
}
uint32_t f_local() { return f->local; }
bool f_attr_get(EmfWorld*, uint32_t e, uint32_t id, int32_t* out) {
    auto it = f->health.find(e);
    if (id != EMF_ATTR_HEALTH || it == f->health.end()) return false;
    *out = it->second;
    return true;
}
uint32_t f_give(EmfWorld*, uint32_t entity, uint32_t item, uint32_t count) {
    if (f->gone.count(entity)) return 0;
    f->gives.push_back({entity, item, count});
    return 900;
}
bool f_tell(EmfWorld*, uint32_t player, const char* text) {
    f->tells.push_back({player, text});
    return true;
}
bool f_name(EmfWorld*, uint32_t player, char* buf, uint32_t size) {
    auto it = f->names.find(player);
    if (it == f->names.end()) return false;
    std::snprintf(buf, size, "%s", it->second.c_str());
    return true;
}
uint32_t f_session() { return f->session; }
void* f_singleton(EmfWorld*, const char*) { return nullptr; }
bool f_teleport(EmfWorld*, const EmfTeleport* t) {
    f->teleports.push_back(*t);
    return true;
}
uint32_t f_find_item(EmfWorld*, const char* name) { return std::strcmp(name, "Rune") == 0 ? 0x77u : 0u; }
bool f_take(EmfWorld*, const EmfItemsRequest* r) {
    f->takes.push_back(*r);
    return true;
}
bool f_put(EmfWorld*, const EmfItemsRequest* r) {
    f->puts.push_back(*r);
    return true;
}
void f_notify(const char* fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    f->notes.push_back(b);
}
void f_log(const char*, const char* fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    f->log.push_back(b);
}

template <class T>
T& plant(uint32_t entity) {
    auto& v = f->comps[{T::kHash, entity}];
    v.assign(sizeof(T), 0);
    return *reinterpret_cast<T*>(v.data());
}
void place(uint32_t e, double x, double y, double z) {
    auto& ct = plant<gen::CurrentTransform>(e);
    ct.transform.position = {static_cast<int64_t>(x * 4294967296.0), static_cast<int64_t>(y * 4294967296.0),
                             static_cast<int64_t>(z * 4294967296.0)};
    if (std::find(f->entities.begin(), f->entities.end(), e) == f->entities.end()) f->entities.push_back(e);
}
void spawn_point(uint32_t e, double x, double z, gen::PlayerSpawnType type) {
    place(e, x, 700, z);
    plant<gen::PlayerSpawnPoint>(e).target = type;
}

struct Rig {
    Fake fake;
    EmfApi api{};
    Rig() {
        f = &fake;
        api.version = EMF_API_VERSION;
        api.size = sizeof api;
        api.get_component = f_get;
        api.world_kind = f_kind;
        api.for_each_entity = f_each;
        api.local_player = f_local;
        api.singleton = f_singleton;
        api.teleport = f_teleport;
        api.find_item = f_find_item;
        api.take_items = f_take;
        api.put_items_ex = f_put;
        api.notify = f_notify;
        api.log = f_log;
        api.attribute_get = f_attr_get;
        api.give_item = f_give;
        api.chat_tell = f_tell;
        api.player_name = f_name;
        api.server_session = f_session;
        REQUIRE(emf::init(&api));
    }
    ~Rig() {
        emf::api_ref() = nullptr;
        f = nullptr;
    }
};
}  // namespace

TEST_CASE("teleports glue: inside the shroud = fogType Dangerous or Death, unless the player stands in a safe zone") {
    Rig r;
    emf::World w(kW);
    CHECK_FALSE(in_shroud(w, 1));  // no EnvironmentSensor
    auto& es = plant<gen::EnvironmentSensor>(1);
    es.fogType = emf::gen::FogType::NoneTemporary;
    CHECK_FALSE(in_shroud(w, 1));
    es.fogType = emf::gen::FogType::Dangerous;
    CHECK(in_shroud(w, 1));
    es.fogType = emf::gen::FogType::Death;
    CHECK(in_shroud(w, 1));
    auto& a = plant<gen::Actor>(1);
    a.currentState = kInSaveZone;  // at a flame altar inside a shroud area
    CHECK_FALSE(in_shroud(w, 1));
    a.currentAction.playbackContext.state = 1;  // an action sequence takes the flag away
    a.currentAction.playbackContext.unsetStateMask = kInSaveZone;
    CHECK(in_shroud(w, 1));
    a.currentAction.playbackContext.state = 0;
    CHECK_FALSE(in_shroud(w, 1));
    a.currentState = 0;
    a.currentAction.playbackContext.state = 1;
    a.currentAction.playbackContext.unsetStateMask = 0;
    a.currentAction.playbackContext.setStateMask = kInSaveZone;  // ... or gives it
    CHECK_FALSE(in_shroud(w, 1));
}

TEST_CASE("teleports glue: dead = replicated health 0") {
    Rig r;
    emf::World w(kW);
    CHECK_FALSE(dead(w, 1));
    auto& h = plant<gen::NetworkHealth>(1);
    h.health = 120;
    CHECK_FALSE(dead(w, 1));
    h.health = 0;
    CHECK(dead(w, 1));
}

TEST_CASE("teleports glue: the player's own spawn point (slot 9), else the nearest flame altar, never a no-respawn one") {
    Rig r;
    emf::World w(kW);
    place(1, 4000, 700, 1200);
    CHECK(spawn_point_of(w, 1) == 0u);
    spawn_point(1100, 4010, 1200, gen::PlayerSpawnType::NoRespawn);  // nearest, but never a respawn point
    spawn_point(1101, 4100, 1200, gen::PlayerSpawnType::SavePoint);  // a spire
    CHECK(spawn_point_of(w, 1) == 1101u);                             // no altar: any respawn point
    spawn_point(1025, 4500, 1200, gen::PlayerSpawnType::Base);       // a flame altar, farther
    CHECK(spawn_point_of(w, 1) == 1025u);                             // altars first
    spawn_point(1026, 4300, 1200, gen::PlayerSpawnType::Base);
    CHECK(spawn_point_of(w, 1) == 1026u);                             // the nearer altar
    auto& st = plant<gen::SelectedTargets>(1);
    st.targets[9].targetEntityId.id = 1025;  // the player's own: wins over the nearest
    CHECK(spawn_point_of(w, 1) == 1025u);
    st.targets[9].targetEntityId.id = 1100;  // a no-respawn one in the slot: not used
    CHECK(spawn_point_of(w, 1) == 1026u);
}

TEST_CASE("teleports glue: ApiGame asks EMF's teleport and maps its answer; lines reach the host on screen") {
    Rig r;
    ApiGame g("teleports");
    g.set_world(emf::World(kW));
    place(1, 4000, 700, 1200);
    Outcome got;
    int calls = 0;
    REQUIRE(g.teleport(1, {{4000, 704, 1200}, true, 0}, [&](const Outcome& o) {
        got = o;
        ++calls;
    }));
    REQUIRE(r.fake.teleports.size() == 1);
    const EmfTeleport& t = r.fake.teleports[0];
    CHECK(t.size == sizeof(EmfTeleport));
    CHECK(t.entity == 1u);
    CHECK(t.y == 704.0);
    CHECK(t.flags == EMF_TELEPORT_FIND_GROUND);
    CHECK(t.spawn_point == 0u);
    EmfTeleportResult res{};
    res.size = sizeof res;
    res.entity = 1;
    res.status = EMF_TELEPORT_MOVED;
    res.flags = EMF_TELEPORT_RAW_TARGET | EMF_TELEPORT_LAST;
    res.x = 4000;
    res.y = 704;
    res.z = 1200;
    res.reason = "";
    t.cb(kW, &res, t.user);
    CHECK(calls == 1);
    CHECK(got.moved);
    CHECK(got.raw_target);
    CHECK(got.pos.y == 704.0);
    REQUIRE(g.teleport(1, {{}, false, 1025}, [&](const Outcome& o) { got = o; }));
    const EmfTeleport& h = r.fake.teleports[1];
    CHECK(h.spawn_point == 1025u);
    CHECK(h.flags == 0u);
    res.status = EMF_TELEPORT_FAILED;
    res.flags = EMF_TELEPORT_LAST;
    res.reason = "entity 1 is dead";
    h.cb(kW, &res, h.user);
    CHECK_FALSE(got.moved);
    CHECK(got.reason == "entity 1 is dead");
    // lines: chat_tell for everyone (the core turns the host's into a note); without chat_tell the host gets a note
    g.tell(1, "Home");
    g.tell(2, "Home");
    CHECK(r.fake.tells.size() == 2);
    CHECK(r.fake.notes.empty());
    r.api.chat_tell = nullptr;
    g.tell(1, "Home");
    g.tell(2, "Home");
    REQUIRE(r.fake.notes.size() == 1);
    CHECK(r.fake.notes[0] == "Home");
    CHECK(r.fake.log.size() == 4);
    CHECK(g.account(7) != 0u);  // without BasicPlayerInfos the entity stands in for the session
}

TEST_CASE("teleports glue: the cost is taken all or nothing by item name; giving back puts it in again") {
    Rig r;
    ApiGame g("teleports");
    g.set_world(emf::World(kW));
    bool taken = false;
    CHECK_FALSE(g.take_cost(1, "Nothing", 2, [&](bool t) { taken = t; }));
    CHECK(r.fake.log.back().find("not an item name") != std::string::npos);
    REQUIRE(g.take_cost(1, "Rune", 2, [&](bool t) { taken = t; }));
    REQUIRE(r.fake.takes.size() == 1);
    CHECK(r.fake.takes[0].entity == 1u);
    CHECK(r.fake.takes[0].item == 0x77u);
    CHECK(r.fake.takes[0].count == 2u);
    r.fake.takes[0].cb(kW, 1, 0x77, 2, 2, r.fake.takes[0].user);
    CHECK(taken);
    g.give_back(1, "Rune", 2);
    REQUIRE(r.fake.puts.size() == 1);
    CHECK(r.fake.puts[0].item == 0x77u);
    CHECK(r.fake.puts[0].count == 2u);
}


// ---- edge cases ----

TEST_CASE("teleports glue: dead = the health attribute (a server-world player has Health, no NetworkHealth), as the core") {
    Rig r;
    emf::World w(kW);
    r.fake.health[1] = 0;
    CHECK(dead(w, 1));  // no NetworkHealth at all
    r.fake.health[1] = 60;
    CHECK_FALSE(dead(w, 1));
    plant<gen::NetworkHealth>(1).health = 0;  // the attribute wins over a replicated value
    CHECK_FALSE(dead(w, 1));
    r.fake.health[1] = 0;
    plant<gen::NetworkHealth>(1).health = 100;
    CHECK(dead(w, 1));
    r.fake.health.erase(1);  // no attribute: NetworkHealth decides
    CHECK_FALSE(dead(w, 1));
}

TEST_CASE("teleports glue: a refund for a player who left goes to the nearest player, else it is logged LOST") {
    Rig r;
    ApiGame g("teleports");
    g.set_world(emf::World(kW));
    place(1, 4000, 700, 1200);
    plant<gen::PlayerState>(1);
    place(2, 4050, 700, 1200);
    plant<gen::PlayerState>(2);
    place(3, 3000, 700, 1200);
    plant<gen::PlayerState>(3);
    g.give_back(1, "Rune", 2);
    REQUIRE(r.fake.puts.size() == 1);
    r.fake.gone.insert(1);  // left before the put ran: nothing fit, and nothing can drop at them
    r.fake.puts[0].cb(kW, 1, 0x77, 2, 0, r.fake.puts[0].user);
    REQUIRE(r.fake.gives.size() == 1);
    CHECK(r.fake.gives[0].entity == 2u);  // the nearest other player (2 is 50 m from where 1 stood, 3 is 1 km)
    CHECK(r.fake.gives[0].count == 2u);
    // nobody to give it to: logged LOST
    g.give_back(1, "Rune", 3);
    r.fake.gone = {1, 2, 3};
    r.fake.puts[1].cb(kW, 1, 0x77, 3, 0, r.fake.puts[1].user);
    CHECK(r.fake.gives.size() == 1);
    bool lost = false;
    for (const auto& l : r.fake.log) lost = lost || l.find("LOST") != std::string::npos;
    CHECK(lost);
}

TEST_CASE("teleports glue: a refund answered in another session gives nothing (the take may not have been saved)") {
    Rig r;
    ApiGame g("teleports");
    g.set_world(emf::World(kW));
    place(1, 4000, 700, 1200);
    g.give_back(1, "Rune", 2);
    REQUIRE(r.fake.puts.size() == 1);
    r.fake.session = 2;
    r.fake.puts[0].cb(kW, 1, 0x77, 2, 0, r.fake.puts[0].user);
    CHECK(r.fake.gives.empty());
}

TEST_CASE("teleports glue: lines for a guest go to their chat (chat_tell); names come from player_name") {
    Rig r;
    ApiGame g("teleports");
    g.set_world(emf::World(kW));
    g.tell(2, "Home in 2...");
    REQUIRE(r.fake.tells.size() == 1);
    CHECK(r.fake.tells[0].first == 2u);
    CHECK(r.fake.tells[0].second == "Home in 2...");
    for (uint32_t id : {1u, 2u, 3u}) {
        place(id, 4000, 700, 1200);
        plant<gen::PlayerState>(id);
    }
    r.fake.names = {{1, "Host"}, {2, "Ember"}, {3, "ember"}};
    CHECK(g.names_available());
    CHECK(g.players_named("EMBER") == std::vector<uint32_t>{2, 3});
    CHECK(g.players_named("host") == std::vector<uint32_t>{1});
    CHECK(g.name_of(2) == "Ember");
}
