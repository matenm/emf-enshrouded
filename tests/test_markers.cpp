// Markers (API v33, private spec 2026-11-11-markers-design): the game-free part: the store and its caps, the projection
// the game's worldToUi uses, the edge arrow, the fade, the distance text and the wire format of emf/markers.
#include <doctest.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "core/api_sizes.h"
#include "core/markers.h"

using namespace emf::core;

namespace {

EmfMarker mk(const char* id, double x = 100, double y = 50, double z = 200) {
    EmfMarker m{};
    m.size = sizeof(EmfMarker);
    m.id = id;
    m.x = x;
    m.y = y;
    m.z = z;
    m.flags = EMF_MARKER_WORLD | EMF_MARKER_DISTANCE;
    return m;
}

// A camera at the origin looking along +z (the game's view: row-vector affine, rows 0-2 the axes, row 3 the
// translation), with a projection of P[0] = 1 / aspect, P[5] = 1 (90 degrees vertical).
CameraSnap camera_at_origin(float aspect = 16.0f / 9.0f) {
    CameraSnap c{};
    c.v[0] = 1;
    c.v[5] = 1;
    c.v[10] = 1;
    c.p[0] = 1.0f / aspect;
    c.p[5] = 1;
    c.valid = true;
    c.frame_ms = 1000;
    return c;
}

}  // namespace

TEST_CASE("markers api: v33 layouts") {
    CHECK(EMF_API_VERSION >= 33);
    CHECK(sizeof(EmfMarker) == 80);
    CHECK(offsetof(EmfMarker, id) == 8);
    CHECK(offsetof(EmfMarker, x) == 16);
    CHECK(offsetof(EmfMarker, text) == 40);
    CHECK(offsetof(EmfMarker, rgba) == 56);
    CHECK(offsetof(EmfMarker, flags) == 64);
    CHECK(offsetof(EmfMarker, game_icon) == 72);
    CHECK(sizeof(EmfMapMarkerType) == 64);
    CHECK(offsetof(EmfMapMarkerType, name) == 16);
    CHECK(kApiV33Size == kApiV32Size + 7 * 8);
}

TEST_CASE("marker store: set, replace, remove and clear by owner") {
    MarkerStore s;
    CHECK(s.set("bounty", mk("target"), false) == MarkerStore::SetResult::Ok);
    CHECK(s.set("bounty", mk("target", 1, 2, 3), false) == MarkerStore::SetResult::Replaced);
    CHECK(s.set("grave", mk("target"), false) == MarkerStore::SetResult::Ok);  // another owner's id is its own
    CHECK(s.set("bounty", mk("target"), true) == MarkerStore::SetResult::Ok);   // received ones apart from local ones
    auto all = s.snapshot();
    CHECK(all.size() == 3);
    CHECK(s.remove("bounty", "target", false));
    CHECK_FALSE(s.remove("bounty", "target", false));
    CHECK(s.clear("grave") == 1);
    CHECK(s.clear_received() == 1);
    CHECK(s.snapshot().empty());
}

TEST_CASE("marker store: 256 per owner and 1024 in all, received ones counted too") {
    MarkerStore s;
    std::vector<std::string> ids;
    for (int i = 0; i < 300; ++i) ids.push_back("m" + std::to_string(i));
    for (int i = 0; i < 256; ++i) REQUIRE(s.set("a", mk(ids[i].c_str()), false) == MarkerStore::SetResult::Ok);
    CHECK(s.set("a", mk(ids[256].c_str()), false) == MarkerStore::SetResult::OwnerFull);
    CHECK(s.set("a", mk(ids[0].c_str()), false) == MarkerStore::SetResult::Replaced);  // a replace always fits
    for (int i = 0; i < 256; ++i) REQUIRE(s.set("b", mk(ids[i].c_str()), true) == MarkerStore::SetResult::Ok);
    for (int i = 0; i < 256; ++i) REQUIRE(s.set("c", mk(ids[i].c_str()), false) == MarkerStore::SetResult::Ok);
    for (int i = 0; i < 256; ++i) REQUIRE(s.set("d", mk(ids[i].c_str()), true) == MarkerStore::SetResult::Ok);
    CHECK(s.snapshot().size() == 1024);
    CHECK(s.set("e", mk("one"), false) == MarkerStore::SetResult::Full);
}

