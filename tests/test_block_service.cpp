// Breaking blocks (API v44): the block service queues requests and runs them in EMF's program, at most 8 spheres a
// step, over fake game calls.
#include <doctest.h>

#include <cstring>
#include <string>
#include <vector>

#include "core/block_service.h"
#include "emf/emf_api.h"

using namespace emf::core;

namespace {

struct Fake {
    MaterialFacts facts[128] = {};
    float factor = 1.0f;
    bool kernel_ok = true;
    std::function<void()> on_kernel;  // runs inside each kernel call (requests arriving meanwhile)
    std::vector<Sphere> spheres;
    std::vector<bool> instants;
    std::vector<float> stone_entries;  // table[128 + 7] as each call saw it
    uint32_t damaged_each = 3, removed_each = 2;
    float removed_stone_each = 0, removed_straw_each = 0;
    struct Drop {
        uint8_t guid0;
        uint32_t count;
        float pos[3];
    };
    std::vector<Drop> drops;

    Fake() {
        facts[2] = {true, 0, 100, 4, {}};
        facts[2].item_guid[0] = 0x22;
        facts[7] = {true, 4, 1000, 8, {}};
        facts[7].item_guid[0] = 0x77;
    }
    BlockEngine engine() {
        BlockEngine e;
        e.facts = [this](MaterialFacts out[128]) {
            std::memcpy(out, facts, sizeof facts);
            return true;
        };
        e.mining_factor = [this] { return factor; };
        e.kernel = [this](const Sphere& s, const float table[256], bool instant, KernelCount* out) {
            spheres.push_back(s);
            instants.push_back(instant);
            stone_entries.push_back(table[128 + 7]);
            if (on_kernel) on_kernel();
            if (!kernel_ok) return false;
            out->damaged += damaged_each;
            out->removed += removed_each;
            out->removed_per_material[128 + 7] += removed_stone_each;
            out->removed_per_material[128 + 2] += removed_straw_each;
            return true;
        };
        e.drop = [this](const uint8_t guid[16], uint32_t count, const float pos[3]) {
            drops.push_back({guid[0], count, {pos[0], pos[1], pos[2]}});
            return count;
        };
        return e;
    }
};

struct Answer {
    uint32_t id = 0;
    EmfBreakResult r{};
};
std::vector<Answer> g_answers;
void on_done(EmfWorld*, uint32_t id, const EmfBreakResult* r, void*) { g_answers.push_back({id, *r}); }

EmfBreakBlocks req(float radius = 1, float height = 0, uint32_t flags = 0) {
    EmfBreakBlocks r{};
    r.size = sizeof r;
    r.x = 5;
    r.y = 10;
    r.z = 15;
    r.radius = radius;
    r.height = height;
    r.flags = flags;
    return r;
}

void deliver(const std::vector<BlockDone>& done) {
    for (const auto& d : done)
        if (d.cb) d.cb(nullptr, d.id, &d.result, d.user);
}

}  // namespace

TEST_CASE("block service: a request runs at the next step and answers once with the kernel's counts") {
    g_answers.clear();
    BlockService s;
    Fake f;
    std::string why;
    const uint32_t id = s.request(req(), on_done, nullptr, &why);
    REQUIRE(id != 0);
    CHECK(why.empty());
    CHECK(s.waiting() == 1);
    CHECK(f.spheres.empty());
    deliver(s.step(f.engine()));
    REQUIRE(g_answers.size() == 1);
    CHECK(g_answers[0].id == id);
    CHECK(g_answers[0].r.size == sizeof(EmfBreakResult));
    CHECK(g_answers[0].r.status == EMF_BREAK_DONE);
    CHECK(g_answers[0].r.damaged == 3);
    CHECK(g_answers[0].r.removed == 2);
    CHECK(g_answers[0].r.dropped == 0);
    REQUIRE(f.spheres.size() == 1);
    CHECK(f.spheres[0].x == 5);
    CHECK(f.spheres[0].y == 10);
    CHECK(f.spheres[0].r == 1);
    CHECK(s.waiting() == 0);
    deliver(s.step(f.engine()));
    CHECK(g_answers.size() == 1);
}

TEST_CASE("block service: at most 8 spheres a step; the rest wait in order") {
    g_answers.clear();
    BlockService s;
    Fake f;
    std::vector<uint32_t> ids;
    for (int i = 0; i < 5; ++i) ids.push_back(s.request(req(1, 1), on_done, nullptr, nullptr));  // 2 spheres each
    deliver(s.step(f.engine()));
    CHECK(f.spheres.size() == 8);
    REQUIRE(g_answers.size() == 4);
    for (int i = 0; i < 4; ++i) CHECK(g_answers[i].id == ids[i]);
    deliver(s.step(f.engine()));
    CHECK(f.spheres.size() == 10);
    REQUIRE(g_answers.size() == 5);
    CHECK(g_answers[4].id == ids[4]);
}

TEST_CASE("block service: the 65th waiting request is refused") {
    BlockService s;
    for (size_t i = 0; i < kBlockRequestsMax; ++i) CHECK(s.request(req(), on_done, nullptr, nullptr) != 0);
    std::string why;
    CHECK(s.request(req(), on_done, nullptr, &why) == 0);
    CHECK(why.find("64") != std::string::npos);
}

TEST_CASE("block service: a bad request is refused at once with the check's text") {
    BlockService s;
    std::string why;
    CHECK(s.request(req(3.6f), on_done, nullptr, &why) == 0);
    CHECK_FALSE(why.empty());
    CHECK(s.waiting() == 0);
}

