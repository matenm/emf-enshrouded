// The server-side chat hooks with the game's two functions faked: a receive loop that delivers players' lines through
// the chatLogAdd detour, and a chatLogAdd that queues lines with the game's per-reader bookkeeping.
#include <doctest.h>

#include <cstring>
#include <string>
#include <vector>

#include "core/server_chat.h"

using namespace emf::core;

namespace {
constexpr uint64_t kAdmin = 0xAA, kPlayer = 0xBB;
constexpr uint32_t kAdminHandle = 0x42;   // connection 2, peer 0x85: reader 5
constexpr uint32_t kPlayerHandle = 0x47;  // connection 7, peer 0x81: reader 1

struct FakeServer {
    std::vector<uint8_t> server = std::vector<uint8_t>(
        server_layout::kConnections + server_layout::kConnectionSize * server_layout::kConnectionCount, 0);
    std::vector<uint8_t> sys = std::vector<uint8_t>(0x200, 0);
    std::vector<std::vector<uint8_t>> nodes;
    FakeServer() {
        uint8_t* p = sys.data();
        std::memcpy(server.data() + 0x60, &p, 8);
        for (int i : {1, 5}) sys[0x0C + 12 * i] = 1;  // two players connected
        connection(kAdminHandle, 0x85, kAdmin);
        connection(kPlayerHandle, 0x81, kPlayer);
    }
    void connection(uint32_t handle, uint32_t peer, uint64_t account) {
        uint8_t* rec = server.data() + *connection_record_offset(handle);
        std::memcpy(rec, &handle, 4);
        std::memcpy(rec + server_layout::kConnectionPeer, &peer, 4);
        std::memcpy(rec + server_layout::kConnectionAccount, &account, 8);
    }
    int32_t pending(int reader) const {
        int32_t v;
        std::memcpy(&v, sys.data() + 0x0C + 12 * reader + 8, 4);
        return v;
    }
    uint32_t tail_mask() const {
        uint8_t* n;
        std::memcpy(&n, sys.data() + 0x1C8, 8);
        uint32_t m;
        std::memcpy(&m, n + 0x20, 4);
        return m;
    }
};
FakeServer* g_fs = nullptr;
ServerChatDetours g_d{};
struct Incoming {
    uint32_t handle;
    uint64_t account;
    std::string text;
};
std::vector<Incoming> g_incoming;
std::vector<std::string> g_added;
std::vector<std::pair<uint32_t, uint64_t>> g_speakers;  // handle and account of each added line
std::string g_other_line;  // set: the next added line is someone else's (another thread got in between)
bool g_game_edits = false;  // set: the game stores a changed text (e.g. shortened) instead of the one it was given
struct Ran {
    std::string line;
    CommandSender sender;
    ReplyFn reply;
};
std::vector<Ran> g_ran;
std::vector<std::string> g_log;
std::vector<ChatLine> g_heard;  // what on_line got (API v19 on_chat)

uint32_t line_size(uint32_t len) {
    uint32_t s = len + 0x17;
    return (s & 3) ? s - (s & 3) + 4 : s;
}

uint64_t g_chat_now = 1000;  // the hooks' clock (ServerChatDeps.now_ms)

uint8_t* __fastcall fake_add(uint8_t* out, uint8_t* sys, uint8_t, const ChatText* t, uint32_t, uint32_t,
                             uint32_t handle, uint64_t account, const void*) {
    const std::string text(t->data, static_cast<size_t>(t->size));
    g_added.push_back(text);
    g_speakers.emplace_back(handle, account);
    const std::string stored = !g_other_line.empty() ? g_other_line : g_game_edits ? text.substr(0, text.size() - 1) : text;
    auto& n = g_fs->nodes.emplace_back(0x40 + stored.size() + 8, uint8_t{0});
    const auto len = static_cast<uint32_t>(stored.size());
    std::memcpy(n.data() + 8, &len, 4);
    std::memcpy(n.data() + 0x40, stored.data(), stored.size());
    uint32_t mask = 0;
    for (int i = 0; i < 32; ++i) {
        uint8_t* r = sys + 0x0C + 12 * i;
        if (r[0]) {
            int32_t p;
            std::memcpy(&p, r + 8, 4);
            p += static_cast<int32_t>(line_size(len));
            std::memcpy(r + 8, &p, 4);
        } else {
            mask |= 1u << i;
        }
    }
    std::memcpy(n.data() + 0x20, &mask, 4);
    uint8_t* np = n.data();
    std::memcpy(sys + 0x1C8, &np, 8);
    out[0] = 0;
    return out;
}

void __fastcall fake_receive(uint8_t* server) {
    uint8_t* sys;
    std::memcpy(&sys, server + 0x60, 8);
    for (const auto& in : g_incoming) {
        const ChatText t{in.text.data(), in.text.size()};
        uint8_t res[16];
        std::memset(res, 0xFF, sizeof res);
        struct {
            uint64_t time, has;
        } when{0, 0};
        g_d.log_add(res, sys, 0, &t, 0, 0, in.handle, in.account, &when);
        CHECK(res[0] == 0);  // a taken line looks added to the caller
    }
    g_incoming.clear();
}

struct Rig {
    FakeServer fs;
    Rig() {
        g_fs = &fs;
        g_added.clear();
        g_speakers.clear();
        g_ran.clear();
        g_log.clear();
        g_incoming.clear();
        g_other_line.clear();
        g_game_edits = false;
        g_heard.clear();
        ServerChatDeps deps;
        deps.is_command = [](std::string_view w) { return w == "give" || w == "help" || w == "where"; };
        deps.for_players = [](std::string_view w) { return w == "where"; };  // a command any player may run (v17)
        deps.policy = parse_chat_policy("0xaa", false);
        deps.run = [](const std::string& line, const CommandSender& s, ReplyFn reply) { g_ran.push_back({line, s, reply}); };
        deps.log = [](const std::string& t) { g_log.push_back(t); };
        deps.now_ms = [] { return g_chat_now; };
        deps.on_line = [](ChatLine l) { g_heard.push_back(std::move(l)); };
        g_chat_now += 60000;  // every test starts with a fresh rate window
        g_d = server_chat_for_test(fake_receive, fake_add, std::move(deps));
    }
    ~Rig() { g_fs = nullptr; }
    void receive() { g_d.receive(fs.server.data()); }
};
bool logged(const std::string& needle) {
    for (const auto& l : g_log)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}
}  // namespace

