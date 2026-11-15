// Mod messages (API v27), the chat transport without the game: a ServerLink and a ClientLink joined by a fake chat. The
// handshake (beacon, hello, welcome), messages both ways (fragments, binary), forged lines, lines out of order, rate
// limits, reconnects and reused handles.
#include <doctest.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/net/net_link.h"
#include "core/net/net_service.h"

using namespace emf::core::net;

namespace {

constexpr uint64_t kAcct = 0xAABBCCDD11223344ull;
constexpr uint32_t kHandle = 0x40;

struct Got {
    uint32_t handle;
    uint64_t account;
    std::string channel;
    uint32_t version;
    std::string data;
};

struct Rig {
    uint64_t now = 1000;
    size_t room = 1000;
    bool greet = true;
    bool client_enabled = true;
    bool refuse_lines = false;  // the outbox refuses a message's lines (all of them)
    std::map<std::string, uint32_t> server_channels{{"bm/pick", 2}, {"bm/show", 1}};
    std::map<std::string, uint32_t> client_channels{{"bm/pick", 2}, {"bm/show", 1}};
    std::optional<uint64_t> self = kAcct;
    std::vector<std::pair<Connection, std::string>> to_players;  // lines the server queued
    std::vector<std::string> logs;
    std::vector<Got> at_server;
    std::vector<Got> at_client;
    std::vector<std::pair<Connection, bool>> peer_events;
    std::vector<bool> server_events;
    uint32_t local_handle = 0xFFFF;
    ServerLink server;
    ClientLink client;

    explicit Rig(bool greet_ = true) : greet(greet_), server(server_deps()), client(client_deps()) {}

    ServerLinkDeps server_deps() {
        ServerLinkDeps d;
        d.channels = [this] { return server_channels; };
        d.send = [this](uint32_t h, uint64_t a, const std::string& line) {
            to_players.push_back({{h, a}, line});
            return true;
        };
        d.send_lines = [this](uint32_t h, uint64_t a, const std::vector<std::string>& lines) {
            if (refuse_lines) return false;
            for (const auto& line : lines) to_players.push_back({{h, a}, line});
            return true;
        };
        d.room = [this] { return room; };
        d.is_local = [this](uint32_t h, uint64_t) { return h == local_handle; };
        d.now_ms = [this] { return now; };
        d.log = [this](const std::string& l) { logs.push_back(l); };
        d.on_message = [this](uint32_t h, uint64_t a, const std::string& c, uint32_t v, const std::string& data) {
            at_server.push_back({h, a, c, v, data});
        };
        d.on_peer = [this](uint32_t h, uint64_t a, bool joined) { peer_events.push_back({{h, a}, joined}); };
        d.emf_version = "0.5.1";
        d.api = 27;
        d.build = 1287369;
        d.greet = greet;
        d.beacon_delay_ms = 0;  // the tests below see the beacon on the first tick; the delay has its own test
        return d;
    }
    ClientLinkDeps client_deps() {
        ClientLinkDeps d;
        d.channels = [this] { return client_channels; };
        d.self_account = [this] { return self; };
        d.on_message = [this](const std::string& c, uint32_t v, const std::string& data) {
            at_client.push_back({0, 0, c, v, data});
        };
        d.on_server = [this](bool up) { server_events.push_back(up); };
        d.enabled = [this] { return client_enabled; };
        d.log = [this](const std::string& l) { logs.push_back(l); };
        d.emf_version = "0.5.1";
        d.api = 27;
        d.build = 1287369;
        return d;
    }

    // The server's lines for a connection go to the client (spoken as itself), and the other way round.
    void deliver_to_client(const Connection& c = {kHandle, kAcct}) {
        auto lines = std::move(to_players);
        to_players.clear();
        for (auto& [conn, line] : lines)
            if (conn.handle == c.handle && conn.account == c.account) client.on_chat_line(line, kAcct);
            else to_players.push_back({conn, line});
    }
    void deliver_to_server(const Connection& c = {kHandle, kAcct}) {
        for (const auto& line : client.take_lines(100)) server.on_line(c.handle, c.account, line);
    }
    // beacon, hello, welcome
    void join(const Connection& c = {kHandle, kAcct}) {
        server.tick({c});
        deliver_to_client(c);
        deliver_to_server(c);
        deliver_to_client(c);
    }
};

}  // namespace

