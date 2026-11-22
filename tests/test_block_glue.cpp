// Breaking blocks (API v44): the emf_blocks program over fake game calls (the building kernel, the config lookup) and
// a fake world (its singletons, the program's iterator).
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <tuple>
#include <string>
#include <vector>

#include "common/offsets.h"
#include "core/api.h"
#include "core/api_sizes.h"
#include "core/block_glue.h"
#include "core/dispatcher.h"
#include "core/system_programs.h"
#include "emf/emf_api.h"
#include "fake_world.h"

using namespace emf;
using namespace emf::core;
using emf::test::FakeProgramCtx;
using emf::test::FakeWorld;
using emf::test::SingletonStore;

namespace {

template <typename T>
T get(const uint8_t* p, size_t off) {
    T v{};
    std::memcpy(&v, p + off, sizeof v);
    return v;
}

// the six singletons the program declares (objects whose addresses matter, not their contents, except the ones read)
alignas(16) uint8_t g_balancing[0x100], g_efficiency[0x40], g_blueprints[0x10], g_damage[0x10], g_snapping[0x10];
alignas(16) uint8_t g_voxel_world[0x10], g_voxel_data[0x10], g_editor[0x10];
alignas(16) uint8_t g_settings[0x40];  // GameSettings: miningDamageFactor at +0x30
bool g_swap_slots = false;

void f_iter_begin(void* ctx, void* iter, uint32_t) {
    auto* it = static_cast<uint64_t*>(iter);
    it[0] = reinterpret_cast<uint64_t>(ctx);
    // sys-read in name order, then sys-write in name order
    it[1] = reinterpret_cast<uint64_t>(g_balancing);
    it[2] = reinterpret_cast<uint64_t>(g_swap_slots ? g_blueprints : g_efficiency);
    it[3] = reinterpret_cast<uint64_t>(g_swap_slots ? g_efficiency : g_blueprints);
    it[4] = reinterpret_cast<uint64_t>(g_damage);
    it[5] = reinterpret_cast<uint64_t>(g_voxel_world);
    it[6] = reinterpret_cast<uint64_t>(g_snapping);
}
uint64_t f_register(void*, const void*, char) { return 1; }

// configs by building id: stone 7 (VeryHard, 1000, 8 blocks per item), straw 2 (Soft, 100, 4 per item)
alignas(4) uint8_t g_cfg[128][0xB4];
bool g_config_faults = false;
const uint8_t* f_config(void* registry, uint8_t id) {
    if (g_config_faults) return *static_cast<const uint8_t* const volatile*>(nullptr);
    CHECK(registry == g_efficiency);
    if (id != 7 && id != 2) return nullptr;
    return g_cfg[id];
}
void set_cfg(uint8_t id, uint8_t hardness, int32_t health, uint32_t rate, uint8_t guid0) {
    std::memset(g_cfg[id], 0, 0xB4);
    g_cfg[id][0] = id;
    g_cfg[id][4] = guid0;
    g_cfg[id][0x14] = hardness;
    std::memcpy(g_cfg[id] + 0x18, &rate, 4);
    std::memcpy(g_cfg[id] + 0x1C, &health, 4);
}

struct KernelCall {
    uint8_t op[kBlockOpBytes];
    float table[256];
    uint64_t table_count;
    float centre[4];
    bool centre_aligned;
    float radius;
    uint8_t mode;
};
std::vector<KernelCall> g_calls;
bool g_kernel_faults = false;
float g_damaged = 4, g_removed = 2, g_removed_straw = 0;
struct Slice {
    const float* data;
    uint64_t count;
};
struct Stats {
    float* per;
    uint64_t cap;
    uint8_t main, pad[3];
    float damaged, removed;
};
bool f_kernel(void* op, void* undo, void* stats, const void* table, const float* centre, float radius, uint8_t mode) {
    if (g_kernel_faults) *static_cast<volatile int*>(nullptr) = 1;
    CHECK(undo == nullptr);
    KernelCall c{};
    std::memcpy(c.op, op, kBlockOpBytes);
    const auto* t = static_cast<const Slice*>(table);
    std::memcpy(c.table, t->data, sizeof c.table);
    c.table_count = t->count;
    std::memcpy(c.centre, centre, sizeof c.centre);
    c.centre_aligned = (reinterpret_cast<uintptr_t>(centre) & 15) == 0;
    c.radius = radius;
    c.mode = mode;
    g_calls.push_back(c);
    auto* s = static_cast<Stats*>(stats);
    REQUIRE(s != nullptr);
    REQUIRE(s->cap >= 256);
    std::memset(s->per, 0, s->cap * 4);
    s->damaged = s->removed = 0;
    if (mode == 0) {
        s->damaged = g_damaged;
    } else {
        s->damaged = g_damaged;  // mode 1 counts the cells that stay too
        s->removed = g_removed;
        s->per[128 + 2] = g_removed_straw;
    }
    return true;
}

// the voxel region read (1x1x1 requests) over a map of cells, and the damage store's get
std::map<std::tuple<int, int, int>, uint16_t> g_voxels;
void* g_editor_seen = nullptr;
struct Req {
    uint16_t* buf;
    uint64_t cap;
    uint32_t sx, sy, sz, level;
    int32_t x, y, z, pad;
};
static_assert(sizeof(Req) == 0x30, "the game's request stride");
bool f_region(const void* slice, void* editor) {
    struct S {
        const Req* data;
        uint64_t count;
    };
    const auto* sl = static_cast<const S*>(slice);
    CHECK(sl->count == 1);
    const Req& r = sl->data[0];
    CHECK(r.sx >= 1);
    CHECK(r.sx <= 16);
    CHECK(r.sy <= 16);
    CHECK(r.sz <= 16);
    CHECK(r.level == 6);
    g_editor_seen = editor;
    for (uint32_t k = 0; k < r.sz; ++k)  // the game's order: x + sx * (y + sy * z)
        for (uint32_t j = 0; j < r.sy; ++j)
            for (uint32_t i = 0; i < r.sx; ++i) {
                const auto it = g_voxels.find({r.x + static_cast<int>(i), r.y + static_cast<int>(j), r.z + static_cast<int>(k)});
                r.buf[i + r.sx * (j + r.sy * k)] = it == g_voxels.end() ? 0 : it->second;
            }
    return true;
}
uint8_t f_damage_get(void* store, const uint32_t cell[3]) {
    CHECK(store == g_damage);
    return cell[0] == 20 ? 34 : 0;
}

// The shipped world's voxels (the base layer) read through the game's column reader: a handle EMF passes zeroed (0xA0
// bytes), filled by ColumnAcquire as the game's does (research ruin_blocks 4.1); g_base by cell, the loaded columns by
// (bx, bz).
std::map<std::tuple<int, int, int>, uint16_t> g_base;
std::vector<std::pair<int, int>> g_base_columns;
int g_column_released = 0, g_column_acquired = 0;
long g_column_lock = 0;     // the column lock's reader count (the game's editor+0x174)
bool g_seek_faults = false;
int g_uniform = 0;          // 0 a coded brick; 1 uniform stone 0x8A with no fill pointer; 2 uniform with no material
struct FakeBrick {
    int bx, by, bz;
};
FakeBrick g_fake_brick{};
void f_column_acquire(void* h, void* editor, uint32_t level, uint64_t column) {
    CHECK(editor == g_editor);
    CHECK(level == 6);
    auto* p = static_cast<uint8_t*>(h);
    const int bx = static_cast<int>(static_cast<uint32_t>(column)), bz = static_cast<int>(static_cast<uint32_t>(column >> 32));
    p[0x90] = 1;  // available (the game sets it first, clears it for a resource that is not loaded)
    long* lock = &g_column_lock;  // the shared lock, taken for every column with bx, bz >= 0 (mode 1)
    ++g_column_lock;
    std::memcpy(p, &lock, 8);
    p[8] = 1;
    if (std::find(g_base_columns.begin(), g_base_columns.end(), std::make_pair(bx, bz)) == g_base_columns.end()) return;
    ++g_column_acquired;
    void* data = &g_fake_brick;
    std::memcpy(p + 0x20, &data, 8);
    void* loader = &g_column_released;
    std::memcpy(p + 0x10, &loader, 8);
    const uint32_t id = 77;
    std::memcpy(p + 0x18, &id, 4);
    std::memcpy(p + 0x38, &bx, 4);  // the fake's own notes, inside the part BrickSeek gets
    std::memcpy(p + 0x3C, &bz, 4);
}
uint8_t g_uniform_material = 0x8A;
bool f_brick_seek(void* h30, uint32_t by) {
    if (g_seek_faults) *static_cast<volatile int*>(nullptr) = 1;
    auto* p = static_cast<uint8_t*>(h30) - 0x30;
    if (g_uniform) {
        p[0x80] = 1;
        const uint8_t* m = g_uniform == 1 ? &g_uniform_material : nullptr;
        std::memcpy(p + 0x68, &m, 8);
        const uint8_t* none = nullptr;
        std::memcpy(p + 0x70, &none, 8);
        return true;
    }
    int bx = 0, bz = 0;
    std::memcpy(&bx, p + 0x38, 4);
    std::memcpy(&bz, p + 0x3C, 4);
    g_fake_brick = {bx, static_cast<int>(by), bz};
    void* start = &g_fake_brick;
    std::memcpy(p + 0x50, &start, 8);
    const uint32_t size = sizeof g_fake_brick;
    std::memcpy(p + 0x60, &size, 4);  // end offset of the current brick
    std::memcpy(p + 0x88, &size, 4);  // its size
    p[0x80] = 0;                      // not uniform
    return true;
}
void f_brick_decode(uint8_t res[8], void* dst, void* src) {
    struct D {
        uint16_t* p;
        uint64_t n;
    };
    struct S {
        const uint8_t* p;
        uint64_t n;
    };
    auto* d = static_cast<D*>(dst);
    const auto* s = static_cast<const S*>(src);
    REQUIRE(d->n == 512);
    const auto* b = reinterpret_cast<const FakeBrick*>(s->p);
    for (int i = 0; i < 512; ++i) {
        const auto it = g_base.find({b->bx * 8 + (i & 7), b->by * 8 + ((i >> 3) & 7), b->bz * 8 + (i >> 6)});
        d->p[i] = it == g_base.end() ? 0 : it->second;
    }
    res[0] = 0;
}
void f_column_release(void* loader, uint32_t id) {
    CHECK(id == 77);
    ++*static_cast<int*>(loader);
}

// v47 terrain: the terrain config lookup (Stone 67: Hard, 5, 5 cells an item, item 0x43; dirt 5: Soft, 2, 10 cells an
// item, item 0x05: the game's terrain health scale) and the box fill "air" (it clears g_voxels inside the box)
alignas(4) uint8_t g_tcfg[128][0xB8];
bool g_box_fill_faults = false;
const uint8_t* f_terrain_config(void* registry, uint8_t material) {
    CHECK(registry == g_efficiency);
    if (material != 67 && material != 5) return nullptr;
    return g_tcfg[material];
}
void set_tcfg(uint8_t m, uint8_t hardness, int32_t health, uint32_t rate, uint8_t guid0) {
    std::memset(g_tcfg[m], 0, 0xB8);
    g_tcfg[m][0] = m;
    g_tcfg[m][4] = guid0;
    g_tcfg[m][0x14] = hardness;
    std::memcpy(g_tcfg[m] + 0x18, &rate, 4);
    std::memcpy(g_tcfg[m] + 0x1C, &health, 4);
}
struct BoxFill {
    float lo[4], hi[4];
    bool aligned;
};
std::vector<BoxFill> g_fills;
bool g_box_fill_keeps_terrain = false;  // a game build whose box fill leaves solid terrain in place
void f_box_fill(void* editor, void* zero, void* voxel_data, const void* box, const void* zero8, uint8_t grow) {
    if (g_box_fill_faults) *static_cast<volatile int*>(nullptr) = 1;
    CHECK((reinterpret_cast<uintptr_t>(zero8) & 15) == 0);
    CHECK(static_cast<const uint64_t*>(zero8)[1] == 0);
    CHECK(editor == g_editor);
    CHECK(zero == nullptr);
    CHECK(voxel_data == g_voxel_data);
    CHECK(*static_cast<const uint64_t*>(zero8) == 0);
    // Seen live (2026-11-22): grow 1, the kernel's, also clears the terrain within 0.5 m around the box (4 ground cells
    // next to one log block went); with 0 exactly the cell goes (a 1 m sphere: its 33 cells, the 92 around unchanged)
    CHECK(grow == 0);
    BoxFill b{};
    std::memcpy(b.lo, box, 16);
    std::memcpy(b.hi, static_cast<const uint8_t*>(box) + 16, 16);
    b.aligned = (reinterpret_cast<uintptr_t>(box) & 15) == 0;
    g_fills.push_back(b);
    for (auto it = g_voxels.begin(); it != g_voxels.end();) {
        const auto [x, y, z] = it->first;
        const bool in = x / 2.0 >= b.lo[0] && x / 2.0 < b.hi[0] && y / 2.0 >= b.lo[1] && y / 2.0 < b.hi[1] &&
                        z / 2.0 >= b.lo[2] && z / 2.0 < b.hi[2];
        const bool terrain = (it->second & 0xFF) < 0x80;
        it = in && !(g_box_fill_keeps_terrain && terrain) ? g_voxels.erase(it) : std::next(it);
    }
}

std::vector<std::string> g_log;
struct Drop {
    uint8_t guid0;
    uint32_t count;
    float pos[3];
};
std::vector<Drop> g_drops;
uint64_t g_now = 1000;

struct Answer {
    uint32_t id;
    EmfBreakResult r;
};
std::vector<Answer> g_answers;
void on_done(EmfWorld*, uint32_t id, const EmfBreakResult* r, void*) { g_answers.push_back({id, *r}); }

struct Fixture {
    FakeWorld w;
    SingletonStore singles;
    SystemPrograms programs;
    std::vector<std::function<void(EmfWorld*)>> after;
    FakeProgramCtx ctx;
    explicit Fixture(bool with_kernel = true, bool with_region = true, bool with_terrain = true) : ctx(w.p()) {
        programs.reset();
        g_calls.clear();
        g_log.clear();
        g_drops.clear();
        g_answers.clear();
        g_kernel_faults = false;
        g_config_faults = false;
        g_swap_slots = false;
        g_damaged = 4;
        g_removed = 2;
        g_removed_straw = 0;
        g_now = 1000;
        g_voxels.clear();
        g_editor_seen = nullptr;
        set_cfg(7, 4, 1000, 8, 0x77);
        set_cfg(2, 0, 100, 4, 0x22);
        void* gs = g_settings;
        std::memcpy(g_balancing + 0x90, &gs, 8);
        const float factor = 1.0f;
        std::memcpy(g_settings + 0x30, &factor, 4);
        void* vd = g_voxel_data;
        void* ed = g_editor;
        std::memcpy(g_voxel_world, &vd, 8);
        std::memcpy(g_voxel_world + 8, &ed, 8);
        singles.plant(w, "BalancingRegistry", g_balancing);
        singles.plant(w, "TerraformingEfficiencyRegistry", g_efficiency);
        singles.plant(w, "VoxelBlueprintItemRegistry", g_blueprints);
        singles.plant(w, "ServerVoxelDamageSystem", g_damage);
        singles.plant(w, "VoxelWorld", g_voxel_world);
        singles.plant(w, "ecs::ServerSnappingSystem", g_snapping);
        w.put<uint8_t>(off::kKind, EMF_WORLD_SERVER);
        BlockGlueDeps d;
        d.fns.kernel = with_kernel ? f_kernel : nullptr;
        d.fns.config_find = f_config;
        d.fns.region_read = with_region ? f_region : nullptr;
        d.fns.damage_get = f_damage_get;
        d.fns.column_acquire = f_column_acquire;
        d.fns.brick_seek = f_brick_seek;
        d.fns.brick_decode = f_brick_decode;
        d.fns.column_release = f_column_release;
        d.fns.terrain_config_find = with_terrain ? f_terrain_config : nullptr;
        d.fns.box_fill_air = with_terrain ? f_box_fill : nullptr;
        set_tcfg(67, 3, 5, 5, 0x43);
        set_tcfg(5, 0, 2, 10, 0x05);
        g_fills.clear();
        g_box_fill_faults = false;
        g_box_fill_keeps_terrain = false;
        g_base.clear();
        g_base_columns.clear();
        g_column_released = g_column_acquired = 0;
        g_column_lock = 0;
        g_seek_faults = false;
        g_uniform = 0;
        d.log = [](const std::string& s) { g_log.push_back(s); };
        d.after_step = [this](std::function<void(EmfWorld*)> fn) { after.push_back(std::move(fn)); };
        d.now_ms = [] { return g_now; };
        d.drop = [](EmfWorld*, const uint8_t guid[16], uint32_t count, const float pos[3]) {
            g_drops.push_back({guid[0], count, {pos[0], pos[1], pos[2]}});
            return count;
        };
        const ProgramSpec spec = blocks_setup(d);
        if (with_kernel) {
            REQUIRE(programs.add(spec, /*internal=*/true));
            REQUIRE(programs.register_into(w.p(), f_register) == 1);
        }
        set_program_iter_begin(f_iter_begin);
    }
    ~Fixture() {
        set_program_iter_begin(nullptr);
        programs.reset();
        blocks_stop();
    }
    EmfWorld* world() { return reinterpret_cast<EmfWorld*>(w.p()); }
    uint32_t ask(uint32_t flags = 0, float height = 0, std::string* why = nullptr) {
        EmfBreakBlocks r{};
        r.size = sizeof r;
        r.flags = flags;
        r.x = 10;
        r.y = 20;
        r.z = 30;
        r.radius = 1.5f;
        r.height = height;
        std::string w2;
        return blocks_request(r, on_done, nullptr, why ? why : &w2);
    }
    void step() {
        system_program_desc(0).update(ctx.p());
        auto jobs = std::move(after);
        after.clear();
        for (auto& j : jobs) j(world());
    }
};

}  // namespace

