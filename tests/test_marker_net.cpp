// Markers (API v33): a server's markers for players' games over the core channel emf/markers: the sender (batches,
// the cap, removing, refreshing followed entities' positions) and the receiver (into the store as received markers).
#include <doctest.h>

#include <algorithm>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/marker_net.h"

using namespace emf::core;

namespace {

EmfMarker mk(const char* id, double x = 10, double y = 0, double z = 20, uint32_t entity = 0) {
    EmfMarker m{};
    m.size = sizeof(EmfMarker);
    m.id = id;
    m.x = x;
    m.y = y;
    m.z = z;
    m.entity = entity;
    m.flags = EMF_MARKER_WORLD;
    return m;
}

struct Sent {
    std::vector<std::pair<uint32_t, std::string>> msgs;  // player (0 = all), data
    MarkerSender::SendFn fn() {
        return [this](uint32_t player, const std::string& data) {
            msgs.emplace_back(player, data);
            return player == 0 ? 3u : 1u;  // three players have the channel
        };
    }
    size_t markers_in(size_t i) const {
        DecodedMarkers d;
        REQUIRE(decode_markers(msgs[i].second, &d));
        return d.markers.size();
    }
};

}  // namespace

TEST_CASE("marker sender: markers reach one player or every player; the count is the players reached") {
    Sent s;
    std::vector<std::string> logs;
    MarkerSender snd(s.fn(), [&](const std::string& l) { logs.push_back(l); });
    const EmfMarker two[] = {mk("a"), mk("b")};
    CHECK(snd.send("lua", 5, two, 2) == 1u);
    REQUIRE(s.msgs.size() == 1);
    CHECK(s.msgs[0].first == 5u);
    DecodedMarkers d;
    REQUIRE(decode_markers(s.msgs[0].second, &d));
    CHECK(d.owner == "lua");
    CHECK(d.markers.size() == 2);
    CHECK(snd.send("lua", 0, two, 2) == 3u);
    CHECK(s.msgs.back().first == 0u);
    CHECK(snd.any_sent());
}

TEST_CASE("marker sender: at most 256 per call, in messages of at most 64; bad ids are refused before sending") {
    Sent s;
    std::vector<std::string> logs;
    MarkerSender snd(s.fn(), [&](const std::string& l) { logs.push_back(l); });
    std::vector<std::string> ids;
    for (int i = 0; i < 300; ++i) ids.push_back("m" + std::to_string(i));
    std::vector<EmfMarker> many;
    for (const auto& id : ids) many.push_back(mk(id.c_str()));
    CHECK(snd.send("lua", 7, many.data(), static_cast<uint32_t>(many.size())) == 1u);
    size_t total = 0;
    for (size_t i = 0; i < s.msgs.size(); ++i) {
        CHECK(s.markers_in(i) <= 64u);
        total += s.markers_in(i);
    }
    CHECK(total == 256u);
    CHECK(logs.size() == 1);  // the cut is logged
    const EmfMarker bad[] = {mk("has space")};
    const size_t before = s.msgs.size();
    CHECK(snd.send("lua", 7, bad, 1) == 0u);
    CHECK(s.msgs.size() == before);
}

TEST_CASE("marker sender: unsend removes it there; a followed entity's position is refreshed only when it moved") {
    Sent s;
    MarkerSender snd(s.fn(), nullptr);
    const EmfMarker beast[] = {mk("beast", 100, 0, 100, 77)};
    REQUIRE(snd.send("lua", 5, beast, 1) == 1u);
    s.msgs.clear();
    std::map<uint32_t, std::array<double, 3>> where = {{77, {101, 0, 101}}};
    auto pos_of = [&](uint32_t e, double out[3]) {
        auto it = where.find(e);
        if (it == where.end()) return false;
        for (int i = 0; i < 3; ++i) out[i] = it->second[i];
        return true;
    };
    snd.refresh(pos_of);  // moved 1.4 m: nothing
    CHECK(s.msgs.empty());
    where[77] = {110, 0, 100};
    snd.refresh(pos_of);  // 10 m: one update
    REQUIRE(s.msgs.size() == 1);
    DecodedMarkers d;
    REQUIRE(decode_markers(s.msgs[0].second, &d));
    REQUIRE(d.positions.size() == 1);
    CHECK(d.positions[0].id == "beast");
    CHECK(d.positions[0].x == 110);
    snd.refresh(pos_of);  // not moved since
    CHECK(s.msgs.size() == 1);
    CHECK(snd.unsend("lua", 5, "beast") == 1u);
    DecodedMarkers r;
    REQUIRE(decode_markers(s.msgs.back().second, &r));
    REQUIRE(r.removes.size() == 1);
    CHECK(r.removes[0] == "beast");
    where[77] = {200, 0, 200};
    const size_t n = s.msgs.size();
    snd.refresh(pos_of);  // no longer followed
    CHECK(s.msgs.size() == n);
}

