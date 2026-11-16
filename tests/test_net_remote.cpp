// Mod messages (API v27), remote peers: two NetService objects (a server and a joined player's game) joined by a fake
// chat. A mod's net_to_server on the client arrives in on_server after the server world's next step with the sender the
// server's own records name; net_to_player / net_to_players go the other way; refusals at the sender; nothing is
// delivered at another channel version; a hosted game has both the loopback and its remote players.
#include <doctest.h>

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/net/net_service.h"

using namespace emf::core::net;

namespace {

constexpr uint64_t kAcct = 0x1122334455667788ull;
constexpr uint32_t kHandle = 0x40;
constexpr uint32_t kEntity = 5;

struct Seen {
    NetFrom from;
    std::string data;
};

struct Side {
    uint32_t local = 0;  // != 0: this process hosts a server world with that local player
    std::vector<std::pair<uint8_t, std::function<void(EmfWorld*)>>> posted;
    std::vector<std::string> logs;
    uint64_t now = 1000;
    NetService s;
    explicit Side(std::function<void(NetDeps&)> tweak = {}) : s(make(tweak)) {}
    NetDeps make(const std::function<void(NetDeps&)>& tweak) {
        NetDeps d;
        d.hosted = [this](uint32_t* id) {
            *id = local;
            return local != 0;
        };
        d.post = [this](uint8_t kind, std::function<void(EmfWorld*)> fn) { posted.emplace_back(kind, std::move(fn)); };
        d.now_ms = [this] { return now; };
        d.log = [this](const std::string& l) { logs.push_back(l); };
        if (tweak) tweak(d);
        return d;
    }
    void step(uint8_t kind) {
        auto jobs = std::move(posted);
        posted.clear();
        for (auto& [k, fn] : jobs) {
            if (k == kind) fn(reinterpret_cast<EmfWorld*>(0x1000 + kind));
            else posted.emplace_back(k, std::move(fn));
        }
    }
};

ChannelDef def(const char* name, uint32_t version, uint32_t flags, std::vector<Seen>* server, std::vector<Seen>* player) {
    ChannelDef d;
    d.name = name;
    d.version = version;
    d.flags = flags;
    if (server) d.on_server = [server](EmfWorld*, const NetFrom& f, const std::string& data) { server->push_back({f, data}); };
    if (player) d.on_player = [player](EmfWorld*, const NetFrom& f, const std::string& data) { player->push_back({f, data}); };
    return d;
}

struct Pair {
    std::vector<std::pair<std::pair<uint32_t, uint64_t>, std::string>> chat_to_player;  // what the server queued
    bool admin = false;
    uint32_t admin_asked_handle = 0;
    Side server;
    Side client;
    std::vector<Seen> at_server, at_client;
    Pair(uint32_t host_local = 0, uint32_t client_pick_version = 2)
        : server([this](NetDeps& d) {
              d.connection_of = [](uint32_t entity) -> std::optional<std::pair<uint32_t, uint64_t>> {
                  if (entity == kEntity) return std::make_pair(kHandle, kAcct);
                  return std::nullopt;
              };
              d.entity_of = [](uint64_t account) { return account == kAcct ? kEntity : 0u; };
              d.is_admin = [this](uint32_t handle, uint64_t) {
                  admin_asked_handle = handle;
                  return admin;
              };
          }) {
        server.local = host_local;
        std::string why;
        REQUIRE(server.s.add_channel("bm", def("bm/pick", 2, kToServer, &at_server, nullptr), &why));
        REQUIRE(server.s.add_channel("bm", def("bm/show", 1, kToPlayers, nullptr, &at_client), &why));
        REQUIRE(client.s.add_channel("bm", def("bm/pick", client_pick_version, kToServer, &at_server, nullptr), &why));
        REQUIRE(client.s.add_channel("bm", def("bm/show", 1, kToPlayers, nullptr, &at_client), &why));
        ServerEnv senv;
        senv.beacon_delay_ms = 0;  // the beacon on the first tick (net_link's tests cover the delay)
        senv.send = [this](uint32_t h, uint64_t a, const std::string& line) {
            chat_to_player.push_back({{h, a}, line});
            return true;
        };
        senv.room = [] { return size_t(100); };
        senv.is_local = [](uint32_t, uint64_t) { return false; };
        senv.emf_version = "0.5.1";
        server.s.enable_server(std::move(senv));
        ClientEnv cenv;
        cenv.self_account = [] { return std::optional<uint64_t>(kAcct); };
        cenv.emf_version = "0.5.1";
        client.s.enable_client(std::move(cenv));
    }
    void to_client() {
        auto lines = std::move(chat_to_player);
        chat_to_player.clear();
        for (auto& [who, line] : lines) client.s.client_line(line, kAcct);
    }
    void to_server() {
        for (const auto& line : client.s.client_take(100)) server.s.server_line(kHandle, kAcct, line);
    }
    void join() {
        server.s.server_tick({{kHandle, kAcct}});
        to_client();
        to_server();
        to_client();
    }
};

}  // namespace

