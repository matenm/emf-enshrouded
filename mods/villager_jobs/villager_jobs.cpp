// villager_jobs: an EMF mod. Villagers work for you at a chest, through the game's own systems (host only).
//   Look at a villager and press the jobs key (default K): it is chosen. Look at a chest and hold the key: a wheel asks
//   what to do there: farm the crops near the chest (harvest into it; the plants grow again), tend the animals near it
//   (collect what they make into it, fill their feeders from it), haul to another chest (then look at that chest and
//   press K), craft (the recipes it can make from the chest; at a trader NPC these are its trades), or gather (pick up
//   the wild pickables around the chest, scatter bushes with twigs or plant fibre..., into it; the game decides when
//   they come back; loose stones are left alone: the game marks them like placed items, keys, weapons, potions and
//   rewards, which a gatherer must never take). Look at a working villager and hold the key to stop it. Jobs are saved per world.
// emf/config/villager_jobs.ini:
//   [villager_jobs]
//   enabled=1
//   key=K                 (a letter or digit, or a virtual-key code like 0x4B)
//   seconds_per_item=30
//   farm_radius=20        (metres around the chest)
//   tend_radius=20
//   haul_per_trip=20
//   gather_radius=20
//   gather_seconds=20     (a gatherer rests this long after each pickable it brought in)
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include "emf/emf.hpp"
#include "emf/generated/components.hpp"
#include "game_data.h"
#include "jobs_logic.h"

namespace gen = emf::gen;

