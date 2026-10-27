// villager_jobs mod: target pick, craftable recipes, the job book and the assignment flow (pure logic).
#include <doctest.h>

#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "../mods/villager_jobs/jobs_logic.h"

using namespace jobs;

namespace {
const Quat kIdentity{0, 0, 0, 1};
const Vec3 kOrigin{0, 0, 0};
Quat yaw(double degrees) {  // rotation about +y
    const double h = degrees * 3.14159265358979323846 / 360.0;
    return Quat{0, std::sin(h), 0, std::cos(h)};
}
}  // namespace

TEST_CASE("jobs: job.recall sends a villager beside its job's chest, else its flame altar, else the player") {
    const Vec3 chest{10, 5, 20}, altar{100, 6, 200}, player{-3, 4, 7};
    auto t = recall_target(chest, altar, player);
    REQUIRE(t);
    CHECK(t->x == 11.5);  // 1.5 m beside, the ground search finds the floor
    CHECK(t->y == 5);
    CHECK(t->z == 20);
    t = recall_target(std::nullopt, altar, player);
    REQUIRE(t);
    CHECK(t->x == 101.5);
    t = recall_target(std::nullopt, std::nullopt, player);
    REQUIRE(t);
    CHECK(t->x == -1.5);
    CHECK_FALSE(recall_target(std::nullopt, std::nullopt, std::nullopt));
}

TEST_CASE("jobs: forward is the camera orientation applied to +z") {
    const Vec3 f = forward(kIdentity);
    CHECK(f.z == doctest::Approx(1.0));
    const Vec3 r = forward(yaw(90));
    CHECK(r.x == doctest::Approx(1.0));
    CHECK(r.z == doctest::Approx(0.0).epsilon(1e-9));
}

TEST_CASE("jobs: pick_target takes the candidate closest to the camera ray, in front, near the player") {
    const std::vector<Candidate> c = {
        {1025, {0.0, 0.0, 5.0}, Kind::Villager},
        {6830, {0.5, 0.0, 5.0}, Kind::Chest},
        {7000, {0.0, 0.0, -4.0}, Kind::Villager},  // behind the camera
        {7001, {3.0, 0.0, 5.0}, Kind::Chest},      // too far from the ray
    };
    auto t = pick_target(kOrigin, kIdentity, kOrigin, c);
    REQUIRE(t.has_value());
    CHECK(t->id == 1025);
    CHECK(t->kind == Kind::Villager);

    auto right = pick_target(kOrigin, yaw(90), kOrigin, {{6830, {5.0, 0.2, 0.0}, Kind::Chest}, {1025, {0.0, 0.0, 5.0}, Kind::Villager}});
    REQUIRE(right.has_value());
    CHECK(right->id == 6830);

    CHECK_FALSE(pick_target(kOrigin, kIdentity, kOrigin, {{7000, {0, 0, -4}, Kind::Villager}}).has_value());
    CHECK_FALSE(pick_target(kOrigin, kIdentity, kOrigin, {{7001, {3, 0, 5}, Kind::Chest}}).has_value());
    CHECK_FALSE(pick_target(kOrigin, kIdentity, kOrigin, {{7002, {0, 0, 20}, Kind::Chest}}).has_value());  // beyond 8 m
    CHECK_FALSE(pick_target(kOrigin, kIdentity, kOrigin, {}).has_value());
    // two on the ray: the nearer one along it wins
    auto nearer = pick_target(kOrigin, kIdentity, kOrigin, {{1, {0, 0, 6}, Kind::Chest}, {2, {0, 0, 3}, Kind::Chest}});
    REQUIRE(nearer.has_value());
    CHECK(nearer->id == 2);
}

TEST_CASE("jobs: a recipe is craftable when the chest holds every input item, and it has no category input") {
    const Recipe mace{0xf94576e0, {{0x2b0cf1d5, 6}, {0x25a43c2f, 1}}, {{0x3a67584f, 1}}, false, 0};
    Contents c{{0x2b0cf1d5, 6}, {0x25a43c2f, 1}};
    CHECK(craftable(mace, c));
    c[0x2b0cf1d5] = 5;
    CHECK_FALSE(craftable(mace, c));  // one log short
    c.erase(0x25a43c2f);
    c[0x2b0cf1d5] = 60;
    CHECK_FALSE(craftable(mace, c));  // no stone
    Recipe torch{0x7cd80a04, {{0x11, 1}}, {{0x22, 1}}, true, 0};
    CHECK_FALSE(craftable(torch, {{0x11, 5}}));  // has a category input ("any ...")
    Recipe nothing_out{1, {{0x11, 1}}, {}, false, 0};
    CHECK_FALSE(craftable(nothing_out, {{0x11, 5}}));
}

namespace {
struct World {
    std::map<uint32_t, Vec3> pos;
    std::map<uint32_t, Kind> kind;     // not listed: a villager below 6000, a chest above (the ids the tests use)
    std::map<uint32_t, uint32_t> npc;  // a villager's npcId (0 if not listed)
    LookupFn fn() {
        return [this](uint32_t id, Kind want) -> std::optional<EntityInfo> {
            auto it = pos.find(id);
            if (it == pos.end()) return std::nullopt;
            auto k = kind.find(id);
            const Kind is = k != kind.end() ? k->second : (id < 6000 ? Kind::Villager : Kind::Chest);
            if (is != want) return std::nullopt;
            auto n = npc.find(id);
            return EntityInfo{it->second, n != npc.end() ? n->second : 0u};
        };
    }
};
size_t count(const std::vector<Action>& a, ActionKind k) {
    size_t n = 0;
    for (const auto& x : a) n += x.kind == k;
    return n;
}
}  // namespace

TEST_CASE("jobs: a villager far from its chest is sent there, at most every 10 s; near it, it crafts every interval") {
    JobBook book;
    World w;
    w.pos[1025] = {10, 0, 0};
    w.pos[6830] = {0, 0, 0};
    REQUIRE(book.assign({1025, 6830, 0xf94576e0}, 1000, 30000));
    auto a = book.tick(1000, w.fn());
    CHECK(count(a, ActionKind::GoTo) == 1);
    CHECK(count(a, ActionKind::Craft) == 0);  // too far to work
    CHECK(count(book.tick(5000, w.fn()), ActionKind::GoTo) == 0);   // not again within 10 s
    CHECK(count(book.tick(11000, w.fn()), ActionKind::GoTo) == 1);
    w.pos[1025] = {3, 0, 0};  // arrived (within 4 m: no more walking, within 6 m: works)
    a = book.tick(12000, w.fn());
    CHECK(count(a, ActionKind::GoTo) == 0);
    REQUIRE(count(a, ActionKind::Craft) == 1);
    CHECK(a[0].job.chest == 6830);
    book.on_output(6830, 0x99, 12100);  // the product came (a job without timing takes any item)
    CHECK(count(book.tick(20000, w.fn()), ActionKind::Craft) == 0);  // next one after the interval
    CHECK(count(book.tick(42000, w.fn()), ActionKind::Craft) == 1);
}

TEST_CASE("jobs: no product within 3 s means waiting for materials: one notice, a retry each interval, back to work") {
    JobBook book;
    World w;
    w.pos[1025] = {1, 0, 0};
    w.pos[6830] = {0, 0, 0};
    book.assign({1025, 6830, 0xf94576e0}, 0, 30000);
    CHECK(count(book.tick(0, w.fn()), ActionKind::Craft) == 1);
    auto a = book.tick(3500, w.fn());  // nothing came
    CHECK(count(a, ActionKind::Notice) == 1);
    CHECK(book.find(1025)->state == JobState::Waiting);
    CHECK(count(book.tick(30000, w.fn()), ActionKind::Craft) == 1);  // retry
    a = book.tick(33500, w.fn());
    CHECK(count(a, ActionKind::Notice) == 0);  // said once only
    CHECK(count(book.tick(60000, w.fn()), ActionKind::Craft) == 1);
    book.on_output(6830, 0x99, 60100);
    CHECK(book.find(1025)->state == JobState::Working);
    CHECK(book.find(1025)->made == 1);
}

