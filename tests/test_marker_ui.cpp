// Markers (API v33): where the overlay draws world markers (the layout only; no ImGui frame).
#include <doctest.h>

#include <string>
#include <vector>

#include "core/overlay/marker_ui.h"

using namespace emf::core;

namespace {

CameraSnap cam_origin() {
    CameraSnap c{};
    c.v[0] = 1;
    c.v[5] = 1;
    c.v[10] = 1;
    c.p[0] = 9.0f / 16.0f;
    c.p[5] = 1;
    c.valid = true;
    c.frame_ms = 1000;
    return c;
}

Marker at(const std::string& id, double x, double y, double z, uint32_t flags = EMF_MARKER_WORLD | EMF_MARKER_DISTANCE) {
    Marker m;
    m.owner = "t";
    m.id = id;
    m.x = x;
    m.y = y;
    m.z = z;
    m.flags = flags;
    m.text = id;
    return m;
}

const double kPlayer[3] = {0, 0, 0};
const ImVec2 kDisplay(1920, 1080);

}  // namespace

TEST_CASE("marker layout: a marker ahead is on screen, opaque, with its text and distance") {
    const auto l = layout_markers({at("a", 0, 0, 10)}, cam_origin(), kPlayer, kDisplay);
    REQUIRE(l.size() == 1);
    CHECK_FALSE(l[0].edge);
    CHECK(l[0].x == doctest::Approx(960));
    CHECK(l[0].y == doctest::Approx(540));
    CHECK(l[0].alpha == 1.0f);
    CHECK(l[0].label);
    CHECK(l[0].text == "a");
    CHECK(l[0].dist == "10 m");
    CHECK(l[0].size == doctest::Approx(26.0f));
}

TEST_CASE("marker layout: height lifts the point; no WORLD flag, beyond max distance or behind without an arrow: left out") {
    Marker lifted = at("h", 0, 0, 10);
    lifted.height = 5;
    auto l = layout_markers({lifted}, cam_origin(), kPlayer, kDisplay);
    REQUIRE(l.size() == 1);
    CHECK(l[0].y < 540);
    CHECK(layout_markers({at("m", 0, 0, 10, EMF_MARKER_GAME_MAP)}, cam_origin(), kPlayer, kDisplay).empty());
    Marker far = at("f", 0, 0, 500);
    far.max_distance = 100;
    CHECK(layout_markers({far}, cam_origin(), kPlayer, kDisplay).empty());
    CHECK(layout_markers({at("b", 0, 0, -10)}, cam_origin(), kPlayer, kDisplay).empty());
    Marker arrow = at("b", 0, 0, -10, EMF_MARKER_WORLD | EMF_MARKER_EDGE_ARROW);
    l = layout_markers({arrow}, cam_origin(), kPlayer, kDisplay);
    REQUIRE(l.size() == 1);
    CHECK(l[0].edge);
    CHECK(l[0].y > 900);  // straight behind: at the bottom,
    CHECK(l[0].y <= 950.5f);  // above the game's hotbar (130 px at 1080 p)
    // off to the side, ahead, without an arrow: left out; with one: at the right edge
    CHECK(layout_markers({at("s", 100, 0, 10)}, cam_origin(), kPlayer, kDisplay).empty());
    l = layout_markers({at("s", 100, 0, 10, EMF_MARKER_WORLD | EMF_MARKER_EDGE_ARROW)}, cam_origin(), kPlayer, kDisplay);
    REQUIRE(l.size() == 1);
    CHECK(l[0].x > 1800);
}

TEST_CASE("marker layout: the 64 nearest get their text; drawn far to near; without a player the camera's distance") {
    std::vector<Marker> ms;
    for (int i = 0; i < 70; ++i) ms.push_back(at("m" + std::to_string(i), 0, 0, 10 + i));
    auto l = layout_markers(ms, cam_origin(), kPlayer, kDisplay);
    REQUIRE(l.size() == 70);
    int labels = 0;
    for (const auto& x : l) labels += x.label ? 1 : 0;
    CHECK(labels == 64);
    CHECK(l.front().distance > l.back().distance);  // far first
    CHECK_FALSE(l.front().label);                   // the farthest has no text
    l = layout_markers({at("a", 0, 0, 10)}, cam_origin(), nullptr, kDisplay);
    REQUIRE(l.size() == 1);
    CHECK(l[0].dist == "10 m");
}

