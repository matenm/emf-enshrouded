// Quick Stack and Chest Machines: the item moves (mods/quick_stack/qs_moves.h) and the marks' chests (qs_glue.h
// resolve_marks) over a fake API: nothing taken is lost (put back, else dropped at either end, else at the nearest
// player), a result after a world switch touches nothing, the item queue is never flooded, a recycling ends only when
// every put answered, and a mark never jumps to another chest put where the marked one stood.
#include <doctest.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../mods/quick_stack/qs_glue.h"
#include "../mods/quick_stack/qs_moves.h"

namespace gen = emf::gen::ecs;
using namespace quickstack;

namespace {
EmfWorld* const kW = reinterpret_cast<EmfWorld*>(0x5000);
const char* const kMod = "qs_test";

struct Fake {
    std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> comps;
    std::vector<uint32_t> entities;
    std::vector<EmfItemsRequest> takes, puts;
    struct Give {
        uint32_t entity, item, count;
    };
    std::vector<Give> gives;
    std::set<uint32_t> give_fails;  // entities that are gone: no drop there
    bool put_ok = true, take_ok = true;
    uint32_t session = 1;
    std::vector<std::string> log;
};
Fake* f = nullptr;

void* f_get(EmfWorld*, uint32_t hash, uint32_t entity, uint64_t* size) {
    auto it = f->comps.find({hash, entity});
    if (it == f->comps.end()) return nullptr;
    if (size) *size = it->second.size();
    return it->second.data();
}
void* f_tpl(EmfWorld*, uint32_t, uint32_t, uint64_t*) { return nullptr; }
uint8_t f_kind(EmfWorld*) { return EMF_WORLD_SERVER; }
void f_each(EmfWorld*, EmfEntityCallback cb, void* user) {
    for (uint32_t e : f->entities) cb(e, user);
}
bool f_take(EmfWorld*, const EmfItemsRequest* r) {
    if (!f->take_ok) return false;
    f->takes.push_back(*r);
    return true;
}
bool f_put(EmfWorld*, const EmfItemsRequest* r) {
    if (!f->put_ok) return false;
    f->puts.push_back(*r);
    return true;
}
uint32_t f_give(EmfWorld*, uint32_t entity, uint32_t item, uint32_t count) {
    if (f->give_fails.count(entity)) return 0;
    f->gives.push_back({entity, item, count});
    return 900;
}
uint32_t f_session() { return f->session; }
void f_log(const char*, const char* fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    f->log.push_back(b);
}

template <class T>
T& plant(uint32_t entity) {
    auto& v = f->comps[{T::kHash, entity}];
    v.assign(sizeof(T), 0);
    return *reinterpret_cast<T*>(v.data());
}
void place(uint32_t id, double x, double y, double z) {
    auto& t = plant<gen::CurrentTransform>(id);
    t.transform.position.x = static_cast<int64_t>(x * 4294967296.0);
    t.transform.position.y = static_cast<int64_t>(y * 4294967296.0);
    t.transform.position.z = static_cast<int64_t>(z * 4294967296.0);
    if (std::find(f->entities.begin(), f->entities.end(), id) == f->entities.end()) f->entities.push_back(id);
}
void chest(uint32_t id, double x, double y, double z) {
    place(id, x, y, z);
    plant<gen::InventorySetup>(id).isInitialized = true;
}
void player(uint32_t id, double x, double y, double z) {
    place(id, x, y, z);
    plant<gen::PlayerState>(id);
}
void remove(uint32_t id) {
    for (auto it = f->comps.begin(); it != f->comps.end();) it = it->first.second == id ? f->comps.erase(it) : std::next(it);
    f->entities.erase(std::remove(f->entities.begin(), f->entities.end(), id), f->entities.end());
}
bool logged(const std::string& part) {
    for (const auto& l : f->log)
        if (l.find(part) != std::string::npos) return true;
    return false;
}
void answer(const EmfItemsRequest& r, uint32_t done) { r.cb(kW, r.entity, r.item, r.count, done, r.user); }

struct Rig {
    Fake fake;
    EmfApi api{};
    emf::World w{kW};
    Rig() {
        f = &fake;
        api.version = EMF_API_VERSION;
        api.size = sizeof api;
        api.get_component = f_get;
        api.get_template_component = f_tpl;
        api.world_kind = f_kind;
        api.for_each_entity = f_each;
        api.take_items = f_take;
        api.put_items_ex = f_put;
        api.give_item = f_give;
        api.server_session = f_session;
        api.log = f_log;
        emf::api_ref() = &api;
    }
    ~Rig() {
        emf::api_ref() = nullptr;
        f = nullptr;
    }
};
}  // namespace