TEST_CASE("server chat hooks: an admin's /command is taken and run as them; replies reach only them, after the loop") {
    Rig r;
    g_incoming = {{kAdminHandle, kAdmin, "/give Wood 5"}, {kPlayerHandle, kPlayer, "hello all"}};
    r.receive();
    REQUIRE(g_added.size() == 1);  // only the plain chat line was added
    CHECK(g_added[0] == "hello all");
    REQUIRE(g_ran.size() == 1);
    CHECK(g_ran[0].line == "give Wood 5");
    CHECK(g_ran[0].sender.from_chat);
    CHECK(g_ran[0].sender.account == kAdmin);
    CHECK(g_ran[0].sender.handle == kAdminHandle);
    const int32_t player_before = r.fs.pending(1);
    g_ran[0].reply("gave 5 x Wood");  // the command ran on the world's thread
    r.receive();                      // the next receive loop sends it
    REQUIRE(g_added.size() == 2);
    CHECK(g_added[1] == "[EMF] gave 5 x Wood");
    CHECK(g_speakers[1] == std::make_pair(kAdminHandle, kAdmin));  // a line without a speaker is not shown by clients
    CHECK((r.fs.tail_mask() & (1u << 1)) != 0);  // the other player is done with it
    CHECK((r.fs.tail_mask() & (1u << 5)) == 0);  // the admin still gets it
    CHECK(r.fs.pending(1) == player_before);     // and the other player's count is as before
}