TEST_CASE("show markers: only with a fresh camera, the game's world UI shown, no map and no console") {
    CameraSnap c = cam_origin();
    MarkerSight s;
    s.world_ui = true;
    CHECK(show_markers(false, s, c, 1100));
    CHECK_FALSE(show_markers(true, s, c, 1100));  // the console
    CHECK_FALSE(show_markers(false, s, c, 1400));  // the camera is 400 ms old
    MarkerSight map = s;
    map.map_open = true;
    CHECK_FALSE(show_markers(false, map, c, 1100));
    MarkerSight none;
    CHECK_FALSE(show_markers(false, none, c, 1100));  // a menu hides the game's world UI
    c.valid = false;
    CHECK_FALSE(show_markers(false, s, c, 1100));
}

TEST_CASE("map markers: placed on the map screen from the map's rectangle, clipped to its frame, all with text") {
    MapView v{};
    v.rect[0] = -1000;  // the whole 10 240 m map is 10 240 UI units wide here: 1 unit per metre
    v.rect[1] = -5000;
    v.rect[2] = 10240;
    v.rect[3] = 10240;
    v.clip[0] = 100;
    v.clip[1] = 50;
    v.clip[2] = 1800;
    v.clip[3] = 1000;
    v.ui_w = 1920;
    v.ui_h = 1080;
    v.ms = 1000;
    Marker in = at("in", 1500, 0, 10240 - 5500, EMF_MARKER_GAME_MAP);  // u = -1000 + 1500 = 500, v = -5000 + 5500 = 500
    Marker out = at("out", 5000, 0, 5000, EMF_MARKER_GAME_MAP);       // u = 4000: right of the frame
    Marker world_only = at("w", 1500, 0, 4740, EMF_MARKER_WORLD);
    const auto l = layout_map_markers({in, out, world_only}, v, ImVec2(1920, 1080));
    REQUIRE(l.size() == 1);
    CHECK(l[0].x == doctest::Approx(500));
    CHECK(l[0].y == doctest::Approx(500));
    CHECK(l[0].label);
    CHECK(l[0].text == "in");
    CHECK(l[0].dist.empty());  // no distance on the map
    // the display twice the UI's size: pixels double
    const auto l2 = layout_map_markers({in}, v, ImVec2(3840, 2160));
    REQUIRE(l2.size() == 1);
    CHECK(l2[0].x == doctest::Approx(1000));
    CHECK(l2[0].size == doctest::Approx(48.0f));  // 24 px at 1080 p
}

TEST_CASE("map markers show only while the map is open, its view is fresh and the console is closed") {
    MapView v{};
    v.rect[2] = v.rect[3] = 100;
    v.ui_w = 1920;
    v.ui_h = 1080;
    v.ms = 1000;
    MarkerSight s;
    s.map_open = true;
    CHECK(show_map_markers(false, s, v, 1100));
    CHECK_FALSE(show_map_markers(true, s, v, 1100));
    CHECK_FALSE(show_map_markers(false, s, v, 1400));
    CHECK_FALSE(show_map_markers(false, MarkerSight{}, v, 1100));
    MapView empty{};
    CHECK_FALSE(show_map_markers(false, s, empty, 1100));
}

TEST_CASE("marker layout (v47): a marker with a bar carries its share; one without has none (negative)") {
    Marker hp = at("hp", 0, 0, 10, EMF_MARKER_WORLD | EMF_MARKER_BAR);
    hp.bar = 0.6f;
    const auto l = layout_markers({hp, at("plain", 0, 0, 12)}, cam_origin(), kPlayer, kDisplay);
    REQUIRE(l.size() == 2);
    for (const auto& m : l) {
        if (m.text == "hp") CHECK(m.bar == doctest::Approx(0.6f));
        if (m.text == "plain") CHECK(m.bar < 0);
    }
}