TEST_CASE("block service: world_reset answers every waiting request CANCELLED once") {
    g_answers.clear();
    BlockService s;
    Fake f;
    s.request(req(), on_done, nullptr, nullptr);
    s.request(req(), on_done, nullptr, nullptr);
    deliver(s.world_reset());
    REQUIRE(g_answers.size() == 2);
    CHECK(g_answers[0].r.status == EMF_BREAK_CANCELLED);
    CHECK(g_answers[1].r.status == EMF_BREAK_CANCELLED);
    deliver(s.step(f.engine()));
    CHECK(f.spheres.empty());
    CHECK(g_answers.size() == 2);
}

TEST_CASE("block service: a failed kernel call (a fault: breaking goes off) ends that request and the rest of its step FAILED; none goes back into the queue") {
    g_answers.clear();
    BlockService s;
    Fake f;
    const uint32_t a = s.request(req(), on_done, nullptr, nullptr);
    const uint32_t b = s.request(req(), on_done, nullptr, nullptr);
    f.kernel_ok = false;
    deliver(s.step(f.engine()));
    REQUIRE(g_answers.size() == 2);
    CHECK(g_answers[0].id == a);
    CHECK(g_answers[1].id == b);
    CHECK(g_answers[0].r.status == EMF_BREAK_FAILED);
    CHECK(g_answers[1].r.status == EMF_BREAK_FAILED);
    CHECK(s.waiting() == 0);
}

TEST_CASE("block service: after a failed kernel call the queue never holds more than 64") {
    g_answers.clear();
    BlockService s;
    Fake f;
    for (size_t i = 0; i < kBlockRequestsMax; ++i) REQUIRE(s.request(req(), on_done, nullptr, nullptr) != 0);
    f.kernel_ok = false;
    f.on_kernel = [&] {  // more come in while the step runs (another thread)
        while (s.request(req(), on_done, nullptr, nullptr) != 0) {
        }
    };
    deliver(s.step(f.engine()));
    CHECK(s.waiting() <= kBlockRequestsMax);
}

TEST_CASE("block service: drops go through the loot rate with a remainder kept between requests") {
    g_answers.clear();
    BlockService s;
    Fake f;
    f.removed_straw_each = 3;  // straw: 4 blocks per item
    s.request(req(1, 0, EMF_BREAK_DROPS), on_done, nullptr, nullptr);
    deliver(s.step(f.engine()));
    CHECK(f.drops.empty());
    REQUIRE(g_answers.size() == 1);
    CHECK(g_answers[0].r.dropped == 0);
    s.request(req(1, 2, EMF_BREAK_DROPS), on_done, nullptr, nullptr);  // 3 spheres: 9 more straw blocks
    deliver(s.step(f.engine()));
    REQUIRE(f.drops.size() == 1);
    CHECK(f.drops[0].guid0 == 0x22);
    CHECK(f.drops[0].count == 3);  // (3 + 9) / 4
    CHECK(f.drops[0].pos[1] == doctest::Approx(11.0));  // the column's middle: y + height / 2
    REQUIRE(g_answers.size() == 2);
    CHECK(g_answers[1].r.dropped == 3);
}

TEST_CASE("block service: without the drops flag nothing drops") {
    BlockService s;
    Fake f;
    f.removed_straw_each = 40;
    s.request(req(), on_done, nullptr, nullptr);
    s.step(f.engine());
    CHECK(f.drops.empty());
}

TEST_CASE("block service: instant requests use table 1.0, damage requests health / damage") {
    BlockService s;
    Fake f;
    f.factor = 2.0f;
    s.request(req(1, 0, EMF_BREAK_INSTANT), on_done, nullptr, nullptr);
    s.request(req(), on_done, nullptr, nullptr);
    s.step(f.engine());
    REQUIRE(f.stone_entries.size() == 2);
    CHECK(f.instants[0]);
    CHECK(f.stone_entries[0] == 1.0f);
    CHECK_FALSE(f.instants[1]);
    CHECK(f.stone_entries[1] == doctest::Approx(1000.0 / (50 * 2)));
}

TEST_CASE("block service: each sphere asks how far it may reach (ruin protection): a smaller radius reaches the kernel, 0 skips the sphere, EMF_BREAK_WORLD asks nothing") {
    g_answers.clear();
    BlockService s;
    Fake f;
    BlockEngine e = f.engine();
    std::vector<Sphere> asked;
    float give = 0.6f;
    e.shrink = [&](const Sphere& sp, const float table[256]) {
        CHECK(table != nullptr);
        asked.push_back(sp);
        return give;
    };
    std::string why;
    REQUIRE(s.request(req(), on_done, nullptr, &why) != 0);
    deliver(s.step(e));
    REQUIRE(asked.size() == 1);
    CHECK(asked[0].r == 1);
    REQUIRE(f.spheres.size() == 1);
    CHECK(f.spheres[0].r == doctest::Approx(0.6f));
    give = 0;  // next to the world's own blocks: skipped
    REQUIRE(s.request(req(), on_done, nullptr, &why) != 0);
    deliver(s.step(e));
    CHECK(f.spheres.size() == 1);
    REQUIRE(g_answers.size() == 2);
    CHECK(g_answers[1].r.status == EMF_BREAK_DONE);
    CHECK(g_answers[1].r.damaged == 0);
    EmfBreakBlocks w = req();
    w.flags = EMF_BREAK_WORLD;
    asked.clear();
    REQUIRE(s.request(w, on_done, nullptr, &why) != 0);
    deliver(s.step(e));
    CHECK(asked.empty());
    REQUIRE(f.spheres.size() == 2);
    CHECK(f.spheres[1].r == 1);
}
