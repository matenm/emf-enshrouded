// The HUD layout model: clamps, percent and pixels, and the layout JSON file (reading what a hand edit or an old file may hold).
#include <doctest.h>

#include <cmath>
#include <string>

#include "core/hud_editor/layout.h"

using namespace emf::core::hud;

namespace {
bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }
}  // namespace

TEST_CASE("clamp_offset and clamp_scale: range, rounding to 0.01, NaN") {
    CHECK(clamp_offset(150) == 100.0f);
    CHECK(clamp_offset(-150) == -100.0f);
    CHECK(clamp_offset(2.346) == doctest::Approx(2.35f));
    CHECK(clamp_offset(std::nan("")) == 0.0f);
    CHECK(clamp_offset(INFINITY) == 0.0f);
    CHECK(clamp_scale(0.1) == 0.5f);
    CHECK(clamp_scale(3) == 2.0f);
    CHECK(clamp_scale(1.234) == doctest::Approx(1.23f));
    CHECK(clamp_scale(std::nan("")) == 1.0f);
}

TEST_CASE("percent and pixels at several resolutions") {
    CHECK(percent_to_pixels(10, 2084) == 208);
    CHECK(percent_to_pixels(5, 1920) == 96);
    CHECK(percent_to_pixels(-2.5f, 3840) == -96);
    CHECK(percent_to_pixels(1.5f, 2160) == 32);
    CHECK(percent_to_pixels(50, 2560) == 1280);
    CHECK(percent_to_pixels(-100, 1080) == -1080);
    CHECK(pixels_to_percent(96, 1920) == 5.0f);
    CHECK(pixels_to_percent(-96, 3840) == -2.5f);
    CHECK(pixels_to_percent(208, 2084) == doctest::Approx(9.98f));
    CHECK(pixels_to_percent(10, 0) == 0.0f);
    CHECK(percent_to_units(10, 2084.2f) == doctest::Approx(208.42f));
}

TEST_CASE("number_text: short numbers for files and replies") {
    CHECK(number_text(5) == "5");
    CHECK(number_text(-2.5f) == "-2.5");
    CHECK(number_text(0.1f) == "0.1");
    CHECK(number_text(1.25f) == "1.25");
    CHECK(number_text(-0.0f) == "0");
    CHECK(number_text(-0.001f) == "0");
}

TEST_CASE("layout_to_json: only changed parts, short numbers") {
    HudLayout l;
    l.name = "Default";
    l.elements[0] = {5, -2.5f, 1, false};
    l.elements[3] = {0, 0, 1, true};
    const std::string want =
        "{\n"
        "  \"format\": 1,\n"
        "  \"name\": \"Default\",\n"
        "  \"build\": 1287369,\n"
        "  \"elements\": {\n"
        "    \"playerStatusParent\": {\"x\": 5, \"y\": -2.5, \"scale\": 1, \"hidden\": false},\n"
        "    \"compassSurvivalMeterParent\": {\"x\": 0, \"y\": 0, \"scale\": 1, \"hidden\": true}\n"
        "  }\n"
        "}\n";
    CHECK(layout_to_json(l, 1287369) == want);
}

TEST_CASE("layout_to_json: an untouched layout has an empty elements object") {
    HudLayout l;
    l.name = "Default";
    const std::string text = layout_to_json(l, 1287369);
    CHECK(contains(text, "  \"elements\": {}\n"));
    CHECK(text.back() == '\n');
}

TEST_CASE("layout_from_json: round trip") {
    HudLayout l;
    l.name = "Building 2";
    l.elements[0] = {5, -2.5f, 1, false};
    l.elements[3] = {-10.25f, 3, 1.5f, true};
    l.elements[12] = {0, 0, 0.75f, true};
    const LayoutRead r = layout_from_json(layout_to_json(l, 1287369));
    REQUIRE(r.layout.has_value());
    CHECK(r.error.empty());
    CHECK(r.layout->name == l.name);
    for (size_t i = 0; i < kHudElementCount; ++i) {
        CAPTURE(i);
        CHECK(r.layout->elements[i] == l.elements[i]);
    }
    CHECK(r.notes.empty());
}

TEST_CASE("layout_from_json: unknown parts are skipped with a note") {
    const LayoutRead r = layout_from_json(R"({"format":1,"elements":{"noSuchScope":{"x":1},"bossRoot":{"y":3}}})");
    REQUIRE(r.layout.has_value());
    CHECK(r.layout->elements[8].y == 3.0f);
    REQUIRE(r.notes.size() == 1);
    CHECK(r.notes[0] == "unknown part \"noSuchScope\" skipped");
}

