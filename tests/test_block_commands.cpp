// Breaking blocks (API v44): the console commands blocks and break, over fake reads and a fake break_blocks.
#include <doctest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "core/block_commands.h"
#include "core/commands.h"
#include "emf/emf_api.h"

using namespace emf::core;

namespace {

std::vector<std::string> g_out;
bool said(const std::string& needle) {
    for (const auto& l : g_out)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}

struct Fake {
    bool camera = true;
    bool wall = true;
    EmfBreakBlocks last{};
    EmfBreakCallback cb = nullptr;
    void* user = nullptr;
    uint32_t next_id = 5;
    float wall_from[3] = {}, wall_to[3] = {};
    BlockCommandDeps deps() {
        BlockCommandDeps d;
        d.camera = [this](EmfWorld*, float o[3], float dir[3]) {
            if (!camera) return false;
            o[0] = 10, o[1] = 20, o[2] = 30;
            dir[0] = 1, dir[1] = 0, dir[2] = 0;
            return true;
        };
        d.find_wall = [this](EmfWorld*, const float from[3], const float to[3], EmfBlockInfo* out) {
            for (int i = 0; i < 3; ++i) wall_from[i] = from[i], wall_to[i] = to[i];
            if (!wall) return false;
            *out = EmfBlockInfo{};
            out->size = sizeof *out;
            out->kind = EMF_BLOCK_BUILDING;
            out->material = 7;
            out->fill = 255;
            out->hardness = 4;
            out->health = 1000;
            out->damage = 34;
            out->x = 13.25f, out->y = 20.25f, out->z = 30.25f;
            return true;
        };
        d.block_at = [](EmfWorld*, float, float, float, EmfBlockInfo* out) {
            *out = EmfBlockInfo{};
            out->size = sizeof *out;
            out->kind = EMF_BLOCK_TERRAIN;
            out->material = 5;
            return true;
        };
        d.break_blocks = [this](EmfWorld*, EmfBreakBlocks* r, EmfBreakCallback c, void* u) -> uint32_t {
            last = *r;
            if (r->radius > 3.5f) {
                std::snprintf(r->why, sizeof r->why, "radius %.2f m: it must be 0.25 to 3.5 m (0 = 1 m)", r->radius);
                return 0;
            }
            cb = c;
            user = u;
            return next_id;
        };
        return d;
    }
};

void run(CommandCenter& c, const std::string& line) {
    c.enqueue(line);
    c.run_pending(reinterpret_cast<EmfWorld*>(0x1000), [](const std::string& s) { g_out.push_back(s); });
}

}  // namespace

TEST_CASE("blocks: names the building block on the camera ray (12 m), else what is at the ray's end") {
    g_out.clear();
    CommandCenter c;
    Fake f;
    REQUIRE(add_block_commands(c, f.deps()));
    run(c, "blocks");
    CHECK(said("building 7"));
    CHECK(said("VeryHard"));
    CHECK(said("1000 health"));
    CHECK(said("damage 34/255"));
    CHECK(said("13.25"));
    CHECK(f.wall_from[0] == 10);
    CHECK(f.wall_to[0] == doctest::Approx(22));
    f.wall = false;
    g_out.clear();
    run(c, "blocks");
    CHECK(said("no building block within 12 m"));
    CHECK(said("terrain 5"));
    f.camera = false;
    g_out.clear();
    run(c, "blocks");
    CHECK(said("no camera"));
}