TEST_CASE("marker sender: a player who left is forgotten; an entity that is gone is not refreshed") {
    Sent s;
    MarkerSender snd(s.fn(), nullptr);
    const EmfMarker beast[] = {mk("beast", 0, 0, 0, 77)};
    snd.send("lua", 5, beast, 1);
    s.msgs.clear();
    snd.refresh([](uint32_t, double*) { return false; });
    CHECK(s.msgs.empty());
    snd.forget(5);
    snd.refresh([](uint32_t, double out[3]) {
        out[0] = out[1] = out[2] = 500;
        return true;
    });
    CHECK(s.msgs.empty());
}

TEST_CASE("marker receiver: a server's markers go into the store as received; removes and positions apply; junk is dropped") {
    MarkerStore store;
    std::vector<std::string> logs;
    MarkerReceiver rx([&](const std::string& l) { logs.push_back(l); });
    REQUIRE(rx.on_message(encode_markers("lua", {mk("a", 1, 2, 3, 77), mk("b")}, {}), store));
    auto all = store.snapshot();
    REQUIRE(all.size() == 2);
    CHECK(all[0].received);
    CHECK(all[0].owner == "lua");
    REQUIRE(rx.on_message(encode_positions("lua", {{"a", 9, 8, 7}}), store));
    for (const auto& m : store.snapshot())
        if (m.id == "a") CHECK(m.x == 9);
    REQUIRE(rx.on_message(encode_markers("lua", {}, {"b"}), store));
    CHECK(store.size() == 1);
    CHECK_FALSE(rx.on_message("{broken", store));
    CHECK_FALSE(rx.on_message("{also broken", store));
    CHECK(logs.size() == 1);  // logged once
    CHECK(store.size() == 1);
    rx.reset(store);  // the world ended or the server went away
    CHECK(store.size() == 0);
}

// A position whose place was no number read as 0: the marker jumped to the world's origin.
TEST_CASE("marker receiver: a position entry whose id is no text or whose place is not three numbers refuses the message") {
    for (const char* bad : {R"({"o":"lua","p":[["a","9",8,7]]})", R"({"o":"lua","p":[["a",9,null,7]]})",
                            R"({"o":"lua","p":[["a",9,8,{}]]})", R"({"o":"lua","p":[["a",9,8,[1]]]})",
                            R"({"o":"lua","p":[["a",9,8,true]]})", R"({"o":"lua","p":[[5,9,8,7]]})",
                            R"({"o":"lua","p":[["a",9,8,7],["a","x",8,7]]})"}) {
        CAPTURE(bad);
        DecodedMarkers d;
        CHECK_FALSE(decode_markers(bad, &d));
        MarkerStore store;
        MarkerReceiver rx(nullptr);
        REQUIRE(rx.on_message(encode_markers("lua", {mk("a", 1, 2, 3)}, {}), store));
        CHECK_FALSE(rx.on_message(bad, store));
        const auto all = store.snapshot();
        REQUIRE(all.size() == 1);
        CHECK(all[0].x == 1);  // where it was
        CHECK(all[0].y == 2);
        CHECK(all[0].z == 3);
    }
    DecodedMarkers ok;
    REQUIRE(decode_markers(R"({"o":"lua","p":[["a",9,8,7]]})", &ok));
    REQUIRE(ok.positions.size() == 1);
    CHECK(ok.positions[0].x == 9);
}

