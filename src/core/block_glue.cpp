// Breaking blocks: the emf_blocks program (block_glue.h).
#include "core/block_glue.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "common/seh.h"
#include "common/world_view.h"
#include "core/voxel_read.h"

namespace emf::core {

namespace {

// Name order within each list (the engine's iterator order; each slot is checked against the world's singleton).
const char* const kRead[] = {"BalancingRegistry", "TerraformingEfficiencyRegistry", "VoxelBlueprintItemRegistry"};
const char* const kWrite[] = {"ServerVoxelDamageSystem", "VoxelWorld", "ecs::ServerSnappingSystem"};

constexpr size_t kCfgItem = 0x04, kCfgHardness = 0x14, kCfgLootRate = 0x18, kCfgHealth = 0x1C;  // BuildingTerraformingConfig
constexpr size_t kBalancingSettings = 0x90, kSettingsMiningFactor = 0x30;  // *(BalancingRegistry + 0x90) + 0x30

struct FloatSlice {
    const float* data;
    uint64_t count;
};
struct alignas(16) Vec4 {
    float x, y, z, w;
};
struct KernelStats {
    float* per_material;
    uint64_t cap;
    uint8_t main;
    uint8_t pad[3];
    float damaged;  // +0x14
    float removed;  // +0x18
    uint8_t spare[0x24];
};
static_assert(offsetof(KernelStats, damaged) == 0x14 && offsetof(KernelStats, removed) == 0x18, "kernel stats layout");

struct PendingDrop {
    uint8_t guid[16];
    uint32_t count;
    float pos[3];
};

std::unique_ptr<BlockService> g_service;
BlockGlueDeps g_deps;
std::mutex g_state_lock;  // g_why
std::string g_why = "not available on this game version";
std::atomic<bool> g_on{false};
std::atomic<uint64_t> g_last_run_ms{0};
std::atomic<bool> g_watchdog_said{false}, g_cb_fault_said{false}, g_kernel_said{false}, g_factor_said{false};
std::vector<PendingDrop> g_drops;  // inside the program only
TerrainDamage g_terrain_damage;    // v47: EMF's damage of terrain cells (inside the program only)
std::atomic<bool> g_terrain_off{false};
std::atomic<bool> g_terrain_reset{false};  // a new world: the program forgets the terrain damage at its next pass
std::string g_terrain_why;                 // why breaking terrain went off (g_state_lock)

uint64_t now() { return g_deps.now_ms ? g_deps.now_ms() : GetTickCount64(); }
void say(const std::string& line) {
    if (g_deps.log) g_deps.log("breaking blocks: " + line);
}
void turn_off(const std::string& why) {
    {
        std::lock_guard<std::mutex> l(g_state_lock);
        g_why = why;
    }
    if (g_on.exchange(false)) say(why);
}
// Breaking terrain alone goes off (breaking building blocks stays on); why: what a terrain request is told from now.
void terrain_turn_off(const std::string& why) {
    {
        std::lock_guard<std::mutex> l(g_state_lock);
        g_terrain_why = "breaking terrain is off: " + why;
    }
    if (!g_terrain_off.exchange(true)) say(why);
}

bool guarded_cb(EmfBreakCallback cb, EmfWorld* w, uint32_t id, const EmfBreakResult* r, void* u) {
    __try {
        cb(w, id, r, u);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool call_kernel(decltype(BlockFns::kernel) fn, uint8_t* op, KernelStats* st, const FloatSlice* t, const Vec4* c,
                 float r, uint8_t mode, bool* ran) {
    __try {
        *ran = fn(op, nullptr, st, t, &c->x, r, mode);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool call_config(decltype(BlockFns::config_find) fn, void* reg, uint8_t id, const uint8_t** out) {
    __try {
        *out = fn(reg, id);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool call_box_fill(decltype(BlockFns::box_fill_air) fn, void* editor, void* voxel_data, const void* box) {
    __try {
        alignas(16) const uint64_t zero[2] = {0, 0};  // the kernel passes &{0, 0}
        fn(editor, nullptr, voxel_data, box, zero, 0);  // grow 0: exactly the box (1 also clears the ground around it)
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool base_reader_usable();  // below, with the shipped world's reader

// v47: breaking terrain needs the terrain config lookup, the box fill, the voxel read and the shipped world's reader
// (without it, or after it faulted, no terrain cell could be judged).
bool terrain_usable() {
    const BlockFns& f = g_deps.fns;
    return !g_terrain_off.load() && f.terrain_config_find && f.box_fill_air && f.region_read && base_reader_usable();
}

// Drops, then each callback under its own guard, after the step on the server world's thread.
void deliver(std::vector<BlockDone> done, std::vector<PendingDrop> drops) {
    if ((done.empty() && drops.empty()) || !g_deps.after_step) return;
    auto d = std::make_shared<std::vector<BlockDone>>(std::move(done));
    auto p = std::make_shared<std::vector<PendingDrop>>(std::move(drops));
    g_deps.after_step([d, p](EmfWorld* w) {
        for (const PendingDrop& x : *p)
            if (g_deps.drop) g_deps.drop(w, x.guid, x.count, x.pos);
        for (const BlockDone& b : *d)
            if (b.cb && !guarded_cb(b.cb, w, b.id, &b.result, b.user) && !g_cb_fault_said.exchange(true))
                say("a mod's break callback faulted (request " + std::to_string(b.id) + "); the others still came");
    });
}

std::vector<BlockDone> failed(std::vector<BlockDone> done) {
    for (BlockDone& b : done) b.result.status = EMF_BREAK_FAILED;
    return done;
}

// The handles from the program's iterator, each checked against the world's own singleton.
bool gather(const ProgramCall& call, BlockHandles& h, std::string& why) {
    if (!call.iter) {
        why = "no iterator (IterBegin not usable)";
        return false;
    }
    const WorldView view(call.world);
    auto slot = [&](const char* name, void** out) -> bool {
        const auto s = program_singleton_slot(call, name);
        *out = s && *s < kProgramIterQwords ? reinterpret_cast<void*>(call.iter[*s]) : nullptr;
        void* own = call.world ? view.singleton(name) : nullptr;
        if (!*out || (own && own != *out)) {
            why = std::string("the ") + name + " slot does not match the world's singleton (iterator layout?)";
            return false;
        }
        return true;
    };
    void* voxel_world = nullptr;
    if (!slot("BalancingRegistry", &h.balancing) || !slot("TerraformingEfficiencyRegistry", &h.efficiency) ||
        !slot("VoxelBlueprintItemRegistry", &h.blueprint_items) || !slot("ServerVoxelDamageSystem", &h.damage_store) ||
        !slot("VoxelWorld", &voxel_world) || !slot("ecs::ServerSnappingSystem", &h.snapping))
        return false;
    const auto* vw = static_cast<const uint8_t*>(voxel_world);
    if (!seh_read(vw, &h.voxel_data, 8) || !seh_read(vw + 8, &h.editor, 8) || !h.voxel_data || !h.editor) {
        why = "the VoxelWorld singleton holds no voxel data or editor";
        return false;
    }
    h.program_context = call.ctx;
    return true;
}

float mining_factor(void* balancing) {
    const uint8_t* settings = nullptr;
    float f = 0;
    if (balancing && seh_read(static_cast<const uint8_t*>(balancing) + kBalancingSettings, &settings, 8) && settings &&
        seh_read(settings + kSettingsMiningFactor, &f, 4) && std::isfinite(f) && f > 0.0f && f <= 100.0f)
        return f;
    if (!g_factor_said.exchange(true)) say("the world's mining damage setting could not be read: 1.0 is used");
    return 1.0f;
}

float shrink_sphere(void* editor, const Sphere& s, const float table[256]);  // below, after the reads
bool terrain_pass(const BlockHandles& h, const Sphere& s, const float table[128], bool instant, KernelCount* k);

void blocks_program(void* ctx, void*) {
    if (!g_service) return;
    if (!g_on.load()) {  // off since a fault: what still waits ends FAILED now, not CANCELLED by the watchdog later
        if (g_service->waiting()) deliver(failed(g_service->cancel_all()), {});
        return;
    }
    const ProgramCall* call = current_program_call();
    if (!call) return;
    g_last_run_ms = now();
    if (!g_service->waiting()) return;
    BlockHandles h;
    std::string why;
    if (!gather(*call, h, why)) {
        turn_off("off (" + why + "); breaks are refused until a restart");
        deliver(failed(g_service->cancel_all()), {});
        return;
    }
    (void)ctx;
    BlockEngine e;
    e.facts = [&](MaterialFacts out[128]) {
        if (blocks_read_facts(h.efficiency, out)) return true;
        turn_off("off (the game's material config lookup faulted); breaks are refused until a restart");
        return false;
    };
    e.mining_factor = [&] { return mining_factor(h.balancing); };
    e.kernel = [&](const Sphere& s, const float table[256], bool instant, KernelCount* k) -> bool {
        if (!g_on.load()) return false;
        alignas(16) uint8_t op[kBlockOpBytes];
        fill_block_op(op, h, instant);
        const FloatSlice t{table, 256};
        const Vec4 c{s.x, s.y, s.z, 0.0f};
        float per[256];
        KernelStats st{};
        st.per_material = per;
        st.cap = 256;
        bool ran = false;
        for (uint8_t mode = instant ? 1 : 0; mode <= 1; ++mode) {
            if (!call_kernel(g_deps.fns.kernel, op, &st, &t, &c, s.r, mode, &ran)) {
                turn_off("off (the game's building kernel faulted); breaks are refused until a restart");
                return false;
            }
            if (!ran) {
                if (!g_kernel_said.exchange(true)) say("the game's kernel did not run for a sphere (its snap query failed?)");
                return true;
            }
            if (mode == 0 || instant) k->damaged += static_cast<uint32_t>(st.damaged);
            if (mode == 1) {
                k->removed += static_cast<uint32_t>(st.removed);
                for (int m = 0; m < 256; ++m) k->removed_per_material[m] += per[m];
            }
        }
        return true;
    };
    e.shrink = [&](const Sphere& s, const float table[256]) -> float { return shrink_sphere(h.editor, s, table); };
    if (terrain_usable()) {  // v47 EMF_BREAK_TERRAIN
        e.terrain_facts = [&](MaterialFacts out[128]) { return blocks_read_terrain_facts(h.efficiency, out); };
        e.terrain = [&](const Sphere& s, const float table[128], bool instant, KernelCount* k) -> bool {
            if (!g_on.load()) return false;
            return terrain_pass(h, s, table, instant, k);
        };
    }
    e.drop = [&](const uint8_t guid[16], uint32_t count, const float pos[3]) -> uint32_t {
        PendingDrop d{};
        std::memcpy(d.guid, guid, 16);
        d.count = count;
        std::memcpy(d.pos, pos, sizeof d.pos);
        g_drops.push_back(d);
        return count;
    };
    std::vector<BlockDone> done = g_service->step(e);
    std::vector<PendingDrop> drops;
    drops.swap(g_drops);
    deliver(std::move(done), std::move(drops));
}

// ---- the reads (block_at, find_wall) ----
struct ReadReq {
    uint16_t* buf;
    uint64_t cap;
    uint32_t sx, sy, sz, level;
    int32_t x, y, z, pad;
};
static_assert(sizeof(ReadReq) == 0x30, "the region read's request stride");
struct ReadSlice {
    const ReadReq* data;
    uint64_t count;
};
constexpr uint32_t kReadLevel = 6;  // the level every VoxelWorld caller reads (the full 0.5 m cells)

bool call_region(decltype(BlockFns::region_read) fn, const ReadSlice* s, void* editor, bool* ok) {
    __try {
        *ok = fn(s, editor);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool call_damage(decltype(BlockFns::damage_get) fn, void* store, const uint32_t cell[3], uint8_t* out) {
    __try {
        *out = fn(store, cell);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
std::atomic<bool> g_reads_off{false};

// ---- the shipped world's voxels (ruin protection, private research ruin_blocks_2026-11-16.md 4.1) ----
constexpr size_t kColumnHandle = 0xA0;
constexpr uint32_t kBaseLevel = 6;  // the level every VoxelWorld caller reads (the full 0.5 m cells)
std::atomic<bool> g_base_off{false};  // a fault: every building cell counts unknown (protected) until a restart
std::atomic<bool> g_base_test_off{false};
std::atomic<bool> g_noread_said{false}, g_nobase_said{false}, g_unloaded_said{false};

bool base_reader_usable() {
    return !g_base_test_off.load() && !g_base_off.load() && g_deps.fns.column_acquire && g_deps.fns.brick_seek &&
           g_deps.fns.brick_decode && g_deps.fns.column_release;
}

int32_t floor_div8(int32_t c) { return c >= 0 ? c / 8 : -((-c + 7) / 8); }

// The column reader in three guarded stages (private research ruin_blocks 4.1): acquire takes the column's lock and a
// reference on its resource, so the release and the unlock run whenever acquire returned, even after a fault in the
// read between (a lock left taken would hang the game's next writer). A fault inside acquire itself cannot be undone.
bool column_acquire_raw(void* editor, int32_t bx, int32_t bz, uint8_t* h) {
    __try {
        g_deps.fns.column_acquire(h, editor, kBaseLevel,
                                  (static_cast<uint64_t>(static_cast<uint32_t>(bz)) << 32) | static_cast<uint32_t>(bx));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// 1 the brick is read into out, 0 unknown, -1 a fault.
int column_read_raw(uint8_t* h, int32_t by, uint16_t out[512]) {
    __try {
        void* data = nullptr;
        std::memcpy(&data, h + 0x20, 8);
        if (h[0x90] == 0 || data == nullptr) return 0;  // not loaded; a tile outside the world has no data (read as air)
        if (!g_deps.fns.brick_seek(h + 0x30, static_cast<uint32_t>(by))) {
            for (int i = 0; i < 512; ++i) out[i] = 0;  // above the column's bricks: air
            return 1;
        }
        if (h[0x80]) {  // one material for the whole brick
            const uint8_t* mp = nullptr;
            const uint8_t* fp = nullptr;
            std::memcpy(&mp, h + 0x68, 8);
            std::memcpy(&fp, h + 0x70, 8);
            if (!mp) return 0;  // the game always has one: not the layout EMF knows
            const uint8_t m = *mp;
            const uint16_t f = fp ? *fp : static_cast<uint16_t>(m ? 0xFF : 0);
            for (int i = 0; i < 512; ++i) out[i] = static_cast<uint16_t>(m | (f << 8));
            return 1;
        }
        uint32_t end = 0, size = 0;
        const uint8_t* start = nullptr;
        std::memcpy(&end, h + 0x60, 4);
        std::memcpy(&size, h + 0x88, 4);
        std::memcpy(&start, h + 0x50, 8);
        if (!start || size == 0 || size > end) return 0;  // not the layout EMF knows
        struct {
            uint16_t* p;
            uint64_t n;
        } dst{out, 512};
        struct {
            const uint8_t* p;
            uint64_t n;
        } src{start + (end - size), size};
        uint8_t res[8] = {};
        g_deps.fns.brick_decode(res, &dst, &src);
        return res[0] == 0 ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
bool column_release_raw(uint8_t* h) {
    __try {
        void* data = nullptr;
        std::memcpy(&data, h + 0x20, 8);
        if (data) {
            void* loader = nullptr;
            uint32_t id = 0;
            std::memcpy(&loader, h + 0x10, 8);
            std::memcpy(&id, h + 0x18, 4);
            g_deps.fns.column_release(loader, id);
        }
        long* lock = nullptr;
        std::memcpy(&lock, h, 8);
        if (lock) {
            if (h[8] == 1)
                _InterlockedDecrement(lock);
            else if (h[8] == 2)
                _InterlockedExchangeAdd(lock, 0x7FFF0000);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// One base brick (512 cells, x + 8y + 64z) through the game's column reader, as its own region read does when a brick
// was never changed; false when unknown (a column not loaded, a layout EMF does not know). *faulted on a fault.
bool base_brick_raw(void* editor, int32_t bx, int32_t by, int32_t bz, uint16_t out[512], bool* faulted) {
    alignas(16) uint8_t h[kColumnHandle] = {};
    *faulted = false;
    if (!column_acquire_raw(editor, bx, bz, h)) {
        *faulted = true;
        return false;
    }
    const int read = column_read_raw(h, by, out);
    if (read < 0) *faulted = true;
    if (!column_release_raw(h)) *faulted = true;
    return read == 1;
}

// The base bricks one read touches (a break sphere: at most 3x3x3), each read once.
struct BaseCache {
    struct Entry {
        int32_t bx, by, bz;
        bool known;
        uint16_t cells[512];
    };
    void* editor = nullptr;
    std::vector<std::unique_ptr<Entry>> entries;
    // The shipped world's cell; false when not known.
    bool cell(int32_t cx, int32_t cy, int32_t cz, uint16_t* base) {
        if (!base_reader_usable()) return false;
        const int32_t bx = floor_div8(cx), by = floor_div8(cy), bz = floor_div8(cz);
        Entry* e = nullptr;
        for (auto& x : entries)
            if (x->bx == bx && x->by == by && x->bz == bz) e = x.get();
        if (!e) {
            entries.push_back(std::make_unique<Entry>());
            e = entries.back().get();
            e->bx = bx, e->by = by, e->bz = bz;
            if (bx < 0 || bz < 0) {
                e->known = false;
            } else if (by < 0) {  // below the world: all terrain
                e->known = true;
                for (auto& c : e->cells) c = 0xFF01;
            } else {
                bool faulted = false;
                e->known = base_brick_raw(editor, bx, by, bz, e->cells, &faulted);
                if (faulted && !g_base_off.exchange(true))
                    say("ruin protection: the game's column reader faulted; every building block counts as the world's own "
                        "(never broken) until a restart");
            }
        }
        if (!e->known) return false;
        *base = e->cells[(cx - bx * 8) + 8 * (cy - by * 8) + 64 * (cz - bz * 8)];
        return true;
    }
};

// Ruin protection for one sphere: the cells of its box read at once (the region read, x + sx * (y + sy * z)), each
// building cell judged against the shipped world (world_verdict), the radius shrunk off the world's own and the unknown
// ones (shrink_off_world). A box that cannot be read: 0 (the sphere is skipped: never break what cannot be judged).
float shrink_sphere(void* editor, const Sphere& s, const float table[256]) {
    int32_t lo[3];
    uint32_t n[3];
    const double c[3] = {s.x, s.y, s.z};
    const double reach = s.r + kShrinkMargin;  // the box shrink_off_world looks at
    for (int a = 0; a < 3; ++a) {
        lo[a] = static_cast<int32_t>(std::floor((c[a] - reach) * 2.0));
        const auto hi = static_cast<int32_t>(std::floor((c[a] + reach) * 2.0));
        n[a] = static_cast<uint32_t>(hi - lo[a] + 1);
        if (n[a] > 16) return 0.0f;  // larger than the game's own reads (radius <= 3.5: at most 15)
    }
    if (!g_deps.fns.region_read || g_reads_off.load()) {
        if (!g_noread_said.exchange(true))
            say("ruin protection: the game's voxel read is not available: breaks spare every block (EMF_BREAK_WORLD "
                "still breaks)");
        return 0.0f;
    }
    std::vector<uint16_t> cells(static_cast<size_t>(n[0]) * n[1] * n[2], 0);
    const ReadReq q{cells.data(), cells.size(), n[0], n[1], n[2], kReadLevel, lo[0], lo[1], lo[2], 0};
    const ReadSlice sl{&q, 1};
    bool ok = false;
    if (!call_region(g_deps.fns.region_read, &sl, editor, &ok)) {
        if (!g_reads_off.exchange(true)) say("block reads off (the game's voxel region read faulted)");
        return 0.0f;
    }
    if (!ok) {  // part of the box is not loaded
        if (!g_unloaded_said.exchange(true)) say("ruin protection: a break's place is not loaded; that sphere spares everything");
        return 0.0f;
    }
    if (!base_reader_usable() && !g_base_off.load() && !g_nobase_said.exchange(true))
        say("ruin protection: the shipped world cannot be read on this game build: breaks spare every building block "
            "(EMF_BREAK_WORLD still breaks)");
    auto cache = std::make_unique<BaseCache>();
    cache->editor = editor;
    return shrink_off_world(s.x, s.y, s.z, s.r, [&](int32_t i, int32_t j, int32_t k) {
        const int64_t di = i - lo[0], dj = j - lo[1], dk = k - lo[2];
        if (di < 0 || dj < 0 || dk < 0 || di >= n[0] || dj >= n[1] || dk >= n[2]) return -1;  // outside the read box
        const uint16_t cur = cells[static_cast<size_t>(di) + n[0] * (static_cast<size_t>(dj) + n[1] * static_cast<size_t>(dk))];
        if ((cur & 0xFF) < 0x80) return 0;
        if (!(table[cur & 0xFF] > 0)) return 0;  // a material this break never touches (Unbreakable, not asked for)
        uint16_t base = 0;
        const bool known = cache->cell(i, j, k, &base);
        return world_verdict(cur, known, base);
    });
}

// v47 EMF_BREAK_TERRAIN, one sphere: its box read at once, each terrain cell judged against the shipped world
// (terrain_sphere: only a player's), each cell that went cleared with its own box fill of exactly that cell, the call
// the game's building kernel makes to remove a block. Not loaded or not readable: nothing (true). A fault in the box
// fill: false (breaking goes off).
bool terrain_pass(const BlockHandles& h, const Sphere& s, const float table[128], bool instant, KernelCount* k) {
    int32_t lo[3];
    uint32_t n[3];
    const double c[3] = {s.x, s.y, s.z};
    for (int a = 0; a < 3; ++a) {
        lo[a] = static_cast<int32_t>(std::floor((c[a] - s.r) * 2.0));
        const auto hi = static_cast<int32_t>(std::floor((c[a] + s.r) * 2.0));
        n[a] = static_cast<uint32_t>(hi - lo[a] + 1);
        if (n[a] > 16) return true;  // larger than the game's own reads (radius <= 3.5: at most 15)
    }
    if (!g_deps.fns.region_read || g_reads_off.load()) return true;
    std::vector<uint16_t> cells(static_cast<size_t>(n[0]) * n[1] * n[2], 0);
    const ReadReq q{cells.data(), cells.size(), n[0], n[1], n[2], kReadLevel, lo[0], lo[1], lo[2], 0};
    const ReadSlice sl{&q, 1};
    bool ok = false;
    if (!call_region(g_deps.fns.region_read, &sl, h.editor, &ok)) {
        if (!g_reads_off.exchange(true)) say("block reads off (the game's voxel region read faulted)");
        return true;
    }
    if (!ok) return true;  // part of the box is not loaded: every cell spared
    auto cache = std::make_unique<BaseCache>();
    cache->editor = h.editor;
    const uint64_t t = now();
    if (g_terrain_reset.exchange(false)) g_terrain_damage.clear();  // a new world since the last pass
    g_terrain_damage.heal(t);
    const TerrainPassResult res =
        terrain_sphere(s, table, instant, g_terrain_damage, t, [&](int32_t i, int32_t j, int32_t kk) -> TerrainCellRead {
            TerrainCellRead r;
            const int64_t di = i - lo[0], dj = j - lo[1], dk = kk - lo[2];
            if (di < 0 || dj < 0 || dk < 0 || di >= n[0] || dj >= n[1] || dk >= n[2]) return r;  // outside the read box
            r.ok = true;
            r.cur = cells[static_cast<size_t>(di) + n[0] * (static_cast<size_t>(dj) + n[1] * static_cast<size_t>(dk))];
            const uint32_t m = r.cur & 0xFFu;
            if (m != 0 && m < 0x80 && (r.cur >> 8) != 0) r.base_known = cache->cell(i, j, kk, &r.base);  // terrain only
            return r;
        });
    TerrainPassResult got = res;
    for (const TerrainRun& run : res.runs)
        for (int32_t y = run.y0; y <= run.y1; ++y) {
            alignas(16) float box[8] = {run.x / 2.0f, y / 2.0f, run.z / 2.0f, 0.0f,
                                        run.x / 2.0f + 0.5f, y / 2.0f + 0.5f, run.z / 2.0f + 0.5f, 0.0f};
            if (!call_box_fill(g_deps.fns.box_fill_air, h.editor, h.voxel_data, box)) {
                turn_off("off (the game's box fill faulted); breaks are refused until a restart");
                return false;
            }
        }
    // The game calls its box fill on building cells only: read the cells back and count only those it cleared, so a
    // build where it leaves solid terrain never counts a hole (raiders would hit on, drops would pay out every hit).
    if (!res.runs.empty()) {
        std::vector<uint16_t> after(cells.size(), 0);
        const ReadReq q2{after.data(), after.size(), n[0], n[1], n[2], kReadLevel, lo[0], lo[1], lo[2], 0};
        const ReadSlice sl2{&q2, 1};
        bool ok2 = false;
        if (call_region(g_deps.fns.region_read, &sl2, h.editor, &ok2) && ok2) {
            uint32_t kept = 0;
            for (const TerrainRun& run : res.runs)
                for (int32_t y = run.y0; y <= run.y1; ++y) {
                    const size_t i = static_cast<size_t>(run.x - lo[0]) +
                                     n[0] * (static_cast<size_t>(y - lo[1]) + n[1] * static_cast<size_t>(run.z - lo[2]));
                    const uint16_t v = after[i];
                    const uint32_t m = v & 0xFFu;
                    if (m == 0 || (m < 0x80 && (v >> 8) == 0)) continue;  // cleared
                    ++kept;
                    --got.removed;
                    got.removed_per_material[run.material] -= 1.0f;
                }
            if (kept)
                terrain_turn_off("the game's box fill left " + std::to_string(kept) +
                                 " terrain cell(s) in place on this game build; breaking terrain stays off until a restart");
        }
    }
    k->damaged += got.damaged;
    k->removed += got.removed;
    for (int m = 0; m < 128; ++m) k->removed_per_material[m] += got.removed_per_material[m];
    return true;
}

struct Reads {
    void* editor = nullptr;
    void* damage_store = nullptr;
    MaterialFacts facts[128] = {};
    MaterialFacts terrain[128] = {};  // v47: by terrain material (unknown: 0 hardness and health)
};

bool reads_for(EmfWorld* w, Reads& r, std::string* why) {
    auto refuse = [&](const char* text) {
        if (why) *why = text;
        return false;
    };
    if (!w || WorldView(reinterpret_cast<uint8_t*>(w)).kind() != EMF_WORLD_SERVER)
        return refuse("not a server world (a joined game has no blocks to read: ask on the host or the dedicated server)");
    if (!g_deps.fns.region_read || g_reads_off.load()) return refuse("not available on this game version");
    const WorldView view(reinterpret_cast<uint8_t*>(w));
    const auto* vw = static_cast<const uint8_t*>(view.singleton("VoxelWorld"));
    if (!vw || !seh_read(vw + 8, &r.editor, 8) || !r.editor) return refuse("the world has no voxel editor yet");
    blocks_read_facts(view.singleton("TerraformingEfficiencyRegistry"), r.facts);  // unknown facts: 0 health
    if (g_deps.fns.terrain_config_find && !g_terrain_off.load())
        blocks_read_terrain_facts(view.singleton("TerraformingEfficiencyRegistry"), r.terrain);
    r.damage_store = view.singleton("ServerVoxelDamageSystem");
    return true;
}

// A terrain cell's hardness and health are its terrain material's (v47); building cells have theirs from decode_voxel.
void fill_terrain_facts(const Reads& r, EmfBlockInfo& b) {
    if (b.kind != EMF_BLOCK_TERRAIN || b.material >= 128 || !r.terrain[b.material].known) return;
    b.hardness = r.terrain[b.material].hardness;
    b.health = r.terrain[b.material].health;
}

VoxelReader reader_of(const Reads& r) {
    return [&r](int32_t x, int32_t y, int32_t z, uint16_t* v) -> bool {
        *v = 0;
        const ReadReq q{v, 1, 1, 1, 1, kReadLevel, x, y, z, 0};
        const ReadSlice s{&q, 1};
        bool ok = false;
        if (!call_region(g_deps.fns.region_read, &s, r.editor, &ok)) {
            if (!g_reads_off.exchange(true)) say("block reads off (the game's voxel region read faulted)");
            return false;
        }
        return ok;
    };
}

void fill_damage(const Reads& r, EmfBlockInfo& b) {
    b.damage = -1;
    if (b.kind != EMF_BLOCK_BUILDING || !g_deps.fns.damage_get || !r.damage_store) return;
    const uint32_t cell[3] = {static_cast<uint32_t>(std::floor(b.x * 2.0f)), static_cast<uint32_t>(std::floor(b.y * 2.0f)),
                              static_cast<uint32_t>(std::floor(b.z * 2.0f))};
    uint8_t d = 0;
    if (call_damage(g_deps.fns.damage_get, r.damage_store, cell, &d)) b.damage = d;
}

// EmfBlockInfo.world (v45) for the cell whose voxel is v; for a terrain cell (v47) 1 the world's own ground, 0 a
// player placed it, -1 not known.
void fill_world(const Reads& r, EmfBlockInfo& b, uint16_t v) {
    b.world = 0;
    if (b.kind != EMF_BLOCK_BUILDING && b.kind != EMF_BLOCK_TERRAIN) return;
    auto cache = std::make_unique<BaseCache>();
    cache->editor = r.editor;
    uint16_t base = 0;
    const bool known = cache->cell(static_cast<int32_t>(std::floor(b.x * 2.0f)), static_cast<int32_t>(std::floor(b.y * 2.0f)),
                                   static_cast<int32_t>(std::floor(b.z * 2.0f)), &base);
    b.world = b.kind == EMF_BLOCK_BUILDING ? world_verdict(v, known, base) : terrain_verdict(v, known, base);
}

}  // namespace

bool blocks_block_at(EmfWorld* w, float x, float y, float z, EmfBlockInfo* out, std::string* why) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        if (why) *why = "the point is not a number";
        return false;
    }
    auto r = std::make_unique<Reads>();
    if (!reads_for(w, *r, why)) return false;
    const auto cx = static_cast<int32_t>(std::floor(x * 2.0f)), cy = static_cast<int32_t>(std::floor(y * 2.0f)),
               cz = static_cast<int32_t>(std::floor(z * 2.0f));
    uint16_t v = 0;
    if (!reader_of(*r)(cx, cy, cz, &v)) {
        if (why) *why = "that place is not loaded";
        return false;
    }
    EmfBlockInfo b = decode_voxel(v, r->facts);
    b.x = (static_cast<float>(cx) + 0.5f) / 2.0f;
    b.y = (static_cast<float>(cy) + 0.5f) / 2.0f;
    b.z = (static_cast<float>(cz) + 0.5f) / 2.0f;
    fill_terrain_facts(*r, b);
    fill_damage(*r, b);
    fill_world(*r, b, v);
    *out = b;
    return true;
}

bool blocks_origin(EmfWorld* w, float x, float y, float z, BlockOrigin* out, std::string* why) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        if (why) *why = "the point is not a number";
        return false;
    }
    auto r = std::make_unique<Reads>();
    if (!reads_for(w, *r, why)) return false;
    const auto cx = static_cast<int32_t>(std::floor(x * 2.0f)), cy = static_cast<int32_t>(std::floor(y * 2.0f)),
               cz = static_cast<int32_t>(std::floor(z * 2.0f));
    uint16_t v = 0;
    if (!reader_of(*r)(cx, cy, cz, &v)) {
        if (why) *why = "that place is not loaded";
        return false;
    }
    BlockOrigin o;
    o.cur = v;
    o.x = (static_cast<float>(cx) + 0.5f) / 2.0f;
    o.y = (static_cast<float>(cy) + 0.5f) / 2.0f;
    o.z = (static_cast<float>(cz) + 0.5f) / 2.0f;
    auto cache = std::make_unique<BaseCache>();
    cache->editor = r->editor;
    uint16_t base = 0;
    o.base_known = cache->cell(cx, cy, cz, &base);
    o.base = o.base_known ? base : 0;
    o.world = world_verdict(v, o.base_known, base);
    *out = o;
    return true;
}

void blocks_base_reader_off_for_test(bool off) { g_base_test_off = off; }

bool blocks_find_wall(EmfWorld* w, const float from[3], const float to[3], EmfBlockInfo* out, std::string* why,
                      uint32_t flags) {
    if (why) why->clear();
    for (int a = 0; a < 3; ++a)
        if (!std::isfinite(from[a]) || !std::isfinite(to[a])) {
            if (why) *why = "a point is not a number";
            return false;
        }
    if (flags & ~EMF_FIND_TERRAIN) {
        if (why) *why = "unknown flags";
        return false;
    }
    auto r = std::make_unique<Reads>();
    if (!reads_for(w, *r, why)) return false;
    // v47 EMF_FIND_TERRAIN: also a terrain cell a player placed (the shipped world had no ground there); a cell whose
    // shipped state is not known is passed over
    auto cache = std::make_unique<BaseCache>();
    cache->editor = r->editor;
    PlacedTerrain placed;
    if (flags & EMF_FIND_TERRAIN)
        placed = [&](int32_t x, int32_t y, int32_t z, uint16_t v) {
            uint16_t base = 0;
            const bool known = cache->cell(x, y, z, &base);
            return terrain_verdict(v, known, base) == 0;
        };
    const auto hit = first_wall_on_line(from, to, reader_of(*r), r->facts, placed);
    if (!hit) return false;
    EmfBlockInfo b = *hit;
    fill_terrain_facts(*r, b);
    fill_damage(*r, b);
    const auto cx = static_cast<int32_t>(std::floor(b.x * 2.0f)), cy = static_cast<int32_t>(std::floor(b.y * 2.0f)),
               cz = static_cast<int32_t>(std::floor(b.z * 2.0f));
    uint16_t v = 0;
    if (reader_of(*r)(cx, cy, cz, &v)) {
        fill_world(*r, b, v);
    } else {
        b.world = -1;
    }
    *out = b;
    return true;
}

void fill_block_op(uint8_t op[kBlockOpBytes], const BlockHandles& h, bool instant) {
    std::memset(op, 0, kBlockOpBytes);
    auto put = [&](size_t at, const void* p) { std::memcpy(op + at, &p, 8); };
    put(0x000, h.program_context);
    put(0x030, h.voxel_data);
    put(0x0F8, h.efficiency);
    put(0x100, h.blueprint_items);
    put(0x118, h.balancing);
    put(0x120, h.snapping);
    put(0x128, instant ? nullptr : h.damage_store);
    put(0x130, h.editor);
    op[0x148] = 1;  // the snapping layer the game's native passes
}

bool blocks_read_facts(void* efficiency, MaterialFacts out[128]) {
    if (!g_deps.fns.config_find || !efficiency) return false;
    for (uint32_t id = 0; id < 128; ++id) {
        out[id] = MaterialFacts{};
        const uint8_t* cfg = nullptr;
        if (!call_config(g_deps.fns.config_find, efficiency, static_cast<uint8_t>(id), &cfg)) return false;
        if (!cfg) continue;
        MaterialFacts f;
        if (!seh_read(cfg + kCfgHardness, &f.hardness, 1) || !seh_read(cfg + kCfgLootRate, &f.loot_rate, 4) ||
            !seh_read(cfg + kCfgHealth, &f.health, 4) || !seh_read(cfg + kCfgItem, f.item_guid, 16))
            continue;
        if (f.hardness > 5 || f.health < 0) continue;  // not the layout EMF knows: never broken
        f.known = true;
        out[id] = f;
    }
    return true;
}

bool blocks_read_terrain_facts(void* efficiency, MaterialFacts out[128]) {
    for (uint32_t m = 0; m < 128; ++m) out[m] = MaterialFacts{};
    if (!g_deps.fns.terrain_config_find || !efficiency || g_terrain_off.load()) return false;
    for (uint32_t m = 1; m < 128; ++m) {
        const uint8_t* cfg = nullptr;
        if (!call_config(g_deps.fns.terrain_config_find, efficiency, static_cast<uint8_t>(m), &cfg)) {
            terrain_turn_off("the game's terrain config lookup faulted; breaking terrain stays off until a restart");
            for (uint32_t i = 0; i < 128; ++i) out[i] = MaterialFacts{};
            return false;
        }
        if (!cfg) continue;
        MaterialFacts f;
        if (!seh_read(cfg + kCfgHardness, &f.hardness, 1) || !seh_read(cfg + kCfgLootRate, &f.loot_rate, 4) ||
            !seh_read(cfg + kCfgHealth, &f.health, 4) || !seh_read(cfg + kCfgItem, f.item_guid, 16))
            continue;
        if (f.hardness > 5 || f.health < 0) continue;  // not the layout EMF knows: never broken
        f.known = true;
        out[m] = f;
    }
    return true;
}

ProgramSpec blocks_setup(BlockGlueDeps deps) {
    g_on = false;
    g_deps = std::move(deps);
    g_service = std::make_unique<BlockService>();
    g_drops.clear();
    g_terrain_damage.clear();
    g_terrain_off = g_terrain_reset = false;
    {
        std::lock_guard<std::mutex> l(g_state_lock);
        g_terrain_why.clear();
    }
    g_watchdog_said = g_cb_fault_said = g_kernel_said = g_factor_said = false;
    g_reads_off = false;
    g_base_off = false;
    g_noread_said = g_nobase_said = g_unloaded_said = false;
    g_last_run_ms = now();
    const bool usable = g_deps.fns.kernel && g_deps.fns.config_find;
    {
        std::lock_guard<std::mutex> l(g_state_lock);
        g_why = usable ? "" : "not available on this game version";
    }
    g_on = usable;
    ProgramSpec s;
    s.name = kBlocksProgramName;
    s.world_kind = EMF_WORLD_SERVER;
    s.fn = blocks_program;
    for (const char* n : kRead) s.singletons_read.emplace_back(n);
    for (const char* n : kWrite) s.singletons_write.emplace_back(n);
    return s;
}

uint32_t blocks_request(const EmfBreakBlocks& r, EmfBreakCallback cb, void* user, std::string* why) {
    if (!g_on.load() || !g_service) {
        if (why) *why = blocks_unavailable_why();
        return 0;
    }
    if ((r.flags & EMF_BREAK_TERRAIN) && !terrain_usable()) {
        if (why) {
            std::lock_guard<std::mutex> l(g_state_lock);
            *why = g_terrain_off.load() && !g_terrain_why.empty() ? g_terrain_why
                                                                 : "breaking terrain is not available on this game version";
        }
        return 0;
    }
    return g_service->request(r, cb, user, why);
}

std::string blocks_unavailable_why() {
    if (g_on.load()) return "";
    std::lock_guard<std::mutex> l(g_state_lock);
    return g_why.empty() ? "off" : g_why;
}

void blocks_world_reset() {
    g_watchdog_said = false;
    g_last_run_ms = now();
    if (g_service) deliver(g_service->world_reset(), {});
    g_terrain_reset = true;  // another world's cells: forgotten inside the program (its thread owns the store)
}

void blocks_server_tick(uint64_t now_ms) {
    if (!g_service || !g_service->waiting()) return;
    if (now_ms - g_last_run_ms.load() < kBlockWatchdogMs) return;
    if (!g_watchdog_said.exchange(true))
        say(std::string(kBlocksProgramName) + " has not run for 3 s in this world: the waiting breaks are cancelled");
    deliver(g_service->cancel_all(), {});
}

void blocks_stop(const std::string& why) { turn_off(why); }

}  // namespace emf::core