namespace {
constexpr const char* kMod = "villager_jobs";
constexpr double kFixed = 4294967296.0;
constexpr uint64_t kLoadGraceMs = 60000;  // after a load, missing or unrecognised job entities are waited for
constexpr uint32_t kActiveNpcStateHash = 0x1a8b5909u;  // template data of villagers (npcId at +0)

bool guarded_copy(void* dst, const void* src, size_t n) {
    if (!src) return false;
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct Config {
    uint32_t key_vk = 'K';
    std::string key_name = "K";
    uint64_t interval_ms = 30000;
} g_cfg;
jobs::WorkConfig g_work;

jobs::JobBook g_book;  // server thread
jobs::Assigner* g_assigner = nullptr;
bool g_key_down = false;
uint64_t g_last_tick_ms = 0;
uint32_t g_session = 0;
std::atomic<uint16_t> g_slot{0};             // world slot of the loaded save (0 = none yet)
std::atomic<uint32_t> g_loaded_session{0};   // the session whose save loaded (on_load)
uint32_t g_jobs_loaded_session = 0;
std::wstring g_dir;                          // <game>\emf\config\villager_jobs

// Events for the mod's program (game worker thread).
std::mutex g_m;
struct Craft {
    uint32_t chest, villager, recipe;
};
struct GoTo {
    uint32_t npc_id;
    gen::WorldTransform target;
};
struct Loot {
    uint32_t guest, host;  // LootInteractionEvent (bypass): host's loot into guest
};
std::vector<Craft> g_crafts;
std::vector<GoTo> g_gotos;
std::vector<Loot> g_loots;

// What a recipe wheel's entries mean (captured by its callback).
struct WheelContext {
    uint32_t villager = 0, chest = 0;
    std::vector<uint32_t> recipes;  // entry -> recipe id
    std::vector<uint64_t> intervals;
    std::vector<jobs::Timing> timings;
};

std::optional<jobs::Vec3> position(emf::World w, uint32_t id) {
    auto* t = w.get<gen::ecs::CurrentTransform>(id);
    if (!t) return std::nullopt;
    return jobs::Vec3{t->transform.position.x / kFixed, t->transform.position.y / kFixed, t->transform.position.z / kFixed};
}

const uint8_t* npc_template(emf::World w, uint32_t id) {
    return static_cast<const uint8_t*>(emf::api()->get_template_component(w.raw(), kActiveNpcStateHash, id, nullptr));
}

std::string item_text(emf::World w, uint32_t id) {
    EmfItemInfo info{};
    info.size = sizeof(info);
    if (emf::api()->item_info(w.raw(), id, &info) && info.name[0]) return jobs::pretty_item(info.name);
    char b[16];
    snprintf(b, sizeof(b), "item %08x", id);
    return b;
}

std::string recipe_label(emf::World w, const jobs::Recipe& r) {
    std::string label = r.outputs.empty() ? "?" : item_text(w, r.outputs[0].item);
    if (!r.outputs.empty() && r.outputs[0].count > 1) label += " x" + std::to_string(r.outputs[0].count);
    std::string in;
    for (const auto& s : r.inputs) in += (in.empty() ? "" : ", ") + std::to_string(s.count) + " " + item_text(w, s.item);
    return label + " (" + in + ")";
}

std::vector<jobs::Recipe> recipes_of(emf::World w, uint32_t villager) {
    auto* c = w.get<gen::ecs::Crafting>(villager);
    auto* reg = static_cast<const uint8_t*>(emf::api()->singleton(w.raw(), "g38_items::RecipeRegistry"));
    if (!c || !reg) return {};
    return jobs::workshop_recipes(reg, c->workshopId.value, guarded_copy);
}

jobs::Contents contents_of(emf::World w, uint32_t chest) {
    auto* setup = w.get<gen::ecs::InventorySetup>(chest);
    if (!setup) return {};
    return jobs::chest_contents(reinterpret_cast<const uint8_t*>(setup), chest, [&](uint32_t id) {
        return reinterpret_cast<const uint8_t*>(w.get<gen::ecs::Inventory>(id));
    }, guarded_copy);
}

// The recipe's product and duration for a job (none if the villager no longer knows the recipe).
std::optional<jobs::Recipe> recipe_of(emf::World w, uint32_t villager, uint32_t recipe) {
    for (const auto& r : recipes_of(w, villager))
        if (r.id == recipe) return r;
    return std::nullopt;
}

std::string recipe_name(emf::World w, uint32_t villager, uint32_t recipe) {
    for (const auto& r : recipes_of(w, villager))
        if (r.id == recipe && !r.outputs.empty()) return item_text(w, r.outputs[0].item);
    char b[24];
    snprintf(b, sizeof(b), "recipe %08x", recipe);
    return b;
}

bool is_villager(emf::World w, uint32_t id) { return npc_template(w, id) && w.get<gen::ecs::Crafting>(id); }

// A storage with an initialized inventory that is not a player and not a villager (villagers carry inventories too).
bool is_chest(emf::World w, uint32_t id) {
    auto* setup = w.get<gen::ecs::InventorySetup>(id);
    return setup && setup->isInitialized && !w.get<gen::ecs::PlayerState>(id) && !is_villager(w, id);
}

// The villager's npcId (ActiveNpcState template data, +0), 0 if unreadable.
uint32_t npc_id_of(emf::World w, uint32_t villager) {
    uint32_t id = 0;
    const uint8_t* npc = npc_template(w, villager);
    return npc && guarded_copy(&id, npc, 4) ? id : 0;
}

// What a job checks about its entities each tick: still that kind, where it is, and the villager's npcId.
std::optional<jobs::EntityInfo> lookup(emf::World w, uint32_t id, jobs::Kind kind) {
    if (!(kind == jobs::Kind::Villager ? is_villager(w, id) : is_chest(w, id))) return std::nullopt;
    const auto p = position(w, id);
    if (!p) return std::nullopt;
    return jobs::EntityInfo{*p, kind == jobs::Kind::Villager ? npc_id_of(w, id) : 0u};
}

// A new job remembers who and where, so a reload that renumbers entities cannot point it at others.
jobs::Job with_identity(emf::World w, jobs::Job j) {
    if (const auto v = lookup(w, j.villager, jobs::Kind::Villager)) j.npc = v->npc;
    if (const auto c = lookup(w, j.chest, jobs::Kind::Chest)) {
        j.has_place = true;
        j.place = c->pos;
    }
    if (j.kind == jobs::JobKind::Haul)
        if (const auto c = lookup(w, j.chest2, jobs::Kind::Chest)) {
            j.has_place2 = true;
            j.place2 = c->pos;
        }
    return j;
}

// ---- what the farm / tend / haul jobs see (docs/engine_parts/re_villages.md) ----
constexpr int kFarmAnimalFaction = 12;

// The last level of a plant's Upgrade template (BlobArray<UpgradeData 0x68 B>, level at +0x10); 0 if unreadable.
int32_t last_upgrade_level(emf::World w, uint32_t root) {
    auto* up = w.get_template<gen::ecs::Upgrade>(root);
    if (!up) return 0;
    const uint8_t* field = up->levels;
    int32_t off = 0, level = 0;
    uint32_t n = 0;
    if (!guarded_copy(&off, field, 4) || !guarded_copy(&n, field + 4, 4) || n == 0 || n > 64) return 0;
    if (!guarded_copy(&level, field + off + 0x68 * (n - 1) + 0x10, 4)) return 0;
    return level;
}

jobs::Contents plain_contents_of(emf::World w, uint32_t chest) {
    auto* setup = w.get<gen::ecs::InventorySetup>(chest);
    if (!setup) return {};
    return jobs::chest_contents(reinterpret_cast<const uint8_t*>(setup), chest, [&](uint32_t id) {
        return reinterpret_cast<const uint8_t*>(w.get<gen::ecs::Inventory>(id));
    }, guarded_copy, true);
}

bool is_animal_food(emf::World w, uint32_t item) {
    EmfItemInfo info{};
    info.size = sizeof(info);
    return emf::api()->item_info(w.raw(), item, &info) && std::strstr(info.name, "_Feeding") != nullptr;
}

// A component on the entity or on its template.
template <class T>
bool has(emf::World w, uint32_t id) {
    return w.get<T>(id) || w.get_template<T>(id);
}
// A wild pickable (the gather job): loot that the game removes when taken, not a crop root, collectible
// (skeletons, fossils, meteorites), container or death bag (InventorySetup: their items sit in linked inventories),
// and not already looted (it fades out for a while). Crop stages look the same: scan_world leaves them out.
bool is_pickable(emf::World w, uint32_t id) {
    if (!w.get<gen::ecs::InteractionOfferRuntime>(id) || !has<gen::ecs::DestroyOnLoot>(w, id) ||
        !has<gen::ecs::DefaultInventory>(w, id) || w.get<gen::ecs::DynamicGrowth>(id) || has<gen::ecs::PickupItem>(w, id) ||
        has<gen::ecs::AreaLootOverride>(w, id) || has<gen::ecs::InventorySetup>(w, id) || w.get<gen::ecs::Inventory>(id) ||
        w.get<gen::ecs::DestroyOnLootActivated>(id))
        return false;
    const auto* d = w.get<gen::ecs::DestroyOnLoot>(id);
    return !(d && d->shouldDestroy);
}

// One pass over the world per jobs tick (only when a farm, tend or gather job runs).
struct Scan {
    std::vector<jobs::Crop> crops;
    std::vector<jobs::Feeder> feeders;
    std::vector<jobs::Animal> animals;
    std::vector<jobs::Pickable> pickables;
};
Scan scan_world(emf::World w) {
    Scan s;
    w.for_each_entity([&](uint32_t id) {
        if (w.get<gen::ecs::DynamicGrowth>(id)) {
            auto* lv = w.get<gen::ecs::Level>(id);
            auto* up = w.get<gen::ecs::DynamicUpgrade>(id);
            const auto p = position(w, id);
            if (!lv || !up || !p) return;
            const int32_t last = last_upgrade_level(w, id);
            const uint32_t stage = up->entityId.id;
            const bool ripe = last > 0 && static_cast<int32_t>(lv->dataStorage[0]) >= last && stage && position(w, stage);
            s.crops.push_back({id, stage, *p, ripe});
            return;
        }
        if (w.get<gen::ecs::AnimalSaved>(id)) {
            auto* f = w.get<gen::ecs::Faction>(id);
            const auto p = position(w, id);
            if (!f || static_cast<int>(f->faction) != kFarmAnimalFaction || !p) return;
            s.animals.push_back({id, *p, !contents_of(w, id).empty()});
            return;
        }
        if (w.get_template<gen::ecs::AnimalTarget>(id) && w.get<gen::ecs::InventorySetup>(id)) {
            const auto p = position(w, id);
            if (!p) return;
            uint32_t items = 0;
            for (const auto& [item, n] : contents_of(w, id)) items += n;
            s.feeders.push_back({id, *p, items});
            return;
        }
        if (is_pickable(w, id))
            if (const auto p = position(w, id)) s.pickables.push_back({id, *p});
    });
    // a crop's harvest stage looks like a pickable, but looting it outside a farm job would end the plant
    std::vector<uint32_t> stages;
    for (const auto& c : s.crops) stages.push_back(c.stage);
    std::sort(stages.begin(), stages.end());
    s.pickables.erase(std::remove_if(s.pickables.begin(), s.pickables.end(),
                                     [&](const jobs::Pickable& p) {
                                         return std::binary_search(stages.begin(), stages.end(), p.id);
                                     }),
                      s.pickables.end());
    return s;
}
template <class T>
std::vector<T> within(const std::vector<T>& all, const jobs::Vec3& c, double r) {
    std::vector<T> out;
    for (const auto& x : all)
        if (jobs::length(jobs::sub(x.pos, c)) <= r) out.push_back(x);
    return out;
}

// A harvested plant grows again: its root goes back to level 1 and its growth timer restarts (seen in game).
void rearm(emf::World w, uint32_t root) {
    auto* lv = w.get<gen::ecs::Level>(root);
    auto* g = w.get<gen::ecs::DynamicGrowth>(root);
    if (!lv || !g) {
        EMF_LOG(kMod, "plant %u was gone before it could grow again", root);
        return;
    }
    lv->dataStorage[0] = 1;
    g->nextLevelTime.value = 0;
}

// Puts items back where they came from; if even that fails, into the villager's own inventory (never lost silently).
void give_back(emf::World w, uint32_t villager, uint32_t to, uint32_t item, uint32_t n) {
    const bool q = emf::put_items_ex(w, to, item, n, 0, 0xFF, [villager, item, n](emf::World w, uint32_t added) {
        if (added >= n) return;
        const uint32_t rest = n - added;
        EMF_LOG(kMod, "%u of item %08x did not fit back; they go to villager %u", rest, item, villager);
        emf::put_items_ex(w, villager, item, rest, 0, 0xFF, [item, rest](emf::World, uint32_t a) {
            if (a < rest) EMF_LOG(kMod, "LOST %u of item %08x: no room anywhere (please report)", rest - a, item);
        });
    });
    if (!q) EMF_LOG(kMod, "LOST %u of item %08x: the put was refused (please report)", n, item);
}

// Moves `n` of `item` from chest `from` into `to` through the game's transactions; what does not fit goes back.
void move_items(emf::World w, uint32_t villager, uint32_t from, uint32_t to, uint32_t item, uint32_t n, bool feeding) {
    const bool queued = emf::take_items(w, from, item, n, 0, [=](emf::World w, uint32_t taken) {
        if (!taken) {
            g_book.on_transfer(villager, 0, GetTickCount64());
            return;
        }
        const bool q = emf::put_items_ex(w, to, item, taken, 0, 0xFF, [=](emf::World w, uint32_t added) {
            if (added < taken) give_back(w, villager, from, item, taken - added);
            if (!added && feeding) g_book.refuse_food(villager, to, item, GetTickCount64());
            g_book.on_transfer(villager, added, GetTickCount64());
        });
        if (!q) {
            give_back(w, villager, from, item, taken);
            g_book.on_transfer(villager, 0, GetTickCount64());
        }
    });
    if (!queued) g_book.on_transfer(villager, 0, GetTickCount64());
}

void do_feed(emf::World w, const jobs::Job& j, uint32_t feeder) {
    const uint64_t now = GetTickCount64();
    uint32_t food = 0, have = 0;
    for (const auto& [item, n] : plain_contents_of(w, j.chest))
        if (n > have && is_animal_food(w, item) && !g_book.food_refused(j.villager, feeder, item, now)) {
            food = item;
            have = n;
        }
    if (!food) {
        g_book.on_transfer(j.villager, 0, now, true);
        return;
    }
    move_items(w, j.villager, j.chest, feeder, food, (std::min)(have, g_work.feed_amount), true);
}

// Haul: the kind in the source that the target already holds (the target sorts), or any kind if the target is empty.
std::pair<uint32_t, uint32_t> haul_pick(emf::World w, uint32_t from, uint32_t to) {
    const auto target = contents_of(w, to);
    uint32_t item = 0, have = 0;
    for (const auto& [id, n] : plain_contents_of(w, from))
        if (n > have && (target.empty() || target.count(id))) {
            item = id;
            have = n;
        }
    return {item, have};
}
void do_haul(emf::World w, const jobs::Job& j) {
    const auto [item, have] = haul_pick(w, j.chest, j.chest2);
    if (!item) {
        g_book.on_transfer(j.villager, 0, GetTickCount64(), true);
        return;
    }
    move_items(w, j.villager, j.chest, j.chest2, item, (std::min)(have, g_work.haul_per_trip), false);
}

void send_npc_to(emf::World w, uint32_t villager, const jobs::Vec3& pos) {
    const uint8_t* npc = npc_template(w, villager);
    auto* t = w.get<gen::ecs::CurrentTransform>(villager);
    uint32_t npc_id = 0;
    if (!npc || !t || !guarded_copy(&npc_id, npc, 4) || !npc_id) return;
    gen::WorldTransform target = t->transform;
    target.position.x = static_cast<int64_t>(pos.x * kFixed);
    target.position.y = static_cast<int64_t>(pos.y * kFixed);
    target.position.z = static_cast<int64_t>(pos.z * kFixed);
    std::lock_guard<std::mutex> l(g_m);
    g_gotos.push_back({npc_id, target});
}

std::string job_label(emf::World w, const jobs::JobStatus& s) {
    char b[200];
    switch (s.job.kind) {
        case jobs::JobKind::Craft:
            snprintf(b, sizeof(b), "makes %s from chest %u: %s, %u made", recipe_name(w, s.job.villager, s.job.recipe).c_str(),
                     s.job.chest, s.state == jobs::JobState::Working ? "working" : "waiting for materials", s.made);
            break;
        case jobs::JobKind::Farm:
            snprintf(b, sizeof(b), "farms around chest %u: %u harvests", s.job.chest, s.work.done);
            break;
        case jobs::JobKind::Tend:
            snprintf(b, sizeof(b), "tends the animals around chest %u: %u collected, %u fed", s.job.chest, s.work.done,
                     s.work.fed);
            break;
        case jobs::JobKind::Haul:
            snprintf(b, sizeof(b), "hauls from chest %u to chest %u: %u items moved", s.job.chest, s.job.chest2, s.work.done);
            break;
        case jobs::JobKind::Gather:
            snprintf(b, sizeof(b), "gathers around chest %u: %u pickables brought in", s.job.chest, s.work.done);
            break;
    }
    return b;
}

void assign_work(emf::World w, uint32_t villager, uint32_t chest, jobs::JobKind kind, uint32_t chest2 = 0) {
    jobs::Job j{villager, chest, 0};
    j.kind = kind;
    j.chest2 = chest2;
    j = with_identity(w, j);
    if (!g_book.assign(j, GetTickCount64(), g_cfg.interval_ms)) {
        emf::api()->notify("That job could not be given (the same chest twice?)");
        return;
    }
    const auto* s = g_book.find(villager);
    emf::api()->notify("Villager %u %s", villager, s ? job_label(w, *s).c_str() : "has a job");
    if (g_assigner) g_assigner->reset();
}

// Villagers and chests within reach of the player, with the point the crosshair should be near.
std::vector<jobs::Candidate> candidates(emf::World w, const jobs::Vec3& player, uint32_t player_id) {
    std::vector<jobs::Candidate> out;
    w.for_each_entity([&](uint32_t id) {
        if (id == player_id) return;
        const auto p = position(w, id);
        if (!p || jobs::length(jobs::sub(*p, player)) > jobs::kPickRange + 1.0) return;
        if (is_villager(w, id))
            out.push_back({id, {p->x, p->y + 0.9, p->z}, jobs::Kind::Villager});
        else if (is_chest(w, id))
            out.push_back({id, {p->x, p->y + 0.4, p->z}, jobs::Kind::Chest});
    });
    return out;
}

void save_jobs() {
    const uint16_t slot = g_slot;
    if (!slot) return;
    CreateDirectoryW(g_dir.c_str(), nullptr);
    wchar_t file[16];
    swprintf(file, 16, L"\\%04x.txt", slot);
    FILE* f = nullptr;
    if (_wfopen_s(&f, (g_dir + file).c_str(), L"wb") == 0 && f) {
        const std::string text = jobs::serialize(g_book.jobs());
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);
    }
}

void load_jobs(emf::World w, uint64_t now) {
    const uint16_t slot = g_slot;
    for (const auto& j : g_book.jobs()) g_book.stop(j.villager);
    if (!slot) return;
    wchar_t file[16];
    swprintf(file, 16, L"\\%04x.txt", slot);
    FILE* f = nullptr;
    std::string text;
    if (_wfopen_s(&f, (g_dir + file).c_str(), L"rb") == 0 && f) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
        fclose(f);
    }
    const auto saved = jobs::parse(text);
    for (const auto& j : saved) {
        const auto r = recipe_of(w, j.villager, j.recipe);
        g_book.assign(j, now, r ? jobs::job_interval(g_cfg.interval_ms, *r) : g_cfg.interval_ms,
                      r ? jobs::job_timing(*r) : jobs::Timing{});
    }
    g_book.hold_drops_until(now + kLoadGraceMs);  // the world may still be filling in
    g_book.changed();  // just loaded: nothing new to save
    if (!saved.empty()) EMF_LOG(kMod, "%zu job(s) loaded for world slot %04x", saved.size(), slot);
}