// A marker entry's place given as no number read as 0 as well: the marker drew at the world's origin.
TEST_CASE("marker receiver: a marker entry whose place or size is given but is no number refuses the message") {
    for (const char* bad : {R"({"o":"lua","s":[{"i":"a","x":"9","y":8,"z":7}]})",
                            R"({"o":"lua","s":[{"i":"a","x":9,"y":null,"z":7}]})",
                            R"({"o":"lua","s":[{"i":"a","x":9,"y":8,"z":[7]}]})",
                            R"({"o":"lua","s":[{"i":"a","x":9,"y":8,"z":7,"h":"tall"}]})",
                            R"({"o":"lua","s":[{"i":"a","x":9,"y":8,"z":7,"d":{}}]})"}) {
        CAPTURE(bad);
        DecodedMarkers d;
        CHECK_FALSE(decode_markers(bad, &d));
    }
    DecodedMarkers ok;
    REQUIRE(decode_markers(R"({"o":"lua","s":[{"i":"a","x":9,"y":8,"z":7,"h":2}]})", &ok));
    REQUIRE(ok.markers.size() == 1);
    CHECK(ok.markers[0].marker.x == 9);
    CHECK(ok.markers[0].marker.height == 2);
}

TEST_CASE("marker sender: when a player's game links up, what that player should have is sent again") {
    Sent s;
    MarkerSender snd(s.fn(), nullptr);
    const EmfMarker ab[] = {mk("a"), mk("b", 1, 2, 3, 77)};
    snd.send("lua", 5, ab, 2);
    const EmfMarker c[] = {mk("c")};
    snd.send("lua", 0, c, 1);
    snd.send("other", 6, c, 1);  // another player's: not replayed to 5
    snd.unsend("lua", 5, "a");
    snd.refresh([](uint32_t, double out[3]) {  // b's entity moved: the replay has its new place
        out[0] = 50;
        out[1] = 0;
        out[2] = 60;
        return true;
    });
    s.msgs.clear();
    snd.replay(5);
    std::vector<std::string> ids;
    for (const auto& [p, data] : s.msgs) {
        CHECK(p == 5u);
        DecodedMarkers d;
        REQUIRE(decode_markers(data, &d));
        CHECK(d.owner == "lua");
        for (const auto& m : d.markers) {
            ids.push_back(m.id);
            if (m.id == "b") CHECK(m.marker.x == 50);
        }
    }
    std::sort(ids.begin(), ids.end());
    CHECK(ids == std::vector<std::string>{"b", "c"});
    snd.forget(5);  // gone: only what every player has is left for 5
    s.msgs.clear();
    snd.replay(5);
    REQUIRE(s.msgs.size() == 1);
    CHECK(s.markers_in(0) == 1u);
    snd.reset();
    s.msgs.clear();
    snd.replay(5);
    CHECK(s.msgs.empty());
}

TEST_CASE("marker sender: the store's checks before sending: bad numbers skipped, text cut, messages kept under 15 KB") {
    Sent s;
    std::vector<std::string> logs;
    MarkerSender snd(s.fn(), [&](const std::string& l) { logs.push_back(l); });
    EmfMarker nan = mk("nan", std::numeric_limits<double>::quiet_NaN());
    const std::string long_text(200, 'x');
    EmfMarker text = mk("text");
    text.text = long_text.c_str();
    const EmfMarker two[] = {nan, text};
    CHECK(snd.send("lua", 5, two, 2) == 1u);
    REQUIRE(s.msgs.size() == 1);
    DecodedMarkers d;
    REQUIRE(decode_markers(s.msgs[0].second, &d));
    REQUIRE(d.markers.size() == 1);  // the NaN one is not sent
    CHECK(d.markers[0].text.size() == 64u);
    // 256 markers with the longest ids and texts: every message fits the channel
    std::vector<std::string> ids;
    for (int i = 0; i < 256; ++i) ids.push_back(std::string(44, 'a') + std::to_string(1000 + i));
    const std::string t64(64, 'y');
    std::vector<EmfMarker> many;
    for (const auto& id : ids) {
        EmfMarker m = mk(id.c_str(), 1234.567891234, 345.678912345, 6789.12345678);
        m.text = t64.c_str();
        m.max_distance = 123.456f;
        m.height = 2.345f;
        many.push_back(m);
    }
    s.msgs.clear();
    CHECK(snd.send("lua", 5, many.data(), 256) == 1u);
    size_t total = 0;
    for (const auto& [p, data] : s.msgs) {
        CHECK(data.size() <= 15000u);
        DecodedMarkers x;
        REQUIRE(decode_markers(data, &x));
        total += x.markers.size();
    }
    CHECK(total == 256u);
}