TEST_CASE("jobs: a job whose villager or chest is gone is dropped with a notice; stop removes; assign replaces") {
    JobBook book;
    World w;
    w.pos[1025] = {1, 0, 0};
    w.pos[1026] = {1, 0, 0};
    w.pos[6830] = {0, 0, 0};
    book.assign({1025, 6830, 1}, 0, 30000);
    book.assign({1026, 9999, 2}, 0, 30000);  // its chest does not exist
    book.changed();
    auto a = book.tick(0, w.fn());
    CHECK(count(a, ActionKind::Dropped) == 1);
    CHECK(book.find(1026) == nullptr);
    CHECK(book.changed());
    book.assign({1025, 6830, 3}, 100, 30000);  // a new job for the same villager replaces the old
    REQUIRE(book.jobs().size() == 1);
    CHECK(book.jobs()[0].recipe == 3);
    CHECK(book.stop(1025));
    CHECK_FALSE(book.stop(1025));
    CHECK(book.jobs().empty());
}

TEST_CASE("jobs: saved as one line per job; bad lines are skipped") {
    const std::vector<Job> j = {{1025, 6830, 0xf94576e0}, {1026, 7001, 0x0000000a}};
    const std::string text = serialize(j);
    CHECK(text == "1025 6830 f94576e0\n1026 7001 0000000a\n");
    const auto back = parse(text + "garbage\n\n12 x 7\n");
    REQUIRE(back.size() == 2);
    CHECK(back[0].villager == 1025);
    CHECK(back[0].chest == 6830);
    CHECK(back[0].recipe == 0xf94576e0);
    CHECK(back[1].recipe == 0xa);
}

TEST_CASE("jobs: assigning: villager, then chest (within 30 s); a working villager offers to stop") {
    Assigner as("K");
    const Candidate villager{1025, {}, Kind::Villager}, chest{6830, {}, Kind::Chest};
    auto r = as.press(std::nullopt, false, 0);
    CHECK(r.kind == Assigner::Result::Notice);
    CHECK(r.text.find("villager") != std::string::npos);
    r = as.press(chest, false, 0);  // chest first: not yet
    CHECK(r.kind == Assigner::Result::Notice);
    r = as.press(villager, false, 1000);
    CHECK(r.kind == Assigner::Result::Notice);
    CHECK(r.text.find("chest") != std::string::npos);
    CHECK(as.step(1000) == Assigner::Step::VillagerChosen);
    r = as.press(chest, false, 5000);
    CHECK(r.kind == Assigner::Result::OpenJobWheel);
    CHECK(r.villager == 1025);
    CHECK(r.chest == 6830);
    as.reset();
    as.press(villager, false, 0);
    CHECK(as.step(31000) == Assigner::Step::Idle);  // expired
    CHECK(as.press(chest, false, 31000).kind == Assigner::Result::Notice);
    r = as.press(villager, true, 40000);  // already working
    CHECK(r.kind == Assigner::Result::OpenStopWheel);
    CHECK(r.villager == 1025);
}

// ---- game data readers (fake game memory) ----
#include <cstring>

#include "../mods/villager_jobs/game_data.h"

namespace {
template <class T>
void put(std::vector<uint8_t>& b, size_t off, T v) {
    std::memcpy(b.data() + off, &v, sizeof(T));
}
// A RecipeInfo blob (0xA8 bytes) followed by its input (48 B each) and output (24 B each) arrays.
std::vector<uint8_t> recipe_blob(uint32_t id, const std::vector<jobs::Stack>& in, const std::vector<jobs::Stack>& out,
                                 int64_t duration_ns) {
    std::vector<uint8_t> b(0xA8 + in.size() * 48 + out.size() * 24, 0);
    put<uint32_t>(b, 0x10, id);
    const size_t in_at = 0xA8, out_at = 0xA8 + in.size() * 48;
    put<int32_t>(b, 0x28, static_cast<int32_t>(in_at - 0x28));
    put<uint32_t>(b, 0x2C, static_cast<uint32_t>(in.size()));
    put<int32_t>(b, 0x30, static_cast<int32_t>(out_at - 0x30));
    put<uint32_t>(b, 0x34, static_cast<uint32_t>(out.size()));
    put<int64_t>(b, 0x38, duration_ns);
    for (size_t i = 0; i < in.size(); ++i) {
        put<uint32_t>(b, in_at + i * 48 + 0x10, in[i].item);
        put<uint32_t>(b, in_at + i * 48 + 0x14, in[i].count);
    }
    for (size_t i = 0; i < out.size(); ++i) {
        put<uint32_t>(b, out_at + i * 24 + 0x10, out[i].item);
        put<uint32_t>(b, out_at + i * 24 + 0x14, out[i].count);
    }
    return b;
}
}  // namespace

TEST_CASE("jobs: a workshop's recipes come from the RecipeRegistry map at +0xF8") {
    auto mace = recipe_blob(0xf94576e0, {{0x2b0cf1d5, 6}, {0x25a43c2f, 1}}, {{0x3a67584f, 1}}, 0);
    auto torch = recipe_blob(0x7cd80a04, {{0x11, 1}, {0, 1}}, {{0x22, 1}}, 5000000000ll);  // a category input
    put<uint64_t>(torch, 0x48, 0x30);  // requiredPropsMask
    std::vector<uint8_t> list(0x1008, 0);  // SmallDynamicArray<RecipeInfo*, 512>
    put<const void*>(list, 0, mace.data());
    put<const void*>(list, 8, torch.data());
    put<uint16_t>(list, 0x1000, 2);
    std::vector<uint8_t> values(8 * 0x1008, 0);
    const uint32_t workshop = 0x1234;
    const size_t slot = workshop & 7;
    std::memcpy(values.data() + slot * 0x1008, list.data(), list.size());
    uint64_t bits = 1ull << slot;
    uint32_t keys[8] = {};
    keys[slot] = workshop;
    std::vector<uint8_t> reg(0x200, 0);
    put<const void*>(reg, 0xF8 + 0x08, &bits);
    put<uint64_t>(reg, 0xF8 + 0x10, 8);
    put<const void*>(reg, 0xF8 + 0x28, keys);
    put<const void*>(reg, 0xF8 + 0x40, values.data());

    const auto r = jobs::workshop_recipes(reg.data(), workshop, jobs::plain_copy);
    REQUIRE(r.size() == 2);
    CHECK(r[0].id == 0xf94576e0);
    REQUIRE(r[0].inputs.size() == 2);
    CHECK(r[0].inputs[0].item == 0x2b0cf1d5);
    CHECK(r[0].inputs[0].count == 6);
    REQUIRE(r[0].outputs.size() == 1);
    CHECK(r[0].outputs[0].item == 0x3a67584f);
    CHECK_FALSE(r[0].category_input);
    CHECK(r[1].category_input);
    CHECK(r[1].duration_ns == 5000000000ll);
    CHECK(r[0].required_props == 0);
    CHECK(r[1].required_props == 0x30);
    CHECK(jobs::workshop_recipes(reg.data(), 0x9999, jobs::plain_copy).empty());  // no such workshop
    CHECK(jobs::workshop_recipes(nullptr, workshop, jobs::plain_copy).empty());
}