void open_recipe_wheel(emf::World w, uint32_t villager, uint32_t chest) {
    const auto contents = contents_of(w, chest);
    std::vector<std::string> labels;
    WheelContext ctx{villager, chest, {}, {}, {}};
    for (const auto& r : recipes_of(w, villager)) {
        if (!jobs::craftable(r, contents)) continue;
        labels.push_back(recipe_label(w, r));
        ctx.recipes.push_back(r.id);
        ctx.intervals.push_back(jobs::job_interval(g_cfg.interval_ms, r));
        ctx.timings.push_back(jobs::job_timing(r));
        if (labels.size() >= 64) break;
    }
    if (labels.empty()) {
        emf::api()->notify("Villager %u can make nothing from chest %u yet: put materials in it", villager, chest);
        return;
    }
    const std::string title = "Villager " + std::to_string(villager) + ": make what?";
    const bool ok = emf::wheel(title.c_str(), labels, g_cfg.key_vk, EMF_WORLD_SERVER, [ctx](emf::World w, int choice) {
        if (choice < 0 || static_cast<size_t>(choice) >= ctx.recipes.size()) return;
        const jobs::Job j = with_identity(w, {ctx.villager, ctx.chest, ctx.recipes[choice]});
        g_book.assign(j, GetTickCount64(), ctx.intervals[choice], ctx.timings[choice]);
        emf::api()->notify("Villager %u will make %s from chest %u", j.villager,
                           recipe_name(w, j.villager, j.recipe).c_str(), j.chest);
        if (g_assigner) g_assigner->reset();
    });
    if (!ok) emf::api()->notify("The wheel could not open (the console may be open; the log says why)");
}

