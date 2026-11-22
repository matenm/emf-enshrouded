#pragma once
// path_find (API v37): mods' path queries on the game's own path grid, answered by a callback after a step or more
// (private research pathfinding_2026-11-12.md, path_query_impl_2026-11-12.md). The service keeps the queries and
// decides; EMF's internal server program calls step() with the game's calls (path_glue.cpp), tests with fakes.
#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "emf/emf_api.h"

namespace emf::core {

constexpr size_t kMaxOpenPaths = 32;         // EMF's queries at once (the game's table of 1024 is shared by all walkers)
constexpr size_t kPathCreatesPerStep = 8;    // new queries sent per step
constexpr uint32_t kMaxPathPoints = 512;     // points kept for the callback
constexpr uint32_t kPathDefaultNodes = 2000, kPathMaxNodes = 20000;
constexpr float kPathMaxDistance = 200.0f;   // the game answers nothing farther apart than this, without searching
constexpr uint64_t kPathTimeoutMs = 10000;   // no answer by then (an unloaded place): ended as OTHER
constexpr float kPathReachAcross = 1.5f, kPathReachUpDown = 3.0f;  // `reaches`: the last point this near the target

struct PathCreature;

// What the game says about a query: done, its PathfindingResult and the points.
struct PathAnswer {
    bool done = false;
    uint32_t game_result = 0;
    std::vector<EmfPathPoint> points;
    std::vector<uint32_t> doors;  // the game's door entity per point (empty, or one per point)
};

// The game's path query calls, made from inside EMF's program.
struct PathEngine {
    // the game's query id (0: refused); as: the creature to plan as (null: none)
    std::function<uint64_t(const EmfPathQuery& q, uint32_t owner, const PathCreature* as)> create;
    std::function<PathAnswer(uint64_t id)> poll;
    std::function<void(uint64_t id, uint32_t owner)> destroy;
    std::function<bool(uint32_t entity)> exists;
};

// A finished query, for its callback (after the step).
struct PathDone {
    uint32_t id = 0;
    EmfPathCallback cb = nullptr;
    void* user = nullptr;
    EmfPathResult result{};
    std::vector<EmfPathPoint> points;
    std::vector<uint32_t> doors;  // one per kept point, or empty when no point is a door
    // the result as a mod's callback gets it: `doors` points into this PathDone (NULL when empty)
    EmfPathResult view() const {
        EmfPathResult r = result;
        r.doors = doors.empty() ? nullptr : doors.data();
        return r;
    }
};

// A creature's walking profile (API v48, EmfPathQuery.as_entity), from its templates: the room and node rules of its
// PathWalker (the game's own rule: trunc(radius x 2) and trunc(height x 2) half metres), the highest steps and start
// search radii of its PathFinder, and from its behaviour template its default moves (an enemy's door move when it can
// open doors) and default costs. Not what its DynamicPathFinder holds this step: the game rewrites that for each query
// (an enemy in Guard or a fight adds its extended moves, a roaming one its road costs).
struct PathCreature {
    uint16_t moves = EMF_PATH_WALK;        // PathMoveType bits (EMF_PATH_*)
    uint32_t across = 0, up = 0;           // the room it needs, half metres
    uint32_t required = 0, forbidden = 0;  // node rules (PathNodeType bits)
    float cost[20] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};  // per PathNodeType, as the query holds them
    bool has_finder = false;               // the PathFinder template's fields below were read
    uint32_t start_search[2] = {};         // start search radius across, up (half metres)
    float height_diff[3] = {};             // the highest step on terrain, buildings, colliders (metres)
};
// A creature's templates (each null when it has none) with their sizes.
struct PathCreatureParts {
    const void* walker = nullptr;  // PathWalker (needed)
    size_t walker_size = 0;
    const void* finder = nullptr;  // PathFinder
    size_t finder_size = 0;
    const void* enemy = nullptr;  // Enemy01BehaviorState (Vukah, the game's enemies and beasts)
    size_t enemy_size = 0;
    const void* wildlife = nullptr;  // Wildlife
    size_t wildlife_size = 0;
    const void* npc = nullptr;  // ActiveNpcState (villagers: no moves in it)
    size_t npc_size = 0;
    const void* animal = nullptr;  // Animal (no moves in it)
    size_t animal_size = 0;
};
// False without a PathWalker, or when a template has another size (a game update changed it).
bool read_path_creature(const PathCreatureParts& p, PathCreature* out);
// A component of one entity by its hash (types.json qualifiedHash): its data and *size, or null.
using PathComponentFn = std::function<const void*(uint32_t hash, uint64_t* size)>;
// as_entity: the creature must have a DynamicPathFinder (get: a creature the game plans paths for) and a PathWalker
// template (tpl, which also gives the others). "" and *out, or why the query is refused.
std::string path_creature_of(uint32_t entity, const PathComponentFn& get, const PathComponentFn& tpl, PathCreature* out);