TEST_CASE("server chat hooks: a refused /command is dropped, the sender alone is told, the account is logged") {
    Rig r;
    g_incoming = {{kPlayerHandle, kPlayer, "/give Wood 5\x1b[2J"}};
    r.receive();
    CHECK(g_ran.empty());
    REQUIRE(g_added.size() == 1);  // the refusal, sent at the end of the same loop
    CHECK(g_added[0].find("[EMF] not run") == 0);
    CHECK((r.fs.tail_mask() & (1u << 5)) != 0);  // not to the admin
    CHECK((r.fs.tail_mask() & (1u << 1)) == 0);
    CHECK(logged("0x00000000000000bb"));
    for (const auto& l : g_log) CHECK(l.find('\x1b') == std::string::npos);  // no escape codes into the log
}

TEST_CASE("server chat hooks: outside the receive loop a /command line is not touched") {
    Rig r;
    const std::string text = "/give Wood 5";
    const ChatText t{text.data(), text.size()};
    uint8_t res[16] = {};
    g_d.log_add(res, r.fs.sys.data(), 0, &t, 0, 0, kAdminHandle, kAdmin, nullptr);
    REQUIRE(g_added.size() == 1);
    CHECK(g_added[0] == text);
    CHECK(g_ran.empty());
}

TEST_CASE("server chat hooks: a reply is not narrowed when the newest line turns out to be someone else's, and says so") {
    Rig r;
    g_incoming = {{kAdminHandle, kAdmin, "/help"}};
    r.receive();
    REQUIRE(g_ran.size() == 1);
    g_ran[0].reply("help text");
    g_other_line = "a line another thread added";
    r.receive();
    CHECK((r.fs.tail_mask() & ((1u << 1) | (1u << 5))) == 0);  // someone else's line is left as the game queued it
    CHECK(logged("could not be limited"));
}

TEST_CASE("server chat hooks: a command's replies stop after 10 lines in the chat") {
    Rig r;
    g_incoming = {{kAdminHandle, kAdmin, "/help"}};
    r.receive();
    REQUIRE(g_ran.size() == 1);
    for (int i = 0; i < 15; ++i) g_ran[0].reply("line " + std::to_string(i));
    r.receive();
    CHECK(g_added.size() == 11);
    CHECK(g_added.back().find("more in the server log") != std::string::npos);
}

TEST_CASE("server chat hooks: a reply whose sender's connection is gone is dropped, never shown to others") {
    Rig r;
    g_incoming = {{kAdminHandle, kAdmin, "/help"}};
    r.receive();
    REQUIRE(g_ran.size() == 1);
    r.fs.connection(kAdminHandle + 0x40, 0x85, kPlayer);  // the admin left; the record is reused by a new connection
    g_ran[0].reply("help text");
    r.receive();
    CHECK(g_added.empty());
    CHECK(logged("dropped"));
}

TEST_CASE("server chat hooks: a record that still holds the handle but another account (a stale peer) drops the reply") {
    Rig r;
    g_incoming = {{kAdminHandle, kAdmin, "/help"}};
    r.receive();
    REQUIRE(g_ran.size() == 1);
    r.fs.connection(kAdminHandle, 0x81, kPlayer);
    g_ran[0].reply("help text");
    r.receive();
    CHECK(g_added.empty());
}

TEST_CASE("server chat hooks: a connection record whose handle field is 0 has no reader: the reply is dropped, logged") {
    Rig r;
    g_incoming = {{kAdminHandle, kAdmin, "/help"}};
    r.receive();
    REQUIRE(g_ran.size() == 1);
    const uint32_t cleared = 0;  // the game cleared the record's handle (the player left); peer and account stay
    std::memcpy(r.fs.server.data() + *connection_record_offset(kAdminHandle), &cleared, 4);
    g_ran[0].reply("help text");
    r.receive();
    CHECK(g_added.empty());
    CHECK(logged("no chat reader for connection 0x42"));
    CHECK(logged("reply dropped: help text"));
}