// The first wheel at a chest: which kind of job (the counts help the choice).
void open_job_wheel(emf::World w, uint32_t villager, uint32_t chest) {
    const auto c = position(w, chest);
    if (!c) return;
    const Scan s = scan_world(w);
    size_t ripe = 0;
    for (const auto& x : within(s.crops, *c, g_work.farm_radius)) ripe += x.ripe;
    const size_t crops = within(s.crops, *c, g_work.farm_radius).size();
    const size_t animals = within(s.animals, *c, g_work.tend_radius).size();
    const size_t feeders = within(s.feeders, *c, g_work.tend_radius).size();
    const size_t pickables = within(s.pickables, *c, g_work.gather_radius).size();
    const auto contents = contents_of(w, chest);
    size_t craftable = 0;
    for (const auto& r : recipes_of(w, villager)) craftable += jobs::craftable(r, contents);
    char farm[96], tend[96], craft[64], gather[64];
    snprintf(farm, sizeof(farm), "Farm the crops here (%zu plants, %zu ripe)", crops, ripe);
    snprintf(tend, sizeof(tend), "Tend the animals here (%zu animals, %zu feeders)", animals, feeders);
    snprintf(craft, sizeof(craft), "Craft (%zu recipes)", craftable);
    snprintf(gather, sizeof(gather), "Gather around here (%zu pickables)", pickables);
    const std::string title = "Villager " + std::to_string(villager);
    const bool ok = emf::wheel(title.c_str(), {farm, tend, "Haul to another chest", craft, gather}, g_cfg.key_vk, EMF_WORLD_SERVER,
                               [villager, chest](emf::World w, int choice) {
                                   switch (choice) {
                                       case 0: assign_work(w, villager, chest, jobs::JobKind::Farm); break;
                                       case 1: assign_work(w, villager, chest, jobs::JobKind::Tend); break;
                                       case 2:
                                           if (g_assigner) g_assigner->want_haul_target(villager, chest, GetTickCount64());
                                           emf::api()->notify("Look at the chest to haul to and press %s",
                                                              g_cfg.key_name.c_str());
                                           break;
                                       case 3: open_recipe_wheel(w, villager, chest); break;
                                       case 4: assign_work(w, villager, chest, jobs::JobKind::Gather); break;
                                       default: break;
                                   }
                               });
    if (!ok) emf::api()->notify("The wheel could not open (the console may be open; the log says why)");
}