TEST_CASE("jobs: a chest's contents are summed over its own and its linked inventories") {
    std::vector<uint8_t> setup(84, 0);  // InventorySetup: linksEntities[16] first
    put<uint32_t>(setup, 0, 500);
    put<uint32_t>(setup, 4, 501);
    std::vector<uint8_t> self(96, 0), a(96, 0), b(96, 0);  // Inventory: 8 x {item, count, pide}
    put<uint32_t>(self, 0, 0x2b0cf1d5);
    put<uint32_t>(self, 4, 4);
    put<uint32_t>(a, 12, 0x2b0cf1d5);
    put<uint32_t>(a, 16, 3);
    put<uint32_t>(b, 24, 0x25a43c2f);
    put<uint32_t>(b, 28, 1);
    put<uint32_t>(b, 36, 0x99);  // an empty count: left out
    auto inv = [&](uint32_t id) -> const uint8_t* {
        if (id == 6830) return self.data();
        if (id == 500) return a.data();
        if (id == 501) return b.data();
        return nullptr;
    };
    const auto c = jobs::chest_contents(setup.data(), 6830, inv, jobs::plain_copy);
    CHECK(c.size() == 2);
    CHECK(c.at(0x2b0cf1d5) == 7);
    CHECK(c.at(0x25a43c2f) == 1);
}

TEST_CASE("jobs: item debug names read as words in the wheel") {
    CHECK(jobs::pretty_item("Material_T1_Wood_Logs") == "Wood Logs");
    CHECK(jobs::pretty_item("Weapon_T0_1H_Mace_Club_Craft") == "1H Mace Club Craft");
    CHECK(jobs::pretty_item("Ammo_T0_Arrow_Wooden_Grassland_lvl3") == "Arrow Wooden Grassland lvl3");
    CHECK(jobs::pretty_item("Shield_T0_Wooden") == "Shield Wooden");  // a lone word after the tier keeps the category
    CHECK(jobs::pretty_item("Torch") == "Torch");
    CHECK(jobs::pretty_item("") == "");
}

TEST_CASE("jobs: a job whose saved ids now name other kinds of entities (after a reload) is dropped") {
    JobBook book;
    World w;
    w.pos[1025] = {1, 0, 0};
    w.pos[6830] = {0, 0, 0};
    w.kind[6830] = Kind::Villager;  // the id of the old chest went to someone else
    book.assign({1025, 6830, 1}, 0, 30000);
    auto a = book.tick(0, w.fn());
    REQUIRE(count(a, ActionKind::Dropped) == 1);
    CHECK(a[0].text.find("chest 6830") != std::string::npos);
    CHECK(count(a, ActionKind::Craft) == 0);
    CHECK(book.jobs().empty());
}

TEST_CASE("jobs: right after a load a job whose villager or chest is not there yet is kept until the grace ends") {
    JobBook book;
    World w;
    w.pos[1025] = {1, 0, 0};
    book.assign({1025, 6830, 1}, 0, 30000);
    book.hold_drops_until(60000);
    book.changed();
    auto a = book.tick(1000, w.fn());  // the chest is not loaded yet
    CHECK(a.empty());
    CHECK(book.jobs().size() == 1);
    CHECK_FALSE(book.changed());  // nothing to save
    w.pos[6830] = {0, 0, 0};  // there now: the job runs
    CHECK(count(book.tick(2000, w.fn()), ActionKind::Craft) == 1);
    w.pos.erase(6830);
    a = book.tick(61000, w.fn());  // gone after the grace: dropped
    CHECK(count(a, ActionKind::Dropped) == 1);
    CHECK(book.jobs().empty());
}

TEST_CASE("jobs: a saved job whose chest is elsewhere or whose villager is another NPC is dropped") {
    JobBook book;
    World w;
    w.pos[1025] = {1, 0, 0};
    w.npc[1025] = 77;
    w.pos[6830] = {5, 0, 0};  // a chest, but not where the chosen one stood
    Job moved{1025, 6830, 1};
    moved.npc = 77;
    moved.has_place = true;
    moved.place = {0, 0, 0};
    book.assign(moved, 0, 30000);
    auto a = book.tick(0, w.fn());
    REQUIRE(count(a, ActionKind::Dropped) == 1);
    CHECK(a[0].text.find("chest 6830") != std::string::npos);
    CHECK(count(a, ActionKind::Craft) == 0);

    w.pos[6830] = {0, 0, 0.3};  // within 0.5 m: the same chest
    w.npc[1025] = 88;           // but the villager id now names another NPC
    book.assign(moved, 0, 30000);
    a = book.tick(0, w.fn());
    REQUIRE(count(a, ActionKind::Dropped) == 1);
    CHECK(a[0].text.find("villager 1025") != std::string::npos);
    CHECK(book.jobs().empty());
}

TEST_CASE("jobs: an old-format job learns its villager's NPC and its chest's place on the first tick") {
    const auto old = parse("1025 6830 f94576e0\n");
    REQUIRE(old.size() == 1);
    CHECK(old[0].npc == 0);
    CHECK_FALSE(old[0].has_place);
    JobBook book;
    World w;
    w.pos[1025] = {1, 0, 0};
    w.npc[1025] = 5;
    w.pos[6830] = {1, 2, 3};
    book.assign(old[0], 0, 30000);
    book.changed();
    book.tick(0, w.fn());
    REQUIRE(book.jobs().size() == 1);
    CHECK(book.jobs()[0].npc == 5);
    CHECK(book.jobs()[0].has_place);
    CHECK(book.jobs()[0].place.z == 3);
    CHECK(book.changed());  // saved again with the new fields
}

TEST_CASE("jobs: the save line carries the villager's NPC and the chest's place") {
    Job j{1025, 6830, 0xf94576e0};
    j.npc = 77;
    j.has_place = true;
    j.place = {3703.25, 905, -1171.5};
    const std::string text = serialize({j});
    CHECK(text == "1025 6830 f94576e0 77 3703.25 905.00 -1171.50\n");
    const auto back = parse(text);
    REQUIRE(back.size() == 1);
    CHECK(back[0].npc == 77);
    CHECK(back[0].has_place);
    CHECK(back[0].place.x == doctest::Approx(3703.25));
    CHECK(back[0].place.z == doctest::Approx(-1171.5));
}

TEST_CASE("jobs: only the recipe's own product confirms a craft; an unrelated item put into the chest does not") {
    JobBook book;
    World w;
    w.pos[1025] = {1, 0, 0};
    w.pos[6830] = {0, 0, 0};
    book.assign({1025, 6830, 0x7814509c}, 0, 10000, Timing{0x42f4a380, 0});
    REQUIRE(count(book.tick(0, w.fn()), ActionKind::Craft) == 1);
    book.on_output(6830, 0x25a43c2f, 500);  // the player put a stone in
    CHECK(book.find(1025)->made == 0);
    book.on_output(6830, 0x42f4a380, 600);  // the wall
    CHECK(book.find(1025)->made == 1);
    CHECK(book.find(1025)->state == JobState::Working);
}

TEST_CASE("jobs: a recipe that takes time is given that time before 'waiting', and never crafts faster than it") {
    JobBook book;
    World w;
    w.pos[1025] = {1, 0, 0};
    w.pos[6830] = {0, 0, 0};
    book.assign({1025, 6830, 1}, 0, 5000, Timing{0x22, 10000});  // a 10 s recipe, 5 s per item configured
    REQUIRE(count(book.tick(0, w.fn()), ActionKind::Craft) == 1);
    CHECK(count(book.tick(11000, w.fn()), ActionKind::Notice) == 0);  // 10 s + 2 s to arrive
    CHECK(count(book.tick(12500, w.fn()), ActionKind::Notice) == 1);
    CHECK(book.find(1025)->interval_ms == 10000);
    Recipe slow{1, {{0x11, 1}}, {{0x22, 1}}, false, 10000000000ll};
    CHECK(job_interval(5000, slow) == 10000);
    CHECK(job_interval(30000, slow) == 30000);
    const Timing t = job_timing(slow);
    CHECK(t.output_item == 0x22);
    CHECK(t.duration_ms == 10000);
}