TEST_CASE("net remote: a player's message reaches on_server with the sender the server's records name") {
    Pair p;
    p.join();
    CHECK(p.client.s.peer(0, "bm/pick") == 2);  // the server's version, a joined game
    CHECK(p.server.s.peer(kEntity, "bm/pick") == 2);
    CHECK(p.server.s.peer(kEntity, "bm/show") == 1);
    CHECK(p.server.s.peer(99, "bm/pick") == 0);  // no such player
    std::string why;
    REQUIRE(p.client.s.to_server("bm", "bm/pick", "option 3", &why));
    p.to_server();
    p.server.step(EMF_WORLD_SERVER);
    REQUIRE(p.at_server.size() == 1);
    CHECK(p.at_server[0].data == "option 3");
    CHECK(p.at_server[0].from.player == kEntity);
    CHECK(p.at_server[0].from.account == kAcct);
    CHECK(p.at_server[0].from.peer_version == 2);
    CHECK_FALSE(p.at_server[0].from.local);
    CHECK_FALSE(p.at_server[0].from.admin);
    p.admin = true;
    REQUIRE(p.client.s.to_server("bm", "bm/pick", std::string("\x00\xff", 2), &why));
    p.to_server();
    p.server.step(EMF_WORLD_SERVER);
    REQUIRE(p.at_server.size() == 2);
    CHECK(p.at_server[1].data == std::string("\x00\xff", 2));
    CHECK(p.at_server[1].from.admin);
}

TEST_CASE("net remote: a player who reconnected gets the server's message on the connection that said hello, while the old record lingers") {
    Pair p;
    const uint32_t kNew = kHandle + 1;  // connection_of still names the old kHandle
    p.server.s.server_tick({{kNew, kAcct}});
    p.to_client();
    for (const auto& line : p.client.s.client_take(100)) p.server.s.server_line(kNew, kAcct, line);
    p.to_client();  // the welcome
    std::string why;
    REQUIRE(p.server.s.to_player("bm", kEntity, "bm/show", "here", &why));
    p.to_client();
    p.client.step(EMF_WORLD_CLIENT);
    REQUIRE(p.at_client.size() == 1);
    CHECK(p.at_client[0].data == "here");
}

TEST_CASE("net remote: the server's message reaches on_player on the client world's thread") {
    Pair p;
    p.join();
    std::string why;
    REQUIRE(p.server.s.to_player("bm", kEntity, "bm/show", "look here", &why));
    CHECK(p.server.s.to_players("bm", "bm/show", "to all") == 1);
    p.to_client();
    CHECK(p.at_client.empty());  // after the client world's next step
    p.client.step(EMF_WORLD_SERVER);
    CHECK(p.at_client.empty());
    p.client.step(EMF_WORLD_CLIENT);
    REQUIRE(p.at_client.size() == 2);
    CHECK(p.at_client[0].data == "look here");
    CHECK(p.at_client[1].data == "to all");
    CHECK_FALSE(p.at_client[0].from.local);
    CHECK(p.at_client[0].from.peer_version == 1);
}