TEST_CASE("net link: channel lists and hello texts round trip; bad entries are skipped") {
    const std::map<std::string, uint32_t> ch{{"a/b", 1}, {"c.d/e_f", 65535}};
    CHECK(channels_text(ch) == "a/b@1,c.d/e_f@65535");
    CHECK(parse_channels_text(channels_text(ch)) == ch);
    CHECK(parse_channels_text("a/b@0,nope@1,/x@1,a/b@70000,c/d@2,e/f@x") == std::map<std::string, uint32_t>{{"c/d", 2}});
    CHECK(parse_channels_text("").empty());
    HelloInfo h;
    h.emf_version = "0.5 1";  // not printable text for a field: cleaned
    h.api = 27;
    h.build = 1287369;
    h.channels = ch;
    const auto back = parse_hello_text(hello_text(h));
    REQUIRE(back);
    CHECK(back->protocol == 1);
    CHECK(back->emf_version == "0.5_1");
    CHECK(back->api == 27);
    CHECK(back->build == 1287369);
    CHECK(back->channels == ch);
    HelloInfo none;
    CHECK(parse_hello_text(hello_text(none))->channels.empty());
    CHECK_FALSE(parse_hello_text("hello"));
    CHECK_FALSE(parse_hello_text("p1;0.5;27"));
    CHECK_FALSE(parse_hello_text("q1;0.5;27;1;"));
    CHECK_FALSE(parse_hello_text("p1;0.5;x;1;"));
}

TEST_CASE("net link: the beacon goes once to each new remote connection, only when the server has channels") {
    Rig r;
    r.local_handle = 0x41;
    r.server.tick({{0x40, 1}, {0x41, 2}});  // the second is this process's own player
    REQUIRE(r.to_players.size() == 1);
    CHECK(r.to_players[0].first.handle == 0x40);
    const auto line = parse_line(r.to_players[0].second, nullptr);
    REQUIRE(line);
    CHECK(line->type == 'S');
    CHECK(line->channel == "emf/net");
    CHECK(line->payload.find("EMF") != std::string::npos);
    r.server.tick({{0x40, 1}, {0x41, 2}});
    CHECK(r.to_players.size() == 1);  // not again before the retry time
    r.server.tick({{0x40, 1}, {0x41, 2}, {0x42, 3}});
    CHECK(r.to_players.size() == 2);
    r.server.tick({{0x42, 3}});  // 0x40 left ...
    r.server.tick({{0x40, 1}, {0x42, 3}});  // ... and came back: a new connection, a new beacon
    CHECK(r.to_players.size() == 3);

    Rig none;
    none.server_channels.clear();
    none.server.tick({{0x40, 1}});
    CHECK(none.to_players.empty());
    Rig quiet(false);
    quiet.server.tick({{0x40, 1}});
    CHECK(quiet.to_players.empty());
}

TEST_CASE("net link: the first beacon waits a few seconds after the connection; it comes again until the game says hello, a few times at most") {
    Rig r;
    ServerLinkDeps d = r.server_deps();
    d.beacon_delay_ms = 5000;
    d.beacon_retry_ms = 15000;
    d.beacon_tries = 4;
    ServerLink server(d);
    server.tick({{kHandle, kAcct}});
    CHECK(r.to_players.empty());  // the game is still loading: a line now can be lost
    r.now += 4000;
    server.tick({{kHandle, kAcct}});
    CHECK(r.to_players.empty());
    r.now += 1000;
    server.tick({{kHandle, kAcct}});
    CHECK(r.to_players.size() == 1);  // 5 s after the connection
    r.now += 14000;
    server.tick({{kHandle, kAcct}});
    CHECK(r.to_players.size() == 1);
    r.now += 1000;
    server.tick({{kHandle, kAcct}});
    CHECK(r.to_players.size() == 2);  // no hello: again after 15 s
    for (int i = 0; i < 10; ++i) {
        r.now += 15000;
        server.tick({{kHandle, kAcct}});
    }
    CHECK(r.to_players.size() == 4);  // at most 4 (a game without EMF shows the text each time)
    // a game that says hello gets no more beacons
    Rig r2;
    ServerLinkDeps d2 = r2.server_deps();
    d2.beacon_retry_ms = 15000;
    ServerLink s2(d2);
    s2.tick({{kHandle, kAcct}});
    REQUIRE(r2.to_players.size() == 1);
    r2.client.on_chat_line(r2.to_players[0].second, kAcct);
    r2.to_players.clear();
    for (const auto& line : r2.client.take_lines(100)) s2.on_line(kHandle, kAcct, line);  // its hello
    r2.to_players.clear();                                                                 // the welcome
    r2.now += 60000;
    s2.tick({{kHandle, kAcct}});
    CHECK(r2.to_players.empty());
}