TEST_CASE("jobs: recipes that need station props are not offered") {
    Recipe at_station{1, {{0x11, 1}}, {{0x22, 1}}, false, 0};
    at_station.required_props = 0x4;
    CHECK_FALSE(craftable(at_station, {{0x11, 5}}));
    Recipe by_hand{2, {{0x11, 1}}, {{0x22, 1}}, false, 0};
    CHECK(craftable(by_hand, {{0x11, 5}}));
}

TEST_CASE("jobs: a job restored before its recipe could be read gets the recipe's timing later") {
    JobBook book;
    World w;
    w.pos[1025] = {1, 0, 0};
    w.pos[6830] = {0, 0, 0};
    book.assign({1025, 6830, 1}, 0, 5000);  // loaded from a save: the villager's recipes were not readable yet
    CHECK(book.find(1025)->timing.output_item == 0);
    CHECK(book.set_timing(1025, Timing{0x22, 10000}));
    CHECK(book.find(1025)->timing.output_item == 0x22);
    CHECK(book.find(1025)->interval_ms == 10000);   // never faster than the recipe
    CHECK(book.find(1025)->confirm_ms == 12000);
    REQUIRE(count(book.tick(0, w.fn()), ActionKind::Craft) == 1);
    book.on_output(6830, 0x99, 100);  // an unrelated item no longer confirms it
    CHECK(book.find(1025)->made == 0);
    CHECK_FALSE(book.set_timing(9999, Timing{0x22, 0}));  // no such job
}

TEST_CASE("jobs: the key setting takes a letter, a digit, a key name or a keyboard virtual-key code") {
    std::string name;
    CHECK(parse_key(L"K", &name) == 'K');
    CHECK(name == "K");
    CHECK(parse_key(L"k", &name) == 'K');
    CHECK(parse_key(L"7", &name) == '7');
    CHECK(parse_key(L"0x71", &name) == 0x71);  // F2
    CHECK(parse_key(L"0x05", &name) == 0);     // a mouse button: the wheel cannot see its release
    CHECK(parse_key(L"0xA0", &name) == 0);     // VK_LSHIFT: raw input reports VK_SHIFT
    CHECK(parse_key(L"F6", &name) == VK_F6);   // key names (emf/keys.hpp)
    CHECK(parse_key(L"Mouse4", &name) == 0);   // unknown names
    CHECK(parse_key(L"", &name) == 0);
}

// ---- job kinds ----
TEST_CASE("job file: farm, tend and haul lines round-trip; craft lines unchanged; unknown lines ignored") {
    Job farm{1025, 6830, 0, 77, true, {1, 2, 3}};
    farm.kind = JobKind::Farm;
    Job tend{1026, 6831, 0, 78, true, {4, 5, 6}};
    tend.kind = JobKind::Tend;
    Job haul{1027, 6832, 0, 79, true, {7, 8, 9}};
    haul.kind = JobKind::Haul;
    haul.chest2 = 6833;
    haul.has_place2 = true;
    haul.place2 = {10, 11, 12};
    Job craft{1028, 6834, 0xf94576e0, 80, true, {13, 14, 15}};
    const std::string text = serialize({farm, tend, haul, craft});
    CHECK(text.find("farm 1025 6830 77 1.00 2.00 3.00\n") != std::string::npos);
    CHECK(text.find("haul 1027 6832 79 7.00 8.00 9.00 6833 10.00 11.00 12.00\n") != std::string::npos);
    CHECK(text.find("1028 6834 f94576e0 80 13.00 14.00 15.00\n") != std::string::npos);  // the craft line's format
    const auto back = parse(text + "dance 1 2 3\n");
    REQUIRE(back.size() == 4);
    CHECK(back[0].kind == JobKind::Farm);
    CHECK(back[0].villager == 1025);
    CHECK(back[0].npc == 77);
    CHECK(back[0].place.z == 3.0);
    CHECK(back[1].kind == JobKind::Tend);
    CHECK(back[2].kind == JobKind::Haul);
    CHECK(back[2].chest2 == 6833);
    CHECK(back[2].has_place2);
    CHECK(back[2].place2.y == 11.0);
    CHECK(back[3].kind == JobKind::Craft);
    CHECK(back[3].recipe == 0xf94576e0);
    CHECK(std::string(kind_name(JobKind::Haul)) == "haul");
}

TEST_CASE("assigner: after a chest the job wheel opens; Haul then waits for a second chest (30 s, not the same chest)") {
    Assigner as("K");
    const Candidate villager{1025, {}, Kind::Villager};
    const Candidate chest{6830, {}, Kind::Chest};
    const Candidate other{6831, {}, Kind::Chest};
    as.press(villager, false, 1000);
    auto r = as.press(chest, false, 2000);
    REQUIRE(r.kind == Assigner::Result::OpenJobWheel);
    CHECK(r.villager == 1025);
    CHECK(r.chest == 6830);
    as.want_haul_target(1025, 6830, 3000);  // "Haul to another chest" was picked
    r = as.press(chest, false, 4000);
    CHECK(r.kind == Assigner::Result::Notice);  // the same chest
    r = as.press(other, false, 5000);
    REQUIRE(r.kind == Assigner::Result::AssignHaul);
    CHECK(r.villager == 1025);
    CHECK(r.chest == 6830);
    CHECK(r.chest2 == 6831);
    CHECK(as.press(other, false, 6000).kind == Assigner::Result::Notice);  // done: back to choosing a villager
    as.press(villager, false, 7000);
    as.press(chest, false, 7100);
    as.want_haul_target(1025, 6830, 7200);
    CHECK(as.press(other, false, 7200 + 31000).kind == Assigner::Result::Notice);  // too late
}

// ---- farm work ----
namespace {
struct Field {
    std::vector<Crop> crops;
    WorkView view() {
        WorkView v;
        v.crops = [this](const Vec3& c, double r) {
            std::vector<Crop> out;
            for (const auto& x : crops)
                if (length(sub(x.pos, c)) <= r) out.push_back(x);
            return out;
        };
        return v;
    }
};
const Action* first_of(const std::vector<Action>& a, ActionKind k) {
    for (const auto& x : a)
        if (x.kind == k) return &x;
    return nullptr;
}
// The work tests pin their own reach and timing (the defaults have their own test).
JobConfig test_cfg() {
    JobConfig c;
    c.work.act_radius = 3.5;
    c.work.goto_every_ms = 10000;
    c.work.walk_give_up_ms = 40000;
    return c;
}
Job farm_job() {
    Job j{1025, 6830, 0};
    j.kind = JobKind::Farm;
    return j;
}
}  // namespace

TEST_CASE("farm: walks to the nearest ripe crop, harvests within 3.5 m, re-arms only on that stage's pickup") {
    JobBook book(test_cfg());
    World w;
    Field f;
    w.pos[1025] = {0, 0, 0};
    w.pos[6830] = {0, 0, 0};
    f.crops = {{7001, 7101, {10, 0, 0}, true}, {7002, 7102, {5, 0, 0}, true}, {7003, 7103, {2, 0, 0}, false},
               {7004, 7104, {30, 0, 0}, true}};  // 7003 still growing, 7004 outside 20 m
    REQUIRE(book.assign(farm_job(), 0, 30000));
    auto a = book.tick(0, w.fn(), f.view());
    const Action* go = first_of(a, ActionKind::GoToPos);
    REQUIRE(go);
    CHECK(go->pos.x == 5);  // the nearest ripe one
    CHECK(count(a, ActionKind::Harvest) == 0);
    w.pos[1025] = {4, 0, 0};  // 1 m from it
    a = book.tick(500, w.fn(), f.view());
    const Action* h = first_of(a, ActionKind::Harvest);
    REQUIRE(h);
    CHECK(h->target == 7102);  // the stage is looted
    CHECK(h->aux == 7002);     // the root
    CHECK(count(book.tick(1000, w.fn(), f.view()), ActionKind::Harvest) == 0);  // waits for the pickup
    CHECK(book.on_pickup(1, 7102, 1100).empty());    // the player picked something from it: not ours
    CHECK(book.on_pickup(6830, 7101, 1100).empty());  // another stage
    const auto roots = book.on_pickup(6830, 7102, 1200);
    REQUIRE(roots == std::vector<uint32_t>{7002});
    CHECK(book.on_pickup(6830, 7102, 1300).empty());  // a second pickup from the same loot: once
    CHECK(book.find(1025)->work.done == 1);
}