TEST_CASE("net remote: refusals at the sender") {
    Pair p;
    std::string why;
    CHECK_FALSE(p.client.s.to_server("bm", "bm/pick", "x", &why));  // no welcome yet
    CHECK(why.find("welcomed") != std::string::npos);
    CHECK_FALSE(p.server.s.to_player("bm", kEntity, "bm/show", "x", &why));  // no hello yet
    CHECK(p.server.s.to_players("bm", "bm/show", "x") == 0);
    p.join();
    CHECK_FALSE(p.client.s.to_server("other", "bm/pick", "x", &why));  // not its channel
    CHECK_FALSE(p.client.s.to_server("bm", "bm/show", "x", &why));      // a channel that does not go to the server
    CHECK_FALSE(p.server.s.to_player("bm", kEntity, "bm/pick", "x", &why));  // does not go to players
    CHECK_FALSE(p.client.s.to_server("bm", "bm/pick", std::string(5000, 'x'), &why));  // over 4096
    CHECK(why.find("too large") != std::string::npos);
    CHECK_FALSE(p.server.s.to_player("bm", 77, "bm/show", "x", &why));  // a player nobody knows
    CHECK(p.client.s.client_take(100).empty());
}

TEST_CASE("net remote: a channel at another version delivers nothing either way") {
    Pair p(0, 3);  // the client has bm/pick at 3, the server at 2
    p.join();
    CHECK(p.server.s.peer(kEntity, "bm/pick") == 3);  // what the player's game said
    CHECK(p.client.s.peer(0, "bm/pick") == 2);
    std::string why;
    CHECK_FALSE(p.client.s.to_server("bm", "bm/pick", "x", &why));
    CHECK(why.find("version") != std::string::npos);
    CHECK(p.server.s.to_player("bm", kEntity, "bm/show", "still fine", &why));  // bm/show matches
}

TEST_CASE("net remote: a message on a channel the server lacks, or too large, is dropped and told once") {
    Pair p;
    p.join();
    // a client that registered a channel the server does not have (its hello lists it)
    Pair q;
    std::string why;
    REQUIRE(q.client.s.add_channel("bm", def("bm/extra", 1, kToServer, nullptr, nullptr), &why));
    q.join();
    // the server did not say bm/extra: the client refuses to send
    CHECK_FALSE(q.client.s.to_server("bm", "bm/extra", "x", &why));
    CHECK(why.find("no channel") != std::string::npos);
    // a forged line from the player's connection naming a channel it did not say: dropped without a call
    const auto forged = encode_line(split_message('M', 1, "bm/show", 1, "x", nullptr)[0]);
    CHECK(q.server.s.server_line(kHandle, kAcct, forged));
    q.server.step(EMF_WORLD_SERVER);
    CHECK(q.at_server.empty());
}

TEST_CASE("net remote: the channel's rate limits a player at the receiving end too") {
    Pair p;
    p.join();
    std::string why;
    // 100 messages in 10 s is the default: send 100 (the client's own bucket), the server takes them all at once
    int sent = 0;
    for (int i = 0; i < 100; ++i) {
        if (p.client.s.to_server("bm", "bm/pick", "m", &why)) ++sent;
        if (i % 10 == 9) p.to_server();  // the line rate (20 a second) lets about ten through at once
    }
    p.to_server();
    p.server.step(EMF_WORLD_SERVER);
    CHECK(sent == 100);
    CHECK(p.at_server.size() >= 35);  // the line limit of the transport holds the rest back: they are dropped
    CHECK(p.at_server.size() <= 100);
    CHECK_FALSE(p.client.s.to_server("bm", "bm/pick", "m", &why));  // the sender's own limit
    CHECK(why.find("rate") != std::string::npos);
}