TEST_CASE("quick stack moves: take, put, and what did not fit goes back where it came from; one queue place while it runs") {
    Rig r;
    Requests req;
    uint32_t added = 99, taken = 99;
    const Move m{700, 0x11, 50, 0x100000001f4ull};
    REQUIRE(move_stack(r.w, 1, m, 1, &req, kMod, [&](emf::World, uint32_t a, uint32_t t) { added = a, taken = t; }));
    CHECK(req.used() == 1);
    REQUIRE(r.fake.takes.size() == 1);
    CHECK(r.fake.takes[0].entity == 1u);
    CHECK((r.fake.takes[0].flags & EMF_ITEMS_EXACT_SLOT) != 0);
    answer(r.fake.takes[0], 50);
    REQUIRE(r.fake.puts.size() == 1);
    CHECK(r.fake.puts[0].entity == 700u);
    answer(r.fake.puts[0], 30);  // 20 had no room
    REQUIRE(r.fake.puts.size() == 2);  // they go back into the player
    CHECK(r.fake.puts[1].entity == 1u);
    CHECK(r.fake.puts[1].count == 20u);
    CHECK(req.used() == 1);  // still running
    answer(r.fake.puts[1], 20);
    CHECK(added == 30u);
    CHECK(taken == 50u);
    CHECK(req.used() == 0);
    CHECK(r.fake.gives.empty());
}

TEST_CASE("quick stack moves: what fits nowhere is dropped at its owner, else at the other end, else at the nearest player; never silently") {
    Rig r;
    player(2, 100, 10, 0);
    player(3, 5, 10, 0);
    chest(700, 0, 10, 0);
    give_back(r.w, 500, 700, 0x11, 20, 1, kMod);  // into sorting chest 500
    REQUIRE(r.fake.puts.size() == 1);
    answer(r.fake.puts[0], 5);
    REQUIRE(r.fake.gives.size() == 1);
    CHECK(r.fake.gives[0].entity == 500u);
    CHECK(r.fake.gives[0].count == 15u);
    // the sorting chest was taken down meanwhile: dropped at the other end of the move
    r.fake.give_fails = {500};
    give_back(r.w, 500, 700, 0x11, 20, 1, kMod);
    answer(r.fake.puts[1], 0);
    CHECK(r.fake.gives.back().entity == 700u);
    CHECK(logged("dropped at 700 (500 was gone)"));
    // both gone: the nearest player
    r.fake.give_fails = {500, 700};
    r.fake.put_ok = false;  // and the put refused at once
    give_back(r.w, 500, 700, 0x11, 7, 1, kMod);
    CHECK(r.fake.gives.back().entity == 3u);
    CHECK(r.fake.gives.back().count == 7u);
    // nobody at all: a LOST line, never silence
    r.fake.give_fails = {500, 700, 2, 3};
    give_back(r.w, 500, 700, 0x11, 4, 1, kMod);
    CHECK(logged("LOST 4 of item 00000011"));
}

TEST_CASE("quick stack moves: a take, put or give back answered after a world switch touches nothing") {
    Rig r;
    Requests req;
    int ended = 0;
    const Move m{700, 0x11, 50, 1};
    move_stack(r.w, 1, m, 1, &req, kMod, [&](emf::World, uint32_t, uint32_t) { ++ended; });
    r.fake.session = 2;  // another world loaded
    answer(r.fake.takes[0], 50);
    CHECK(r.fake.puts.empty());  // nothing put into whatever entity is 700 now
    CHECK(ended == 1);
    CHECK(req.used() == 0);
    CHECK(logged("answered after the world changed"));
    // a give back whose put answers after the switch: no drop
    r.fake.session = 2;
    give_back(r.w, 500, 700, 0x11, 20, 1, kMod);
    answer(r.fake.puts.back(), 0);
    CHECK(r.fake.gives.empty());
    // a put of a move answering after the switch: no give back
    Rig r2;
    Requests req2;
    move_stack(r2.w, 1, m, 1, &req2, kMod, [](emf::World, uint32_t, uint32_t) {});
    answer(r2.fake.takes[0], 50);
    r2.fake.session = 3;
    answer(r2.fake.puts[0], 10);
    CHECK(r2.fake.puts.size() == 1u);
    CHECK(req2.used() == 0);
}