TEST_CASE("farm: no pickup in 2.5 s skips the plant 60 s with one note; a hand harvest elsewhere re-arms nothing") {
    JobBook book(test_cfg());
    World w;
    Field f;
    w.pos[1025] = {4, 0, 0};
    w.pos[6830] = {0, 0, 0};
    f.crops = {{7002, 7102, {5, 0, 0}, true}};
    REQUIRE(book.assign(farm_job(), 0, 30000));
    REQUIRE(count(book.tick(0, w.fn(), f.view()), ActionKind::Harvest) == 1);
    auto a = book.tick(2600, w.fn(), f.view());  // nothing arrived: the chest is full?
    CHECK(count(a, ActionKind::Notice) == 1);
    CHECK(book.on_pickup(6830, 7102, 2700).empty());  // too late: no re-arm
    CHECK(count(book.tick(3000, w.fn(), f.view()), ActionKind::Harvest) == 0);   // skipped
    CHECK(count(book.tick(62000, w.fn(), f.view()), ActionKind::Harvest) == 0);  // still within 60 s of 2600
    a = book.tick(63000, w.fn(), f.view());
    CHECK(count(a, ActionKind::Harvest) == 1);  // tried again
    a = book.tick(66000, w.fn(), f.view());
    CHECK(count(a, ActionKind::Notice) == 0);  // the note was given once
}

TEST_CASE("farm: an unreachable plant is skipped after 40 s; nothing ripe sends it back to the chest") {
    JobBook book(test_cfg());
    World w;
    Field f;
    w.pos[1025] = {0, 0, 0};
    w.pos[6830] = {0, 0, 0};
    f.crops = {{7002, 7102, {15, 0, 0}, true}};
    REQUIRE(book.assign(farm_job(), 0, 30000));
    CHECK(count(book.tick(0, w.fn(), f.view()), ActionKind::GoToPos) == 1);
    CHECK(count(book.tick(5000, w.fn(), f.view()), ActionKind::GoToPos) == 0);  // again only every 10 s
    CHECK(count(book.tick(10000, w.fn(), f.view()), ActionKind::GoToPos) == 1);
    w.pos[1025] = {8, 0, 0};  // stuck halfway
    book.tick(40000, w.fn(), f.view());  // gives up on that plant
    auto a = book.tick(40500, w.fn(), f.view());
    CHECK(count(a, ActionKind::GoToPos) == 0);
    CHECK(count(a, ActionKind::GoTo) == 1);  // nothing else ripe: back to the chest
    f.crops.clear();
    w.pos[1025] = {1, 0, 0};  // at the chest
    CHECK(book.tick(60000, w.fn(), f.view()).empty());
}

// ---- tend and haul work ----
namespace {
struct Barn {
    std::vector<Animal> animals;
    std::vector<Feeder> feeders;
    WorkView view() {
        WorkView v;
        v.animals = [this](const Vec3& c, double r) {
            std::vector<Animal> out;
            for (const auto& x : animals)
                if (length(sub(x.pos, c)) <= r) out.push_back(x);
            return out;
        };
        v.feeders = [this](const Vec3& c, double r) {
            std::vector<Feeder> out;
            for (const auto& x : feeders)
                if (length(sub(x.pos, c)) <= r) out.push_back(x);
            return out;
        };
        return v;
    }
};
Job job_of(JobKind k) {
    Job j{1025, 6830, 0};
    j.kind = k;
    if (k == JobKind::Haul) j.chest2 = 6831;
    return j;
}
}  // namespace

TEST_CASE("tend: collects from an animal with produce before feeding; feeds a feeder below 5 items") {
    JobBook book(test_cfg());
    World w;
    Barn b;
    w.pos[1025] = {0, 0, 0};
    w.pos[6830] = {0, 0, 0};
    b.animals = {{7201, {6, 0, 0}, true}, {7202, {2, 0, 0}, false}};
    b.feeders = {{7301, {-3, 0, 0}, 2}, {7302, {1, 0, 0}, 9}};  // 7302 is full enough
    REQUIRE(book.assign(job_of(JobKind::Tend), 0, 30000));
    auto a = book.tick(0, w.fn(), b.view());
    REQUIRE(first_of(a, ActionKind::GoToPos));
    CHECK(first_of(a, ActionKind::GoToPos)->pos.x == 6);  // the animal with eggs first
    w.pos[1025] = {5, 0, 0};
    a = book.tick(500, w.fn(), b.view());
    REQUIRE(first_of(a, ActionKind::Collect));
    CHECK(first_of(a, ActionKind::Collect)->target == 7201);
    CHECK(book.on_pickup(6830, 7201, 600).empty());  // collected (nothing to re-arm)
    CHECK(book.find(1025)->work.done == 1);
    b.animals[0].has_produce = false;
    a = book.tick(1000, w.fn(), b.view());
    REQUIRE(first_of(a, ActionKind::GoToPos));
    CHECK(first_of(a, ActionKind::GoToPos)->pos.x == -3);  // then the low feeder
    w.pos[1025] = {-3, 0, 0};
    a = book.tick(1500, w.fn(), b.view());
    REQUIRE(first_of(a, ActionKind::Feed));
    CHECK(first_of(a, ActionKind::Feed)->target == 7301);
    CHECK(count(book.tick(2000, w.fn(), b.view()), ActionKind::Feed) == 0);  // waits for the transfer
    book.on_transfer(1025, 10, 2100);
    CHECK(book.find(1025)->work.done == 1);   // collections
    CHECK(book.find(1025)->work.fed == 10);   // items fed
    book.refuse_food(1025, 7301, 0x77, 2200);
    CHECK(book.food_refused(1025, 7301, 0x77, 2300));
    CHECK_FALSE(book.food_refused(1025, 7301, 0x78, 2300));
    CHECK_FALSE(book.food_refused(1025, 7301, 0x77, 2200 + 600000));  // 10 minutes later: try it again
    book.on_transfer(1025, 0, 2400, true);  // (a late call while nothing is awaited changes nothing)
    b.feeders[0].items = 12;
    w.pos[1025] = {1, 0, 0};
    CHECK(book.tick(3000, w.fn(), b.view()).empty());  // nothing to do, at the chest
}

TEST_CASE("tend: a feed with no food in the chest notes it once and leaves the feeder for a while") {
    JobBook book(test_cfg());
    World w;
    Barn b;
    w.pos[1025] = {3, 0, 0};
    w.pos[6830] = {0, 0, 0};
    b.feeders = {{7301, {3, 0, 0}, 0}};
    REQUIRE(book.assign(job_of(JobKind::Tend), 0, 30000));
    REQUIRE(count(book.tick(0, w.fn(), b.view()), ActionKind::Feed) == 1);
    book.on_transfer(1025, 0, 100, true);  // the chest has no animal food
    auto a = book.tick(500, w.fn(), b.view());
    CHECK(count(a, ActionKind::Notice) == 1);
    CHECK(count(a, ActionKind::Feed) == 0);
    CHECK(count(book.tick(30000, w.fn(), b.view()), ActionKind::Feed) == 0);  // left alone for 60 s
    REQUIRE(count(book.tick(61000, w.fn(), b.view()), ActionKind::Feed) == 1);
    book.on_transfer(1025, 0, 61100, true);
    CHECK(count(book.tick(61500, w.fn(), b.view()), ActionKind::Notice) == 0);  // noted once
}