TEST_CASE("block glue: fill_block_op writes exactly the fields the kernel reads") {
    BlockHandles h;
    h.program_context = reinterpret_cast<void*>(0x1000);
    h.voxel_data = reinterpret_cast<void*>(0x2000);
    h.editor = reinterpret_cast<void*>(0x3000);
    h.damage_store = reinterpret_cast<void*>(0x4000);
    h.snapping = reinterpret_cast<void*>(0x5000);
    h.efficiency = reinterpret_cast<void*>(0x6000);
    h.blueprint_items = reinterpret_cast<void*>(0x7000);
    h.balancing = reinterpret_cast<void*>(0x8000);
    alignas(16) uint8_t op[kBlockOpBytes];
    std::memset(op, 0xCC, sizeof op);
    fill_block_op(op, h, false);
    CHECK(get<uint64_t>(op, 0x000) == 0x1000);
    CHECK(get<uint64_t>(op, 0x030) == 0x2000);
    CHECK(get<uint64_t>(op, 0x038) == 0);
    CHECK(get<uint64_t>(op, 0x0E8) == 0);
    CHECK(get<uint64_t>(op, 0x0F0) == 0);
    CHECK(get<uint64_t>(op, 0x0F8) == 0x6000);
    CHECK(get<uint64_t>(op, 0x100) == 0x7000);
    CHECK(get<uint64_t>(op, 0x118) == 0x8000);
    CHECK(get<uint64_t>(op, 0x120) == 0x5000);
    CHECK(get<uint64_t>(op, 0x128) == 0x4000);
    CHECK(get<uint64_t>(op, 0x130) == 0x3000);
    CHECK(op[0x148] == 1);
    CHECK(op[0x151] == 0);
    size_t nonzero = 0;
    for (size_t i = 0; i < kBlockOpBytes; ++i) nonzero += op[i] != 0;
    CHECK(nonzero <= 8 * 9 + 1);  // the nine pointers and the layer byte, nothing else
    fill_block_op(op, h, true);
    CHECK(get<uint64_t>(op, 0x128) == 0);  // instant: no damage store
}