void open_stop_wheel(emf::World w, uint32_t villager) {
    const auto* s = g_book.find(villager);
    if (!s) return;
    // The centre circle is small: the title is just the villager, the job goes on the "Stop working" slice.
    char title[32], stop[200];
    snprintf(title, sizeof(title), "Villager %u", villager);
    switch (s->job.kind) {
        case jobs::JobKind::Craft:
            snprintf(stop, sizeof(stop), "Stop working (%s, %u made)", recipe_name(w, villager, s->job.recipe).c_str(), s->made);
            break;
        case jobs::JobKind::Farm: snprintf(stop, sizeof(stop), "Stop farming (%u harvests)", s->work.done); break;
        case jobs::JobKind::Tend:
            snprintf(stop, sizeof(stop), "Stop tending (%u collected, %u fed)", s->work.done, s->work.fed);
            break;
        case jobs::JobKind::Haul: snprintf(stop, sizeof(stop), "Stop hauling (%u items moved)", s->work.done); break;
        case jobs::JobKind::Gather: snprintf(stop, sizeof(stop), "Stop gathering (%u brought in)", s->work.done); break;
    }
    const bool ok = emf::wheel(title, {stop, "Keep working"}, g_cfg.key_vk, EMF_WORLD_SERVER,
                               [villager](emf::World, int choice) {
                                   if (choice != 0) return;
                                   g_book.stop(villager);
                                   emf::api()->notify("Villager %u stopped working", villager);
                               });
    if (!ok) emf::api()->notify("The wheel could not open (the console may be open; the log says why)");
}

void on_key(emf::World w) {
    const uint32_t player = emf::local_player();
    auto* input = w.get<gen::ecs::PlayerInput>(player);
    const auto pp = position(w, player);
    if (!input || !pp) return;
    const auto& cam = input->fromClient.camera;
    const jobs::Vec3 cp{cam.position.x / kFixed, cam.position.y / kFixed, cam.position.z / kFixed};
    const jobs::Quat q{cam.orientation.x, cam.orientation.y, cam.orientation.z, cam.orientation.w};
    const auto target = jobs::pick_target(cp, q, *pp, candidates(w, *pp, player));
    const bool has_job = target && target->kind == jobs::Kind::Villager && g_book.find(target->id);
    const auto r = g_assigner->press(target, has_job, GetTickCount64());
    EMF_LOG(kMod, "key: target %s %u -> %d", target ? (target->kind == jobs::Kind::Villager ? "villager" : "chest") : "none",
            target ? target->id : 0, static_cast<int>(r.kind));
    switch (r.kind) {
        case jobs::Assigner::Result::Notice: emf::api()->notify("%s", r.text.c_str()); break;
        case jobs::Assigner::Result::OpenJobWheel: open_job_wheel(w, r.villager, r.chest); break;
        case jobs::Assigner::Result::AssignHaul: assign_work(w, r.villager, r.chest, jobs::JobKind::Haul, r.chest2); break;
        case jobs::Assigner::Result::OpenStopWheel: open_stop_wheel(w, r.villager); break;
        default: break;
    }
}

