// API v33 markers through the core state: a mod's markers on a game (refused on the dedicated server), world_to_screen
// from the newest UI frame, a server's marker_send, the game's map types seen, and the client world's tick (the
// UiPlayData pointer, the local player's position, followed entities, a new world clearing received markers).
#include <windows.h>

#include <doctest.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "common/offsets.h"
#include "core/api.h"
#include "core/api_sizes.h"
#include "core/marker_capture.h"
#include "fake_world.h"

using namespace emf;
using namespace emf::core;
using emf::test::FakeWorld;
using emf::test::SingletonStore;

namespace {

constexpr uint32_t kCurrentTransform = 0x027ac564u;
constexpr double kFixed = 4294967296.0;
uint64_t g_now = 10000;
std::vector<std::string> g_lines;
std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> g_comps;

void* fake_gc(void* out, void*, uint32_t hash, uint32_t entity) {
    auto o = static_cast<uint64_t*>(out);
    o[0] = o[1] = 0;
    auto it = g_comps.find({hash, entity});
    if (it != g_comps.end()) {
        o[0] = reinterpret_cast<uint64_t>(it->second.data());
        o[1] = it->second.size();
    }
    return out;
}

void place(uint32_t entity, double x, double y, double z) {
    std::vector<uint8_t> t(0x38, 0);
    const int64_t p[3] = {static_cast<int64_t>(x * kFixed), static_cast<int64_t>(y * kFixed), static_cast<int64_t>(z * kFixed)};
    std::memcpy(t.data(), p, sizeof p);
    g_comps[{kCurrentTransform, entity}] = t;
}

EmfMarker mk(const char* id, double x = 10, double y = 0, double z = 20) {
    EmfMarker m{};
    m.size = sizeof(EmfMarker);
    m.id = id;
    m.x = x;
    m.y = y;
    m.z = z;
    m.flags = EMF_MARKER_WORLD;
    return m;
}

struct Fixture {
    CoreState state;
    FakeWorld server, client, client2;
    SingletonStore ss;
    std::vector<uint8_t> uipd = std::vector<uint8_t>(0x2000, 0);
    alignas(8) uint8_t lpd[0x40] = {};
    std::vector<std::pair<uint32_t, std::string>> sent;
    Fixture() {
        g_lines.clear();
        g_comps.clear();
        g_now = 10000;
        state.get_component = fake_gc;
        state.log_line = [](const std::string& s) { g_lines.push_back(s); };
        state.now_ms = [] { return g_now; };
        state.markers.sender = std::make_unique<MarkerSender>(
            [this](uint32_t p, const std::string& d) {
                sent.emplace_back(p, d);
                return 1u;
            },
            nullptr);
        server.put<uint8_t>(off::kKind, EMF_WORLD_SERVER);
        client.put<uint8_t>(off::kKind, EMF_WORLD_CLIENT);
        client2.put<uint8_t>(off::kKind, EMF_WORLD_CLIENT);
        ss.plant(client, "G38UiPlayData", uipd.data());
        ss.plant(client, "LocalPlayerData", lpd);
        ss.plant(client2, "G38UiPlayData", uipd.data());
        std::memcpy(lpd + off::kLocalPlayerId, "\x01\x00\x00\x00", 4);
        marker_capture_reset_for_tests();
        set_core_state(&state);
    }
    ~Fixture() {
        set_core_state(nullptr);
        marker_capture_reset_for_tests();
    }
    EmfWorld* cw() { return reinterpret_cast<EmfWorld*>(client.p()); }
    EmfWorld* cw2() { return reinterpret_cast<EmfWorld*>(client2.p()); }
};

}  // namespace

TEST_CASE("api v33: version 33, size 920, the seven marker functions set") {
    const EmfApi* api = api_table();
    CHECK(api->version >= 33u);
    CHECK(api->size >= kApiV33Size);
    CHECK(kApiV33Size == 920u);
    CHECK(api->marker_set);
    CHECK(api->marker_remove);
    CHECK(api->marker_clear);
    CHECK(api->marker_types);
    CHECK(api->world_to_screen);
    CHECK(api->marker_send);
    CHECK(api->marker_unsend);
}

TEST_CASE("marker_set: a mod's markers on a game, removed and cleared by that mod; refused on the dedicated server") {
    Fixture f;
    const EmfApi* api = api_table();
    const EmfMarker a = mk("a"), b = mk("b");
    CHECK(api->marker_set(&a));
    CHECK(api->marker_set(&b));
    auto all = f.state.markers.store.snapshot();
    REQUIRE(all.size() == 2);
    CHECK(all[0].owner == "emf_tests.exe");
    CHECK_FALSE(all[0].received);
    CHECK(api->marker_remove("a"));
    CHECK_FALSE(api->marker_remove("a"));
    CHECK(api->marker_clear() == 1u);
    const EmfMarker bad = mk("no spaces");
    CHECK_FALSE(api->marker_set(&bad));
    CHECK_FALSE(api->marker_set(&bad));
    CHECK(g_lines.size() == 1);  // the same refusal once per 10 s
    CHECK_FALSE(api->marker_set(nullptr));
    f.state.exe.server = true;
    CHECK_FALSE(api->marker_set(&a));
}