TEST_CASE("block glue: a damage request calls the kernel in mode 0 then 1; the answer comes after the step") {
    Fixture f;
    const uint32_t id = f.ask();
    REQUIRE(id != 0);
    CHECK(g_calls.empty());
    f.step();
    REQUIRE(g_calls.size() == 2);
    CHECK(g_calls[0].mode == 0);
    CHECK(g_calls[1].mode == 1);
    CHECK(g_calls[0].centre[0] == 10);
    CHECK(g_calls[0].centre[1] == 20);
    CHECK(g_calls[0].centre[2] == 30);
    CHECK(g_calls[0].centre[3] == 0);
    CHECK(g_calls[0].centre_aligned);
    CHECK(g_calls[0].radius == 1.5f);
    CHECK(g_calls[0].table_count == 256);
    CHECK(get<uint64_t>(g_calls[0].op, 0x128) == reinterpret_cast<uint64_t>(g_damage));
    CHECK(get<uint64_t>(g_calls[0].op, 0x130) == reinterpret_cast<uint64_t>(g_editor));
    CHECK(get<uint64_t>(g_calls[0].op, 0x030) == reinterpret_cast<uint64_t>(g_voxel_data));
    CHECK(get<uint64_t>(g_calls[0].op, 0x000) == reinterpret_cast<uint64_t>(f.ctx.p()));
    CHECK(g_calls[0].table[128 + 7] == doctest::Approx(1000.0 / 50));  // the game's config through the lookup
    CHECK(g_calls[0].table[128 + 2] == doctest::Approx(100.0 / 100));
    REQUIRE(g_answers.size() == 1);
    CHECK(g_answers[0].id == id);
    CHECK(g_answers[0].r.status == EMF_BREAK_DONE);
    CHECK(g_answers[0].r.damaged == 4);  // mode 0's count
    CHECK(g_answers[0].r.removed == 2);  // mode 1's
}