void run_jobs(emf::World w, uint64_t now) {
    // Jobs restored before their villager's recipes were readable learn the recipe's product and duration now.
    std::vector<std::pair<uint32_t, uint32_t>> untimed;  // villager, recipe
    for (const auto& s : g_book.status())
        if (s.job.kind == jobs::JobKind::Craft && !s.timing.output_item) untimed.emplace_back(s.job.villager, s.job.recipe);
    for (const auto& [villager, recipe] : untimed)
        if (const auto r = recipe_of(w, villager, recipe)) g_book.set_timing(villager, jobs::job_timing(*r));
    bool needs_scan = false;
    for (const auto& s : g_book.status())
        needs_scan |= s.job.kind == jobs::JobKind::Farm || s.job.kind == jobs::JobKind::Tend ||
                      s.job.kind == jobs::JobKind::Gather;
    const Scan scan = needs_scan ? scan_world(w) : Scan{};
    jobs::WorkView view;
    view.crops = [&scan](const jobs::Vec3& c, double r) { return within(scan.crops, c, r); };
    view.feeders = [&scan](const jobs::Vec3& c, double r) { return within(scan.feeders, c, r); };
    view.animals = [&scan](const jobs::Vec3& c, double r) { return within(scan.animals, c, r); };
    view.pickables = [&scan](const jobs::Vec3& c, double r) { return within(scan.pickables, c, r); };
    view.haul_has_work = [w](uint32_t from, uint32_t to) { return haul_pick(w, from, to).first != 0; };
    view.has_food = [w](uint32_t chest) {
        for (const auto& [item, n] : plain_contents_of(w, chest))
            if (n && is_animal_food(w, item)) return true;
        return false;
    };
    const auto actions = g_book.tick(now, [&](uint32_t id, jobs::Kind kind) { return lookup(w, id, kind); }, view);
    for (const auto& a : actions) {
        switch (a.kind) {
            case jobs::ActionKind::GoTo: {
                const uint8_t* npc = npc_template(w, a.job.villager);
                auto* t = w.get<gen::ecs::CurrentTransform>(a.job.chest);
                uint32_t npc_id = 0;
                if (npc && t && guarded_copy(&npc_id, npc, 4) && npc_id) {
                    std::lock_guard<std::mutex> l(g_m);
                    g_gotos.push_back({npc_id, t->transform});
                }
                break;
            }
            case jobs::ActionKind::Craft: {
                std::lock_guard<std::mutex> l(g_m);
                g_crafts.push_back({a.job.chest, a.job.villager, a.job.recipe});
                break;
            }
            case jobs::ActionKind::Notice:
            case jobs::ActionKind::Dropped: emf::api()->notify("%s", a.text.c_str()); break;
            case jobs::ActionKind::GoToPos: send_npc_to(w, a.job.villager, a.pos); break;
            case jobs::ActionKind::Harvest:
            case jobs::ActionKind::Collect:
            case jobs::ActionKind::Gather: {
                std::lock_guard<std::mutex> l(g_m);
                g_loots.push_back({a.job.chest, a.target});
                break;
            }
            case jobs::ActionKind::Feed: do_feed(w, a.job, a.target); break;
            case jobs::ActionKind::Haul: do_haul(w, a.job); break;
        }
    }
    if (g_book.changed()) save_jobs();
}

void on_tick(emf::World w) {
    if (!w.is_server()) return;
    const uint64_t now = GetTickCount64();
    const uint32_t session = emf::server_session();
    if (session != g_session) {  // a new hosted session: forget the old one's jobs until its save loads
        g_session = session;
        if (g_assigner) g_assigner->reset();
        for (const auto& j : g_book.jobs()) g_book.stop(j.villager);
        g_book.changed();
        if (g_loaded_session != session) g_slot = 0;  // jobs given before this session's save loads are not saved
        std::lock_guard<std::mutex> l(g_m);           // nor are walk/craft orders for the old world's ids sent
        g_crafts.clear();
        g_gotos.clear();
        g_loots.clear();
    }
    if (g_loaded_session == session && g_jobs_loaded_session != session) {
        g_jobs_loaded_session = session;
        load_jobs(w, now);
    }
    const bool down = emf::keys::down(g_cfg.key_vk);  // only with the game window in front
    if (down && !g_key_down) on_key(w);
    g_key_down = down;
    if (now - g_last_tick_ms >= 500) {
        g_last_tick_ms = now;
        run_jobs(w, now);
    }
}

// Server program (game worker thread): sends the queued crafts and walk orders.
void jobs_program(void* ctx) {
    std::vector<Craft> crafts;
    std::vector<GoTo> gotos;
    std::vector<Loot> loots;
    {
        std::lock_guard<std::mutex> l(g_m);
        crafts.swap(g_crafts);
        gotos.swap(g_gotos);
        loots.swap(g_loots);
    }
    for (const auto& lo : loots)  // harvest a stage / collect from an animal straight into the chest (re_villages.md)
        if (auto* e = emf::emit<gen::ecs::LootInteractionEvent>(ctx)) {
            e->guestId.id = lo.guest;
            e->hostId.id = lo.host;
            e->offerId = 0;
            e->lootAll = true;
            e->bypassInteraction = true;
        }
    for (const auto& c : crafts)
        if (auto* e = emf::emit<gen::ecs::CraftRecipeEvent>(ctx)) {
            e->craftingOperatorId.id = c.chest;
            e->craftingStationId.id = c.villager;
            e->recipeId.value = c.recipe;
            e->recipeAmount = 1;
        }
    for (const auto& g : gotos)
        if (auto* e = emf::emit<gen::ecs::NpcCommandEvent>(ctx)) {
            e->type = gen::ecs::NpcCommandType::GoToPosition;
            e->npcId.value = g.npc_id;
            e->targetTransform = g.target;
        }
}


std::wstring ini_path() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    g_dir = dir + L"\\emf\\config\\villager_jobs";
    return dir + L"\\emf\\config\\villager_jobs.ini";
}

void cmd_jobs(emf::World w, const std::vector<std::string>&) {
    if (g_book.status().empty()) EMF_REPLY("no villager has a job (world slot %04x)", static_cast<unsigned>(g_slot.load()));
    for (const auto& s : g_book.status()) EMF_REPLY("villager %u %s", s.job.villager, job_label(w, s).c_str());
}