TEST_CASE("marker store: bad ids, numbers and sizes are refused; text is cut at 64 bytes on a character") {
    MarkerStore s;
    CHECK(s.set("a", mk(""), false) == MarkerStore::SetResult::BadId);
    CHECK(s.set("a", mk("has space"), false) == MarkerStore::SetResult::BadId);
    CHECK(s.set("a", mk(std::string(49, 'x').c_str()), false) == MarkerStore::SetResult::BadId);
    CHECK(s.set("a", mk("ok:script.id-1_A"), false) == MarkerStore::SetResult::Ok);
    EmfMarker nul = mk("n");
    nul.id = nullptr;
    CHECK(s.set("a", nul, false) == MarkerStore::SetResult::BadId);
    CHECK(s.set("a", mk("nan", std::numeric_limits<double>::quiet_NaN()), false) == MarkerStore::SetResult::BadNumbers);
    CHECK(s.set("a", mk("far", 1e9), false) == MarkerStore::SetResult::BadNumbers);
    EmfMarker h = mk("h");
    h.height = std::numeric_limits<float>::infinity();
    CHECK(s.set("a", h, false) == MarkerStore::SetResult::BadNumbers);
    EmfMarker sh = mk("sh");
    sh.shape = 9;
    CHECK(s.set("a", sh, false) == MarkerStore::SetResult::BadNumbers);
    EmfMarker small = mk("small");
    small.size = 8;
    CHECK(s.set("a", small, false) == MarkerStore::SetResult::BadNumbers);
    // 63 ASCII bytes and a 2-byte character: the character would end past 64, so it goes
    std::string text(63, 'a');
    text += "\xC3\xA9";
    EmfMarker t = mk("t");
    t.text = text.c_str();
    REQUIRE(s.set("a", t, false) == MarkerStore::SetResult::Ok);
    for (const auto& m : s.snapshot())
        if (m.id == "t") CHECK(m.text == std::string(63, 'a'));
}

TEST_CASE("marker store: a followed entity's position is used when known, the last one kept otherwise") {
    MarkerStore s;
    EmfMarker f = mk("beast", 10, 0, 10);
    f.entity = 77;
    REQUIRE(s.set("bm", f, true) == MarkerStore::SetResult::Ok);
    s.follow([](uint32_t e, double out[3]) {
        if (e != 77) return false;
        out[0] = 500;
        out[1] = 60;
        out[2] = 700;
        return true;
    });
    auto a = s.snapshot();
    REQUIRE(a.size() == 1);
    CHECK(a[0].x == 500);
    CHECK(a[0].z == 700);
    s.follow([](uint32_t, double*) { return false; });  // gone from the client world: the last position stays
    a = s.snapshot();
    CHECK(a[0].x == 500);
    CHECK(a[0].z == 700);
}

TEST_CASE("project: the game's worldToUi math, behind and without a projection refused") {
    const CameraSnap c = camera_at_origin();
    auto p = project(c, 0, 0, 10, 1920, 1080);
    REQUIRE(p);
    CHECK(p->x == doctest::Approx(960));
    CHECK(p->y == doctest::Approx(540));
    // 10 m ahead, 10 m up: at the top edge with a 90 degree vertical view
    p = project(c, 0, 10, 10, 1920, 1080);
    REQUIRE(p);
    CHECK(p->y == doctest::Approx(0).epsilon(0.001));
    // right is +x on the screen
    p = project(c, 5, 0, 10, 1920, 1080);
    REQUIRE(p);
    CHECK(p->x > 960);
    CHECK_FALSE(project(c, 0, 0, -10, 1920, 1080));  // behind
    CHECK_FALSE(project(c, 0, 0, 0, 1920, 1080));    // at the eye
    CameraSnap zero = c;
    std::memset(zero.p, 0, sizeof zero.p);
    CHECK_FALSE(project(zero, 0, 0, 10, 1920, 1080));
    CameraSnap invalid = c;
    invalid.valid = false;
    CHECK_FALSE(project(invalid, 0, 0, 10, 1920, 1080));
    // the translation row: a camera 100 m along +x sees (100, 0, 10) in the middle
    CameraSnap moved = c;
    moved.v[12] = -100;
    p = project(moved, 100, 0, 10, 1920, 1080);
    REQUIRE(p);
    CHECK(p->x == doctest::Approx(960));
}

