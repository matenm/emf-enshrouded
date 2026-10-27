// The sample mod's replay of the engine's NpcCommandEvent GoToPosition handler (0x14044f040, type 1).
#include <doctest.h>

#include <cstring>
#include <vector>

#include "../mods/sample_mod/npc_command.h"

namespace {
// A fake villager worker context: write entries at +0x1b0 (DynamicActiveNpcState) and +0x300 (NpcCommon).
struct FakeCtx {
    alignas(8) uint8_t ctx[0x500] = {};
    uint8_t npc_state[136] = {};
    alignas(4) uint8_t common[1688] = {};

    void plant(uint32_t entry, void* data, uint64_t a, uint64_t b, uint64_t c, uint16_t type_idx, uint8_t variant,
               void* tracker) {
        std::memcpy(ctx + entry + 0x00, &data, 8);
        std::memcpy(ctx + entry + 0x08, &a, 8);
        std::memcpy(ctx + entry + 0x10, &b, 8);
        std::memcpy(ctx + entry + 0x18, &c, 8);
        std::memcpy(ctx + entry + 0x20, &type_idx, 2);
        ctx[entry + 0x22] = variant;
        std::memcpy(ctx + entry + 0x28, &tracker, 8);
    }
    FakeCtx(void* state_tracker, void* common_tracker) {
        plant(0x1b0, npc_state, 11, 12, 13, 0x0101, 1, state_tracker);
        plant(0x300, common, 21, 22, 23, 0x0202, 2, common_tracker);
    }
    float target(int i) const {
        float f;
        std::memcpy(&f, common + 0x64 + 4 * i, 4);
        return f;
    }
};

struct MarkCall {
    void* tracker;
    uint64_t a, b, c;
    uint32_t type_idx;
    uint8_t variant;
};
std::vector<MarkCall> g_marks;
std::vector<uint8_t> g_states;
FakeCtx* g_current = nullptr;

void fake_mark(void* tracker, uint64_t a, uint64_t b, uint64_t c, uint32_t type_idx, uint8_t variant) {
    g_marks.push_back({tracker, a, b, c, type_idx & 0xFFFF, variant});
}
void fake_set_state(void* ctx, uint8_t state) {
    g_states.push_back(state);
    if (g_current && ctx == g_current->ctx) {  // what the engine's SetState does to the bytes we look at
        g_current->npc_state[1] = g_current->npc_state[0];
        g_current->npc_state[0] = state;
    }
}
int g_state_tracker = 0, g_common_tracker = 0;
const double kTarget[3] = {3704.25, 905.5, 1171.0};
}  // namespace

TEST_CASE("apply_go_to_position does what the engine's GoToPosition handler does") {
    g_marks.clear();
    g_states.clear();
    FakeCtx f(&g_state_tracker, &g_common_tracker);
    g_current = &f;
    f.npc_state[0] = 3;  // Idle
    f.npc_state[2] = 0;  // decision None
    REQUIRE(sample::apply_go_to_position(f.ctx, kTarget, fake_set_state, fake_mark));
    CHECK(g_states == std::vector<uint8_t>{7});  // SetState(ctx, Roam)
    CHECK(f.npc_state[0] == 7);
    CHECK(f.npc_state[2] == 1);                  // decision = WantsToRoam
    CHECK(f.target(0) == doctest::Approx(3704.25f));
    CHECK(f.target(1) == doctest::Approx(905.5f));
    CHECK(f.target(2) == doctest::Approx(1171.0f));
    REQUIRE(g_marks.size() == 2);                // both written components marked, with their own entry args
    CHECK(g_marks[0].tracker == &g_state_tracker);
    CHECK(g_marks[0].a == 11);
    CHECK(g_marks[0].c == 13);
    CHECK(g_marks[0].type_idx == 0x0101);
    CHECK(g_marks[0].variant == 1);
    CHECK(g_marks[1].tracker == &g_common_tracker);
    CHECK(g_marks[1].b == 22);
    CHECK(g_marks[1].type_idx == 0x0202);
    CHECK(g_marks[1].variant == 2);
    g_current = nullptr;
}

TEST_CASE("apply_go_to_position skips markWritten when change tracking is off (tracker NULL)") {
    g_marks.clear();
    g_states.clear();
    FakeCtx f(nullptr, nullptr);
    f.npc_state[0] = 7;
    REQUIRE(sample::apply_go_to_position(f.ctx, kTarget, fake_set_state, fake_mark));
    CHECK(g_marks.empty());
    CHECK(f.npc_state[2] == 1);
}

TEST_CASE("apply_go_to_position leaves a villager in state Start alone, like the engine") {
    g_marks.clear();
    g_states.clear();
    FakeCtx f(&g_state_tracker, &g_common_tracker);
    f.npc_state[0] = 1;  // Start
    CHECK_FALSE(sample::apply_go_to_position(f.ctx, kTarget, fake_set_state, fake_mark));
    CHECK(g_states.empty());
    CHECK(g_marks.empty());
    CHECK(f.npc_state[2] == 0);
    CHECK(f.target(0) == 0.0f);
}

TEST_CASE("apply_go_to_position refuses missing pointers") {
    FakeCtx f(nullptr, nullptr);
    CHECK_FALSE(sample::apply_go_to_position(nullptr, kTarget, fake_set_state, fake_mark));
    CHECK_FALSE(sample::apply_go_to_position(f.ctx, kTarget, nullptr, fake_mark));
    CHECK_FALSE(sample::apply_go_to_position(f.ctx, kTarget, fake_set_state, nullptr));
    FakeCtx empty(nullptr, nullptr);
    std::memset(empty.ctx, 0, sizeof(empty.ctx));  // no component pointers
    CHECK_FALSE(sample::apply_go_to_position(empty.ctx, kTarget, fake_set_state, fake_mark));
}

TEST_CASE("PendingCommand hands the command to the matching villager once, before it expires") {
    sample::PendingCommand p;
    double t[3] = {0, 0, 0};
    CHECK_FALSE(p.take_if(5, 1000, t));  // nothing posted
    const double target[3] = {1, 2, 3};
    p.post(1025, target, 1000);
    CHECK_FALSE(p.take_if(1026, 1100, t));  // another villager
    REQUIRE(p.take_if(1025, 1200, t));
    CHECK(t[0] == 1.0);
    CHECK(t[2] == 3.0);
    CHECK_FALSE(p.take_if(1025, 1300, t));  // already taken
    p.post(1025, target, 2000);
    CHECK_FALSE(p.take_if(1025, 2000 + sample::PendingCommand::kLifetimeMs + 1, t));  // expired
    CHECK_FALSE(p.armed());
}

TEST_CASE("PendingCommand::expire disarms a command that nobody took within its lifetime") {
    sample::PendingCommand p;
    const double target[3] = {1, 2, 3};
    p.post(7, target, 1000);
    p.expire(1000 + sample::PendingCommand::kLifetimeMs);  // still valid at exactly its lifetime
    CHECK(p.armed());
    p.expire(1000 + sample::PendingCommand::kLifetimeMs + 1);
    CHECK_FALSE(p.armed());
}