TEST_CASE("block glue: instant: one kernel call in mode 1 without the damage store") {
    Fixture f;
    f.ask(EMF_BREAK_INSTANT);
    f.step();
    REQUIRE(g_calls.size() == 1);
    CHECK(g_calls[0].mode == 1);
    CHECK(get<uint64_t>(g_calls[0].op, 0x128) == 0);
    CHECK(g_calls[0].table[128 + 7] == 1.0f);
    REQUIRE(g_answers.size() == 1);
    CHECK(g_answers[0].r.removed == 2);
}

TEST_CASE("block glue: the world's mining damage setting scales damage mode") {
    Fixture f;
    const float factor = 2.0f;
    std::memcpy(g_settings + 0x30, &factor, 4);
    f.ask();
    f.step();
    REQUIRE_FALSE(g_calls.empty());
    CHECK(g_calls[0].table[128 + 7] == doctest::Approx(1000.0 / 100));
}

TEST_CASE("block glue: the kernel never sees a non-zero entry below 128") {
    Fixture f;
    f.ask();
    f.ask(EMF_BREAK_INSTANT);
    f.step();
    REQUIRE(g_calls.size() == 3);
    for (const auto& c : g_calls)
        for (int m = 0; m < 128; ++m) CHECK(c.table[m] == 0.0f);
}

TEST_CASE("block glue: drops are made after the step, at the column's middle") {
    Fixture f;
    g_removed_straw = 9;  // 4 straw blocks per item: 2 items, 1 kept for later
    f.ask(EMF_BREAK_DROPS, 2.0f);  // radius 1.5, height 2: 3 spheres -> 27 straw blocks
    f.step();
    REQUIRE(g_drops.size() == 1);
    CHECK(g_drops[0].guid0 == 0x22);
    CHECK(g_drops[0].count == 6);
    CHECK(g_drops[0].pos[1] == doctest::Approx(21.0));
    REQUIRE(g_answers.size() == 1);
    CHECK(g_answers[0].r.dropped == 6);
}

TEST_CASE("block glue: a singleton slot that does not match the world's refuses the step (logged), nothing called") {
    Fixture f;
    g_swap_slots = true;
    f.ask();
    f.step();
    CHECK(g_calls.empty());
    REQUIRE(g_answers.size() == 1);
    CHECK(g_answers[0].r.status == EMF_BREAK_FAILED);
    REQUIRE_FALSE(g_log.empty());
    CHECK(g_log.back().find("does not match") != std::string::npos);
}

TEST_CASE("block glue: a fault in the kernel turns breaking off; the request answers FAILED, later ones are refused") {
    Fixture f;
    g_kernel_faults = true;
    f.ask();
    f.step();
    REQUIRE(g_answers.size() == 1);
    CHECK(g_answers[0].r.status == EMF_BREAK_FAILED);
    std::string why;
    CHECK(f.ask(0, 0, &why) == 0);
    CHECK(why.find("off") != std::string::npos);
    CHECK_FALSE(blocks_unavailable_why().empty());
}

TEST_CASE("block glue: no BlockKernel signature: requests refused with 'not available on this game version'") {
    Fixture f(/*with_kernel=*/false);
    std::string why;
    CHECK(f.ask(0, 0, &why) == 0);
    CHECK(why == "not available on this game version");
    CHECK(blocks_unavailable_why() == "not available on this game version");
}

TEST_CASE("block glue: the watchdog cancels requests when emf_blocks has not run for 3 s") {
    Fixture f;
    f.ask();
    blocks_server_tick(g_now + 2000);
    CHECK(f.after.empty());
    blocks_server_tick(g_now + 3500);
    REQUIRE(f.after.size() == 1);
    f.after[0](f.world());
    REQUIRE(g_answers.size() == 1);
    CHECK(g_answers[0].r.status == EMF_BREAK_CANCELLED);
}

TEST_CASE("block glue: a new world answers waiting requests CANCELLED") {
    Fixture f;
    f.ask();
    blocks_world_reset();
    REQUIRE(f.after.size() == 1);
    f.after[0](f.world());
    REQUIRE(g_answers.size() == 1);
    CHECK(g_answers[0].r.status == EMF_BREAK_CANCELLED);
}

namespace {
// Runs fn as a server world callback (block_at and find_wall are refused anywhere else).
struct InCallback {
    std::function<void(EmfWorld*)> fn;
};
void run_in_callback(EmfWorld* w, uint8_t kind, std::function<void(EmfWorld*)> fn) {
    Dispatcher d;
    InCallback c{std::move(fn)};
    d.post(kind, [](EmfWorld* world, void* u) { static_cast<InCallback*>(u)->fn(world); }, &c);
    d.run(w, kind, [](const std::string&) {});
}
}  // namespace

TEST_CASE("api block_at (v44): a server world callback only; the voxel decoded with its material facts and damage") {
    Fixture f;
    g_voxels[{20, 40, 60}] = 0xFF87;  // stone at 10..10.5, 20..20.5, 30..30.5 m
    const EmfApi* api = api_table();
    EmfBlockInfo out{};
    out.size = sizeof out;
    CHECK_FALSE(api->block_at(f.world(), 10.2f, 20.2f, 30.2f, &out));  // not inside a world callback
    bool ok = false;
    run_in_callback(f.world(), EMF_WORLD_SERVER, [&](EmfWorld* w) { ok = api_table()->block_at(w, 10.2f, 20.2f, 30.2f, &out); });
    REQUIRE(ok);
    CHECK(out.kind == EMF_BLOCK_BUILDING);
    CHECK(out.material == 7);
    CHECK(out.fill == 255);
    CHECK(out.hardness == 4);
    CHECK(out.health == 1000);
    CHECK(out.damage == 34);
    CHECK(out.x == doctest::Approx(10.25));
    CHECK(out.y == doctest::Approx(20.25));
    CHECK(out.z == doctest::Approx(30.25));
    CHECK(g_editor_seen == g_editor);
    run_in_callback(f.world(), EMF_WORLD_SERVER, [&](EmfWorld* w) { ok = api_table()->block_at(w, 1.2f, 2.2f, 3.2f, &out); });
    REQUIRE(ok);
    CHECK(out.kind == EMF_BLOCK_EMPTY);
}