TEST_CASE("net link: by default the beacon goes 10 s after a connection appears and once more 35 s later, never a third time; it says the server runs EMF mods and that one can play without it") {
    CHECK(ServerLinkDeps{}.beacon_delay_ms == 10000u);
    CHECK(ServerLinkDeps{}.beacon_retry_ms == 35000u);
    CHECK(ServerLinkDeps{}.beacon_tries == 2u);
    CHECK(ServerEnv{}.beacon_delay_ms == 10000u);
    Rig r;
    ServerLinkDeps d = r.server_deps();
    d.beacon_delay_ms = ServerLinkDeps{}.beacon_delay_ms;
    ServerLink server(d);
    server.tick({{kHandle, kAcct}});
    r.now += 9999;
    server.tick({{kHandle, kAcct}});
    CHECK(r.to_players.empty());
    r.now += 1;
    server.tick({{kHandle, kAcct}});
    REQUIRE(r.to_players.size() == 1);
    const auto line = parse_line(r.to_players[0].second, nullptr);
    REQUIRE(line);
    CHECK(line->payload == "This server runs EMF mods. Get EMF to see their windows; you can play without it.");
    r.now += 34999;
    server.tick({{kHandle, kAcct}});
    CHECK(r.to_players.size() == 1);
    r.now += 1;
    server.tick({{kHandle, kAcct}});
    CHECK(r.to_players.size() == 2);
    for (int i = 0; i < 10; ++i) {
        r.now += 35000;
        server.tick({{kHandle, kAcct}});
    }
    CHECK(r.to_players.size() == 2);  // a game without EMF shows each one
}

TEST_CASE("net link: beacon, hello, welcome; each side then knows the other's channels") {
    Rig r;
    r.server.tick({{kHandle, kAcct}});
    REQUIRE(r.to_players.size() == 1);
    CHECK(r.client.on_chat_line(r.to_players[0].second, kAcct));  // the beacon: taken, never shown
    r.to_players.clear();
    CHECK_FALSE(r.client.welcomed());
    CHECK(r.client.waiting() == 1);  // its hello
    r.deliver_to_server();
    CHECK(r.server.peers().size() == 1);
    CHECK(r.server.peer_version(kHandle, kAcct, "bm/pick") == 2);
    CHECK(r.server.peer_version(kHandle, kAcct, "bm/none") == 0);
    REQUIRE(r.peer_events.size() == 1);
    CHECK(r.peer_events[0].second);
    r.deliver_to_client();  // the welcome
    CHECK(r.client.welcomed());
    CHECK(r.server_events == std::vector<bool>{true});
    CHECK(r.client.server_version("bm/pick") == 2);
    CHECK(r.client.server_version("bm/show") == 1);
    CHECK(r.client.server_version("bm/other") == 0);
}

TEST_CASE("net link: each step of the greeting is one log line on its side (beacon, hello, welcome)") {
    Rig r;
    const auto has = [&r](const char* part) {
        size_t n = 0;
        for (const auto& l : r.logs) n += l.find(part) != std::string::npos;
        return n;
    };
    r.join();
    CHECK(has("the beacon to the player on connection 0x40") == 1);
    CHECK(has("the server's beacon") == 1);       // the client: hidden, answered
    CHECK(has("hello from account") == 1);         // the server: who, with how many channels; welcome sent
    CHECK(has("the server welcomed this game") == 1);
    r.server.tick({{kHandle, kAcct}});  // nothing new: no more lines
    CHECK(has("the beacon to the player") == 1);
    // a marked line not spoken as this player: one line, not one per line
    const auto beacon = encode_line(split_message('S', 0, "emf/net", 1, "fake", nullptr)[0]);
    r.client.on_chat_line(beacon, kAcct + 1);
    r.client.on_chat_line(beacon, kAcct + 1);
    CHECK(has("not spoken as this player") == 1);
}

TEST_CASE("net link: a message the outbox refuses is queued not at all and uses up no line numbers") {
    Rig r;
    r.join();
    std::string why;
    r.refuse_lines = true;
    CHECK_FALSE(r.server.send(kHandle, kAcct, "bm/show", 1, "lost", &why));
    CHECK(r.to_players.empty());
    r.refuse_lines = false;
    REQUIRE(r.server.send(kHandle, kAcct, "bm/show", 1, "next", &why));
    r.deliver_to_client();
    REQUIRE(r.at_client.size() == 1);
    CHECK(r.at_client[0].data == "next");
    bool gap = false;
    for (const auto& l : r.logs) gap |= l.find("gap") != std::string::npos;
    CHECK_FALSE(gap);  // the numbers went on where they were
}