TEST_CASE("quick stack moves: no queue room, no move (nothing taken); a refused take gives the place back") {
    Rig r;
    Requests req;
    REQUIRE(req.reserve(Requests::kMax));
    CHECK_FALSE(move_stack(r.w, 1, Move{700, 0x11, 5, 1}, 1, &req, kMod, nullptr));
    CHECK(r.fake.takes.empty());
    req.clear();
    r.fake.take_ok = false;
    CHECK_FALSE(move_stack(r.w, 1, Move{700, 0x11, 5, 1}, 1, &req, kMod, nullptr));
    CHECK(req.used() == 0);
    CHECK(left_note(0).empty());
    CHECK(left_note(7) == "; 7 more stack(s): press again for the rest");
}

TEST_CASE("quick stack moves: a recycling holds a queue place per material and ends only when every put answered") {
    Rig r;
    Requests req;
    Recycle plan;
    plan.take = 4;
    plan.refund = {{0x21, 8}, {0x22, 2}};
    int ended = 0;
    bool ok = false;
    REQUIRE(recycle_stack(r.w, 900, 950, 0x500, 7, plan, 1, &req, kMod, [&](emf::World, bool recycled) { ++ended, ok = recycled; }));
    CHECK(req.used() == 2);
    answer(r.fake.takes[0], 4);
    REQUIRE(r.fake.puts.size() == 2);
    CHECK(r.fake.puts[0].entity == 950u);
    answer(r.fake.puts[0], 8);
    CHECK(ended == 0);  // still busy: one material on its way
    CHECK(req.used() == 1);
    // the target was taken down: the rest is dropped at the recycler
    r.fake.give_fails = {950};
    answer(r.fake.puts[1], 0);
    CHECK(ended == 1);
    CHECK(ok);
    CHECK(req.used() == 0);
    REQUIRE(r.fake.gives.size() == 1);
    CHECK(r.fake.gives[0].entity == 900u);
    CHECK(r.fake.gives[0].count == 2u);
    // not enough queue room for its materials: not started
    Requests full;
    full.reserve(Requests::kMax - 1);
    CHECK_FALSE(recycle_stack(r.w, 900, 950, 0x500, 7, plan, 1, &full, kMod, nullptr));
    // its take answered after a world switch: no put
    Requests req3;
    REQUIRE(recycle_stack(r.w, 900, 950, 0x500, 7, plan, 1, &req3, kMod, [&](emf::World, bool recycled) { ok = recycled; }));
    r.fake.session = 2;
    const size_t puts = r.fake.puts.size();
    answer(r.fake.takes.back(), 4);
    CHECK(r.fake.puts.size() == puts);
    CHECK_FALSE(ok);
    CHECK(req3.used() == 0);
}

TEST_CASE("chest machines: a mark never jumps to another chest put where the marked one stood (this session); after a load the first counts") {
    Rig r;
    Marks marks;
    MarkBinder binder;
    MissBook miss;
    miss.reset(0);
    player(1, 0, 10, 0);
    chest(1500, 10, 10, 0);
    marks.set({10, 10, 0}, Mark::Recycle);
    std::vector<DroppedMark> dropped;
    auto got = resolve_marks(r.w, marks, binder, miss, 1000, 40, &dropped);
    REQUIRE(got.size() == 1);
    CHECK(got[0].id == 1500u);
    // the host takes the recycler down and puts another chest in its spot within the 30 s a miss needs
    remove(1500);
    chest(1501, 10, 10, 0);
    got = resolve_marks(r.w, marks, binder, miss, 6000, 40, &dropped);
    CHECK(got.empty());
    REQUIRE(dropped.size() == 1);
    CHECK(dropped[0].mark == Mark::Recycle);
    CHECK(dropped[0].why.find("another chest") != std::string::npos);
    CHECK(marks.all().empty());
    // after a load (a new session) ids change: the first chest found at a mark's place is taken
    marks.set({10, 10, 0}, Mark::Sort);
    binder.reset();
    remove(1501);
    chest(1777, 10, 10, 0);
    dropped.clear();
    got = resolve_marks(r.w, marks, binder, miss, 9000, 40, &dropped);
    REQUIRE(got.size() == 1);
    CHECK(got[0].id == 1777u);
    CHECK(dropped.empty());
    // a chest that is gone with a player near: dropped only after 30 s of misses
    remove(1777);
    miss.reset(0);
    got = resolve_marks(r.w, marks, binder, miss, 40000, 40, &dropped);
    CHECK(dropped.empty());
    got = resolve_marks(r.w, marks, binder, miss, 71000, 40, &dropped);
    REQUIRE(dropped.size() == 1);
    CHECK(dropped[0].why.find("gone") != std::string::npos);
}