TEST_CASE("api find_wall (v44): the first building block on the line; refused off the server world") {
    Fixture f;
    g_voxels[{20, 40, 60}] = 0xFF87;
    const float from[3] = {8.1f, 20.2f, 30.2f}, to[3] = {12.1f, 20.2f, 30.2f};
    EmfBlockInfo out{};
    out.size = sizeof out;
    bool ok = false;
    run_in_callback(f.world(), EMF_WORLD_SERVER, [&](EmfWorld* w) { ok = api_table()->find_wall(w, from, to, &out); });
    REQUIRE(ok);
    CHECK(out.material == 7);
    CHECK(out.x == doctest::Approx(10.25));
    CHECK(out.damage == 34);
    const float clear[3] = {8.1f, 25.2f, 30.2f}, clear_to[3] = {12.1f, 25.2f, 30.2f};
    run_in_callback(f.world(), EMF_WORLD_SERVER, [&](EmfWorld* w) { ok = api_table()->find_wall(w, clear, clear_to, &out); });
    CHECK_FALSE(ok);
    f.w.put<uint8_t>(off::kKind, EMF_WORLD_CLIENT);
    run_in_callback(f.world(), EMF_WORLD_CLIENT, [&](EmfWorld* w) { ok = api_table()->find_wall(w, from, to, &out); });
    CHECK_FALSE(ok);
}

TEST_CASE("api find_wall (v44): without the region read signature nothing is read") {
    Fixture f(/*with_kernel=*/true, /*with_region=*/false);
    g_voxels[{20, 40, 60}] = 0xFF87;
    const float from[3] = {8.1f, 20.2f, 30.2f}, to[3] = {12.1f, 20.2f, 30.2f};
    EmfBlockInfo out{};
    out.size = sizeof out;
    bool ok = true;
    run_in_callback(f.world(), EMF_WORLD_SERVER, [&](EmfWorld* w) { ok = api_table()->find_wall(w, from, to, &out); });
    CHECK_FALSE(ok);
}

TEST_CASE("api break_blocks (v44): queued on a server world; refused on a client world with the reason written") {
    Fixture f;
    const EmfApi* api = api_table();
    EmfBreakBlocks r{};
    r.size = sizeof r;
    r.x = 1;
    r.y = 2;
    r.z = 3;
    CHECK(api->break_blocks(f.world(), &r, on_done, nullptr) != 0);
    CHECK(r.why[0] == 0);
    r.radius = 9;
    CHECK(api->break_blocks(f.world(), &r, on_done, nullptr) == 0);
    CHECK(std::string(r.why).find("radius") != std::string::npos);
    r.radius = 1;
    f.w.put<uint8_t>(off::kKind, EMF_WORLD_CLIENT);
    CHECK(api->break_blocks(f.world(), &r, on_done, nullptr) == 0);
    CHECK(std::string(r.why).find("server") != std::string::npos);
    CHECK(api->break_blocks(f.world(), nullptr, on_done, nullptr) == 0);
}

TEST_CASE("api table (v44, v45): break_blocks, block_at and find_wall appended; v45 is the version only (EmfBlockInfo.world, EMF_BREAK_WORLD)") {
    const EmfApi* api = api_table();
    CHECK(api->version >= 45u);  // the newest version is checked by its own test
    CHECK(EMF_API_VERSION >= 45);
    CHECK(api->size >= kApiV44Size);
    CHECK(kApiV45Size == kApiV44Size);
    CHECK(offsetof(EmfApi, break_blocks) == kApiV43Size);
    CHECK(api->break_blocks != nullptr);
    CHECK(api->block_at != nullptr);
    CHECK(api->find_wall != nullptr);
    CHECK(kBlockInfoV44Size == 40);
    CHECK(kBlockInfoV45Size == sizeof(EmfBlockInfo));
    CHECK(offsetof(EmfBlockInfo, world) == 40);
    CHECK(kBreakResultV44Size == sizeof(EmfBreakResult));
}

TEST_CASE("blocks origin: a building cell equal to the shipped world's is the world's own; one on air or of another material is a player's; an unloaded column is unknown; every column read is released") {
    Fixture f;
    g_base_columns = {{2, 7}};  // cell (20, 40, 60): brick (2, 5, 7), column bx 2, bz 7
    g_voxels[{20, 40, 60}] = 0xFF87;
    g_base[{20, 40, 60}] = 0x7F87;  // stone as shipped (another paint byte)
    BlockOrigin o{};
    std::string why;
    bool ok = false;
    run_in_callback(f.world(), EMF_WORLD_SERVER, [&](EmfWorld* w) { ok = blocks_origin(w, 10.2f, 20.2f, 30.2f, &o, &why); });
    REQUIRE_MESSAGE(ok, why);
    CHECK(o.cur == 0xFF87);
    CHECK(o.base_known);
    CHECK(o.base == 0x7F87);
    CHECK(o.world == 1);
    g_base[{20, 40, 60}] = 0;  // built on air
    run_in_callback(f.world(), EMF_WORLD_SERVER, [&](EmfWorld* w) { ok = blocks_origin(w, 10.2f, 20.2f, 30.2f, &o, &why); });
    REQUIRE(ok);
    CHECK(o.world == 0);
    g_voxels[{100, 40, 60}] = 0xFF87;  // a column that is not loaded: unknown
    run_in_callback(f.world(), EMF_WORLD_SERVER, [&](EmfWorld* w) { ok = blocks_origin(w, 50.2f, 20.2f, 30.2f, &o, &why); });
    REQUIRE(ok);
    CHECK_FALSE(o.base_known);
    CHECK(o.world == -1);
    CHECK(g_column_acquired == 2);
    CHECK(g_column_released == 2);
    // below the world (y < 0): terrain there, so no building cell is the world's own
    g_voxels[{20, -2, 60}] = 0xFF87;
    run_in_callback(f.world(), EMF_WORLD_SERVER, [&](EmfWorld* w) { ok = blocks_origin(w, 10.2f, -0.8f, 30.2f, &o, &why); });
    REQUIRE(ok);
    CHECK(o.base_known);
    CHECK(o.world == 0);
    // off the server world: refused
    f.w.put<uint8_t>(off::kKind, EMF_WORLD_CLIENT);
    run_in_callback(f.world(), EMF_WORLD_CLIENT, [&](EmfWorld* w) { ok = blocks_origin(w, 10.2f, 20.2f, 30.2f, &o, &why); });
    CHECK_FALSE(ok);
}

TEST_CASE("blocks origin: without the column reader signatures every building cell is unknown (protected)") {
    Fixture f;
    g_voxels[{20, 40, 60}] = 0xFF87;
    blocks_base_reader_off_for_test(true);
    BlockOrigin o{};
    std::string why;
    bool ok = false;
    run_in_callback(f.world(), EMF_WORLD_SERVER, [&](EmfWorld* w) { ok = blocks_origin(w, 10.2f, 20.2f, 30.2f, &o, &why); });
    blocks_base_reader_off_for_test(false);
    REQUIRE(ok);
    CHECK_FALSE(o.base_known);
    CHECK(o.world == -1);
}