TEST_CASE("net link: a welcome the outbox refuses makes no peer; the beacon comes again and the hello is answered then") {
    Rig r;
    ServerLinkDeps d = r.server_deps();
    d.beacon_retry_ms = 15000;
    ServerLink server(d);
    server.tick({{kHandle, kAcct}});
    REQUIRE(r.to_players.size() == 1);
    r.client.on_chat_line(r.to_players[0].second, kAcct);
    r.to_players.clear();
    r.refuse_lines = true;
    for (const auto& line : r.client.take_lines(100)) server.on_line(kHandle, kAcct, line);
    CHECK(server.peers().empty());  // no welcome went: not a peer
    r.refuse_lines = false;
    r.now += 15000;
    server.tick({{kHandle, kAcct}});
    REQUIRE(r.to_players.size() == 1);  // the beacon again
    r.client.on_chat_line(r.to_players[0].second, kAcct);
    r.to_players.clear();
    for (const auto& line : r.client.take_lines(100)) server.on_line(kHandle, kAcct, line);
    CHECK(server.peers().size() == 1);
}

TEST_CASE("net link: no beacon again while a hello is still coming in (a second one would restart the conversation)") {
    Rig r;
    ServerLinkDeps d = r.server_deps();
    d.beacon_retry_ms = 15000;
    ServerLink server(d);
    server.tick({{kHandle, kAcct}});
    REQUIRE(r.to_players.size() == 1);
    // the first line of a two-line hello arrives, the second is late
    const auto lines = split_message('H', 0, "emf/net", 1, std::string(1500, 'h'), nullptr);
    REQUIRE(lines.size() == 2);
    server.on_line(kHandle, kAcct, encode_line(lines[0]));
    r.now += 15000;
    server.tick({{kHandle, kAcct}});
    CHECK(r.to_players.size() == 1);  // no second beacon
}

TEST_CASE("net link: a hello whose next line never comes is dropped after 60 s; the beacon comes again and the new hello is answered") {
    Rig r;
    ServerLinkDeps d = r.server_deps();
    d.beacon_retry_ms = 15000;
    ServerLink server(d);
    server.tick({{kHandle, kAcct}});
    REQUIRE(r.to_players.size() == 1);
    r.to_players.clear();
    // the first line of a two-line hello arrives, the second never does
    const auto lines = split_message('H', 0, "emf/net", 1, std::string(1500, 'h'), nullptr);
    REQUIRE(lines.size() == 2);
    server.on_line(kHandle, kAcct, encode_line(lines[0]));
    r.now += kPartialMaxAgeMs;  // 60 s: at the limit, still coming in
    server.tick({{kHandle, kAcct}});
    CHECK(r.to_players.empty());
    r.now += 1000;  // over the limit: the half hello is dropped and the beacon comes again
    server.tick({{kHandle, kAcct}});
    REQUIRE(r.to_players.size() == 1);
    int told = 0;
    for (const auto& l : r.logs) told += l.find("a hello that never finished") != std::string::npos;
    CHECK(told == 1);
    // the game answers it with a whole hello: welcomed
    r.client.on_chat_line(r.to_players[0].second, kAcct);
    r.to_players.clear();
    for (const auto& line : r.client.take_lines(100)) server.on_line(kHandle, kAcct, line);
    CHECK(server.peers().size() == 1);
}

TEST_CASE("net link: a client with no channels sends no hello") {
    Rig r;
    r.client_channels.clear();
    r.server.tick({{kHandle, kAcct}});
    r.deliver_to_client();
    CHECK(r.client.waiting() == 0);
    CHECK(r.server.peers().empty());
}

TEST_CASE("net link: messages both ways; text, binary and a message of many lines") {
    Rig r;
    r.join();
    std::string why;
    REQUIRE(r.client.send("bm/pick", 2, "3", &why));
    REQUIRE(r.client.send("bm/pick", 2, std::string("\x00\x01\xff binary \x7f", 13), &why));
    std::string big;
    for (int i = 0; i < 5000; ++i) big += static_cast<char>('a' + i % 26);
    REQUIRE(r.client.send("bm/pick", 2, big, &why));
    std::string wide;
    for (int i = 0; i < 3000; ++i) wide += static_cast<char>(i % 256);
    REQUIRE(r.client.send("bm/pick", 2, wide, &why));
    r.deliver_to_server();
    REQUIRE(r.at_server.size() == 4);
    CHECK(r.at_server[0].data == "3");
    CHECK(r.at_server[0].channel == "bm/pick");
    CHECK(r.at_server[0].version == 2);
    CHECK(r.at_server[0].handle == kHandle);
    CHECK(r.at_server[0].account == kAcct);
    CHECK(r.at_server[1].data == std::string("\x00\x01\xff binary \x7f", 13));
    CHECK(r.at_server[2].data == big);
    CHECK(r.at_server[3].data == wide);

    REQUIRE(r.server.send(kHandle, kAcct, "bm/show", 1, "hello player", &why));
    REQUIRE(r.server.send(kHandle, kAcct, "bm/show", 1, wide, &why));
    r.deliver_to_client();
    REQUIRE(r.at_client.size() == 2);
    CHECK(r.at_client[0].data == "hello player");
    CHECK(r.at_client[0].channel == "bm/show");
    CHECK(r.at_client[1].data == wide);
}

