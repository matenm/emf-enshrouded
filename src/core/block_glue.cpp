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

float shrink_sphere(void* editor, const Sphere& s);  // below, after the reads

void blocks_program(void* ctx, void*) {
    if (!g_on.load() || !g_service) return;
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
    e.facts = [&](MaterialFacts out[128]) { return blocks_read_facts(h.efficiency, out); };
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
    e.shrink = [&](const Sphere& s) -> float { return shrink_sphere(h.editor, s); };
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
std::atomic<bool> g_shrink_said{false};

bool base_reader_usable() {
    return !g_base_test_off.load() && !g_base_off.load() && g_deps.fns.column_acquire && g_deps.fns.brick_seek &&
           g_deps.fns.brick_decode && g_deps.fns.column_release;
}

int32_t floor_div8(int32_t c) { return c >= 0 ? c / 8 : -((-c + 7) / 8); }

// One base brick (512 cells, x + 8y + 64z) through the game's column reader, as its own region read does when a brick
// was never changed; false when unknown (a column not loaded, a decode that failed). *faulted on a fault.
bool base_brick_raw(void* editor, int32_t bx, int32_t by, int32_t bz, uint16_t out[512], bool* faulted) {
    *faulted = false;
    __try {
        alignas(16) uint8_t h[kColumnHandle] = {};
        g_deps.fns.column_acquire(h, editor, kBaseLevel,
                                  (static_cast<uint64_t>(static_cast<uint32_t>(bz)) << 32) | static_cast<uint32_t>(bx));
        void* data = nullptr;
        std::memcpy(&data, h + 0x20, 8);
        bool ok = h[0x90] != 0 && data != nullptr;  // a tile outside the world: available but no data (read as air)
        if (ok) {
            if (!g_deps.fns.brick_seek(h + 0x30, static_cast<uint32_t>(by))) {
                for (int i = 0; i < 512; ++i) out[i] = 0;  // above the column's bricks: air
            } else if (h[0x80]) {                          // one material for the whole brick
                const uint8_t* mp = nullptr;
                const uint8_t* fp = nullptr;
                std::memcpy(&mp, h + 0x68, 8);
                std::memcpy(&fp, h + 0x70, 8);
                const uint8_t m = mp ? *mp : 0;
                const uint16_t f = fp ? *fp : static_cast<uint16_t>(m ? 0xFF : 0);
                for (int i = 0; i < 512; ++i) out[i] = static_cast<uint16_t>(m | (f << 8));
            } else {
                uint32_t end = 0, size = 0;
                const uint8_t* start = nullptr;
                std::memcpy(&end, h + 0x60, 4);
                std::memcpy(&size, h + 0x88, 4);
                std::memcpy(&start, h + 0x50, 8);
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
                ok = res[0] == 0;
            }
        }
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
        return ok;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *faulted = true;
        return false;
    }
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
float shrink_sphere(void* editor, const Sphere& s) {
    int32_t lo[3];
    uint32_t n[3];
    const float c[3] = {s.x, s.y, s.z};
    for (int a = 0; a < 3; ++a) {
        lo[a] = static_cast<int32_t>(std::floor((c[a] - s.r) * 2.0f));
        const auto hi = static_cast<int32_t>(std::floor((c[a] + s.r) * 2.0f));
        n[a] = static_cast<uint32_t>(hi - lo[a] + 1);
        if (n[a] > 16) return 0.0f;  // larger than the game's own reads (radius <= 3.5: at most 15)
    }
    if (!g_deps.fns.region_read || g_reads_off.load()) {
        if (!g_shrink_said.exchange(true))
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
    if (!ok) return 0.0f;  // part of the box is not loaded
    if (!base_reader_usable() && !g_shrink_said.exchange(true))
        say("ruin protection: the shipped world cannot be read on this game build: breaks spare every building block "
            "(EMF_BREAK_WORLD still breaks)");
    auto cache = std::make_unique<BaseCache>();
    cache->editor = editor;
    return shrink_off_world(s.x, s.y, s.z, s.r, [&](int32_t i, int32_t j, int32_t k) {
        const uint16_t cur = cells[static_cast<size_t>(i - lo[0]) +
                                   n[0] * (static_cast<size_t>(j - lo[1]) + n[1] * static_cast<size_t>(k - lo[2]))];
        if ((cur & 0xFF) < 0x80) return 0;
        uint16_t base = 0;
        const bool known = cache->cell(i, j, k, &base);
        return world_verdict(cur, known, base);
    });
}

struct Reads {
    void* editor = nullptr;
    void* damage_store = nullptr;
    MaterialFacts facts[128] = {};
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
    r.damage_store = view.singleton("ServerVoxelDamageSystem");
    return true;
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

// EmfBlockInfo.world (v45) for the cell whose voxel is v.
void fill_world(const Reads& r, EmfBlockInfo& b, uint16_t v) {
    b.world = 0;
    if (b.kind != EMF_BLOCK_BUILDING) return;
    auto cache = std::make_unique<BaseCache>();
    cache->editor = r.editor;
    uint16_t base = 0;
    const bool known = cache->cell(static_cast<int32_t>(std::floor(b.x * 2.0f)), static_cast<int32_t>(std::floor(b.y * 2.0f)),
                                   static_cast<int32_t>(std::floor(b.z * 2.0f)), &base);
    b.world = world_verdict(v, known, base);
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

bool blocks_find_wall(EmfWorld* w, const float from[3], const float to[3], EmfBlockInfo* out, std::string* why) {
    if (why) why->clear();
    for (int a = 0; a < 3; ++a)
        if (!std::isfinite(from[a]) || !std::isfinite(to[a])) {
            if (why) *why = "a point is not a number";
            return false;
        }
    auto r = std::make_unique<Reads>();
    if (!reads_for(w, *r, why)) return false;
    const auto hit = first_building_on_line(from, to, reader_of(*r), r->facts);
    if (!hit) return false;
    EmfBlockInfo b = *hit;
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

ProgramSpec blocks_setup(BlockGlueDeps deps) {
    g_on = false;
    g_deps = std::move(deps);
    g_service = std::make_unique<BlockService>();
    g_drops.clear();
    g_watchdog_said = g_cb_fault_said = g_kernel_said = g_factor_said = false;
    g_reads_off = false;
    g_base_off = false;
    g_shrink_said = false;
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
