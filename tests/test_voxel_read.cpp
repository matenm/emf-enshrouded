// Breaking blocks (API v44): what a voxel is (empty, terrain, building) and the first building block on a line.
#include <doctest.h>

#include <map>
#include <tuple>

#include "core/voxel_read.h"

using namespace emf::core;

namespace {

struct FakeVoxels {
    std::map<std::tuple<int, int, int>, uint16_t> cells;
    int reads = 0;
    bool loaded = true;
    VoxelReader reader() {
        return [this](int32_t x, int32_t y, int32_t z, uint16_t* v) {
            ++reads;
            if (!loaded) return false;
            const auto it = cells.find({x, y, z});
            *v = it == cells.end() ? 0 : it->second;
            return true;
        };
    }
};

MaterialFacts g_facts[128];
struct FactsInit {
    FactsInit() {
        g_facts[7] = {true, 4, 1000, 8, {}};
        g_facts[11] = {true, 5, 1000, 1, {}};
    }
} g_facts_init;

}  // namespace

TEST_CASE("voxel read: decode empty, terrain and building") {
    EmfBlockInfo e = decode_voxel(0x0000, g_facts);
    CHECK(e.size == sizeof(EmfBlockInfo));
    CHECK(e.kind == EMF_BLOCK_EMPTY);
    CHECK(decode_voxel(0x0005, g_facts).kind == EMF_BLOCK_EMPTY);  // a material with no fill
    CHECK(decode_voxel(0xFF00, g_facts).kind == EMF_BLOCK_EMPTY);  // fill without a material
    const EmfBlockInfo t = decode_voxel(0xFF05, g_facts);
    CHECK(t.kind == EMF_BLOCK_TERRAIN);
    CHECK(t.material == 5);
    CHECK(t.fill == 255);
    CHECK(t.hardness == 0);
    CHECK(t.health == 0);
    const EmfBlockInfo b = decode_voxel(0x8087, g_facts);
    CHECK(b.kind == EMF_BLOCK_BUILDING);
    CHECK(b.material == 7);
    CHECK(b.fill == 0x80);
    CHECK(b.hardness == 4);
    CHECK(b.health == 1000);
    CHECK(b.damage == -1);
    const EmfBlockInfo u = decode_voxel(0xFF80 + 3, g_facts);  // building id 3: no config
    CHECK(u.kind == EMF_BLOCK_BUILDING);
    CHECK(u.material == 3);
    CHECK(u.health == 0);
}

// Reported from a hosted game (0.6.16, build 1287369): Rough Wood Block, Rough Stone Block and a floor placed with the
// construction hammer read 0x008f, 0x009b and 0x00b1, a building material with a zero high byte. The game decides
// "building" by the material byte alone (research: the building kernel tests only the low byte); the high byte is a
// fill for terrain only.
TEST_CASE("voxel read: a building block is building whatever its high byte (blocks a player placed read 0x00..)") {
    const EmfBlockInfo b = decode_voxel(0x0087, g_facts);
    CHECK(b.kind == EMF_BLOCK_BUILDING);
    CHECK(b.material == 7);
    CHECK(b.fill == 0);
    CHECK(b.hardness == 4);
    CHECK(b.health == 1000);
    CHECK(decode_voxel(0x008f, g_facts).kind == EMF_BLOCK_BUILDING);
    CHECK(decode_voxel(0x009b, g_facts).kind == EMF_BLOCK_BUILDING);
    CHECK(decode_voxel(0x00b1, g_facts).kind == EMF_BLOCK_BUILDING);
    CHECK(decode_voxel(0x0005, g_facts).kind == EMF_BLOCK_EMPTY);  // terrain without fill stays empty
    FakeVoxels v;
    v.cells[{10, 2, 2}] = 0x0087;
    const float from[3] = {0.1f, 1.1f, 1.1f}, to[3] = {10.1f, 1.1f, 1.1f};
    const auto hit = first_building_on_line(from, to, v.reader(), g_facts);
    REQUIRE(hit);
    CHECK(hit->material == 7);
    CHECK(hit->x == doctest::Approx(5.25));
}

TEST_CASE("voxel read: the first building cell on a line, with its centre") {
    FakeVoxels v;
    for (int y = 0; y < 8; ++y)
        for (int z = 0; z < 8; ++z) v.cells[{10, y, z}] = 0xFF87;  // a stone wall at x = 5..5.5 m
    const float from[3] = {0.1f, 1.1f, 1.1f}, to[3] = {10.1f, 1.1f, 1.1f};
    const auto hit = first_building_on_line(from, to, v.reader(), g_facts);
    REQUIRE(hit);
    CHECK(hit->kind == EMF_BLOCK_BUILDING);
    CHECK(hit->material == 7);
    CHECK(hit->x == doctest::Approx(5.25));
    CHECK(hit->y == doctest::Approx(1.25));
    CHECK(hit->z == doctest::Approx(1.25));
    // the other way round finds the wall's far face first
    const auto back = first_building_on_line(to, from, v.reader(), g_facts);
    REQUIRE(back);
    CHECK(back->x == doctest::Approx(5.25));
}

TEST_CASE("voxel read: terrain on the way is skipped") {
    FakeVoxels v;
    v.cells[{4, 2, 2}] = 0xFF05;  // terrain
    v.cells[{10, 2, 2}] = 0xFF87;
    const float from[3] = {0.1f, 1.1f, 1.1f}, to[3] = {10.1f, 1.1f, 1.1f};
    const auto hit = first_building_on_line(from, to, v.reader(), g_facts);
    REQUIRE(hit);
    CHECK(hit->x == doctest::Approx(5.25));
}

TEST_CASE("voxel read: nothing within 64 m; a place not loaded ends the search") {
    FakeVoxels v;
    v.cells[{400, 2, 2}] = 0xFF87;  // 200 m away
    const float from[3] = {0.1f, 1.1f, 1.1f}, far[3] = {300.1f, 1.1f, 1.1f};
    CHECK_FALSE(first_building_on_line(from, far, v.reader(), g_facts));
    CHECK(v.reads <= 2 * 64 + 2);  // stopped at 64 m
    FakeVoxels unloaded;
    unloaded.loaded = false;
    unloaded.cells[{2, 2, 2}] = 0xFF87;
    const float to[3] = {10.1f, 1.1f, 1.1f};
    CHECK_FALSE(first_building_on_line(from, to, unloaded.reader(), g_facts));
    CHECK(unloaded.reads == 1);
}

TEST_CASE("voxel read: a line straight down and a diagonal visit every cell they cross") {
    FakeVoxels down;
    const float top[3] = {0.1f, 10.1f, 0.1f}, bottom[3] = {0.1f, 0.1f, 0.1f};
    CHECK_FALSE(first_building_on_line(top, bottom, down.reader(), g_facts));
    CHECK(down.reads == 21);  // cells y 20 down to 0
    FakeVoxels diag;
    const float a[3] = {0.1f, 0.1f, 0.1f}, b[3] = {2.1f, 2.1f, 0.1f};
    CHECK_FALSE(first_building_on_line(a, b, diag.reader(), g_facts));
    CHECK(diag.reads == 9);  // (0,0) to (4,4): one boundary at a time
    FakeVoxels same;
    CHECK_FALSE(first_building_on_line(a, a, same.reader(), g_facts));
    CHECK(same.reads == 1);  // a point: its own cell
}