TEST_CASE("net link: nothing is sent without a welcome, to a channel the other side lacks, or at another version") {
    Rig r;
    std::string why;
    CHECK_FALSE(r.client.send("bm/pick", 2, "x", &why));  // no welcome yet
    CHECK(why.find("welcomed") != std::string::npos);
    CHECK_FALSE(r.server.send(kHandle, kAcct, "bm/show", 1, "x", &why));  // no hello yet
    CHECK(why.find("hello") != std::string::npos);
    r.client_channels = {{"bm/pick", 3}};  // this game has pick at version 3, the server at 2
    r.join();
    CHECK_FALSE(r.client.send("bm/pick", 3, "x", &why));
    CHECK(why.find("version") != std::string::npos);
    CHECK_FALSE(r.server.send(kHandle, kAcct, "bm/show", 1, "x", &why));  // the client did not say it has bm/show
    CHECK(why.find("no channel") != std::string::npos);
    CHECK_FALSE(r.server.send(kHandle, kAcct, "bm/pick", 2, "x", &why));  // it has it at 3
    CHECK(why.find("version") != std::string::npos);
    CHECK_FALSE(r.server.send(kHandle, kAcct ^ 1, "bm/pick", 3, "x", &why));  // a reused handle: another player
}

TEST_CASE("net link: a message over 24 lines, a full outbox and the line rate refuse the whole message") {
    Rig r;
    r.join();
    std::string why;
    CHECK_FALSE(r.client.send("bm/pick", 2, std::string(30000, 'x'), &why));
    CHECK(why.find("24") != std::string::npos);
    CHECK(r.client.waiting() == 0);
    std::string big(1500, 'y');
    int sent = 0;
    while (r.client.send("bm/pick", 2, big, &why)) ++sent;
    CHECK(sent >= 20);
    CHECK(r.client.waiting() <= kClientOutboxLines);
    CHECK(why.find("waiting") != std::string::npos);

    r.to_players.clear();
    r.room = 1;  // a two-line message does not fit: nothing goes out, not even the first line
    CHECK_FALSE(r.server.send(kHandle, kAcct, "bm/show", 1, std::string(1500, 'z'), &why));
    CHECK(why.find("outbox") != std::string::npos);
    CHECK(r.to_players.empty());
    r.room = 1000;
    int to_player = 0;
    while (r.server.send(kHandle, kAcct, "bm/show", 1, std::string(100, 'q'), &why)) ++to_player;
    CHECK(to_player >= 30);
    CHECK(to_player <= 41);  // the bucket of 40 lines (the welcome was free)
    CHECK(why.find("rate") != std::string::npos);
    r.now += 2000;
    CHECK(r.server.send(kHandle, kAcct, "bm/show", 1, "again", &why));
}

TEST_CASE("net link: a forged line (another speaker) is taken and does nothing; a forged beacon cannot start a hello") {
    Rig r;
    r.join();
    std::string why;
    REQUIRE(r.server.send(kHandle, kAcct, "bm/show", 1, "real", &why));
    const std::string real = r.to_players.back().second;
    r.to_players.clear();
    CHECK(r.client.on_chat_line(real, kAcct + 1));  // spoken by someone else
    CHECK(r.at_client.empty());
    CHECK(r.client.on_chat_line(real, kAcct));
    REQUIRE(r.at_client.size() == 1);
    // a beacon spoken by another player: no new session, the welcome stays
    const auto beacon = encode_line(split_message('S', 0, "emf/net", 1, "fake", nullptr)[0]);
    CHECK(r.client.on_chat_line(beacon, kAcct + 1));
    CHECK(r.client.welcomed());
    CHECK(r.client.waiting() == 0);
    // not an EMF line at all: left to the game
    CHECK_FALSE(r.client.on_chat_line("hello there", kAcct));
    CHECK_FALSE(r.client.on_chat_line("#!emf2 M 0 a/b@1 0/1 t x", kAcct));
    // malformed: taken (hidden), logged, nothing delivered
    CHECK(r.client.on_chat_line("#!emf1 M zz", kAcct));
}