TEST_CASE("server chat hooks: a reply whose text the game changed is not narrowed (it may not be ours), and says so") {
    Rig r;
    g_incoming = {{kAdminHandle, kAdmin, "/help"}};
    r.receive();
    REQUIRE(g_ran.size() == 1);
    g_ran[0].reply("help text");
    g_game_edits = true;  // the game keeps "[EMF] help tex"
    const int32_t player_before = r.fs.pending(1);
    r.receive();
    REQUIRE(g_added.size() == 1);
    CHECK((r.fs.tail_mask() & ((1u << 1) | (1u << 5))) == 0);  // left as the game queued it: no reader marked done
    CHECK(r.fs.pending(1) > player_before);                     // nor any count lowered
    CHECK(logged("could not be limited to its sender"));
    CHECK(logged("the game changed its text"));
}

TEST_CASE("server chat hooks: any player's /command marked for players runs, restricted; an admin's is not restricted") {
    Rig r;
    g_incoming = {{kPlayerHandle, kPlayer, "/where"}, {kAdminHandle, kAdmin, "/where"}, {kPlayerHandle, kPlayer, "/give Wood 1"}};
    r.receive();
    REQUIRE(g_ran.size() == 2);
    CHECK(g_ran[0].line == "where");
    CHECK(g_ran[0].sender.account == kPlayer);
    CHECK(g_ran[0].sender.restricted);  // a player who is no admin: only players' commands, checked again when it runs
    CHECK(g_ran[1].sender.account == kAdmin);
    CHECK_FALSE(g_ran[1].sender.restricted);
    REQUIRE(g_added.size() == 1);  // the refusal of /give, to that player only
    CHECK(g_added[0].find("[EMF] not run") == 0);
    CHECK(g_added[0].find("/help") != std::string::npos);  // where to find the commands they may run
    CHECK((r.fs.tail_mask() & (1u << 5)) != 0);
}

TEST_CASE("server chat hooks: a player who is no admin cannot flood the server: 5 commands per 10 s, one note, the rest dropped") {
    Rig r;
    for (int i = 0; i < 8; ++i) g_incoming.push_back({kPlayerHandle, kPlayer, "/where"});
    for (int i = 0; i < 8; ++i) g_incoming.push_back({kAdminHandle, kAdmin, "/where"});
    r.receive();
    int player_runs = 0, admin_runs = 0;
    for (const auto& x : g_ran) (x.sender.account == kPlayer ? player_runs : admin_runs)++;
    CHECK(player_runs == 5);
    CHECK(admin_runs == 8);  // admins are not limited
    REQUIRE(g_added.size() == 1);  // one note to the player, nothing for the dropped lines
    CHECK(g_added[0].find("[EMF] not run: too many commands") == 0);
    g_ran.clear();
    g_chat_now += 10001;
    g_incoming = {{kPlayerHandle, kPlayer, "/where"}};
    r.receive();
    CHECK(g_ran.size() == 1);  // a new window
}

// ---- API v19: mods' lines (chat_tell, chat_broadcast) and players' lines (on_chat) ----

TEST_CASE("server chat hooks v19: a mod's tell reaches only that player, as their speaker, without \"[EMF] \"") {
    Rig r;
    CHECK(server_chat_on());
    CHECK(chat_server() == nullptr);  // no receive loop ran yet
    REQUIRE(queue_chat_tell(kPlayerHandle, kPlayer, "Welcome to the server"));
    r.receive();
    CHECK(chat_server() == r.fs.server.data());
    REQUIRE(g_added.size() == 1);
    CHECK(g_added[0] == "Welcome to the server");
    CHECK(g_speakers[0] == std::make_pair(kPlayerHandle, kPlayer));
    CHECK((r.fs.tail_mask() & (1u << 5)) != 0);  // the admin does not get it
    CHECK((r.fs.tail_mask() & (1u << 1)) == 0);  // the player does
}

