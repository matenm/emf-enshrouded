// Night Raids' game side (mods/raids/raids_glue.h) over a fake API: who a raider is, the level it gets, the destroy
// loop (dawn, stop) and the per-world switch's mirror setting.
#include <doctest.h>

#include <map>
#include <vector>

#include "../mods/raids/raids_glue.h"

namespace gen = emf::gen::ecs;

namespace {
EmfWorld* const kW = reinterpret_cast<EmfWorld*>(0x5000);
std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> g_comps;
std::map<std::pair<uint32_t, uint32_t>, uintptr_t> g_tpl;
std::vector<uint32_t> g_destroyed;
void* f_get(EmfWorld*, uint32_t hash, uint32_t entity, uint64_t* size) {
    auto it = g_comps.find({hash, entity});
    if (it == g_comps.end()) return nullptr;
    if (size) *size = it->second.size();
    return it->second.data();
}
void* f_tpl(EmfWorld*, uint32_t hash, uint32_t entity, uint64_t*) {
    auto it = g_tpl.find({hash, entity});
    return it == g_tpl.end() ? nullptr : reinterpret_cast<void*>(it->second);
}
bool f_destroy(EmfWorld*, uint32_t id) {
    g_destroyed.push_back(id);
    return true;
}
uint8_t f_kind(EmfWorld*) { return EMF_WORLD_SERVER; }
template <class T>
T& plant(uint32_t entity) {
    auto& v = g_comps[{T::kHash, entity}];
    v.assign(sizeof(T), 0);
    return *reinterpret_cast<T*>(v.data());
}
struct Rig {
    EmfApi api{};
    Rig() {
        g_comps.clear();
        g_tpl.clear();
        g_destroyed.clear();
        api.version = EMF_API_VERSION;
        api.size = sizeof api;
        api.get_component = f_get;
        api.get_template_component = f_tpl;
        api.destroy = f_destroy;
        api.world_kind = f_kind;
        emf::api_ref() = &api;
    }
    ~Rig() { emf::api_ref() = nullptr; }
};
// A Vukah raider as spawned: its template's data, a Level and the scaling it rescales with.
void raider(uint32_t id, uintptr_t tpl = 0xA000) {
    plant<gen::CurrentTransform>(id);
    plant<gen::Level>(id).dataStorage[0] = 1;
    plant<gen::DynamicEnemyScaling>(id).nextUpdateTime.value = 12345;
    g_tpl[{gen::Enemy01BehaviorState::kHash, id}] = tpl;
}
}  // namespace

TEST_CASE("raids glue: a raider is known by its template and, once written, its level; a reused id is not it") {
    Rig r;
    emf::World w(kW);
    raider(5000);
    raids::RaiderId id = raids::identify(w, 5000);
    CHECK(id.tpl == 0xA000u);
    CHECK(raids::is_raider(w, 5000, id));
    CHECK(raids::write_level(w, 5000, 7));
    CHECK(w.get<gen::Level>(5000)->dataStorage[0] == 7u);
    CHECK(w.get<gen::DynamicEnemyScaling>(5000)->nextUpdateTime.value == 0);
    id.level = 7;
    CHECK(raids::is_raider(w, 5000, id));
    w.get<gen::Level>(5000)->dataStorage[0] = 3;  // another creature got the id (another level)
    CHECK_FALSE(raids::is_raider(w, 5000, id));
    raider(5001, 0xB000);  // another template under the id
    CHECK_FALSE(raids::is_raider(w, 5001, raids::RaiderId{0xA000, 0}));
    CHECK_FALSE(raids::is_raider(w, 9999, id));  // gone
    CHECK_FALSE(raids::write_level(w, 9999, 7));
}

TEST_CASE("raids glue: the destroy loop (dawn, stop) removes only raiders that still exist and are still theirs") {
    Rig r;
    emf::World w(kW);
    raider(5000);
    raider(5001);
    raider(5002, 0xB000);  // its id was reused by something else
    std::map<uint32_t, raids::RaiderId> known = {{5000, {0xA000, 1}}, {5001, {0xA000, 1}}, {5002, {0xA000, 1}},
                                                 {5003, {0xA000, 1}}};  // 5003 is gone
    const int n = raids::destroy_raiders(w, {5000, 5001, 5002, 5003}, known);
    CHECK(n == 2);
    CHECK(g_destroyed == std::vector<uint32_t>{5000, 5001});
}

TEST_CASE("raids glue: the per-world switch's setting follows the world's file; a change of the setting in a loaded world switches it") {
    using raids::switch_step;
    CHECK_FALSE(switch_step(false, 1, false).has_value());  // no world file yet: nothing to switch
    CHECK_FALSE(switch_step(true, -1, false).has_value());  // the setting is not declared (no mod.json): never guessed
    CHECK_FALSE(switch_step(true, 0, false).has_value());   // in step
    CHECK_FALSE(switch_step(true, 1, true).has_value());
    REQUIRE(switch_step(true, 1, false).has_value());       // switched on in the Mods tab
    CHECK(*switch_step(true, 1, false) == true);
    CHECK(*switch_step(true, 0, true) == false);
}