TEST_CASE("api block_at and find_wall (v45): world is 1 for the world's own block, 0 for a player's, -1 when the base is not known; a v44 caller's struct is not written past its 40 bytes") {
    Fixture f;
    g_base_columns = {{2, 7}};
    g_voxels[{20, 40, 60}] = 0xFF87;
    g_base[{20, 40, 60}] = 0xFF87;  // the shipped world's stone
    EmfBlockInfo out{};
    out.size = sizeof out;
    bool ok = false;
    run_in_callback(f.world(), EMF_WORLD_SERVER, [&](EmfWorld* w) { ok = api_table()->block_at(w, 10.2f, 20.2f, 30.2f, &out); });
    REQUIRE(ok);
    CHECK(out.world == 1);
    g_base[{20, 40, 60}] = 0;  // a player built it on air
    const float from[3] = {8.1f, 20.2f, 30.2f}, to[3] = {12.1f, 20.2f, 30.2f};
    run_in_callback(f.world(), EMF_WORLD_SERVER, [&](EmfWorld* w) { ok = api_table()->find_wall(w, from, to, &out); });
    REQUIRE(ok);
    CHECK(out.world == 0);
    g_base_columns.clear();  // not loaded
    run_in_callback(f.world(), EMF_WORLD_SERVER, [&](EmfWorld* w) { ok = api_table()->block_at(w, 10.2f, 20.2f, 30.2f, &out); });
    REQUIRE(ok);
    CHECK(out.world == -1);
    run_in_callback(f.world(), EMF_WORLD_SERVER, [&](EmfWorld* w) { ok = api_table()->block_at(w, 1.2f, 2.2f, 3.2f, &out); });
    REQUIRE(ok);
    CHECK(out.kind == EMF_BLOCK_EMPTY);
    CHECK(out.world == 0);  // no building block
    // a v44 mod's 40-byte struct: the 4 bytes after it stay as they were
    struct {
        EmfBlockInfo info;
        uint32_t guard;
    } v44{};
    v44.info.size = 40;
    std::memset(reinterpret_cast<uint8_t*>(&v44.info) + 40, 0xAB, sizeof v44 - 40);
    g_base_columns = {{2, 7}};
    g_base[{20, 40, 60}] = 0xFF87;
    run_in_callback(f.world(), EMF_WORLD_SERVER, [&](EmfWorld* w) { ok = api_table()->block_at(w, 10.2f, 20.2f, 30.2f, &v44.info); });
    REQUIRE(ok);
    CHECK(v44.info.material == 7);
    CHECK(static_cast<uint32_t>(v44.info.world) == 0xABABABABu);
}

TEST_CASE("block glue: a break stops just short of the world's own blocks (ruin protection); EMF_BREAK_WORLD breaks them too; a block of unknown origin counts as the world's") {
    Fixture f;
    // the break at (10, 20, 30), 1.5 m; a ruin cell 1 m away: (22, 40, 60) -> (11.25, 20.25, 30.25)
    g_base_columns = {{2, 7}};
    g_voxels[{22, 40, 60}] = 0xFF87;
    g_base[{22, 40, 60}] = 0xF787;  // shipped building 7 (weathered paint): a material the break would take
    g_voxels[{18, 40, 60}] = 0xFF87;  // a player's block on the other side, 0.83 m away (shipped air): no matter
    f.ask();
    f.step();
    REQUIRE(g_calls.size() == 2);  // damage mode: pass 0 and pass 1
    const double d = std::sqrt(1.25 * 1.25 + 0.25 * 0.25 + 0.25 * 0.25);  // centre to the ruin cell's centre
    CHECK(g_calls[0].radius == doctest::Approx(d - 0.01).epsilon(0.001));
    CHECK(g_calls[0].radius < d);
    // with EMF_BREAK_WORLD the radius stays
    g_calls.clear();
    f.ask(EMF_BREAK_WORLD);
    f.step();
    REQUIRE(g_calls.size() == 2);
    CHECK(g_calls[0].radius == doctest::Approx(1.5f));
    // the column not loaded: no cell's origin is known, so the player's block (0.83 m away) is spared too
    g_calls.clear();
    g_base_columns.clear();
    f.ask();
    f.step();
    REQUIRE(g_calls.size() == 2);
    const double dp = std::sqrt(0.75 * 0.75 + 0.25 * 0.25 + 0.25 * 0.25);
    CHECK(g_calls[0].radius == doctest::Approx(dp - 0.01).epsilon(0.001));
    // a ruin cell next to the centre (the centre is a cell corner: 0.43 m to the nearest cell centre)
    g_calls.clear();
    g_voxels[{20, 40, 60}] = 0xFF87;
    g_base_columns = {{2, 7}};
    g_base[{20, 40, 60}] = 0xFF87;
    f.ask();
    f.step();
    REQUIRE(g_calls.size() == 2);
    CHECK(g_calls[0].radius == doctest::Approx(std::sqrt(3 * 0.25 * 0.25) - 0.01).epsilon(0.001));
    REQUIRE(!g_answers.empty());
    CHECK(g_answers.back().r.status == EMF_BREAK_DONE);
}

TEST_CASE("blocks origin: every column read gives the game's lock back; a fault while reading still releases the column and its lock, turns the reader off (said once), and later cells are unknown") {
    Fixture f;
    g_base_columns = {{2, 7}};
    g_voxels[{20, 40, 60}] = 0xFF8A;
    g_base[{20, 40, 60}] = 0xFF8A;
    BlockOrigin o{};
    std::string why;
    bool ok = false;
    run_in_callback(f.world(), EMF_WORLD_SERVER, [&](EmfWorld* w) { ok = blocks_origin(w, 10.2f, 20.2f, 30.2f, &o, &why); });
    REQUIRE(ok);
    CHECK(o.world == 1);
    CHECK(g_column_lock == 0);  // taken and given back
    // a uniform brick of stone (no fill pointer: 0xFF), and one whose material pointer is missing (unknown)
    g_uniform = 1;
    run_in_callback(f.world(), EMF_WORLD_SERVER, [&](EmfWorld* w) { ok = blocks_origin(w, 10.2f, 20.2f, 30.2f, &o, &why); });
    REQUIRE(ok);
    CHECK(o.base == 0xFF8A);
    CHECK(o.world == 1);
    g_uniform = 2;
    run_in_callback(f.world(), EMF_WORLD_SERVER, [&](EmfWorld* w) { ok = blocks_origin(w, 10.2f, 20.2f, 30.2f, &o, &why); });
    REQUIRE(ok);
    CHECK_FALSE(o.base_known);
    CHECK(o.world == -1);
    CHECK(g_column_lock == 0);
    // a fault inside the read: the column and the lock are given back all the same
    g_uniform = 0;
    g_seek_faults = true;
    const int released = g_column_released;
    run_in_callback(f.world(), EMF_WORLD_SERVER, [&](EmfWorld* w) { ok = blocks_origin(w, 10.2f, 20.2f, 30.2f, &o, &why); });
    REQUIRE(ok);
    CHECK(o.world == -1);
    CHECK(g_column_released == released + 1);
    CHECK(g_column_lock == 0);
    size_t said = 0;
    for (const auto& l : g_log) said += l.find("column reader faulted") != std::string::npos;
    CHECK(said == 1);
    g_seek_faults = false;  // off until a restart: unknown without calling the game
    const int acquired = g_column_acquired;
    run_in_callback(f.world(), EMF_WORLD_SERVER, [&](EmfWorld* w) { ok = blocks_origin(w, 10.2f, 20.2f, 30.2f, &o, &why); });
    REQUIRE(ok);
    CHECK(o.world == -1);
    CHECK(g_column_acquired == acquired);
}