TEST_CASE("world_to_screen: false without a UI frame; the newest frame's camera when there is one") {
    Fixture f;
    float x = 0, y = 0;
    CHECK_FALSE(api_table()->world_to_screen(0, 0, 10, &x, &y));
    float v[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0};
    float p[16] = {};
    p[0] = 0.5f;
    p[5] = 1;
    std::memcpy(f.uipd.data() + 0x1d20, v, sizeof v);
    std::memcpy(f.uipd.data() + 0x1d60, p, sizeof p);
    marker_capture_set_layout(0x1d20);
    markers_client_tick(f.cw());
    alignas(16) uint8_t node[0x170] = {};
    const float r[4] = {0, 0, 1920, 1080};
    std::memcpy(node + 0x114, r, sizeof r);
    markers_ui_frame(kMarkerUiSpaceHash, node, g_now);
    REQUIRE(api_table()->world_to_screen(0, 0, 10, &x, &y));
    CHECK(x == doctest::Approx(960));
    CHECK(y == doctest::Approx(540));
    CHECK_FALSE(api_table()->world_to_screen(0, 0, -10, &x, &y));
}

TEST_CASE("marker_send / marker_unsend: through the sender; refused where no world runs it") {
    Fixture f;
    const EmfMarker two[] = {mk("a"), mk("b")};
    CHECK(api_table()->marker_send(5, two, 2) == 1u);
    REQUIRE(f.sent.size() == 1);
    CHECK(f.sent[0].first == 5u);
    CHECK(api_table()->marker_unsend(5, "a") == 1u);
    CHECK(f.sent.size() == 2);
    f.state.markers.sender.reset();
    CHECK(api_table()->marker_send(5, two, 2) == 0u);
}

TEST_CASE("the client tick: the local player's position, followed entities, and a new client world clears received markers") {
    Fixture f;
    place(1, 100, 50, 200);
    place(77, 300, 60, 400);
    EmfMarker beast = mk("beast", 0, 0, 0);
    beast.entity = 77;
    REQUIRE(f.state.markers.store.set("lua", beast, true) == MarkerStore::SetResult::Ok);
    markers_client_tick(f.cw());
    double p[3];
    REQUIRE(marker_player_pos(p));
    CHECK(p[0] == doctest::Approx(100));
    CHECK(p[2] == doctest::Approx(200));
    auto all = f.state.markers.store.snapshot();
    REQUIRE(all.size() == 1);
    CHECK(all[0].x == doctest::Approx(300));
    CHECK(all[0].z == doctest::Approx(400));
    markers_client_tick(f.cw2());  // another world (a server joined or left): the received ones go
    CHECK(f.state.markers.store.size() == 0u);
}

TEST_CASE("marker_types: the types the game's own map entries used") {
    Fixture f;
    std::vector<uint8_t> entries(4 * 0xA0, 0);
    const uint32_t ent = 9, type = 0x813080bcu;
    std::memcpy(entries.data(), &ent, 4);
    std::memcpy(entries.data() + 0x30, &type, 4);
    MapList l;
    l.data = entries.data();
    l.count = 1;
    l.capacity = 4;
    f.state.markers.map.pre(&l);
    EmfMapMarkerType t[4];
    REQUIRE(api_table()->marker_types(t, 4) == 1u);
    CHECK(t[0].id == type);
    CHECK(t[0].size == sizeof(EmfMapMarkerType));
    CHECK(api_table()->marker_types(nullptr, 0) == 1u);
}

TEST_CASE("markers.test: a marker 10 m ahead of the player, in the world and on the map; refused without a player or camera") {
    Fixture f;
    std::string why;
    CHECK_FALSE(markers_test("Here", 0, &why));
    CHECK_FALSE(why.empty());
    place(1, 100, 50, 200);
    float v[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -100, -52, -190, 0};  // looking north (+z)
    float p[16] = {};
    p[0] = 0.5f;
    p[5] = 1;
    std::memcpy(f.uipd.data() + 0x1d20, v, sizeof v);
    std::memcpy(f.uipd.data() + 0x1d60, p, sizeof p);
    marker_capture_set_layout(0x1d20);
    markers_client_tick(f.cw());
    alignas(16) uint8_t node[0x170] = {};
    markers_ui_frame(kMarkerUiSpaceHash, node, g_now);
    REQUIRE(markers_test("Here", 0x813080bcu, &why));
    auto all = f.state.markers.store.snapshot();
    REQUIRE(all.size() == 1);
    CHECK(all[0].owner == "emf");
    CHECK(all[0].text == "Here");
    CHECK(all[0].x == doctest::Approx(100));
    CHECK(all[0].z == doctest::Approx(210));
    CHECK(all[0].game_icon == 0x813080bcu);
    CHECK((all[0].flags & (EMF_MARKER_WORLD | EMF_MARKER_GAME_MAP | EMF_MARKER_EDGE_ARROW | EMF_MARKER_DISTANCE)) ==
          (EMF_MARKER_WORLD | EMF_MARKER_GAME_MAP | EMF_MARKER_EDGE_ARROW | EMF_MARKER_DISTANCE));
    const auto lines = markers_status();
    REQUIRE_FALSE(lines.empty());
    CHECK(lines[0].find("1 marker") != std::string::npos);
}