// job.farm / job.tend / job.gather <villager> <chest>, job.haul <villager> <from> <to>
void cmd_work(emf::World w, const std::vector<std::string>& a, jobs::JobKind kind) {
    const size_t want = kind == jobs::JobKind::Haul ? 4 : 3;
    if (a.size() != want) {
        EMF_REPLY(kind == jobs::JobKind::Haul ? "usage: job.haul <villager> <from chest> <to chest>"
                                              : "usage: job.%s <villager> <chest>", jobs::kind_name(kind));
        return;
    }
    const uint32_t v = static_cast<uint32_t>(std::strtoul(a[1].c_str(), nullptr, 10));
    const uint32_t c = static_cast<uint32_t>(std::strtoul(a[2].c_str(), nullptr, 10));
    const uint32_t c2 = want == 4 ? static_cast<uint32_t>(std::strtoul(a[3].c_str(), nullptr, 10)) : 0u;
    if (want == 4 && c == c2) {
        EMF_REPLY("the two chests must be different");
        return;
    }
    if (!is_villager(w, v) || !is_chest(w, c) || (want == 4 && !is_chest(w, c2))) {
        EMF_REPLY("%u is not a villager, or a chest id is not a chest", v);
        return;
    }
    assign_work(w, v, c, kind, c2);
    if (const auto* s = g_book.find(v)) EMF_REPLY("villager %u %s", v, job_label(w, *s).c_str());
    if (kind == jobs::JobKind::Gather)
        if (const auto at = position(w, c))
            EMF_REPLY("%zu pickables within %.0f m of chest %u", within(scan_world(w).pickables, *at, g_work.gather_radius).size(),
                      g_work.gather_radius, c);
}

void cmd_job(emf::World w, const std::vector<std::string>& a) {
    if (a.size() != 4) {
        EMF_REPLY("usage: job <villager> <chest> <recipe id hex> (job.recipes lists them)");
        return;
    }
    jobs::Job j{static_cast<uint32_t>(std::strtoul(a[1].c_str(), nullptr, 10)),
                static_cast<uint32_t>(std::strtoul(a[2].c_str(), nullptr, 10)),
                static_cast<uint32_t>(std::strtoul(a[3].c_str(), nullptr, 16))};
    if (!is_villager(w, j.villager) || !is_chest(w, j.chest)) {
        EMF_REPLY("%u is not a villager or %u is not a chest", j.villager, j.chest);
        return;
    }
    j = with_identity(w, j);
    const auto r = recipe_of(w, j.villager, j.recipe);
    g_book.assign(j, GetTickCount64(), r ? jobs::job_interval(g_cfg.interval_ms, *r) : g_cfg.interval_ms,
                  r ? jobs::job_timing(*r) : jobs::Timing{});
    EMF_REPLY("villager %u will make %s from chest %u", j.villager, recipe_name(w, j.villager, j.recipe).c_str(), j.chest);
}

void cmd_stop(emf::World, const std::vector<std::string>& a) {
    if (a.size() != 2) {
        EMF_REPLY("usage: job.stop <villager>");
        return;
    }
    const uint32_t v = static_cast<uint32_t>(std::strtoul(a[1].c_str(), nullptr, 10));
    EMF_REPLY(g_book.stop(v) ? "villager %u stopped" : "villager %u has no job", v);
}

// job.recall [villager]: a stuck villager back beside its job's chest (else its base's flame altar, else you), with the
// game's ground search (EMF's teleport, API v20). Without an id: the villager you look at.
void cmd_recall(emf::World w, const std::vector<std::string>& a) {
    if (a.size() > 2) {
        EMF_REPLY("usage: job.recall [villager] (without an id: the villager you look at)");
        return;
    }
    const uint32_t player = emf::local_player();
    const auto pp = position(w, player);
    uint32_t v = 0;
    if (a.size() == 2) {
        v = static_cast<uint32_t>(std::strtoul(a[1].c_str(), nullptr, 10));
    } else if (auto* input = pp ? w.get<gen::ecs::PlayerInput>(player) : nullptr) {
        const auto& cam = input->fromClient.camera;
        const jobs::Vec3 cp{cam.position.x / kFixed, cam.position.y / kFixed, cam.position.z / kFixed};
        const jobs::Quat q{cam.orientation.x, cam.orientation.y, cam.orientation.z, cam.orientation.w};
        const auto target = jobs::pick_target(cp, q, *pp, candidates(w, *pp, player));
        if (target && target->kind == jobs::Kind::Villager) v = target->id;
    }
    if (!v || !is_villager(w, v)) {
        EMF_REPLY(a.size() == 2 ? "job.recall: %u is no villager" : "job.recall: look at a villager (or name its id)", v);
        return;
    }
    std::optional<jobs::Vec3> chest, altar;
    if (const auto* s = g_book.find(v)) chest = position(w, s->job.chest);
    if (const auto here = position(w, v)) {  // the nearest flame altar (its base's), within 2 km
        double best = 2000.0;
        w.for_each_entity([&](uint32_t id) {
            const auto* sp = w.get<gen::ecs::PlayerSpawnPoint>(id);
            if (!sp || sp->target != gen::ecs::PlayerSpawnType::Base) return;
            const auto p = position(w, id);
            if (!p) return;
            const double d = jobs::length(jobs::sub(*p, *here));
            if (d < best) {
                best = d;
                altar = p;
            }
        });
    }
    const auto to = jobs::recall_target(chest, altar, pp);
    if (!to) {
        EMF_REPLY("job.recall: nowhere known to send villager %u (no job chest, no flame altar, no player)", v);
        return;
    }
    const EmfApi* api = emf::api();
    if (api->size < offsetof(EmfApi, teleport) + sizeof(api->teleport) || !api->teleport) {
        EMF_REPLY("job.recall needs EMF's teleport (API v20)");
        return;
    }
    EmfTeleport t{};
    t.size = sizeof t;
    t.entity = v;
    t.x = to->x;
    t.y = to->y;
    t.z = to->z;
    t.flags = EMF_TELEPORT_FIND_GROUND;
    t.cb = [](EmfWorld*, const EmfTeleportResult* r, void*) {
        if (r->status == EMF_TELEPORT_FAILED)
            emf::api()->notify("Villager %u could not be moved: %s", r->entity, r->reason ? r->reason : "?");
        else
            emf::api()->notify("Villager %u is back (%.0f, %.0f, %.0f)", r->entity, r->x, r->y, r->z);
        EMF_LOG(kMod, "job.recall %u: %s", r->entity, r->status == EMF_TELEPORT_FAILED ? r->reason : "moved");
    };
    const bool ok = api->teleport(w.raw(), &t);
    EMF_REPLY(ok ? "job.recall: villager %u goes to (%.1f, %.1f, %.1f) (%s)" : "job.recall: refused (see the log)", v, to->x,
              to->y, to->z, chest ? "its job's chest" : altar ? "its flame altar" : "you");
}