TEST_CASE("haul: walks to the source, then the target, then sends Haul; nothing to move waits 15 s") {
    JobBook book(test_cfg());
    World w;
    w.pos[1025] = {0, 0, 10};
    w.pos[6830] = {0, 0, 0};   // from
    w.pos[6831] = {20, 0, 0};  // to
    REQUIRE(book.assign(job_of(JobKind::Haul), 0, 30000));
    auto a = book.tick(0, w.fn());
    REQUIRE(first_of(a, ActionKind::GoToPos));
    CHECK(first_of(a, ActionKind::GoToPos)->pos.z == 0);  // to the source first
    w.pos[1025] = {1, 0, 0};
    a = book.tick(500, w.fn());  // at the source: now to the target
    a = book.tick(1000, w.fn());
    REQUIRE(first_of(a, ActionKind::GoToPos));
    CHECK(first_of(a, ActionKind::GoToPos)->pos.x == 20);
    w.pos[1025] = {19, 0, 0};
    a = book.tick(1500, w.fn());
    REQUIRE(first_of(a, ActionKind::Haul));
    CHECK(first_of(a, ActionKind::Haul)->target == 6831);
    book.on_transfer(1025, 20, 1600);
    CHECK(book.find(1025)->work.done == 20);
    a = book.tick(2000, w.fn());
    REQUIRE(first_of(a, ActionKind::GoToPos));
    CHECK(first_of(a, ActionKind::GoToPos)->pos.x == 0);  // back to the source
    w.pos[1025] = {1, 0, 0};
    book.tick(2500, w.fn());
    w.pos[1025] = {19, 0, 0};
    REQUIRE(count(book.tick(3000, w.fn()), ActionKind::Haul) == 1);
    book.on_transfer(1025, 0, 3100, true);  // nothing to move
    w.pos[1025] = {1, 0, 0};
    book.tick(3500, w.fn());
    w.pos[1025] = {19, 0, 0};
    CHECK(count(book.tick(10000, w.fn()), ActionKind::Haul) == 0);  // waits 15 s
    w.pos[1025] = {1, 0, 0};
    book.tick(18200, w.fn());  // the wait is over: at the source, off to the target
    w.pos[1025] = {19, 0, 0};
    CHECK(count(book.tick(18700, w.fn()), ActionKind::Haul) == 1);
}

TEST_CASE("haul: a second chest that is gone after the grace ends the job with a note") {
    JobBook book(test_cfg());
    World w;
    w.pos[1025] = {0, 0, 0};
    w.pos[6830] = {0, 0, 0};
    w.pos[6831] = {20, 0, 0};
    REQUIRE(book.assign(job_of(JobKind::Haul), 0, 30000));
    book.tick(0, w.fn());
    CHECK(book.jobs()[0].has_place2);  // learnt where the target chest stands
    w.pos.erase(6831);
    book.hold_drops_until(5000);
    CHECK(count(book.tick(1000, w.fn()), ActionKind::Dropped) == 0);  // just loaded: kept
    auto a = book.tick(6000, w.fn());
    REQUIRE(count(a, ActionKind::Dropped) == 1);
    CHECK(first_of(a, ActionKind::Dropped)->text.find("chest 6831 is gone") != std::string::npos);
    CHECK(book.jobs().empty());
    Job same{1025, 6830, 0};
    same.kind = JobKind::Haul;
    same.chest2 = 6830;
    CHECK_FALSE(book.assign(same, 7000, 30000));  // from and to the same chest
}

TEST_CASE("jobs: plain contents leave out stacks with their own data entity (gear), which a move would strip") {
    std::vector<uint8_t> setup(84, 0);
    std::vector<uint8_t> self(96, 0);
    put<uint32_t>(self, 0, 0x2b0cf1d5);  // logs x4
    put<uint32_t>(self, 4, 4);
    put<uint32_t>(self, 12, 0x43108313);  // a sword with its pide
    put<uint32_t>(self, 16, 1);
    put<uint32_t>(self, 20, 6751);
    auto inv = [&](uint32_t id) -> const uint8_t* { return id == 6830 ? self.data() : nullptr; };
    const auto all = jobs::chest_contents(setup.data(), 6830, inv, jobs::plain_copy);
    CHECK(all.size() == 2);
    const auto plain = jobs::chest_contents(setup.data(), 6830, inv, jobs::plain_copy, true);
    REQUIRE(plain.size() == 1);
    CHECK(plain.at(0x2b0cf1d5) == 4);
}

TEST_CASE("work defaults: act within 6 m, walk orders every 5 s, give up after 60 s with one note") {
    JobBook book;
    World w;
    Field f;
    w.pos[1025] = {0, 0, 0};
    w.pos[6830] = {0, 0, 0};
    f.crops = {{7002, 7102, {12, 0, 0}, true}, {7003, 7103, {-12, 0, 0}, true}};
    REQUIRE(book.assign(farm_job(), 0, 30000));
    CHECK(count(book.tick(0, w.fn(), f.view()), ActionKind::GoToPos) == 1);
    CHECK(count(book.tick(2500, w.fn(), f.view()), ActionKind::GoToPos) == 0);
    CHECK(count(book.tick(5000, w.fn(), f.view()), ActionKind::GoToPos) == 1);  // villagers roam off: re-sent often
    w.pos[1025] = {6.5, 0, 0};  // 5.5 m from it: close enough
    CHECK(count(book.tick(5500, w.fn(), f.view()), ActionKind::Harvest) == 1);
    book.on_pickup(6830, 7102, 5600);
    w.pos[1025] = {0, 0, 0};
    book.tick(6000, w.fn(), f.view());  // now off to the other plant
    CHECK(count(book.tick(65000, w.fn(), f.view()), ActionKind::Notice) == 0);  // gives up at 66 s
    auto a = book.tick(66000, w.fn(), f.view());
    a = book.tick(66500, w.fn(), f.view());
    CHECK(count(a, ActionKind::Notice) == 1);  // "cannot get close"
}

// ---- farm, tend and haul: edge cases ----
TEST_CASE("farm: two farmers never harvest the same plant; one claims it, the other leaves it") {
    JobBook book(test_cfg());
    World w;
    Field f;
    w.pos[1025] = {4, 0, 0};
    w.pos[1026] = {4, 0, 1};
    w.pos[6830] = {0, 0, 0};
    f.crops = {{7002, 7102, {5, 0, 0}, true}};
    REQUIRE(book.assign(farm_job(), 0, 30000));
    Job other = farm_job();
    other.villager = 1026;
    REQUIRE(book.assign(other, 0, 30000));
    auto a = book.tick(0, w.fn(), f.view());
    CHECK(count(a, ActionKind::Harvest) == 1);  // only one loot for that stage
    CHECK(count(book.tick(500, w.fn(), f.view()), ActionKind::Harvest) == 0);
}

TEST_CASE("farm: a harvest that does not fit pauses the whole job for 60 s (a full chest refuses every plant)") {
    JobBook book(test_cfg());
    World w;
    Field f;
    w.pos[1025] = {4, 0, 0};
    w.pos[6830] = {0, 0, 0};
    f.crops = {{7002, 7102, {5, 0, 0}, true}, {7003, 7103, {4, 0, 2}, true}};
    REQUIRE(book.assign(farm_job(), 0, 30000));
    REQUIRE(count(book.tick(0, w.fn(), f.view()), ActionKind::Harvest) == 1);
    book.tick(2600, w.fn(), f.view());  // no pickup: the chest is full
    CHECK(count(book.tick(3000, w.fn(), f.view()), ActionKind::Harvest) == 0);   // not the next plant either
    CHECK(count(book.tick(60000, w.fn(), f.view()), ActionKind::Harvest) == 0);
    CHECK(count(book.tick(63000, w.fn(), f.view()), ActionKind::Harvest) == 1);  // tries again after the pause
}