TEST_CASE("server chat hooks v19: a tell whose handle hint does not match its record finds the account's connection") {
    Rig r;
    REQUIRE(queue_chat_tell(0x40 /* no such connection */, kAdmin, "found by account"));
    r.receive();
    REQUIRE(g_added.size() == 1);
    CHECK(g_speakers[0] == std::make_pair(kAdminHandle, kAdmin));
    CHECK((r.fs.tail_mask() & (1u << 5)) == 0);
    CHECK((r.fs.tail_mask() & (1u << 1)) != 0);
}

TEST_CASE("server chat hooks v19: a tell for an account without a connection is dropped and logged, never shown") {
    Rig r;
    REQUIRE(queue_chat_tell(kPlayerHandle, 0xCC, "nobody"));
    r.receive();
    CHECK(g_added.empty());
    CHECK(logged("reply dropped: nobody"));
}

TEST_CASE("server chat hooks v19: a broadcast reaches every connected player, each line as its own speaker, for it alone") {
    Rig r;
    REQUIRE(queue_chat_everyone("Server restart in 5 minutes"));
    const int32_t admin_before = r.fs.pending(5), player_before = r.fs.pending(1);
    r.receive();
    REQUIRE(g_added.size() == 2);
    CHECK(g_added[0] == "Server restart in 5 minutes");
    CHECK(g_added[1] == "Server restart in 5 minutes");
    CHECK(g_speakers[0] == std::make_pair(kAdminHandle, kAdmin));  // record 2 before record 7
    CHECK(g_speakers[1] == std::make_pair(kPlayerHandle, kPlayer));
    // each reader got exactly one of the two lines (its own), the other was marked done for it
    CHECK(r.fs.pending(5) == admin_before + static_cast<int32_t>(line_size(27)));
    CHECK(r.fs.pending(1) == player_before + static_cast<int32_t>(line_size(27)));
}

TEST_CASE("server chat hooks v19: players' ordinary lines go to on_line; commands and refused commands do not") {
    Rig r;
    g_incoming = {{kPlayerHandle, kPlayer, "hello all"},
                  {kAdminHandle, kAdmin, "/give Wood 5"},
                  {kPlayerHandle, kPlayer, "/give Wood 1"},
                  {kPlayerHandle, kPlayer, "/shrug not a command\x1b"}};
    r.receive();
    REQUIRE(g_heard.size() == 2);
    CHECK(g_heard[0].account == kPlayer);
    CHECK(g_heard[0].handle == kPlayerHandle);
    CHECK(g_heard[0].text == "hello all");
    CHECK(g_heard[1].text == "/shrug not a command?");  // printable
}

TEST_CASE("server chat hooks v19: with commands off (no runner), lines still reach on_line") {
    Rig r;
    ServerChatDeps deps;
    deps.on_line = [](ChatLine l) { g_heard.push_back(std::move(l)); };
    g_d = server_chat_for_test(fake_receive, fake_add, std::move(deps));
    g_incoming = {{kPlayerHandle, kPlayer, "/give Wood 5"}};
    r.receive();
    REQUIRE(g_heard.size() == 1);
    CHECK(g_heard[0].text == "/give Wood 5");
    REQUIRE(g_added.size() == 1);  // and the game adds the line as usual
}

TEST_CASE("server chat hooks: at most 32 lines are added per receive loop run (the game's chat pool evicts its oldest lines); the rest wait, in order") {
    Rig r;
    for (int i = 0; i < 40; ++i) REQUIRE(queue_chat_tell(kPlayerHandle, kPlayer, "line " + std::to_string(i)));
    r.receive();
    REQUIRE(g_added.size() == 32);
    CHECK(g_added[31] == "line 31");
    r.receive();
    REQUIRE(g_added.size() == 40);
    CHECK(g_added[32] == "line 32");
    CHECK(g_added[39] == "line 39");
    // a broadcast counts once per connected player and is never split
    for (int i = 0; i < 31; ++i) REQUIRE(queue_chat_tell(kPlayerHandle, kPlayer, "x"));
    REQUIRE(queue_chat_everyone("for all"));
    r.receive();
    CHECK(g_added.size() == 40 + 31);
    r.receive();
    CHECK(g_added.size() == 40 + 31 + 2);
}