TEST_CASE("edge point: on the inset rectangle toward the marker, mirrored when behind") {
    const CameraSnap c = camera_at_origin();
    // far to the right, ahead: on the right edge
    EdgePoint e = edge_point(c, 100, 0, 10, 1920, 1080, 40);
    REQUIRE(e.valid);
    CHECK(e.x == doctest::Approx(1920 - 40));
    CHECK(e.angle == doctest::Approx(0).epsilon(0.05));
    // behind and to the left: on the left side
    e = edge_point(c, -10, 0, -10, 1920, 1080, 40);
    REQUIRE(e.valid);
    CHECK(e.behind);
    CHECK(e.x == doctest::Approx(40));
    // straight behind: at the bottom
    e = edge_point(c, 0, 0, -10, 1920, 1080, 40);
    REQUIRE(e.valid);
    CHECK(e.y == doctest::Approx(1080 - 40));
    CameraSnap invalid = c;
    invalid.valid = false;
    CHECK_FALSE(edge_point(invalid, 0, 0, 10, 1920, 1080, 40).valid);
}

TEST_CASE("fade and distance text") {
    CHECK(fade(10, 0) == 1.0f);
    CHECK(fade(79, 100) == 1.0f);
    CHECK(fade(90, 100) == doctest::Approx(0.5f));
    CHECK(fade(100, 100) == 0.0f);
    CHECK(fade(150, 100) == 0.0f);
    CHECK(distance_text(0.4) == "0 m");
    CHECK(distance_text(42.4) == "42 m");
    CHECK(distance_text(999.4) == "999 m");
    CHECK(distance_text(1000) == "1.0 km");
    CHECK(distance_text(1234) == "1.2 km");
    CHECK(distance_text(std::numeric_limits<double>::quiet_NaN()).empty());
}

TEST_CASE("wire format: markers and removals round trip; broken or oversized messages are refused") {
    EmfMarker a = mk("beast.1", 1.5, 2.25, 3.125);
    a.entity = 42;
    a.text = "Wolf \"Bjorn\"";
    a.height = 2.2f;
    a.rgba = 0xffcc33ffu;
    a.shape = EMF_MARKER_SQUARE;
    a.flags = EMF_MARKER_WORLD | EMF_MARKER_GAME_MAP | EMF_MARKER_EDGE_ARROW;
    a.max_distance = 200;
    a.game_icon = 1234;
    const std::string msg = encode_markers("lua", {a}, {"old.1"});
    DecodedMarkers d;
    REQUIRE(decode_markers(msg, &d));
    CHECK(d.owner == "lua");
    REQUIRE(d.markers.size() == 1);
    const DecodedMarker& m = d.markers[0];
    CHECK(m.id == "beast.1");
    CHECK(m.text == "Wolf \"Bjorn\"");
    CHECK(m.marker.x == 1.5);
    CHECK(m.marker.y == 2.25);
    CHECK(m.marker.z == 3.125);
    CHECK(m.marker.entity == 42);
    CHECK(m.marker.height == doctest::Approx(2.2f));
    CHECK(m.marker.rgba == 0xffcc33ffu);
    CHECK(m.marker.shape == EMF_MARKER_SQUARE);
    CHECK(m.marker.flags == (EMF_MARKER_WORLD | EMF_MARKER_GAME_MAP | EMF_MARKER_EDGE_ARROW));
    CHECK(m.marker.max_distance == 200);
    CHECK(m.marker.game_icon == 1234);
    REQUIRE(d.removes.size() == 1);
    CHECK(d.removes[0] == "old.1");
    // positions only (the server's refresh of followed markers)
    const std::string pos = encode_positions("lua", {{"beast.1", 10, 20, 30}});
    DecodedMarkers dp;
    REQUIRE(decode_markers(pos, &dp));
    REQUIRE(dp.positions.size() == 1);
    CHECK(dp.positions[0].id == "beast.1");
    CHECK(dp.positions[0].z == 30);

    DecodedMarkers bad;
    CHECK_FALSE(decode_markers("{not json", &bad));
    CHECK_FALSE(decode_markers("[1,2]", &bad));
    CHECK_FALSE(decode_markers("{\"o\":\"\",\"s\":[]}", &bad));  // no owner
    // an owner name is printed (the console's markers): control characters and bidi controls are refused
    CHECK_FALSE(decode_markers(encode_markers("evil\xe2\x80\xae" "lld.dom", {mk("x")}, {}), &bad));
    CHECK_FALSE(decode_markers(encode_markers("evil\xc2\x9b[2J", {mk("x")}, {}), &bad));
    CHECK_FALSE(decode_markers("{\"o\":\"line\\nbreak\",\"s\":[]}", &bad));
    REQUIRE(decode_markers(encode_markers("My Mod.dll", {mk("x")}, {}), &bad));  // a module name with a space
    CHECK(bad.owner == "My Mod.dll");
    std::vector<EmfMarker> many(300, mk("x"));
    CHECK_FALSE(decode_markers(encode_markers("lua", many, {}), &bad));  // more than 256 in one message
}