TEST_CASE("haul: nothing in the source for the target waits at the source, checked there every 15 s") {
    JobBook book(test_cfg());
    World w;
    w.pos[1025] = {1, 0, 0};
    w.pos[6830] = {0, 0, 0};
    w.pos[6831] = {20, 0, 0};
    bool has = false;
    WorkView v;
    v.haul_has_work = [&has](uint32_t from, uint32_t to) { return from == 6830 && to == 6831 && has; };
    REQUIRE(book.assign(job_of(JobKind::Haul), 0, 30000));
    book.tick(0, w.fn(), v);  // at the source, nothing to carry
    auto a = book.tick(500, w.fn(), v);
    CHECK(count(a, ActionKind::GoToPos) == 0);  // stays
    has = true;
    CHECK(count(book.tick(10000, w.fn(), v), ActionKind::GoToPos) == 0);  // looks again only after 15 s
    book.tick(15500, w.fn(), v);  // something now: off to the target
    a = book.tick(16000, w.fn(), v);
    REQUIRE(first_of(a, ActionKind::GoToPos));
    CHECK(first_of(a, ActionKind::GoToPos)->pos.x == 20);
}

TEST_CASE("jobs: a transfer result never touches a farm job (a late one must not cancel a harvest's re-arm)") {
    JobBook book(test_cfg());
    World w;
    Field f;
    w.pos[1025] = {4, 0, 0};
    w.pos[6830] = {0, 0, 0};
    f.crops = {{7002, 7102, {5, 0, 0}, true}};
    REQUIRE(book.assign(farm_job(), 0, 30000));
    REQUIRE(count(book.tick(0, w.fn(), f.view()), ActionKind::Harvest) == 1);
    book.on_transfer(1025, 5, 100);  // a stray feed/haul result for this villager
    CHECK(book.on_pickup(6830, 7102, 200) == std::vector<uint32_t>{7002});  // the harvest still re-arms its plant
}

// ---- edge cases ----
TEST_CASE("haul: a chest the villager cannot reach is noted once") {
    JobBook book(test_cfg());
    World w;
    w.pos[1025] = {0, 0, 10};
    w.pos[6830] = {0, 0, 0};
    w.pos[6831] = {20, 0, 0};
    REQUIRE(book.assign(job_of(JobKind::Haul), 0, 30000));
    book.tick(0, w.fn());
    book.tick(40000, w.fn());  // still 10 m away: gives up
    auto a = book.tick(40500, w.fn());
    CHECK(count(a, ActionKind::Notice) == 1);
    book.tick(56000, w.fn());
    book.tick(96000, w.fn());  // gives up again
    CHECK(count(book.tick(96500, w.fn()), ActionKind::Notice) == 0);  // noted once
}

TEST_CASE("tend: with no animal food in the chest the feeders are left alone (noted once), animals still collected") {
    JobBook book(test_cfg());
    World w;
    Barn b;
    w.pos[1025] = {0, 0, 0};
    w.pos[6830] = {0, 0, 0};
    b.feeders = {{7301, {8, 0, 0}, 0}};
    bool food = false;
    WorkView v = b.view();
    v.has_food = [&food](uint32_t chest) { return chest == 6830 && food; };
    REQUIRE(book.assign(job_of(JobKind::Tend), 0, 30000));
    auto a = book.tick(0, w.fn(), v);
    CHECK(count(a, ActionKind::GoToPos) == 0);  // no walk to a feeder it cannot fill
    CHECK(count(a, ActionKind::Notice) == 1);
    CHECK(count(book.tick(500, w.fn(), v), ActionKind::Notice) == 0);
    food = true;
    a = book.tick(1000, w.fn(), v);
    REQUIRE(first_of(a, ActionKind::GoToPos));
    CHECK(first_of(a, ActionKind::GoToPos)->pos.x == 8);
}

TEST_CASE("assigner: pressing the key at a villager cancels the haul-target step") {
    Assigner as("K");
    const Candidate villager{1025, {}, Kind::Villager};
    const Candidate chest{6830, {}, Kind::Chest};
    const Candidate other{6831, {}, Kind::Chest};
    as.press(villager, false, 0);
    as.press(chest, false, 100);
    as.want_haul_target(1025, 6830, 200);
    auto r = as.press(villager, false, 300);
    CHECK(r.kind == Assigner::Result::Notice);
    CHECK(r.text.find("cancelled") != std::string::npos);
    CHECK(as.press(other, false, 400).kind == Assigner::Result::Notice);  // no haul assigned
}

TEST_CASE("haul: idling at an empty source never adds up to a false 'cannot get close' note") {
    JobBook book(test_cfg());
    World w;
    w.pos[1025] = {1, 0, 0};
    w.pos[6830] = {0, 0, 0};
    w.pos[6831] = {20, 0, 0};
    WorkView v;
    v.haul_has_work = [](uint32_t, uint32_t) { return false; };  // the source stays empty
    REQUIRE(book.assign(job_of(JobKind::Haul), 0, 30000));
    size_t notes = 0;
    for (uint64_t t = 0; t <= 60000; t += 500) notes += count(book.tick(t, w.fn(), v), ActionKind::Notice);
    w.pos[1025] = {8, 0, 0};  // it roamed off while waiting
    for (uint64_t t = 60500; t <= 80000; t += 500) notes += count(book.tick(t, w.fn(), v), ActionKind::Notice);  // past the idle
    CHECK(notes == 0);
}

// ---- gather ----
namespace {
struct Wilds {
    std::vector<Pickable> items;
    WorkView view() {
        WorkView v;
        v.pickables = [this](const Vec3& c, double r) {
            std::vector<Pickable> out;
            for (const auto& x : items)
                if (length(sub(x.pos, c)) <= r) out.push_back(x);
            return out;
        };
        return v;
    }
};
Job gather_job(uint32_t villager = 1025) {
    Job j{villager, 6830, 0};
    j.kind = JobKind::Gather;
    return j;
}
}  // namespace

TEST_CASE("gather: walks to the nearest pickable, loots it within reach, counts its pickup, then rests 20 s") {
    JobBook book(test_cfg());
    World w;
    Wilds f;
    w.pos[1025] = {0, 0, 0};
    w.pos[6830] = {0, 0, 0};
    f.items = {{8001, {10, 0, 0}}, {8002, {6, 0, 0}}, {8003, {25, 0, 0}}};  // 8003 outside 20 m
    REQUIRE(book.assign(gather_job(), 0, 30000));
    auto a = book.tick(0, w.fn(), f.view());
    const Action* go = first_of(a, ActionKind::GoToPos);
    REQUIRE(go);
    CHECK(go->pos.x == 6);
    w.pos[1025] = {5, 0, 0};
    a = book.tick(500, w.fn(), f.view());
    const Action* g = first_of(a, ActionKind::Gather);
    REQUIRE(g);
    CHECK(g->target == 8002);
    CHECK(count(book.tick(1000, w.fn(), f.view()), ActionKind::Gather) == 0);  // waits for the pickup
    CHECK(book.on_pickup(1, 8002, 1100).empty());     // someone else got something from it: not ours
    CHECK(book.on_pickup(6830, 8002, 1200).empty());  // ours: counted, nothing to re-arm
    CHECK(book.find(1025)->work.done == 1);
    f.items.erase(f.items.begin() + 1);  // the game removed the looted pickable
    w.pos[1025] = {9, 0, 0};             // right next to the next one
    CHECK(count(book.tick(2000, w.fn(), f.view()), ActionKind::Gather) == 0);   // resting
    CHECK(count(book.tick(21000, w.fn(), f.view()), ActionKind::Gather) == 0);  // still within 20 s of 1200
    a = book.tick(21300, w.fn(), f.view());
    g = first_of(a, ActionKind::Gather);
    REQUIRE(g);
    CHECK(g->target == 8001);
}

