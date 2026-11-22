// path_find (path_service.h).
#include "core/path_service.h"

#include "core/api_sizes.h"
#include "emf/generated/components.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>

namespace emf::core {

namespace {
constexpr float kWorldLimit = 100000.0f;  // metres: anything beyond is a bad number

bool finite3(const float v[3]) {
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(v[i]) || std::fabs(v[i]) > kWorldLimit) return false;
    return true;
}
float dist(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
constexpr uint16_t kKnownMoves = EMF_PATH_WALK | EMF_PATH_JUMP_UP | EMF_PATH_JUMP_DOWN | EMF_PATH_JUMP_ACROSS |
                                 EMF_PATH_OPEN_DOOR | EMF_PATH_SWIM;
uint32_t half_metres(float m) { return m > 0 ? static_cast<uint32_t>(std::ceil(m * 2.0f)) : 0u; }

// The query's 20 costs (per PathNodeType) from PathNodeCostFactors' 17 floats, as the game's `pathfinding` program
// fills them (game 0x14046c083..0x14046c182): the factor's index for each query slot.
constexpr int kCostFrom[20] = {0, 0, 4, 5, 6, 7, 8, 9, 10, 1, 2, 3, 11, 11, 11, 12, 14, 15, 16, 13};
}  // namespace

namespace {
// The query's costs from a behaviour's default cost factors.
void put_costs(const emf::gen::ecs::PathNodeCostFactors& f, PathCreature* c) {
    float factors[17];
    static_assert(sizeof factors == sizeof f);
    std::memcpy(factors, &f, sizeof factors);
    for (int i = 0; i < 20; ++i) c->cost[i] = factors[kCostFrom[i]];
}
// Copies a template of type T when it is there and has T's size; false when it has another size.
template <class T>
bool take(const void* p, size_t size, T* out, bool* had) {
    *had = false;
    if (!p) return true;
    if (size != sizeof(T)) return false;
    std::memcpy(out, p, sizeof(T));
    *had = true;
    return true;
}
}  // namespace

bool read_path_creature(const PathCreatureParts& p, PathCreature* out) {
    namespace g = emf::gen::ecs;
    if (!out || !p.walker) return false;
    // large templates: kept off the stack
    struct All {
        g::PathWalker walker;
        g::PathFinder finder;
        g::Enemy01BehaviorState enemy;
        g::Wildlife wildlife;
        g::ActiveNpcState npc;
        g::Animal animal;
    };
    auto t = std::make_unique<All>();
    bool has_walker = false, has_finder = false, has_enemy = false, has_wildlife = false, has_npc = false,
         has_animal = false;
    if (!take(p.walker, p.walker_size, &t->walker, &has_walker) || !take(p.finder, p.finder_size, &t->finder, &has_finder) ||
        !take(p.enemy, p.enemy_size, &t->enemy, &has_enemy) ||
        !take(p.wildlife, p.wildlife_size, &t->wildlife, &has_wildlife) || !take(p.npc, p.npc_size, &t->npc, &has_npc) ||
        !take(p.animal, p.animal_size, &t->animal, &has_animal))
        return false;
    PathCreature c;
    // the game's own rule (path_walking, game 0x1404682ab): truncated to half metres
    c.across = t->walker.horizontalRadius > 0 ? static_cast<uint32_t>(t->walker.horizontalRadius * 2.0f) : 0u;
    c.up = t->walker.height > 0 ? static_cast<uint32_t>(t->walker.height * 2.0f) : 0u;
    c.required = t->walker.pathNodeRequirements.requiredTypes;
    c.forbidden = t->walker.pathNodeRequirements.forbiddenTypes;
    if (has_enemy) {
        c.moves = static_cast<uint16_t>(EMF_PATH_WALK | t->enemy.movementSetup.defaultMoveTypes |
                                        (t->enemy.openDoorSetup.canOpenDoors ? EMF_PATH_OPEN_DOOR : 0));
        put_costs(t->enemy.pathNodeSetup.defaultCostFactors, &c);
    } else if (has_wildlife) {
        c.moves = static_cast<uint16_t>(EMF_PATH_WALK | t->wildlife.movementSetup.defaultMoveTypes);
        put_costs(t->wildlife.pathNodeSetup.defaultCostFactors, &c);
    } else if (has_npc) {
        put_costs(t->npc.pathNodeSetup.defaultCostFactors, &c);
    } else if (has_animal) {
        put_costs(t->animal.pathNodeSetup.defaultCostFactors, &c);
    }
    if (has_finder) {
        c.has_finder = true;
        c.start_search[0] = t->finder.horizontalStartSearchRadius.halfMeters;
        c.start_search[1] = t->finder.verticalStartSearchRadius.halfMeters;
        c.height_diff[0] = t->finder.settings.maxHeightDifferenceSettings.terrain;
        c.height_diff[1] = t->finder.settings.maxHeightDifferenceSettings.building;
        c.height_diff[2] = t->finder.settings.maxHeightDifferenceSettings.collider;
    }
    *out = c;
    return true;
}

std::string path_creature_of(uint32_t entity, const PathComponentFn& get, const PathComponentFn& tpl, PathCreature* out) {
    namespace g = emf::gen::ecs;
    const std::string who = "as_entity " + std::to_string(entity);
    uint64_t size = 0;
    if (!get || !get(g::DynamicPathFinder::kHash, &size))
        return who + " has no DynamicPathFinder (not a creature the game plans paths for, or not in the server world)";
    PathCreatureParts p;
    auto one = [&](uint32_t hash, const void** ptr, size_t* n) {
        uint64_t s = 0;
        *ptr = tpl ? tpl(hash, &s) : nullptr;
        *n = *ptr ? static_cast<size_t>(s) : 0;
    };
    one(g::PathWalker::kHash, &p.walker, &p.walker_size);
    if (!p.walker) return who + " has no PathWalker template (not a creature that walks by the path grid)";
    one(g::PathFinder::kHash, &p.finder, &p.finder_size);
    one(g::Enemy01BehaviorState::kHash, &p.enemy, &p.enemy_size);
    one(g::Wildlife::kHash, &p.wildlife, &p.wildlife_size);
    one(g::ActiveNpcState::kHash, &p.npc, &p.npc_size);
    one(g::Animal::kHash, &p.animal, &p.animal_size);
    if (!read_path_creature(p, out))
        return who + ": its path templates have another layout on this game build (EMF needs an update)";
    return {};
}

PathUsed path_used(const EmfPathQuery& in, const PathCreature* as) {
    PathUsed u;
    u.moves = static_cast<uint16_t>(in.moves | EMF_PATH_WALK | (as ? as->moves : 0));
    u.jump_up = in.max_jump_up;
    u.jump_down = in.max_jump_down;
    if (in.width > 0 || !as) u.across = half_metres(in.width);
    else u.across = (u.moves & EMF_PATH_OPEN_DOOR) ? 0u : as->across;
    u.up = in.height > 0 || !as ? half_metres(in.height) : as->up;
    return u;
}

void fill_path_query(uint8_t q[kPathQueryBytes], const EmfPathQuery& in, uint32_t owner, const PathCreature* as) {
    auto put = [&](size_t off, const void* v, size_t n) { std::memcpy(q + off, v, n); };
    auto put_u32 = [&](size_t off, uint32_t v) { put(off, &v, 4); };
    auto get_u32 = [&](size_t off) {
        uint32_t v = 0;
        std::memcpy(&v, q + off, 4);
        return v;
    };
    const PathUsed u = path_used(in, as);
    put_u32(0x00, owner);
    put(0x10, in.from, 12);
    put(0x1c, in.to, 12);
    q[0x36] = in.closest ? 2 : 0;
    put_u32(0x48, in.max_nodes ? in.max_nodes : kPathDefaultNodes);
    put(0x54, &u.moves, 2);
    q[0x56] = u.jump_up;
    q[0x57] = u.jump_down;
    q[0xca] = 1;
    // search radii (half metres): the game's defaults across, 4 up as the game's own aggro and animal queries
    uint32_t start_search[2] = {2, 4}, search[2] = {4, 4};
    if (as) {  // the creature's profile, where the game's program puts a creature's own
        put_u32(0x5c, as->required);
        put_u32(0x60, as->forbidden);
        put_u32(0x64, as->required);  // the target follows the path's rules (as the game's without target rules)
        put_u32(0x68, as->forbidden);
        put(0x6c, as->cost, sizeof as->cost);
        if (as->has_finder) {
            put(0xbc, as->height_diff, sizeof as->height_diff);
            start_search[0] = (std::max)(start_search[0], as->start_search[0]);
            start_search[1] = (std::max)(start_search[1], as->start_search[1]);
        }
        put_u32(0x108, u.across);
        put_u32(0x10c, u.up);
    }
    put_u32(0xf0, start_search[0]);
    put_u32(0xf4, start_search[1]);
    put_u32(0xf8, search[0]);
    put_u32(0xfc, search[1]);
    put_u32(0x100, u.across);
    put_u32(0x104, u.up);
    // closed doors (node type 19) only with the door move: a walk or jump move may otherwise enter a ClosedDoor node that
    // counts as ground (a closed gate with a floor piece under it was walked through); the door move skips node rules
    put_u32(0x60, get_u32(0x60) | (1u << 19));
}

std::string path_answer_text(uint32_t id, const EmfPathResult& r, const EmfPathPoint* points) {
    const std::string head = "path " + std::to_string(id) + ": ";
    switch (r.status) {
        case EMF_PATH_FOUND: break;
        case EMF_PATH_NO_PATH: return head + "no path";
        case EMF_PATH_TOO_LONG: return head + "too long (the search ran out of nodes)";
        case EMF_PATH_NO_START: return head + "no path grid at the start (not loaded, or inside something)";
        case EMF_PATH_NO_TARGET: return head + "no path grid at the target (not loaded, or inside something)";
        case EMF_PATH_NO_MEMORY: return head + "the search ran out of memory";
        case EMF_PATH_CANCELLED: return head + "cancelled";
        default: return head + "no answer from the game";
    }
    uint32_t jumps = 0, doors = 0;
    std::string named;  // the door entities the game names (v42 results)
    const bool has_doors = r.size >= offsetof(EmfPathResult, doors) + sizeof(r.doors) && r.doors;
    for (uint32_t i = 0; points && i < r.count; ++i) {
        if (points[i].move >= 2 && points[i].move <= 4) ++jumps;
        if (points[i].move == 5) ++doors;
        if (has_doors && r.doors[i] && named.size() < 200)
            named += (named.empty() ? "" : ", ") + std::to_string(r.doors[i]) + " at point " + std::to_string(i + 1);
    }
    char buf[256];
    const EmfPathPoint* last = points && r.count ? &points[r.count - 1] : nullptr;
    std::snprintf(buf, sizeof buf, "found, %s, %u points, %.1f m, ends at (%.1f, %.1f, %.1f); %u jump%s, %u door%s",
                  r.reaches ? "reaches the target" : "stops short of the target", r.count, r.length,
                  last ? last->x : 0.0f, last ? last->y : 0.0f, last ? last->z : 0.0f, jumps, jumps == 1 ? "" : "s",
                  doors, doors == 1 ? "" : "s");
    return head + buf + (named.empty() ? "" : " (" + named + ")");
}

std::string path_used_text(uint32_t id, const EmfPathResult& r) {
    if (r.size < offsetof(EmfPathResult, height) + sizeof(r.height)) return {};
    static const struct {
        uint16_t bit;
        const char* name;
    } kMoves[] = {{EMF_PATH_WALK, "walk"},           {EMF_PATH_JUMP_UP, "jump up"}, {EMF_PATH_JUMP_DOWN, "jump down"},
                  {EMF_PATH_JUMP_ACROSS, "jump across"}, {EMF_PATH_OPEN_DOOR, "door"}, {EMF_PATH_SWIM, "swim"}};
    std::string moves;
    for (const auto& m : kMoves)
        if (r.moves & m.bit) moves += (moves.empty() ? "" : ", ") + std::string(m.name);
    std::string jumps;
    if (r.moves & (EMF_PATH_JUMP_UP | EMF_PATH_JUMP_DOWN | EMF_PATH_JUMP_ACROSS)) {
        auto height = [](uint8_t m) { return m ? "at most " + std::to_string(m) + " m" : std::string("any height"); };
        jumps = "; jumps up " + height(r.jump_up) + ", down " + height(r.jump_down);
    }
    char room[64];
    std::snprintf(room, sizeof room, "; room %.1f m across, %.1f m up", r.width, r.height);
    return "path " + std::to_string(id) + ": planned with " + moves + jumps + room;
}

uint32_t path_status(uint32_t game_result, size_t points) {
    switch (game_result) {
        case 1: return points ? EMF_PATH_FOUND : EMF_PATH_NO_PATH;  // PathFound (also the closest-node fallback)
        case 2:                                                      // NoPathFound
        case 3:                                                      // NoFreePath
        case 4: return EMF_PATH_NO_PATH;                             // NoOccupiedPath
        case 5: return EMF_PATH_NO_START;
        case 6: return EMF_PATH_NO_TARGET;
        case 7: return EMF_PATH_TOO_LONG;                            // MaxIterationsReached
        case 8:                                                      // OpenListOutOfMemory
        case 9: return EMF_PATH_NO_MEMORY;                           // NodesOutOfMemory
        default: return EMF_PATH_OTHER;
    }
}

std::string path_query_check(const EmfPathQuery& q) {
    if (q.size < kPathQueryV37Size) return "the query's size field is not sizeof(EmfPathQuery)";
    if (!finite3(q.from) || !finite3(q.to) || !std::isfinite(q.width) || !std::isfinite(q.height) || q.width < 0 ||
        q.height < 0 || q.width > 64 || q.height > 64)
        return "bad numbers (positions in metres, width and height 0-64 m)";
    if (dist(q.from, q.to) > kPathMaxDistance) return "from and to are more than 200 m apart (the game does not search that far)";
    if (q.moves & ~kKnownMoves) return "unknown move bits (EMF_PATH_*)";
    if (q.max_nodes > kPathMaxNodes) return "max_nodes over 20000";
    return {};
}

uint32_t PathService::request(const EmfPathQuery& q, uint32_t owner, EmfPathCallback cb, void* user, uint64_t now_ms,
                              std::string* why, const PathCreature* as) {
    std::string w = path_query_check(q);
    if (w.empty() && !owner) w = "no owner entity (the game ties a query to one; pass owner, or have a player in the world)";
    if (!w.empty()) {
        if (why) *why = w;
        return 0;
    }
    AcquireSRWLockExclusive(&lock_);
    if (queries_.size() >= kMaxOpenPaths) {
        ReleaseSRWLockExclusive(&lock_);
        if (why) *why = "32 path queries are open already (wait for answers)";
        return 0;
    }
    Query x;
    x.id = next_id_++;
    if (!next_id_) next_id_ = 1;
    x.q = q;
    x.owner = owner;
    x.cb = cb;
    x.user = user;
    x.asked_ms = now_ms;
    if (as) {
        x.has_as = true;
        x.as = *as;
    }
    queries_.push_back(x);
    ReleaseSRWLockExclusive(&lock_);
    return x.id;
}

bool PathService::cancel(uint32_t id) {
    AcquireSRWLockExclusive(&lock_);
    bool found = false;
    for (Query& x : queries_)
        if (x.id == id && !x.cancelled) x.cancelled = found = true;
    ReleaseSRWLockExclusive(&lock_);
    return found;
}

PathDone PathService::finish(const Query& x, uint32_t status, const std::vector<EmfPathPoint>& pts,
                             const std::vector<uint32_t>& doors) const {
    PathDone d;
    d.id = x.id;
    d.cb = x.cb;
    d.user = x.user;
    d.result.size = sizeof(EmfPathResult);
    d.result.status = status;
    const PathUsed u = path_used(x.q, x.has_as ? &x.as : nullptr);
    d.result.moves = u.moves;
    d.result.jump_up = u.jump_up;
    d.result.jump_down = u.jump_down;
    d.result.width = static_cast<float>(u.across) * 0.5f;
    d.result.height = static_cast<float>(u.up) * 0.5f;
    if (status == EMF_PATH_FOUND && !pts.empty()) {
        const EmfPathPoint& last = pts.back();  // the game's last point, also when fewer are kept
        const float dx = last.x - x.q.to[0], dz = last.z - x.q.to[2], dy = last.y - x.q.to[1];
        d.result.reaches = std::sqrt(dx * dx + dz * dz) <= kPathReachAcross && std::fabs(dy) <= kPathReachUpDown;
    }
    const size_t keep = (std::min)(pts.size(), size_t{kMaxPathPoints});
    d.points.assign(pts.begin(), pts.begin() + keep);
    d.result.count = static_cast<uint32_t>(keep);
    // the game's door entity per point: only when it has one per point and names a door among the kept ones
    if (doors.size() == pts.size() && std::any_of(doors.begin(), doors.begin() + keep, [](uint32_t e) { return e != 0; }))
        d.doors.assign(doors.begin(), doors.begin() + keep);
    float len = 0;
    float at[3] = {x.q.from[0], x.q.from[1], x.q.from[2]};
    for (const EmfPathPoint& p : d.points) {
        const float to[3] = {p.x, p.y, p.z};
        len += dist(at, to);
        at[0] = p.x;
        at[1] = p.y;
        at[2] = p.z;
    }
    d.result.length = len;
    return d;
}

std::vector<PathDone> PathService::step(const PathEngine& e, uint64_t now_ms) {
    std::vector<PathDone> done;
    AcquireSRWLockExclusive(&lock_);
    std::vector<Query> work = queries_;  // the engine is called without the lock (cancel may come from any thread)
    ReleaseSRWLockExclusive(&lock_);
    std::vector<std::pair<uint32_t, uint64_t>> sent;  // id -> game id
    std::vector<uint32_t> ended;
    size_t creates = 0;
    for (Query& x : work) {
        auto end = [&](uint32_t status, const std::vector<EmfPathPoint>& pts, const std::vector<uint32_t>& doors = {}) {
            if (x.game_id && e.destroy) e.destroy(x.game_id, x.owner);  // the game never drops a query by itself
            done.push_back(finish(x, status, pts, doors));
            ended.push_back(x.id);
        };
        if (x.cancelled || (e.exists && !e.exists(x.owner))) {
            end(EMF_PATH_CANCELLED, {});
            continue;
        }
        if (!x.game_id) {
            if (creates >= kPathCreatesPerStep || !e.create) continue;
            ++creates;
            x.game_id = e.create(x.q, x.owner, x.has_as ? &x.as : nullptr);
            if (!x.game_id) {
                end(EMF_PATH_OTHER, {});  // the game's command buffer was full
                continue;
            }
            sent.push_back({x.id, x.game_id});
            continue;  // the record exists after this step's flush: the answer comes later
        }
        const PathAnswer a = e.poll ? e.poll(x.game_id) : PathAnswer{};
        if (a.done) {
            end(path_status(a.game_result, a.points.size()), a.points, a.doors);
        } else if (now_ms - x.asked_ms > kPathTimeoutMs) {
            end(EMF_PATH_OTHER, {});
        }
    }
    AcquireSRWLockExclusive(&lock_);
    for (PathDone& d : done)  // cancelled while the step talked to the game: the answer says so
        for (const Query& x : queries_)
            if (x.id == d.id && x.cancelled && d.result.status != EMF_PATH_CANCELLED) d = finish(x, EMF_PATH_CANCELLED, {});
    for (const auto& [id, gid] : sent)
        for (Query& x : queries_)
            if (x.id == id) x.game_id = gid;
    queries_.erase(std::remove_if(queries_.begin(), queries_.end(),
                                  [&](const Query& x) {
                                      return std::find(ended.begin(), ended.end(), x.id) != ended.end();
                                  }),
                   queries_.end());
    ReleaseSRWLockExclusive(&lock_);
    return done;
}

std::vector<PathDone> PathService::world_reset(std::vector<std::pair<uint64_t, uint32_t>>* created) {
    AcquireSRWLockExclusive(&lock_);
    std::vector<Query> all;
    all.swap(queries_);
    ReleaseSRWLockExclusive(&lock_);
    std::vector<PathDone> done;
    for (const Query& x : all) {
        if (created && x.game_id) created->push_back({x.game_id, x.owner});
        done.push_back(finish(x, EMF_PATH_CANCELLED, {}));
    }
    return done;
}

size_t PathService::open() const {
    AcquireSRWLockShared(&lock_);
    const size_t n = queries_.size();
    ReleaseSRWLockShared(&lock_);
    return n;
}

}  // namespace emf::core