TEST_CASE("the server tick: a new server world starts the sender afresh") {
    Fixture f;
    EmfMarker beast = mk("beast", 0, 0, 0);
    beast.entity = 77;
    REQUIRE(api_table()->marker_send(5, &beast, 1) == 1u);
    FakeWorld other;
    other.put<uint8_t>(off::kKind, EMF_WORLD_SERVER);
    markers_server_tick(reinterpret_cast<EmfWorld*>(f.server.p()));
    markers_server_tick(reinterpret_cast<EmfWorld*>(other.p()));  // another world: nothing of the old one is followed
    place(77, 500, 0, 500);
    f.sent.clear();
    g_now += 5000;
    markers_server_tick(reinterpret_cast<EmfWorld*>(other.p()));
    CHECK(f.sent.empty());
}

TEST_CASE("a new world: a client world's setup drops a server's markers, the map list's state and the UiPlayData; a server's restarts the sender") {
    Fixture f;
    EmfMarker r = mk("r");
    REQUIRE(f.state.markers.store.set("lua", r, true) == MarkerStore::SetResult::Ok);
    const EmfMarker own = mk("own");
    REQUIRE(api_table()->marker_set(&own));
    markers_client_tick(f.cw());
    markers_world_setup(EMF_WORLD_CLIENT);
    auto all = f.state.markers.store.snapshot();
    REQUIRE(all.size() == 1);  // the game's own mods keep theirs
    CHECK(all[0].id == "own");
    EmfMarker beast = mk("beast", 0, 0, 0);
    beast.entity = 77;
    REQUIRE(api_table()->marker_send(5, &beast, 1) == 1u);
    markers_world_setup(EMF_WORLD_SERVER);
    place(77, 500, 0, 500);
    f.sent.clear();
    g_now += 5000;
    markers_server_tick(reinterpret_cast<EmfWorld*>(f.server.p()));
    CHECK(f.sent.empty());  // nothing of the old world is followed
}

TEST_CASE("world_to_screen: false once the newest frame is older than 250 ms (a menu, a loading screen)") {
    Fixture f;
    float v[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0};
    float p[16] = {};
    p[0] = 0.5f;
    p[5] = 1;
    std::memcpy(f.uipd.data() + 0x1d20, v, sizeof v);
    std::memcpy(f.uipd.data() + 0x1d60, p, sizeof p);
    marker_capture_set_layout(0x1d20);
    markers_client_tick(f.cw());
    alignas(16) uint8_t node[0x170] = {};
    const float r[4] = {0, 0, 1920, 1080};
    std::memcpy(node + 0x114, r, sizeof r);
    markers_ui_frame(kMarkerUiSpaceHash, node, g_now);
    float x = 0, y = 0;
    CHECK(api_table()->world_to_screen(0, 0, 10, &x, &y));
    g_now += 1000;
    CHECK_FALSE(api_table()->world_to_screen(0, 0, 10, &x, &y));
}

TEST_CASE("the markers commands run on any game, also one the player joined (no world of theirs)") {
    Fixture f;
    CommandCenter center;
    REQUIRE(add_marker_commands(center));
    std::vector<std::string> replies;
    center.run_without_world("markers", [&](const std::string& l) { replies.push_back(l); }, "this is someone else's game");
    bool status = false;
    for (const auto& l : replies) status |= l.find("markers: ") != std::string::npos;
    CHECK(status);
}

TEST_CASE("markers: the console says when the game's HUD scope and its map screen were last seen") {
    Fixture f;
    const auto said = [](const std::string& part) {
        for (const auto& l : markers_status())
            if (l.find(part) != std::string::npos) return true;
        return false;
    };
    CHECK(said("the game's HUD scope (doCachedEntityHealthWidgets): never seen"));
    CHECK(said("the game's map screen (mapScaled): never seen"));
    CHECK(said("canvases and world markers draw while the HUD scope was seen in the last 0.25 s and the map screen was "
               "not"));
    g_now = 100000;
    alignas(16) uint8_t node[0x170] = {};
    markers_ui_frame(kMarkerWorldUiHash, node, g_now - 120);
    CHECK(said("the game's HUD scope (doCachedEntityHealthWidgets): seen 0.12 s ago"));
    markers_ui_frame(kMarkerMapScaledHash, node, g_now - 40000);
    g_now += 5000;
    CHECK(said("the game's HUD scope (doCachedEntityHealthWidgets): seen 5.12 s ago"));
    CHECK(said("the game's map screen (mapScaled): seen 45 s ago"));
    g_now += 600000;
    CHECK(said("the game's HUD scope (doCachedEntityHealthWidgets): seen 10 min ago"));
    markers_ui_frame(kMarkerMapScaledHash, node, g_now + 3);  // the UI thread's clock a moment ahead
    CHECK(said("the game's map screen (mapScaled): seen 0.00 s ago"));
}