TEST_CASE("block glue: a world cell whose material the break would not touch (not in its materials, or Unbreakable) does not shrink the sphere") {
    Fixture f;
    g_base_columns = {{2, 7}};
    g_voxels[{22, 40, 60}] = 0xFF8A;  // ruin stone (building 10): not in the request's list
    g_base[{22, 40, 60}] = 0xFF8A;
    EmfBreakBlocks r{};
    r.size = sizeof r;
    r.x = 10, r.y = 20, r.z = 30;
    r.radius = 1.5f;
    const uint8_t only[1] = {7};
    r.material_count = 1;
    r.materials = only;
    std::string why;
    REQUIRE(blocks_request(r, on_done, nullptr, &why) != 0);
    f.step();
    REQUIRE(g_calls.size() == 2);
    CHECK(g_calls[0].radius == doctest::Approx(1.5f));
}

TEST_CASE("block glue: after a kernel fault every other waiting request answers FAILED at the next step, not CANCELLED by the watchdog") {
    Fixture f;
    for (int i = 0; i < 10; ++i) REQUIRE(f.ask() != 0);  // more than one step's spheres
    g_kernel_faults = true;
    f.step();
    REQUIRE_FALSE(g_answers.empty());
    CHECK(g_answers.size() < 10);
    f.step();  // breaking is off: what still waited ends FAILED now
    REQUIRE(g_answers.size() == 10);
    for (const auto& a : g_answers) CHECK(a.r.status == EMF_BREAK_FAILED);
    blocks_server_tick(g_now + 5000);
    CHECK(f.after.empty());  // nothing left for the watchdog
}

TEST_CASE("block glue: a fault in the material config lookup turns breaking off (logged); later requests are refused") {
    Fixture f;
    REQUIRE(f.ask() != 0);
    g_config_faults = true;
    f.step();
    REQUIRE(g_answers.size() == 1);
    CHECK(g_answers[0].r.status == EMF_BREAK_FAILED);
    std::string why;
    CHECK(f.ask(0, 0, &why) == 0);
    CHECK(why.find("off") != std::string::npos);
    CHECK(blocks_unavailable_why().find("config") != std::string::npos);
}

// ---- API v47: terrain a player placed ----

namespace {
// A Stone wall a player built (cells x 19..20, y 39..40, z 59..60: 9.5..10.5, 19.5..20.5, 29.5..30.5 m) on natural dirt
// (y 38) next to a natural Stone hill (x 21): the shipped world has the dirt and the hill, nothing where the wall is.
void terrain_scene() {
    g_base_columns = {{2, 7}};
    for (int i = 17; i <= 23; ++i)
        for (int k = 57; k <= 63; ++k) g_voxels[{i, 38, k}] = g_base[{i, 38, k}] = 0xFF05;
    for (int j = 39; j <= 40; ++j)
        for (int k = 59; k <= 60; ++k) {
            g_voxels[{19, j, k}] = g_voxels[{20, j, k}] = 0xFF43;
            g_voxels[{21, j, k}] = g_base[{21, j, k}] = 0xFF43;
        }
}
int stone_wall_cells() {
    int n = 0;
    for (int i = 19; i <= 20; ++i)
        for (int j = 39; j <= 40; ++j)
            for (int k = 59; k <= 60; ++k) n += g_voxels.count({i, j, k}) ? 1 : 0;
    return n;
}
}  // namespace

TEST_CASE("block glue: EMF_BREAK_TERRAIN clears a player's Stone wall one cell a box fill (as the game's kernel), never the natural dirt or hill") {
    Fixture f;
    terrain_scene();
    REQUIRE(f.ask(EMF_BREAK_TERRAIN | EMF_BREAK_INSTANT) != 0);
    f.step();
    REQUIRE(g_fills.size() == 8);
    for (const BoxFill& b : g_fills) {
        CHECK(b.aligned);
        CHECK(b.hi[0] - b.lo[0] == doctest::Approx(0.5f));  // exactly one 0.5 m cell, as the kernel's own call
        CHECK(b.hi[1] - b.lo[1] == doctest::Approx(0.5f));
        CHECK(b.hi[2] - b.lo[2] == doctest::Approx(0.5f));
        CHECK((b.lo[1] == doctest::Approx(19.5f) || b.lo[1] == doctest::Approx(20.0f)));
        CHECK((b.lo[0] == doctest::Approx(9.5f) || b.lo[0] == doctest::Approx(10.0f)));
    }
    CHECK(stone_wall_cells() == 0);
    CHECK(g_voxels.count({21, 39, 59}) == 1);  // the hill
    CHECK(g_voxels.count({19, 38, 59}) == 1);  // the dirt under the wall
    CHECK(g_voxels.count({20, 38, 60}) == 1);
    REQUIRE(g_answers.size() == 1);
    CHECK(g_answers[0].r.status == EMF_BREAK_DONE);
    CHECK(g_answers[0].r.removed == 2 + 8);  // the fake kernel's 2 and the wall's 8 cells
    // without the flag the terrain stays
    terrain_scene();
    g_fills.clear();
    REQUIRE(f.ask(EMF_BREAK_INSTANT) != 0);
    f.step();
    CHECK(g_fills.empty());
    CHECK(stone_wall_cells() == 8);
}

TEST_CASE("block glue: terrain in damage mode takes the material's hits (Stone: 22 at the default damage) and heals 20 s after the last") {
    Fixture f;
    terrain_scene();
    for (int i = 0; i < 21; ++i) {
        REQUIRE(f.ask(EMF_BREAK_TERRAIN) != 0);
        f.step();
    }
    CHECK(g_fills.empty());  // 5 x 250 / 60 = 20.8 hits: 12 bytes each, so 22 reach 255
    REQUIRE(f.ask(EMF_BREAK_TERRAIN) != 0);
    f.step();
    CHECK(g_fills.size() == 8);
    CHECK(stone_wall_cells() == 0);
    // healed in between: from the start again
    terrain_scene();
    g_fills.clear();
    for (int i = 0; i < 5; ++i) {
        REQUIRE(f.ask(EMF_BREAK_TERRAIN) != 0);
        f.step();
    }
    g_now += 20001;
    for (int i = 0; i < 21; ++i) {
        REQUIRE(f.ask(EMF_BREAK_TERRAIN) != 0);
        f.step();
    }
    CHECK(g_fills.empty());
}

TEST_CASE("block glue: terrain drops each terrain material's own item; a shipped world not known spares every terrain cell") {
    Fixture f;
    terrain_scene();
    REQUIRE(f.ask(EMF_BREAK_TERRAIN | EMF_BREAK_INSTANT | EMF_BREAK_DROPS) != 0);
    f.step();  // 8 Stone cells, 5 an item: 1 (3 kept)
    bool stone = false;
    for (const auto& d : g_drops) stone = stone || (d.guid0 == 0x43 && d.count == 1);
    CHECK(stone);
    Fixture g;
    terrain_scene();
    g_base_columns.clear();  // the shipped world's columns not loaded
    REQUIRE(g.ask(EMF_BREAK_TERRAIN | EMF_BREAK_INSTANT) != 0);
    g.step();
    CHECK(g_fills.empty());
    CHECK(stone_wall_cells() == 8);
}

TEST_CASE("block glue: without the terrain signatures EMF_BREAK_TERRAIN is refused with the reason; other breaks still run") {
    Fixture f(true, true, false);
    std::string why;
    CHECK(f.ask(EMF_BREAK_TERRAIN, 0, &why) == 0);
    CHECK(why == "breaking terrain is not available on this game version");
    CHECK(f.ask(0, 0, &why) != 0);
}