TEST_CASE("net remote: a hosted game delivers to its own player in process and to remote players over the chat") {
    Pair p(7);  // this process hosts the server world with player entity 7
    p.join();
    CHECK(p.server.s.peer(7, "bm/show") == 1);       // the host's own game: loopback
    CHECK(p.server.s.peer(kEntity, "bm/show") == 1);  // the remote player
    CHECK(p.server.s.to_players("bm", "bm/show", "everyone") == 2);
    CHECK(p.server.s.to_player("bm", 7, "bm/show", "host only", nullptr));
    p.server.step(EMF_WORLD_CLIENT);
    REQUIRE(p.at_client.size() == 2);  // both on_player callbacks are the same vector here
    CHECK(p.at_client[0].from.local);
    CHECK(p.at_client[0].data == "everyone");
    CHECK(p.at_client[1].data == "host only");
    p.to_client();
    p.client.step(EMF_WORLD_CLIENT);
    CHECK(p.at_client.size() == 3);
    CHECK(p.at_client[2].data == "everyone");
    CHECK_FALSE(p.at_client[2].from.local);
}

TEST_CASE("net remote: without the transport switched on nothing is taken and nothing goes") {
    Side s;
    std::string why;
    REQUIRE(s.s.add_channel("bm", def("bm/pick", 1, kToServer, nullptr, nullptr), &why));
    CHECK_FALSE(s.s.server_line(1, 2, "#!emf1 H 0 emf/net@1 0/1 t p1;x;1;1;"));
    CHECK_FALSE(s.s.client_line("#!emf1 S 0 emf/net@1 0/1 t hi", 1));
    CHECK(s.s.client_take(10).empty());
    CHECK_FALSE(s.s.to_server("bm", "bm/pick", "x", &why));
    CHECK(s.s.peer(0, "bm/pick") == 0);
    s.s.server_tick({{1, 2}});  // nothing happens
    s.s.client_reset();
}

TEST_CASE("net remote: the server's peer watch hears a game's hello and its leaving") {
    Pair p;
    std::vector<std::pair<uint64_t, bool>> seen;
    p.server.s.set_peer_watch([&](uint64_t account, bool joined) { seen.emplace_back(account, joined); });
    p.join();
    REQUIRE(seen.size() == 1);
    CHECK(seen[0] == std::make_pair(kAcct, true));
    p.server.s.server_tick({});  // the connection is gone
    REQUIRE(seen.size() == 2);
    CHECK(seen[1] == std::make_pair(kAcct, false));
}

TEST_CASE("net remote: the reasons for dropped messages are remembered up to a limit, however many channels a player names") {
    Side server;
    ServerEnv senv;
    senv.send = [](uint32_t, uint64_t, const std::string&) { return true; };
    senv.room = [] { return size_t(100); };
    senv.is_local = [](uint32_t, uint64_t) { return false; };
    server.s.enable_server(std::move(senv));
    const auto told = [&] {
        size_t n = 0;
        for (const auto& l : server.logs) n += l.find("was dropped: no such channel") != std::string::npos;
        return n;
    };
    // a game that says hello with a new channel name each time and sends one message on it: the server has none of them
    const auto say = [&](const std::string& name) {
        HelloInfo hello;
        hello.channels = {{name, 1}};
        for (const auto& line : split_message('H', 0, kControlChannel, kControlVersion, hello_text(hello), nullptr))
            server.s.server_line(kHandle, kAcct, encode_line(line));
        for (const auto& line : split_message('M', 1, name, 1, "x", nullptr))
            server.s.server_line(kHandle, kAcct, encode_line(line));
        server.now += 1000;  // under the lines a player may send
    };
    say("bm/x0");
    CHECK(told() == 1);
    say("bm/x0");
    CHECK(told() == 1);  // told once
    for (int i = 1; i < 512; ++i) say("bm/x" + std::to_string(i));
    CHECK(told() == 128);
    std::vector<std::string> notices;  // the 128th says that later ones are not logged
    for (const auto& l : server.logs)
        if (l.find("later ones are not logged") != std::string::npos) notices.push_back(l);
    REQUIRE(notices.size() == 1);
    CHECK(notices[0].find("bm/x127") != std::string::npos);
    // an hour after the first reason was told the list is forgotten: a full list does not silence the log for good
    server.now += 59 * 60 * 1000 - 512 * 1000;  // 59 minutes after the first: still remembered, still full
    say("bm/x0");
    say("bm/new");
    CHECK(told() == 128);
    server.now += 2 * 60 * 1000;
    say("bm/x0");  // a reason that still happens is told again
    CHECK(told() == 129);
    say("bm/new");  // and one that never fitted is told now
    CHECK(told() == 130);
    say("bm/x0");
    CHECK(told() == 130);  // once, as before
    CHECK(server.posted.empty());  // and none of them reached a mod
}