TEST_CASE("layout_from_json: part names, vitals_order and colours bar names in any case, as the console takes them") {
    const size_t mana = static_cast<size_t>(bar_element(kBarMana));
    for (const char* name : {"health", "Health", "HEALTH"}) {
        CAPTURE(name);
        const std::string n = name;
        const LayoutRead r = layout_from_json(
            R"({"format": 1, "elements": {"BOSSROOT": {"y": 3}, "Bar:Mana": {"y": 2}}, "vitals_order": ["Mana", ")" + n +
            R"(", "STAMINA"], "colours": {")" + n + R"(": {"Fill": "#ff4040"}, "Panel": "#20202080"}})");
        REQUIRE(r.layout);
        CHECK(r.notes.empty());
        CHECK(r.layout->elements[8].y == 3.0f);
        CHECK(r.layout->elements[mana].y == doctest::Approx(2));
        CHECK(r.layout->order == BarOrder{1, 0, 2});
        CHECK(r.layout->colours[0].part[static_cast<size_t>(BarPart::Fill)] == *parse_colour("#ff4040").colour);
        CHECK(r.layout->panel == *parse_colour("#20202080").colour);
    }
}

TEST_CASE("layout_from_json: the same bar in two spellings: the later one wins, \"game\" too") {
    const size_t fill = static_cast<size_t>(BarPart::Fill);
    LayoutRead r = layout_from_json(R"({"format": 1, "colours": {"Health": {"fill": "#ff4040"}, "health": {"fill": "game"}}})");
    REQUIRE(r.layout);
    CHECK(r.notes.empty());
    CHECK(r.layout->colours[0].part[fill] == Colour{});
    r = layout_from_json(R"({"format": 1, "colours": {"health": {"fill": "game"}, "HEALTH": {"fill": "#ff4040"}}})");
    REQUIRE(r.layout);
    CHECK(r.layout->colours[0].part[fill] == *parse_colour("#ff4040").colour);
    r = layout_from_json(R"({"format": 1, "colours": {"Panel": "#20202080", "panel": "game"}})");
    REQUIRE(r.layout);
    CHECK(r.layout->panel == Colour{});
    // review: a later spelling with a bad value leaves the game's colour, as its note says (not the earlier one)
    r = layout_from_json(R"({"format": 1, "colours": {"health": {"fill": "#ff0000"}, "Health": {"fill": "red"}}})");
    REQUIRE(r.layout);
    REQUIRE(r.notes.size() == 1u);
    CHECK(r.notes[0].find("left at the game's") != std::string::npos);
    CHECK(r.layout->colours[0].part[fill] == Colour{});
}

TEST_CASE("layout_from_json: a part that is not an object is skipped with a note") {
    const LayoutRead r = layout_from_json(R"({"format":1,"elements":{"bossRoot":5}})");
    REQUIRE(r.layout.has_value());
    REQUIRE(r.notes.size() == 1);
    CHECK(r.notes[0] == "bossRoot: not an object, skipped");
}

TEST_CASE("layout_from_json: out-of-range values are clamped with a note; wrong types keep defaults") {
    const LayoutRead r =
        layout_from_json(R"({"format":1,"elements":{"buffRow":{"x":150,"y":"5","scale":3,"hidden":1}}})");
    REQUIRE(r.layout.has_value());
    const ElementLayout& e = r.layout->elements[1];
    CHECK(e.x == 100.0f);
    CHECK(e.y == 0.0f);
    CHECK(e.scale == 2.0f);
    CHECK_FALSE(e.hidden);
    REQUIRE(r.notes.size() == 4);
    CHECK(r.notes[0] == "buffRow.x: 150 is outside -100 to 100, kept at 100");
    CHECK(r.notes[1] == "buffRow.y is not a number, left at 0");
    CHECK(r.notes[2] == "buffRow.scale: 3 is outside 0.5 to 2, kept at 2");
    CHECK(r.notes[3] == "buffRow.hidden is not true or false, left at false");
}

TEST_CASE("layout_from_json: a bar's scale in a hand-edited file is left at 1 with a note (the bars have none of their own); its move stays") {
    const size_t mana = static_cast<size_t>(bar_element(kBarMana));
    const size_t health = static_cast<size_t>(bar_element(kBarHealth));
    const LayoutRead r = layout_from_json(
        R"({"format":1,"elements":{"bar:mana":{"y":2,"scale":1.5},"bar:health":{"scale":1},"buffRow":{"scale":1.5}}})");
    REQUIRE(r.layout.has_value());
    CHECK(r.layout->elements[mana].y == 2.0f);
    CHECK(r.layout->elements[mana].scale == 1.0f);
    CHECK(r.layout->elements[health].is_default());  // stored as nothing: no "(moved)", nothing written back
    CHECK(r.layout->elements[1].scale == 1.5f);       // the other parts keep theirs
    REQUIRE(r.notes.size() == 1);
    CHECK(r.notes[0] == "bar:mana.scale: health, mana and stamina have no scale of their own (scale playerStatusParent), "
                        "left at 1");
}

TEST_CASE("layout_from_json: refuses a newer format, a missing format, a non-object, bad JSON, a file over 64 KB") {
    CHECK(layout_from_json(R"({"format":2})").error == "made by a newer EMF (format 2): update EMF");
    CHECK(layout_from_json(R"({"format":0})").error == "format 0 is not a HUD layout format");
    CHECK(layout_from_json("{}").error == "no \"format\"");
    CHECK(layout_from_json("[]").error == "not a HUD layout (the file is not a JSON object)");
    const LayoutRead bad = layout_from_json("{\"format\":1,");
    CHECK(bad.error.rfind("not JSON: ", 0) == 0);
    CHECK_FALSE(bad.layout.has_value());
    CHECK(layout_from_json(std::string(64 * 1024 + 1, ' ')).error == "larger than 64 KB");
    CHECK(layout_from_json(R"({"format":1,"elements":[]})").error == "\"elements\" is not an object");
}

TEST_CASE("layout_from_json: a BOM, a huge number and a string") {  // Review Focus 1
    const LayoutRead r = layout_from_json(
        "\xEF\xBB\xBF{\"format\":1,\"name\":\"Mine\",\"elements\":{\"journalInfo\":{\"x\":1e308,\"y\":\"-7.5\"},"
        "\"bossRoot\":{\"y\":-7.5}}}");
    REQUIRE(r.layout.has_value());
    CHECK(r.layout->name == "Mine");
    CHECK(r.layout->elements[5].x == 100.0f);
    CHECK(r.layout->elements[5].y == 0.0f);
    CHECK(r.layout->elements[8].y == -7.5f);
    CHECK(r.notes.size() == 2);
}

TEST_CASE("layout_from_json: a name a profile could not have is left empty with a note") {
    const LayoutRead r = layout_from_json(R"({"format":1,"name":"a/b"})");
    REQUIRE(r.layout.has_value());
    CHECK(r.layout->name.empty());
    REQUIRE(r.notes.size() == 1);
    CHECK(r.notes[0].rfind("name: ", 0) == 0);
}

TEST_CASE("layout_to_json/from_json: the bar order and buffs_up round trip; defaults are not written") {
    HudLayout l;
    l.name = "Bars";
    CHECK(layout_to_json(l, 1).find("vitals_order") == std::string::npos);
    CHECK(layout_to_json(l, 1).find("buffs_up") == std::string::npos);
    l.order = {1, 0, 2};
    l.buffs_up = true;
    l.elements[static_cast<size_t>(bar_element(kBarMana))].y = -3;
    const std::string j = layout_to_json(l, 1);
    CHECK(j.find("\"vitals_order\": [\"mana\", \"health\", \"stamina\"]") != std::string::npos);
    CHECK(j.find("\"buffs_up\": true") != std::string::npos);
    CHECK(j.find("\"bar:mana\"") != std::string::npos);
    const LayoutRead r = layout_from_json(j);
    REQUIRE(r.layout);
    CHECK(r.layout->order == BarOrder{1, 0, 2});
    CHECK(r.layout->buffs_up);
    CHECK(r.layout->elements[static_cast<size_t>(bar_element(kBarMana))].y == doctest::Approx(-3));
    CHECK(r.notes.empty());
}

TEST_CASE("layout_from_json: an order that is not a permutation is left at the default with a note") {  // Review Focus 3
    for (const char* bad : {R"(["mana","mana","health"])", R"(["mana","health"])", R"(["mana","health","fire"])",
                            R"("mana")", R"([1,0,2])"}) {
        CAPTURE(bad);
        const LayoutRead r =
            layout_from_json(std::string(R"({"format": 1, "elements": {"bar:mana": {"y": 2}}, "vitals_order": )") + bad + "}");
        REQUIRE(r.layout);
        CHECK(r.layout->order == kDefaultOrder);
        CHECK(r.notes.size() == 1);
        CHECK(r.layout->elements[static_cast<size_t>(bar_element(kBarMana))].y == doctest::Approx(2));  // the rest loads
    }
    const LayoutRead b = layout_from_json(R"({"format": 1, "elements": {}, "buffs_up": "yes"})");
    REQUIRE(b.layout);
    CHECK_FALSE(b.layout->buffs_up);
    CHECK(b.notes.size() == 1);
}

TEST_CASE("is_default: the order and buffs_up count") {
    HudLayout l;
    CHECK(l.is_default());
    l.order = {0, 2, 1};
    CHECK_FALSE(l.is_default());
    l.order = kDefaultOrder;
    l.buffs_up = true;
    CHECK_FALSE(l.is_default());
}

TEST_CASE("layout colours round trip") {
    HudLayout l;
    l.name = "Colours";
    l.colours[0].part[static_cast<size_t>(BarPart::Fill)] = *parse_colour("#ff4040").colour;
    l.colours[1].part[static_cast<size_t>(BarPart::Empty)] = *parse_colour("#00000000").colour;
    l.panel = *parse_colour("#20202080").colour;
    CHECK_FALSE(l.is_default());
    const std::string j = layout_to_json(l, 1287369);
    CHECK(contains(j, "\"colours\": {\"health\": {\"fill\": \"#ff4040\"}, \"mana\": {\"empty\": \"#00000000\"}, \"panel\": \"#20202080\"}"));
    const LayoutRead r = layout_from_json(j);
    REQUIRE(r.layout.has_value());
    CHECK(r.notes.empty());
    CHECK(r.layout->colours == l.colours);
    CHECK(r.layout->panel == l.panel);
    HudLayout plain;
    plain.name = "Plain";
    CHECK_FALSE(contains(layout_to_json(plain, 1287369), "colours"));
    HudLayout only_panel;
    only_panel.panel = *parse_colour("#000000").colour;
    CHECK_FALSE(only_panel.is_default());
    CHECK(contains(layout_to_json(only_panel, 1), "\"colours\": {\"panel\": \"#000000\"}"));
}

TEST_CASE("layout colours: bad values are skipped with notes, the rest loads") {  // Review Focus 4
    auto read = [](const std::string& colours) {
        return layout_from_json(std::string("{\"format\": 1, \"elements\": {\"buffRow\": {\"x\": 3}}, \"colours\": ") +
                                colours + "}");
    };
    auto has_note = [](const LayoutRead& r, const std::string& note) {
        for (const auto& n : r.notes)
            if (n == note) return true;
        return false;
    };
    struct Case {
        const char* colours;
        const char* note;
    } cases[] = {
        {"[1]", "colours is not an object, skipped"},
        {"{\"fire\": {}}", "colours: unknown bar \"fire\" skipped"},
        {"{\"health\": 3}", "colours.health: not an object, skipped"},
        {"{\"health\": {\"glow\": \"#fff000\"}}", "colours.health: unknown part \"glow\" skipped"},
        {"{\"health\": {\"fill\": \"#zz\"}}",
         "colours.health.fill: \"#zz\" is not a colour (#rrggbb or #rrggbbaa), left at the game's"},
        {"{\"panel\": 5}", "colours.panel: 5 is not a colour (#rrggbb or #rrggbbaa), left at the game's"},
    };
    for (const Case& c : cases) {
        const LayoutRead r = read(c.colours);
        REQUIRE_MESSAGE(r.layout.has_value(), c.colours);
        CHECK_MESSAGE(has_note(r, c.note), c.colours);
        CHECK_MESSAGE(r.layout->elements[1].x == 3.0f, c.colours);
    }
    const LayoutRead g = read("{\"health\": {\"fill\": \"game\"}, \"panel\": \"game\"}");  // the game's: no note
    REQUIRE(g.layout.has_value());
    CHECK(g.notes.empty());
    CHECK_FALSE(g.layout->colours[0].any());
    CHECK_FALSE(g.layout->panel.set);
}

TEST_CASE("layout: a mod lines part round trips under its key") {
    const LayoutRead r = layout_from_json(
        "{\"format\": 1, \"elements\": {\"lines:bottom_left\": {\"x\": 2, \"y\": -5, \"scale\": 1.5, \"hidden\": false}}}");
    REQUIRE(r.layout.has_value());
    CHECK(r.notes.empty());
    const ElementLayout& e = r.layout->elements[static_cast<size_t>(lines_element(2))];
    CHECK(e.x == 2.0f);
    CHECK(e.y == -5.0f);
    CHECK(e.scale == 1.5f);
    const std::string j = layout_to_json(*r.layout, 1);
    CHECK(contains(j, "\"lines:bottom_left\": {\"x\": 2, \"y\": -5, \"scale\": 1.5, \"hidden\": false}"));
}
