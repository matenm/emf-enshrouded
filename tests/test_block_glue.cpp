// Breaking blocks (API v44): the emf_blocks program over fake game calls (the building kernel, the config lookup) and
// a fake world (its singletons, the program's iterator).
#include <doctest.h>

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
const uint8_t* f_config(void* registry, uint8_t id) {
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
    CHECK(r.sx == 1);
    CHECK(r.sy == 1);
    CHECK(r.sz == 1);
    CHECK(r.level == 6);
    g_editor_seen = editor;
    const auto it = g_voxels.find({r.x, r.y, r.z});
    r.buf[0] = it == g_voxels.end() ? 0 : it->second;
    return true;
}
uint8_t f_damage_get(void* store, const uint32_t cell[3]) {
    CHECK(store == g_damage);
    return cell[0] == 20 ? 34 : 0;
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
    explicit Fixture(bool with_kernel = true, bool with_region = true) : ctx(w.p()) {
        programs.reset();
        g_calls.clear();
        g_log.clear();
        g_drops.clear();
        g_answers.clear();
        g_kernel_faults = false;
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

TEST_CASE("api table (v44): break_blocks, block_at and find_wall appended; version 44") {
    const EmfApi* api = api_table();
    CHECK(api->version == 44u);
    CHECK(EMF_API_VERSION == 44);
    CHECK(api->size == kApiV44Size);
    CHECK(offsetof(EmfApi, break_blocks) == kApiV43Size);
    CHECK(api->break_blocks != nullptr);
    CHECK(api->block_at != nullptr);
    CHECK(api->find_wall != nullptr);
    CHECK(kBlockInfoV44Size == sizeof(EmfBlockInfo));
    CHECK(kBreakResultV44Size == sizeof(EmfBreakResult));
}