// The moves, jumps and room a query plans with (the result's v48 fields): the mod's, and with a creature its profile
// under them. Moves: the creature's | the mod's | walk. Jump heights: the mod's. Room across and up: the mod's where it
// gives them (not 0), else the creature's; with the door move the room across is 0 unless the mod gives a width (a door
// node has no room around it, so any room loses the door).
struct PathUsed {
    uint16_t moves = 0;
    uint8_t jump_up = 0, jump_down = 0;
    uint32_t across = 0, up = 0;  // half metres
};
PathUsed path_used(const EmfPathQuery& in, const PathCreature* as);

// The game's 0x110-byte path query (private research path_query_impl_2026-11-12.md 3.2).
constexpr size_t kPathQueryBytes = 0x110;
// Writes EMF's fields over the game's defaults (PathQueryDefaults filled `q` first): owner, start, target, targetType
// (2 = closest node as fallback), maxIterations, moveTypes (walk always), jump heights, the vertical search radii
// (4 half metres, as the game's own callers), the room the unit needs (half metres, rounded up), the time-slice flag.
// With a creature (v48) its profile goes in at the offsets the game's own program writes it (node rules for the path
// and the target, the 20 costs, the highest steps, its start search radii where larger than EMF's), with path_used's
// values and the same room for the target.
void fill_path_query(uint8_t q[kPathQueryBytes], const EmfPathQuery& in, uint32_t owner,
                     const PathCreature* as = nullptr);

// The game's PathfindingResult and the number of points -> EMF_PATH_*.
uint32_t path_status(uint32_t game_result, size_t points);
// One line about an answer (the console's path command).
std::string path_answer_text(uint32_t id, const EmfPathResult& r, const EmfPathPoint* points);
// The path command's second line: the moves, jump heights and room the search planned with ("" for a result older
// than v48).
std::string path_used_text(uint32_t id, const EmfPathResult& r);
// "" when the query can be sent, else why not.
std::string path_query_check(const EmfPathQuery& q);

class PathService {
public:
    // Checks and queues a query (owner: an entity the game ties it to; as: a creature's settings to plan with, copied).
    // Its id (> 0), or 0 and *why. Any thread.
    uint32_t request(const EmfPathQuery& q, uint32_t owner, EmfPathCallback cb, void* user, uint64_t now_ms,
                     std::string* why, const PathCreature* as = nullptr);
    // Ends a query as EMF_PATH_CANCELLED at the next step. False for an unknown or finished id. Any thread.
    bool cancel(uint32_t id);
    // Inside EMF's program on the server world: sends waiting queries, reads answers; returns the finished ones.
    std::vector<PathDone> step(const PathEngine& e, uint64_t now_ms);
    // Every query ends as cancelled, without calling the game (a new world, or the watchdog). created, when given,
    // gets the (game id, owner) of the queries the game has, for destroying them later in the same world.
    std::vector<PathDone> world_reset(std::vector<std::pair<uint64_t, uint32_t>>* created = nullptr);
    size_t open() const;

private:
    struct Query {
        uint32_t id = 0;
        EmfPathQuery q{};
        uint32_t owner = 0;
        EmfPathCallback cb = nullptr;
        void* user = nullptr;
        uint64_t asked_ms = 0;
        uint64_t game_id = 0;  // 0 until sent
        bool cancelled = false;
        bool has_as = false;
        PathCreature as{};
    };
    PathDone finish(const Query& q, uint32_t status, const std::vector<EmfPathPoint>& pts,
                    const std::vector<uint32_t>& doors = {}) const;

    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    std::vector<Query> queries_;
    uint32_t next_id_ = 1;
};

}  // namespace emf::core