TEST_CASE("marker store (review C1): EMF_MARKER_MINIMAP is kept, for a local and a received marker") {
    MarkerStore s;
    EmfMarker m{};
    m.size = sizeof m;
    m.id = "a";
    m.x = 1;
    m.flags = EMF_MARKER_MINIMAP | EMF_MARKER_WORLD;
    REQUIRE(s.set("mod", m, false) == MarkerStore::SetResult::Ok);
    m.id = "b";
    REQUIRE(s.set("mod", m, true) == MarkerStore::SetResult::Ok);
    const auto all = s.snapshot();
    REQUIRE(all.size() == 2u);
    for (const auto& k : all) CHECK((k.flags & EMF_MARKER_MINIMAP) != 0u);
}

TEST_CASE("marker store (v47): a bar (EMF_MARKER_BAR, 0..1) is kept; outside 0..1 or not a number refused; none without the flag") {
    MarkerStore s;
    EmfMarker m = mk("hp");
    m.flags |= EMF_MARKER_BAR;
    m.bar = 0.4f;
    REQUIRE(s.set("mod", m, false) == MarkerStore::SetResult::Ok);
    auto all = s.snapshot();
    REQUIRE(all.size() == 1u);
    CHECK((all[0].flags & EMF_MARKER_BAR) != 0u);
    CHECK(all[0].bar == doctest::Approx(0.4f));
    m.bar = 1.5f;
    CHECK(s.set("mod", m, false) == MarkerStore::SetResult::BadNumbers);
    m.bar = -0.1f;
    CHECK(s.set("mod", m, false) == MarkerStore::SetResult::BadNumbers);
    m.bar = std::nanf("");
    CHECK(s.set("mod", m, false) == MarkerStore::SetResult::BadNumbers);
    EmfMarker plain = mk("plain");
    plain.bar = 7.0f;  // no flag: the value is not read
    REQUIRE(s.set("mod", plain, false) == MarkerStore::SetResult::Ok);
    for (const auto& k : s.snapshot())
        if (k.id == "plain") CHECK((k.flags & EMF_MARKER_BAR) == 0u);
}

TEST_CASE("wire format (v47): a marker's bar goes along (\"b\"); a message without one has none; a bar given as no number is refused") {
    EmfMarker a = mk("beast.1");
    a.flags |= EMF_MARKER_BAR;
    a.bar = 0.25f;
    DecodedMarkers d;
    REQUIRE(decode_markers(encode_markers("lua", {a}, {}), &d));
    REQUIRE(d.markers.size() == 1);
    CHECK((d.markers[0].marker.flags & EMF_MARKER_BAR) != 0u);
    CHECK(d.markers[0].marker.bar == doctest::Approx(0.25f));
    DecodedMarkers n;
    REQUIRE(decode_markers(encode_markers("lua", {mk("b2")}, {}), &n));
    CHECK(n.markers[0].marker.bar == 0.0f);
    CHECK_FALSE(decode_markers(R"({"o":"lua","s":[{"i":"x","x":1,"y":2,"z":3,"f":33,"b":"full"}]})", &n));
}