void cmd_recipes(emf::World w, const std::vector<std::string>& a) {
    if (a.size() != 2 && a.size() != 3) {
        EMF_REPLY("usage: job.recipes <villager> [chest]");
        return;
    }
    const uint32_t v = static_cast<uint32_t>(std::strtoul(a[1].c_str(), nullptr, 10));
    if (a.size() == 2) {  // everything the villager's workshop knows (a trader: its trades)
        const auto all = recipes_of(w, v);
        for (const auto& r : all) EMF_REPLY("  %08x: %s", r.id, recipe_label(w, r).c_str());
        EMF_REPLY("%zu recipe(s) villager %u knows", all.size(), v);
        return;
    }
    const uint32_t c = static_cast<uint32_t>(std::strtoul(a[2].c_str(), nullptr, 10));
    const auto contents = contents_of(w, c);
    size_t n = 0;
    for (const auto& r : recipes_of(w, v))
        if (jobs::craftable(r, contents)) {
            ++n;
            EMF_REPLY("  %08x: %s", r.id, recipe_label(w, r).c_str());
        }
    EMF_REPLY("%zu recipe(s) villager %u can make from chest %u (%zu kinds of items in it)", n, v, c, contents.size());
}
}  // namespace

extern "C" __declspec(dllexport) bool emf_mod_load(const EmfApi* api) {
    if (!emf::init(api)) return false;
    const std::wstring ini = ini_path();
    if (GetPrivateProfileIntW(L"villager_jobs", L"enabled", 1, ini.c_str()) == 0) {
        EMF_LOG(kMod, "disabled (set [villager_jobs] enabled=1 in emf\\config\\villager_jobs.ini)");
        return true;
    }
    wchar_t key[32] = {};
    GetPrivateProfileStringW(L"villager_jobs", L"key", L"K", key, 32, ini.c_str());
    if (const uint32_t vk = jobs::parse_key(key, &g_cfg.key_name))
        g_cfg.key_vk = vk;
    else
        EMF_LOG(kMod, "key=%ls is not a letter, a digit or a keyboard key's virtual-key number (like 0x71 for F2; no mouse buttons): using K", key);
    g_cfg.interval_ms = 1000ull * (std::max)(1u, GetPrivateProfileIntW(L"villager_jobs", L"seconds_per_item", 30, ini.c_str()));
    g_work.farm_radius = (std::max)(1u, GetPrivateProfileIntW(L"villager_jobs", L"farm_radius", 20, ini.c_str()));
    g_work.tend_radius = (std::max)(1u, GetPrivateProfileIntW(L"villager_jobs", L"tend_radius", 20, ini.c_str()));
    g_work.haul_per_trip = (std::max)(1u, GetPrivateProfileIntW(L"villager_jobs", L"haul_per_trip", 20, ini.c_str()));
    g_work.gather_radius = (std::max)(1u, GetPrivateProfileIntW(L"villager_jobs", L"gather_radius", 20, ini.c_str()));
    g_work.gather_every_ms =
        1000ull * (std::max)(1u, GetPrivateProfileIntW(L"villager_jobs", L"gather_seconds", 20, ini.c_str()));
    jobs::JobConfig jc;
    jc.work = g_work;
    g_book = jobs::JobBook(jc);
    static jobs::Assigner assigner(g_cfg.key_name);
    g_assigner = &assigner;
    if (!emf::add_program("villager_jobs", EMF_WORLD_SERVER, {}, {"CraftRecipeEvent", "NpcCommandEvent", "LootInteractionEvent"},
                          jobs_program)) {
        EMF_LOG(kMod, "could not add the villager_jobs program: disabled");
        return false;
    }
    emf::on_event<gen::ecs::PickupEvent>(EMF_WORLD_SERVER, [](emf::World w, const gen::ecs::PickupEvent& e) {
        const uint64_t now = GetTickCount64();
        g_book.on_output(e.pickedUpByEntityId.id, e.itemId.value, now, e.pickedUpFromEntityId.id);
        // A farm job's harvest arrived: re-arm its plant in this same step, before the game removes it.
        for (uint32_t root : g_book.on_pickup(e.pickedUpByEntityId.id, e.pickedUpFromEntityId.id, now)) rearm(w, root);
    });
    api->on_load([](uint16_t slot, void*) {
        g_slot = slot;
        g_loaded_session = emf::server_session();
    }, nullptr);
    emf::add_command("jobs", ": the villagers' jobs", cmd_jobs);
    emf::add_command("job", "<villager> <chest> <recipe id hex>: give a villager a job (host)", cmd_job);
    emf::add_command("job.stop", "<villager>: end a villager's job", cmd_stop);
    emf::add_command("job.recall", "[villager]: unstick a villager (host)",
                     cmd_recall);
    emf::add_command("job.recipes", "<villager> [chest]: what the villager can make (from the chest)", cmd_recipes);
    emf::add_command("job.farm", "<villager> <chest>: farm the crops around the chest (host)",
                     [](emf::World w, const std::vector<std::string>& a) { cmd_work(w, a, jobs::JobKind::Farm); });
    emf::add_command("job.tend", "<villager> <chest>: tend the animals around the chest (host)",
                     [](emf::World w, const std::vector<std::string>& a) { cmd_work(w, a, jobs::JobKind::Tend); });
    emf::add_command("job.haul", "<villager> <from chest> <to chest>: carry items between two chests (host)",
                     [](emf::World w, const std::vector<std::string>& a) { cmd_work(w, a, jobs::JobKind::Haul); });
    emf::add_command("job.gather", "<villager> <chest>: gather the wild pickables around the chest into it (host)",
                     [](emf::World w, const std::vector<std::string>& a) { cmd_work(w, a, jobs::JobKind::Gather); });
    emf::on_tick(on_tick);
    EMF_LOG(kMod, "ready: look at a villager and press %s, then at a chest (every %llu s per item)", g_cfg.key_name.c_str(),
            g_cfg.interval_ms / 1000ull);
    return true;
}