TEST_CASE("break: radius, instant, drops and height reach the request; the answer follows after the step") {
    g_out.clear();
    CommandCenter c;
    Fake f;
    REQUIRE(add_block_commands(c, f.deps()));
    run(c, "break 1.5 instant drops height 1");
    CHECK(f.last.size == sizeof(EmfBreakBlocks));
    CHECK(f.last.radius == 1.5f);
    CHECK(f.last.height == 1.0f);
    CHECK(f.last.flags == (EMF_BREAK_INSTANT | EMF_BREAK_DROPS));
    CHECK(f.last.x == 13.25f);
    CHECK(f.last.y == 20.25f);
    CHECK(f.last.z == 30.25f);
    CHECK(said("break 5 queued"));
    REQUIRE(f.cb != nullptr);
    EmfBreakResult r{};
    r.size = sizeof r;
    r.status = EMF_BREAK_DONE;
    r.damaged = 12;
    r.removed = 9;
    r.dropped = 2;
    f.cb(nullptr, 5, &r, f.user);
    CHECK(said("break 5: damaged 12, removed 9, dropped 2"));
    g_out.clear();
    run(c, "break");
    CHECK(f.last.radius == 0.0f);  // 0 = 1 m
    CHECK(f.last.flags == 0u);
    r.status = EMF_BREAK_CANCELLED;
    f.cb(nullptr, 5, &r, f.user);
    CHECK(said("cancelled"));
}

TEST_CASE("break: refused with a bad radius (the request's own text); nothing where you look; a bad word") {
    g_out.clear();
    CommandCenter c;
    Fake f;
    REQUIRE(add_block_commands(c, f.deps()));
    run(c, "break 9");
    CHECK(said("radius 9.00 m"));
    f.wall = false;
    g_out.clear();
    run(c, "break");
    CHECK(said("no building block within 12 m"));
    g_out.clear();
    run(c, "break wide");
    CHECK(said("usage: break"));
}

TEST_CASE("blocks and break at x y z: no camera needed (a dedicated server's console)") {
    g_out.clear();
    CommandCenter c;
    Fake f;
    f.camera = false;
    REQUIRE(add_block_commands(c, f.deps()));
    run(c, "blocks at 1 2 3");
    CHECK(said("terrain 5"));  // what block_at says at that point
    run(c, "break 2 instant at 4.5 6 7.25");
    CHECK(f.last.radius == 2.0f);
    CHECK(f.last.flags == EMF_BREAK_INSTANT);
    CHECK(f.last.x == 4.5f);
    CHECK(f.last.y == 6.0f);
    CHECK(f.last.z == 7.25f);
    CHECK(said("break 5 queued"));
    g_out.clear();
    run(c, "break at 1 2");
    CHECK(said("usage: break"));
    g_out.clear();
    run(c, "blocks at x 2 3");
    CHECK(said("usage: blocks"));
}

TEST_CASE("blocks and break are admin commands (no player flag)") {
    CommandCenter c;
    Fake f;
    REQUIRE(add_block_commands(c, f.deps()));
    int found = 0;
    for (const auto& e : c.entries())
        if (e.name == "blocks" || e.name == "break") {
            ++found;
            CHECK_FALSE(e.players);
        }
    CHECK(found == 2);
}

TEST_CASE("blocks origin: says whether the block where you look, or at a point, is the world's own (a ruin) or a player's") {
    g_out.clear();
    CommandCenter c;
    Fake f;
    auto d = f.deps();
    d.origin = [](EmfWorld*, float x, float, float) -> std::string {
        return x > 12 ? "origin at 13.25 20.25 30.25: now 0xff87, shipped 0xff87: the world's own (a ruin)" : "";
    };
    REQUIRE(add_block_commands(c, d));
    run(c, "blocks origin");  // where you look: the wall at 13.25
    CHECK(said("the world's own"));
    g_out.clear();
    run(c, "blocks origin at 1 2 3");
    CHECK(said("nothing read there"));
    g_out.clear();
    run(c, "blocks origin at 1 2");
    CHECK(said("usage: blocks"));
}

TEST_CASE("break world: the word asks to break the world's own blocks (ruins) too (EMF_BREAK_WORLD)") {
    g_out.clear();
    CommandCenter c;
    Fake f;
    f.camera = false;
    REQUIRE(add_block_commands(c, f.deps()));
    run(c, "break 1 world at 1 2 3");
    CHECK(f.last.flags == EMF_BREAK_WORLD);
    run(c, "break 1 at 1 2 3");
    CHECK(f.last.flags == 0);
}