TEST_CASE("net remote: a message whose next line takes more than 60 s is dropped at either end; the next message arrives") {
    Pair p;
    p.join();
    std::string why;
    const std::string big(3000, 'q');
    // a player's message: its first line, a long silence, the rest
    REQUIRE(p.client.s.to_server("bm", "bm/pick", big, &why));
    const auto up = p.client.s.client_take(100);
    REQUIRE(up.size() >= 3u);
    p.server.s.server_line(kHandle, kAcct, up[0]);
    p.server.now += kPartialMaxAgeMs + 1;
    for (size_t i = 1; i < up.size(); ++i) p.server.s.server_line(kHandle, kAcct, up[i]);
    p.server.step(EMF_WORLD_SERVER);
    CHECK(p.at_server.empty());
    size_t timed_out = 0, out_of_place = 0;  // the server's log says why: late, not out of place
    for (const auto& l : p.server.logs) {
        timed_out += l.find("a line dropped: a fragment of a message that timed out") != std::string::npos;
        out_of_place += l.find("without the ones before") != std::string::npos;
    }
    CHECK(timed_out == 1);
    CHECK(out_of_place == 0);
    REQUIRE(p.client.s.to_server("bm", "bm/pick", "next", &why));
    p.to_server();
    p.server.step(EMF_WORLD_SERVER);
    REQUIRE(p.at_server.size() == 1);
    CHECK(p.at_server[0].data == "next");
    // the server's message to the player, the same way
    REQUIRE(p.server.s.to_player("bm", kEntity, "bm/show", big, &why));
    const auto down = std::move(p.chat_to_player);
    p.chat_to_player.clear();
    REQUIRE(down.size() >= 3u);
    p.client.s.client_line(down[0].second, kAcct);
    p.client.now += kPartialMaxAgeMs + 1;
    for (size_t i = 1; i < down.size(); ++i) p.client.s.client_line(down[i].second, kAcct);
    p.client.step(EMF_WORLD_CLIENT);
    CHECK(p.at_client.empty());
    REQUIRE(p.server.s.to_player("bm", kEntity, "bm/show", "next", &why));
    p.to_client();
    p.client.step(EMF_WORLD_CLIENT);
    REQUIRE(p.at_client.size() == 1);
    CHECK(p.at_client[0].data == "next");
    // within the limit a slow message still arrives whole: 45 s between its lines, more than the server's outbox wait
    REQUIRE(p.client.s.to_server("bm", "bm/pick", big, &why));
    const auto slow = p.client.s.client_take(100);
    for (const auto& line : slow) {
        p.server.s.server_line(kHandle, kAcct, line);
        p.server.now += 45000;
    }
    p.server.step(EMF_WORLD_SERVER);
    REQUIRE(p.at_server.size() == 2);
    CHECK(p.at_server[1].data == big);
}