TEST_CASE("gather: no pickup in 2.5 s means a full chest: one note, the job pauses 60 s, nothing is skipped for good") {
    JobBook book(test_cfg());
    World w;
    Wilds f;
    w.pos[1025] = {5, 0, 0};
    w.pos[6830] = {0, 0, 0};
    f.items = {{8002, {6, 0, 0}}};
    REQUIRE(book.assign(gather_job(), 0, 30000));
    REQUIRE(count(book.tick(0, w.fn(), f.view()), ActionKind::Gather) == 1);
    auto a = book.tick(2600, w.fn(), f.view());
    CHECK(count(a, ActionKind::Notice) == 1);
    CHECK(count(book.tick(30000, w.fn(), f.view()), ActionKind::Gather) == 0);
    w.pos[1025] = {5, 0, 0};
    a = book.tick(63000, w.fn(), f.view());
    CHECK(count(a, ActionKind::Gather) == 1);  // tries again after the pause
    a = book.tick(66000, w.fn(), f.view());
    CHECK(count(a, ActionKind::Notice) == 0);  // noted once
}

TEST_CASE("gather: a pickable taken by someone else is replaced by the next nearest at once") {
    JobBook book(test_cfg());
    World w;
    Wilds f;
    w.pos[1025] = {0, 0, 0};
    w.pos[6830] = {0, 0, 0};
    f.items = {{8001, {10, 0, 0}}, {8002, {6, 0, 0}}};
    REQUIRE(book.assign(gather_job(), 0, 30000));
    REQUIRE(first_of(book.tick(0, w.fn(), f.view()), ActionKind::GoToPos)->pos.x == 6);
    f.items.erase(f.items.begin() + 1);  // a player picked 8002
    const auto a = book.tick(500, w.fn(), f.view());
    const Action* go = first_of(a, ActionKind::GoToPos);
    REQUIRE(go);
    CHECK(go->pos.x == 10);
}

TEST_CASE("gather: nothing in range sends it back to the chest; two gatherers never take the same pickable") {
    JobBook book(test_cfg());
    World w;
    Wilds f;
    w.pos[1025] = {8, 0, 0};
    w.pos[1026] = {8, 0, 1};
    w.pos[6830] = {0, 0, 0};
    REQUIRE(book.assign(gather_job(), 0, 30000));
    auto a = book.tick(0, w.fn(), f.view());
    CHECK(count(a, ActionKind::GoTo) == 1);
    CHECK(count(a, ActionKind::Gather) == 0);
    w.pos[1025] = {5, 0, 0};
    w.pos[1026] = {5, 0, 1};
    f.items = {{8002, {6, 0, 0}}};
    REQUIRE(book.assign(gather_job(1026), 0, 30000));
    a = book.tick(1000, w.fn(), f.view());
    CHECK(count(a, ActionKind::Gather) == 1);
}

TEST_CASE("job file: gather lines round-trip") {
    Job g{1029, 6835, 0, 81, true, {16, 17, 18}};
    g.kind = JobKind::Gather;
    const std::string text = serialize({g});
    CHECK(text == "gather 1029 6835 81 16.00 17.00 18.00\n");
    const auto back = parse(text);
    REQUIRE(back.size() == 1);
    CHECK(back[0].kind == JobKind::Gather);
    CHECK(back[0].npc == 81);
    CHECK(back[0].place.y == 17.0);
    CHECK(std::string(kind_name(JobKind::Gather)) == "gather");
}

TEST_CASE("work defaults: gather within 20 m of the chest, one pickable every 20 s") {
    const WorkConfig c;
    CHECK(c.gather_radius == 20.0);
    CHECK(c.gather_every_ms == 20000u);
}

TEST_CASE("gather: a looted pickable that is still there (fading out) is not taken again by any gatherer for 30 s") {
    JobBook book(test_cfg());
    World w;
    Wilds f;
    w.pos[1025] = {5, 0, 0};
    w.pos[1026] = {5, 0, 1};
    w.pos[6830] = {0, 0, 0};
    f.items = {{8002, {6, 0, 0}}};
    REQUIRE(book.assign(gather_job(), 0, 30000));
    REQUIRE(count(book.tick(0, w.fn(), f.view()), ActionKind::Gather) == 1);
    book.on_pickup(6830, 8002, 500);  // looted; the game fades it out for a while
    REQUIRE(book.assign(gather_job(1026), 600, 30000));
    CHECK(count(book.tick(1000, w.fn(), f.view()), ActionKind::Gather) == 0);   // 1026 leaves it
    CHECK(count(book.tick(21000, w.fn(), f.view()), ActionKind::Gather) == 0);  // 1025 after its rest too
    CHECK(count(book.tick(31000, w.fn(), f.view()), ActionKind::Gather) == 1);  // still there after 30 s: a new one
}

// ---- full-chest pauses and unconfirmed crafts ----
TEST_CASE("farm: after a full-chest pause the refused plant waits longer, so another plant is tried first") {
    JobBook book(test_cfg());
    World w;
    Field f;
    w.pos[1025] = {4, 0, 0};
    w.pos[6830] = {0, 0, 0};
    f.crops = {{7002, 7102, {5, 0, 0}, true}, {7003, 7103, {4, 0, 2}, true}};
    REQUIRE(book.assign(farm_job(), 0, 30000));
    auto a = book.tick(0, w.fn(), f.view());
    const Action* first = first_of(a, ActionKind::Harvest);
    REQUIRE(first);
    const uint32_t refused = first->target;
    book.tick(2600, w.fn(), f.view());  // no pickup: the chest refused it
    a = book.tick(63000, w.fn(), f.view());
    const Action* next = first_of(a, ActionKind::Harvest);
    REQUIRE(next);
    CHECK(next->target != refused);
}

TEST_CASE("gather: after a full-chest pause the refused pickable waits longer, so another one is tried first") {
    JobBook book(test_cfg());
    World w;
    Wilds f;
    w.pos[1025] = {5, 0, 0};
    w.pos[6830] = {0, 0, 0};
    f.items = {{8002, {6, 0, 0}}, {8003, {5, 0, 1}}};
    REQUIRE(book.assign(gather_job(), 0, 30000));
    auto a = book.tick(0, w.fn(), f.view());
    const Action* first = first_of(a, ActionKind::Gather);
    REQUIRE(first);
    const uint32_t refused = first->target;
    book.tick(2600, w.fn(), f.view());
    a = book.tick(63000, w.fn(), f.view());
    const Action* next = first_of(a, ActionKind::Gather);
    REQUIRE(next);
    CHECK(next->target != refused);
}

TEST_CASE("jobs: a craft whose product is unknown is not confirmed by an item looted into the chest from the world") {
    JobBook book;
    World w;
    w.pos[1025] = {0, 0, 0};
    w.pos[6830] = {1, 0, 0};
    REQUIRE(book.assign(Job{1025, 6830, 0xf94576e0}, 0, 30000));  // no timing: the product is unknown
    REQUIRE(count(book.tick(0, w.fn()), ActionKind::Craft) == 1);
    book.on_output(6830, 0x55, 500, 8002);  // a pickable a gatherer looted into the same chest
    CHECK(book.find(1025)->made == 0);
    book.on_output(6830, 0x55, 600, 0);  // the craft's product (the game reports no source)
    CHECK(book.find(1025)->made == 1);
}