TEST_CASE("net link: a beacon that comes before this game knows its own account is answered once it does (spoken as this player)") {
    Rig r;
    r.self.reset();  // the game has not told it yet (its world's player list is not in: still loading)
    const uint64_t me = 0x1234;
    r.server.tick({{kHandle, me}});
    REQUIRE(r.to_players.size() == 1);
    CHECK(r.client.on_chat_line(r.to_players[0].second, me));  // taken (never shown) ...
    CHECK(r.client.waiting() == 0);                             // ... not answered yet: a forged beacon could be it
    size_t told = 0;
    for (const auto& l : r.logs) told += l.find("does not know its own account") != std::string::npos;
    CHECK(told == 1);
    r.self = me;  // the game knows now: the beacon it held was spoken as this player, the hello goes
    CHECK(r.client.waiting() == 1);
    const auto lines = r.client.take_lines(8);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find(" H ") != std::string::npos);
    CHECK(r.client.waiting() == 0);  // answered once
    // the next beacon, as before: the hello goes again
    CHECK(r.client.on_chat_line(r.to_players[0].second, me));
    CHECK(r.client.waiting() == 1);
}

TEST_CASE("net link: a beacon held before this game knew its own account and spoken as another player is never answered") {
    Rig r;
    r.self.reset();
    const auto forged = encode_line(split_message('S', 0, "emf/net", 1, "fake", nullptr)[0]);
    CHECK(r.client.on_chat_line(forged, kAcct + 7));  // hidden
    r.self = kAcct;
    CHECK(r.client.waiting() == 0);
    CHECK(r.client.take_lines(8).empty());
    size_t told = 0;
    for (const auto& l : r.logs) told += l.find("not spoken as this player") != std::string::npos;
    CHECK(told == 1);
}

TEST_CASE("net link: a beacon held before this game knew its own account is gone with a new connection, and says so") {
    Rig r;
    r.self.reset();
    const uint64_t me = 0x1234;
    r.server.tick({{kHandle, me}});
    REQUIRE(r.to_players.size() == 1);
    CHECK(r.client.on_chat_line(r.to_players[0].second, me));
    r.client.reset();  // a new connection (another server, or the same again)
    r.self = me;
    CHECK(r.client.waiting() == 0);
    size_t told = 0;
    for (const auto& l : r.logs) told += l.find("held beacon was dropped") != std::string::npos;
    CHECK(told == 1);
}

TEST_CASE("net link: a beacon held before this game knew its own account outlives a new client world on the same connection") {
    Rig r;
    r.self.reset();
    const uint64_t me = 0x1234;
    r.server.tick({{kHandle, me}});
    REQUIRE(r.to_players.size() == 1);
    CHECK(r.client.on_chat_line(r.to_players[0].second, me));
    r.client.reset(/*keep_held_beacon=*/true);  // the client world is made while the game still loads
    r.self = me;
    CHECK(r.client.waiting() == 1);
}

TEST_CASE("net link: a beacon typed by another player is never taken for the server's, whatever comes first") {
    Rig r;
    const auto forged = encode_line(split_message('S', 0, "emf/net", 1, "fake", nullptr)[0]);
    CHECK(r.client.on_chat_line(forged, kAcct + 7));  // hidden
    CHECK(r.client.waiting() == 0);                    // no hello to anyone
    r.server.tick({{kHandle, kAcct}});
    r.deliver_to_client();                             // the real one, spoken as this player
    CHECK(r.client.waiting() == 1);
}

TEST_CASE("net link: the server drops marked lines it should not read") {
    Rig r;
    const std::string m = encode_line(split_message('M', 0, "bm/pick", 2, "x", nullptr)[0]);
    CHECK(r.server.on_line(kHandle, kAcct, m));  // taken, but no hello yet: dropped
    CHECK(r.at_server.empty());
    r.join();
    CHECK(r.server.on_line(kHandle, kAcct, "#!emf1 W 0 emf/net@1 0/1 t p1;x;1;1;"));  // a player's 'W': not theirs
    CHECK(r.server.on_line(kHandle, kAcct, "#!emf1 S 0 emf/net@1 0/1 t hi"));
    CHECK(r.server.on_line(kHandle, kAcct, "#!emf1 M"));  // malformed
    // a channel its hello did not name
    CHECK(r.server.on_line(kHandle, kAcct, encode_line(split_message('M', 1, "bm/zzz", 1, "x", nullptr)[0])));
    // a channel at another version than its hello said
    CHECK(r.server.on_line(kHandle, kAcct, encode_line(split_message('M', 2, "bm/pick", 9, "x", nullptr)[0])));
    CHECK(r.at_server.empty());
    CHECK(r.server.peers().size() == 1);
    // from another account on the same handle: no peer
    CHECK(r.server.on_line(kHandle, kAcct + 1, encode_line(split_message('M', 1, "bm/pick", 2, "x", nullptr)[0])));
    CHECK(r.at_server.empty());
}