TEST_CASE("block glue: a fault in the box fill turns breaking off (logged); the request answers FAILED") {
    Fixture f;
    terrain_scene();
    g_box_fill_faults = true;
    REQUIRE(f.ask(EMF_BREAK_TERRAIN | EMF_BREAK_INSTANT) != 0);
    f.step();
    REQUIRE(g_answers.size() == 1);
    CHECK(g_answers[0].r.status == EMF_BREAK_FAILED);
    std::string why;
    CHECK(f.ask(0, 0, &why) == 0);
    CHECK(why.find("box fill") != std::string::npos);
    bool said = false;
    for (const auto& l : g_log) said = said || l.find("box fill") != std::string::npos;
    CHECK(said);
}

TEST_CASE("block glue: find_wall with EMF_FIND_TERRAIN stops at terrain a player placed; block_at tells the world's ground from a player's") {
    Fixture f;
    terrain_scene();
    const float from[3] = {8.1f, 20.2f, 29.8f}, to[3] = {12.1f, 20.2f, 29.8f};
    EmfBlockInfo out{};
    out.size = sizeof out;
    std::string why;
    bool ok = true;
    run_in_callback(f.world(), EMF_WORLD_SERVER, [&](EmfWorld* w) { ok = blocks_find_wall(w, from, to, &out, &why); });
    CHECK_FALSE(ok);  // terrain is passed over without the flag
    run_in_callback(f.world(), EMF_WORLD_SERVER,
                    [&](EmfWorld* w) { ok = blocks_find_wall(w, from, to, &out, &why, EMF_FIND_TERRAIN); });
    REQUIRE(ok);
    CHECK(out.kind == EMF_BLOCK_TERRAIN);
    CHECK(out.material == 67);
    CHECK(out.world == 0);
    CHECK(out.x == doctest::Approx(9.75));
    // from the hill's side: the natural hill is passed over, the wall's far face found
    run_in_callback(f.world(), EMF_WORLD_SERVER,
                    [&](EmfWorld* w) { ok = blocks_find_wall(w, to, from, &out, &why, EMF_FIND_TERRAIN); });
    REQUIRE(ok);
    CHECK(out.x == doctest::Approx(10.25));
    // block_at: the hill is the world's own ground, the wall a player's, a cell whose base is not known unknown
    run_in_callback(f.world(), EMF_WORLD_SERVER, [&](EmfWorld* w) { ok = blocks_block_at(w, 10.7f, 20.2f, 29.8f, &out, &why); });
    REQUIRE(ok);
    CHECK(out.kind == EMF_BLOCK_TERRAIN);
    CHECK(out.world == 1);
    run_in_callback(f.world(), EMF_WORLD_SERVER, [&](EmfWorld* w) { ok = blocks_block_at(w, 9.7f, 20.2f, 29.8f, &out, &why); });
    REQUIRE(ok);
    CHECK(out.world == 0);
    g_base_columns.clear();
    run_in_callback(f.world(), EMF_WORLD_SERVER, [&](EmfWorld* w) { ok = blocks_block_at(w, 9.7f, 20.2f, 29.8f, &out, &why); });
    REQUIRE(ok);
    CHECK(out.world == -1);
}

TEST_CASE("api table (v47): find_wall_ex appended; EMF_FIND_TERRAIN reaches the glue; unknown flags refused") {
    Fixture f;
    terrain_scene();
    const EmfApi* api = api_table();
    CHECK(api->version == 47u);
    CHECK(api->size == kApiV47Size);
    CHECK(offsetof(EmfApi, find_wall_ex) == kApiV46Size);
    REQUIRE(api->find_wall_ex != nullptr);
    const float from[3] = {8.1f, 20.2f, 29.8f}, to[3] = {12.1f, 20.2f, 29.8f};
    EmfBlockInfo out{};
    out.size = sizeof out;
    bool ok = false;
    run_in_callback(f.world(), EMF_WORLD_SERVER, [&](EmfWorld* w) { ok = api_table()->find_wall_ex(w, from, to, EMF_FIND_TERRAIN, &out); });
    REQUIRE(ok);
    CHECK(out.kind == EMF_BLOCK_TERRAIN);
    CHECK(out.material == 67);
    CHECK(out.hardness == 3);  // the terrain material's (Stone: Hard), as the game has it
    CHECK(out.health == 5);
    CHECK(out.world == 0);
    run_in_callback(f.world(), EMF_WORLD_SERVER, [&](EmfWorld* w) { ok = api_table()->find_wall_ex(w, from, to, 0, &out); });
    CHECK_FALSE(ok);  // flags 0: building blocks only, as find_wall
    run_in_callback(f.world(), EMF_WORLD_SERVER, [&](EmfWorld* w) { ok = api_table()->find_wall_ex(w, from, to, 0x80, &out); });
    CHECK_FALSE(ok);
    CHECK_FALSE(api->find_wall_ex(f.world(), from, to, EMF_FIND_TERRAIN, &out));  // not inside a world callback
}

// Review (2026-11-18): the game only ever calls its box fill on building cells. A build where it left solid terrain in
// place would count every hit as a hole (raiders stuck hitting, drops paid out every hit): EMF reads the cells back.
TEST_CASE("block glue: terrain cells the box fill left in place are not counted removed; breaking terrain goes off (logged)") {
    Fixture f;
    terrain_scene();
    g_box_fill_keeps_terrain = true;
    REQUIRE(f.ask(EMF_BREAK_TERRAIN | EMF_BREAK_INSTANT | EMF_BREAK_DROPS) != 0);
    f.step();
    REQUIRE(g_answers.size() == 1);
    CHECK(g_answers[0].r.status == EMF_BREAK_DONE);
    CHECK(g_answers[0].r.removed == 2);  // the fake kernel's only: no wall cell went
    bool stone_dropped = false;
    for (const auto& d : g_drops) stone_dropped = stone_dropped || d.guid0 == 0x43;
    CHECK_FALSE(stone_dropped);
    bool said = false;
    for (const auto& l : g_log) said = said || l.find("left") != std::string::npos;
    CHECK(said);
    std::string why;
    CHECK(f.ask(EMF_BREAK_TERRAIN, 0, &why) == 0);
    CHECK(why.find("breaking terrain is off") != std::string::npos);
    CHECK(f.ask(0, 0, &why) != 0);  // building blocks still break
}

TEST_CASE("block glue: a terrain sphere whose box is not loaded spares everything; a new world forgets the terrain damage") {
    Fixture f;
    terrain_scene();
    g_base_columns.clear();
    g_base_columns.push_back({2, 7});
    for (int i = 0; i < 5; ++i) {  // 5 of Stone's 22 hits
        REQUIRE(f.ask(EMF_BREAK_TERRAIN) != 0);
        f.step();
    }
    blocks_world_reset();
    for (int i = 0; i < 17; ++i) {  // 22 in all, but the reset forgot the first 5
        REQUIRE(f.ask(EMF_BREAK_TERRAIN) != 0);
        f.step();
    }
    CHECK(g_fills.empty());
    CHECK(stone_wall_cells() == 8);
}