TEST_CASE("net link: a hello of another protocol gets an E line and no peer") {
    Rig r;
    CHECK(r.server.on_line(kHandle, kAcct, "#!emf1 H 0 emf/net@1 0/1 t p9;0.9;99;1;bm/pick@2"));
    CHECK(r.server.peers().empty());
    REQUIRE(r.to_players.size() == 1);
    CHECK(parse_line(r.to_players[0].second, nullptr)->type == 'E');
}

TEST_CASE("net link: a duplicate line is dropped, a lost line drops only the message it broke") {
    Rig r;
    r.join();
    std::string why;
    const std::string big(2500, 'k');  // 3 lines
    REQUIRE(r.client.send("bm/pick", 2, "one", &why));
    REQUIRE(r.client.send("bm/pick", 2, big, &why));
    REQUIRE(r.client.send("bm/pick", 2, "three", &why));
    auto lines = r.client.take_lines(100);
    REQUIRE(lines.size() == 5);
    r.server.on_line(kHandle, kAcct, lines[0]);
    r.server.on_line(kHandle, kAcct, lines[0]);  // a duplicate
    r.server.on_line(kHandle, kAcct, lines[1]);
    // lines[2] is lost
    r.server.on_line(kHandle, kAcct, lines[3]);
    r.server.on_line(kHandle, kAcct, lines[4]);
    REQUIRE(r.at_server.size() == 2);
    CHECK(r.at_server[0].data == "one");
    CHECK(r.at_server[1].data == "three");
}

TEST_CASE("net link: too many lines from one account are dropped, then ignored for a while") {
    Rig r;
    r.join();
    std::string why;
    REQUIRE(r.client.send("bm/pick", 2, "x", &why));
    const std::string line = r.client.take_lines(1)[0];
    int delivered = 0;
    for (int i = 0; i < 1000; ++i) {
        r.server.on_line(kHandle, kAcct, line);  // the same line: all but the first are duplicates anyway
    }
    delivered = static_cast<int>(r.at_server.size());
    CHECK(delivered == 1);
    CHECK_FALSE(r.logs.empty());
    size_t notes = r.logs.size();
    for (int i = 0; i < 1000; ++i) r.server.on_line(kHandle, kAcct, line);
    CHECK(r.logs.size() <= notes + 1);  // one note a minute
    // far over the rate: a good line is dropped now, and heard again after a minute
    REQUIRE(r.client.send("bm/pick", 2, "later", &why));
    const std::string later = r.client.take_lines(1)[0];
    r.server.on_line(kHandle, kAcct, later);
    CHECK(r.at_server.size() == 1);
    r.now += 61000;
    r.server.on_line(kHandle, kAcct, later);
    REQUIRE(r.at_server.size() == 2);
    CHECK(r.at_server.back().data == "later");
}

TEST_CASE("net link: a reconnect with a new handle replaces the peer; leaving ends it; a reused handle is no peer") {
    Rig r;
    r.join();
    CHECK(r.server.peers().size() == 1);
    // the same account on another handle says hello: the old peer goes
    Rig& q = r;
    q.client.reset();
    q.peer_events.clear();
    const Connection second{0x41, kAcct};
    q.server.tick({second});  // the old connection is gone, the new one is greeted
    REQUIRE(q.peer_events.size() == 1);
    CHECK_FALSE(q.peer_events[0].second);
    CHECK(q.peer_events[0].first.handle == kHandle);
    q.deliver_to_client(second);
    q.deliver_to_server(second);
    q.deliver_to_client(second);
    CHECK(q.server.peers().size() == 1);
    CHECK(q.server.peers()[0].handle == 0x41);
    CHECK(q.server.peer_version(kHandle, kAcct, "bm/pick") == 0);
    // leaving
    q.peer_events.clear();
    q.server.tick({});
    CHECK(q.server.peers().empty());
    REQUIRE(q.peer_events.size() == 1);
    CHECK_FALSE(q.peer_events[0].second);
    // a handle another account took
    Rig r2;
    r2.join();
    r2.peer_events.clear();
    r2.server.tick({{kHandle, kAcct + 7}});
    CHECK(r2.server.peers().empty());
    CHECK(r2.peer_events.size() == 1);
}

TEST_CASE("net link: the same account saying hello again on its handle replaces its state") {
    Rig r;
    r.join();
    r.client.reset();
    r.server.tick({{kHandle, kAcct}});  // still connected, already greeted: no new beacon
    CHECK(r.to_players.empty());
    // a client that restarted its session (a new beacon was never sent: it sends hello on its own)
    r.server_channels.clear();
    r.server.on_line(kHandle, kAcct, "#!emf1 H 0 emf/net@1 0/1 t p1;0.5.1;27;1287369;bm/pick@2");
    CHECK(r.server.peers().size() == 1);
    CHECK(r.server.peer_version(kHandle, kAcct, "bm/pick") == 2);
}

TEST_CASE("net link: bye ends the peer and the welcome; a lines from the host's own player are ignored") {
    Rig r;
    r.join();
    r.peer_events.clear();
    const auto bye = encode_line(split_message('B', 9, "emf/net", 1, "bye", nullptr)[0]);
    r.server.on_line(kHandle, kAcct, bye);
    CHECK(r.server.peers().empty());
    REQUIRE(r.peer_events.size() == 1);
    CHECK_FALSE(r.peer_events[0].second);

    Rig q;
    q.join();
    q.server.on_line(kHandle, kAcct, "x");  // not marked: the caller never sends it, but it is safe
    const auto bye2 = encode_line(split_message('B', 0, "emf/net", 1, "bye", nullptr)[0]);
    CHECK(q.client.on_chat_line(bye2, kAcct));
    CHECK_FALSE(q.client.welcomed());
    CHECK(q.server_events == std::vector<bool>{true, false});

    Rig l;
    l.local_handle = kHandle;
    CHECK(l.server.on_line(kHandle, kAcct, "#!emf1 H 0 emf/net@1 0/1 t p1;0.5.1;27;1;bm/pick@2"));
    CHECK(l.server.peers().empty());
}

TEST_CASE("net link: the client's world going away ends the welcome and empties its queue; lines go back in front") {
    Rig r;
    r.join();
    std::string why;
    REQUIRE(r.client.send("bm/pick", 2, "a", &why));
    REQUIRE(r.client.send("bm/pick", 2, "b", &why));
    auto taken = r.client.take_lines(1);
    REQUIRE(taken.size() == 1);
    r.client.put_back(taken);  // the game would not take it (pool full)
    auto all = r.client.take_lines(10);
    REQUIRE(all.size() == 2);
    CHECK(parse_line(all[0], nullptr)->payload == "a");  // the order kept
    CHECK(parse_line(all[1], nullptr)->payload == "b");
    r.client.put_back(all);
    r.client.reset();
    CHECK(r.client.waiting() == 0);
    CHECK_FALSE(r.client.welcomed());
    CHECK(r.server_events == std::vector<bool>{true, false});
    CHECK_FALSE(r.client.send("bm/pick", 2, "c", &why));
}

TEST_CASE("net link: a second beacon starts the conversation again") {
    Rig r;
    r.join();
    r.server_events.clear();
    r.server.tick({});
    r.server.tick({{kHandle, kAcct}});  // a new connection of the same player
    r.deliver_to_client();
    CHECK(r.server_events == std::vector<bool>{false});  // the old welcome is gone
    r.deliver_to_server();
    r.deliver_to_client();
    CHECK(r.client.welcomed());
    CHECK(r.server_events == (std::vector<bool>{false, true}));
}

TEST_CASE("net link: a game that hosts its server hides marked lines but does not answer the beacon") {
    Rig r;
    r.client_enabled = false;
    r.server.tick({{kHandle, kAcct}});
    REQUIRE(r.to_players.size() == 1);
    CHECK(r.client.on_chat_line(r.to_players[0].second, kAcct));  // taken: never shown
    CHECK(r.client.waiting() == 0);
    CHECK_FALSE(r.client.welcomed());
}

TEST_CASE("net link: a server whose greets says no sends no beacon; one that says yes beacons without a mod channel") {
    Rig r;
    auto d = r.server_deps();
    d.greets = [] { return false; };
    ServerLink quiet(d);
    quiet.tick({{0x40, 1}});
    CHECK(r.to_players.empty());
    r.server_channels.clear();
    auto d2 = r.server_deps();
    d2.greets = [] { return true; };
    ServerLink loud(d2);
    loud.tick({{0x40, 1}});
    CHECK(r.to_players.size() == 1);
}

TEST_CASE("net link: only the core's emf/ channels greet only when the core says so; a mod's channel always does") {
    using emf::core::net::greets_for;
    CHECK_FALSE(greets_for({}, true));  // no channel at all: nothing to talk about
    CHECK_FALSE(greets_for({{"emf/mods", 1}}, false));
    CHECK(greets_for({{"emf/mods", 1}}, true));
    CHECK(greets_for({{"emf/mods", 1}, {"beastmaster/ui", 1}}, false));
}
