// API v19: chat_tell, chat_broadcast, on_chat and request_save through the core state, with the server chat glue and
// the game's requestServerSave faked (the hooks themselves: test_server_chat_hooks.cpp).
#include <windows.h>

#include <doctest.h>

#include <cstring>
#include <string>
#include <tuple>
#include <vector>

#include "common/offsets.h"
#include "core/api.h"
#include "core/api_sizes.h"
#include "emf/emf.hpp"
#include "fake_world.h"

using namespace emf;
using namespace emf::core;
using emf::test::FakeWorld;
using emf::test::SingletonStore;

namespace {

uint64_t g_now = 900000;
std::vector<std::string> g_lines, g_notes;
std::vector<std::tuple<uint32_t, uint64_t, std::string>> g_tells;  // handle hint, account, line
std::vector<std::string> g_everyone;
bool g_chat_on = true;
bool g_queue_ok = true;
std::vector<void*> g_saves;  // the instance each requestServerSave call got

void fake_save(void* inst) { g_saves.push_back(inst); }
void faulting_save(void*) { *static_cast<volatile int*>(nullptr) = 1; }

struct Fixture {
    CoreState state;
    FakeWorld server, client;
    SingletonStore server_ss, client_ss;
    std::vector<uint8_t> infos = std::vector<uint8_t>(kPlayerInfoSize * 8, 0);
    uint8_t lpd[8] = {};
    explicit Fixture(bool dedicated = true) {
        g_lines.clear();
        g_notes.clear();
        g_tells.clear();
        g_everyone.clear();
        g_saves.clear();
        g_chat_on = true;
        g_queue_ok = true;
        g_now += 60000;  // a fresh chat budget window for every test
        state.log_line = [](const std::string& s) { g_lines.push_back(s); };
        state.now_ms = [] { return g_now; };
        state.notify = [](const std::string& s) { g_notes.push_back(s); };
        state.exe.server = dedicated;
        state.chat_on = [] { return g_chat_on; };
        state.chat_tell_line = [](uint32_t h, uint64_t a, const std::string& l) {
            if (!g_queue_ok) return false;
            g_tells.emplace_back(h, a, l);
            return true;
        };
        state.chat_everyone_line = [](const std::string& l) {
            if (!g_queue_ok) return false;
            g_everyone.push_back(l);
            return true;
        };
        server_ss.plant(server, "BasicPlayerInfos", infos.data());
        client.put<uint8_t>(off::kKind, EMF_WORLD_CLIENT);
        client_ss.plant(client, "LocalPlayerData", lpd);
        state.tracker.on_tick(w(), EMF_WORLD_SERVER, g_now);
        set_core_state(&state);
    }
    ~Fixture() { set_core_state(nullptr); }
    EmfWorld* w() { return reinterpret_cast<EmfWorld*>(server.p()); }
    EmfWorld* c() { return reinterpret_cast<EmfWorld*>(client.p()); }
    // player `entity` (1..8) with connection handle `handle` and `account` in BasicPlayerInfos
    void player(uint32_t entity, uint32_t handle, uint64_t account) {
        uint8_t* e = infos.data() + (entity - 1) * kPlayerInfoSize;
        std::memcpy(e, &entity, 4);
        std::memcpy(e + 4, &handle, 4);
        std::memcpy(e + 8, &account, 8);
    }
    // this game hosts the world and its own player is `id` (a client world with LocalPlayerData)
    void host_player(uint32_t id) {
        state.exe.server = false;
        std::memcpy(lpd + 4, &id, 4);
        state.tracker.on_tick(c(), EMF_WORLD_CLIENT, g_now);
    }
    template <class F>
    void in_callback(F f) {
        auto* p = new F(std::move(f));
        api_table()->run_on_world(EMF_WORLD_SERVER, [](EmfWorld* w, void* u) {
            auto* fn = static_cast<F*>(u);
            (*fn)(w);
            delete fn;
        }, p);
        step();
    }
    void step() { state.dispatcher.run(w(), EMF_WORLD_SERVER, [](const std::string&) {}); }
};

bool logged(const std::string& needle) {
    for (auto& l : g_lines)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}

struct Heard {
    EmfWorld* world;
    uint64_t account;
    uint32_t entity;
    std::string text;
};
std::vector<Heard> g_heard;
void hear(EmfWorld* w, uint64_t account, uint32_t entity, const char* text, void*) {
    g_heard.push_back({w, account, entity, text});
}
void faulting_listener(EmfWorld*, uint64_t, uint32_t, const char*, void*) { *static_cast<volatile int*>(nullptr) = 1; }

}  // namespace

TEST_CASE("api v19: the table has chat_tell, chat_broadcast, player_name, on_chat, request_save after damage") {
    const EmfApi* api = api_table();
    CHECK(api->version >= 19u);
    CHECK(EMF_API_VERSION >= 19);
    CHECK(api->size == sizeof(EmfApi));
    CHECK(kApiV18Size == offsetof(EmfApi, chat_tell));
    CHECK(kApiV19Size == offsetof(EmfApi, teleport));  // v20 appends teleport
    CHECK(api->chat_tell);
    CHECK(api->chat_broadcast);
    CHECK(api->player_name);
    CHECK(api->on_chat);
    CHECK(api->request_save);
    CHECK(EMF_CHAT_MAX_LINES == static_cast<unsigned>(kMaxChatReplyLines));
    CHECK(EMF_CHAT_LINE_BYTES == kChatMaxBytes);
}

TEST_CASE("chat_tell: in a server callback, each line goes to the outbox with the player's handle and account") {
    Fixture f;
    f.player(3, 0x41, 0xBEEF);
    bool ok = false;
    f.in_callback([&](EmfWorld* w) { ok = api_table()->chat_tell(w, 3, "Welcome!\nRules: be nice\x07"); });
    CHECK(ok);
    REQUIRE(g_tells.size() == 2);
    CHECK(g_tells[0] == std::make_tuple(0x41u, 0xBEEFull, std::string("Welcome!")));
    CHECK(std::get<2>(g_tells[1]) == "Rules: be nice?");  // control characters become '?'
    CHECK(g_notes.empty());
    CHECK(logged("chat to player 3: Welcome!"));
}

TEST_CASE("chat_tell: at most 10 lines per call, then one '...' line") {
    Fixture f;
    f.player(1, 0x40, 0xA1);
    std::string text;
    for (int i = 0; i < 14; ++i) text += "line " + std::to_string(i) + "\n";
    f.in_callback([&](EmfWorld* w) { CHECK(api_table()->chat_tell(w, 1, text.c_str())); });
    REQUIRE(g_tells.size() == 11);
    CHECK(std::get<2>(g_tells[9]) == "line 9");
    CHECK(std::get<2>(g_tells[10]) == "...");
}

TEST_CASE("chat_tell: refused for a client world, an unknown player, no text, or players' chat off; logged") {
    Fixture f;
    f.player(1, 0x40, 0xA1);
    f.in_callback([&](EmfWorld* w) {
        const EmfApi* a = api_table();
        CHECK_FALSE(a->chat_tell(f.c(), 1, "hi"));
        CHECK_FALSE(a->chat_tell(w, 5, "hi"));  // no entry for 5
        CHECK_FALSE(a->chat_tell(w, 1, ""));
        CHECK_FALSE(a->chat_tell(w, 1, nullptr));
        g_chat_on = false;
        CHECK_FALSE(a->chat_tell(w, 1, "hi"));
    });
    CHECK(g_tells.empty());
    CHECK(logged("not a server world"));
    CHECK(logged("no such player"));
    CHECK(logged("players' chat is off here"));
}

TEST_CASE("chat_tell: outside a callback it is sent after the server world's next step, if that world still runs") {
    Fixture f;
    f.player(2, 0x42, 0xB2);
    CHECK(api_table()->chat_tell(f.w(), 2, "later"));
    CHECK(g_tells.empty());
    f.step();
    REQUIRE(g_tells.size() == 1);
    CHECK(std::get<2>(g_tells[0]) == "later");
    CHECK(api_table()->chat_tell(f.w(), 2, "too late"));
    ++f.state.server_session;  // a new session before the next step: the line is for a world that is gone
    f.step();
    CHECK(g_tells.size() == 1);
    CHECK(logged("not sent"));
}

TEST_CASE("chat_tell: in a game you host, a line for the host is a notify (its own game does not show server lines)") {
    Fixture f(false);
    f.host_player(1);
    f.player(1, 0x40, 0xA1);
    f.player(2, 0x41, 0xA2);
    f.in_callback([&](EmfWorld* w) {
        CHECK(api_table()->chat_tell(w, 1, "for the host\nsecond"));
        CHECK(api_table()->chat_tell(w, 2, "for the guest"));
    });
    CHECK(g_notes == std::vector<std::string>{"for the host", "second"});
    REQUIRE(g_tells.size() == 1);
    CHECK(std::get<2>(g_tells[0]) == "for the guest");
    g_chat_on = false;  // without [chat] players=1 the host still gets its notes
    f.in_callback([&](EmfWorld* w) { CHECK(api_table()->chat_tell(w, 1, "still")); });
    CHECK(g_notes.back() == "still");
}

TEST_CASE("chat_tell / chat_broadcast: a mod may send 30 lines in 10 s; more are refused whole, then a new window") {
    Fixture f;
    f.player(1, 0x40, 0xA1);
    const std::string ten = "1\n2\n3\n4\n5\n6\n7\n8\n9\n10";
    f.in_callback([&](EmfWorld* w) {
        CHECK(api_table()->chat_tell(w, 1, ten.c_str()));
        CHECK(api_table()->chat_broadcast(ten.c_str()));
        CHECK(api_table()->chat_tell(w, 1, "a\nb\nc\nd\ne\nf\ng\nh\ni"));  // 29 lines
        CHECK_FALSE(api_table()->chat_tell(w, 1, "x\ny"));                  // 31: refused, nothing sent
        CHECK(api_table()->chat_tell(w, 1, "z"));                          // 30
        CHECK_FALSE(api_table()->chat_broadcast("one more"));
    });
    CHECK(g_tells.size() == 20);
    CHECK(g_everyone.size() == 10);
    CHECK(logged("more than 30 chat lines in 10 s"));
    g_now += 10000;
    f.in_callback([&](EmfWorld* w) { CHECK(api_table()->chat_tell(w, 1, "new window")); });
    CHECK(std::get<2>(g_tells.back()) == "new window");
}

TEST_CASE("chat_tell / chat_broadcast: refused lines use none of the mod's 30 lines in 10 s") {
    Fixture f;
    f.player(1, 0x40, 0xA1);
    const std::string ten = "1\n2\n3\n4\n5\n6\n7\n8\n9\n10";
    f.in_callback([&](EmfWorld* w) {
        const EmfApi* a = api_table();
        for (int i = 0; i < 4; ++i) CHECK_FALSE(a->chat_tell(w, 5, ten.c_str()));  // no such player
        g_chat_on = false;
        for (int i = 0; i < 4; ++i) CHECK_FALSE(a->chat_tell(w, 1, ten.c_str()));    // players' chat off
        for (int i = 0; i < 4; ++i) CHECK_FALSE(a->chat_broadcast(ten.c_str()));     // the same, on a dedicated server
        g_chat_on = true;
        g_queue_ok = false;
        CHECK_FALSE(a->chat_tell(w, 1, ten.c_str()));  // the outbox is full
        CHECK_FALSE(a->chat_broadcast(ten.c_str()));
        g_queue_ok = true;
    });
    // a line for a world that is gone before the next step is not sent either
    CHECK(api_table()->chat_tell(f.w(), 1, ten.c_str()));
    ++f.state.server_session;
    f.step();
    CHECK(g_tells.empty());
    f.in_callback([&](EmfWorld* w) {
        CHECK(api_table()->chat_tell(w, 1, ten.c_str()));  // all 30 are still there
        CHECK(api_table()->chat_broadcast(ten.c_str()));
        CHECK(api_table()->chat_tell(w, 1, ten.c_str()));
        CHECK_FALSE(api_table()->chat_tell(w, 1, "31"));
    });
    CHECK(g_tells.size() == 20);
    CHECK(g_everyone.size() == 10);
}

TEST_CASE("chat_tell: a full outbox refuses (logged)") {
    Fixture f;
    f.player(1, 0x40, 0xA1);
    g_queue_ok = false;
    f.in_callback([&](EmfWorld* w) { CHECK_FALSE(api_table()->chat_tell(w, 1, "hi")); });
    CHECK(logged("too many chat lines are waiting"));
}

TEST_CASE("chat_broadcast: every connected player on a dedicated server; any thread") {
    Fixture f;
    CHECK(api_table()->chat_broadcast("Server restart in 10 minutes\nsave your work"));
    CHECK(g_everyone == std::vector<std::string>{"Server restart in 10 minutes", "save your work"});
    CHECK(g_notes.empty());
    CHECK(logged("chat to everyone: Server restart in 10 minutes"));
    CHECK_FALSE(api_table()->chat_broadcast(""));
    g_chat_on = false;  // a dedicated server whose chat functions were not found
    CHECK_FALSE(api_table()->chat_broadcast("nobody"));
}

TEST_CASE("chat_broadcast: in a game you host the host gets a notify and the guests the line; without players' chat only the host") {
    Fixture f(false);
    f.host_player(1);
    CHECK(api_table()->chat_broadcast("hello all"));
    CHECK(g_notes == std::vector<std::string>{"hello all"});
    CHECK(g_everyone == std::vector<std::string>{"hello all"});
    g_chat_on = false;
    CHECK(api_table()->chat_broadcast("host only"));
    CHECK(g_notes.back() == "host only");
    CHECK(g_everyone.size() == 1);
    CHECK(logged("shown to the host only"));
}

TEST_CASE("on_chat: refused while players' chat is off; lines reach every listener on the server world with the entity") {
    Fixture f;
    g_heard.clear();
    g_chat_on = false;
    CHECK_FALSE(api_table()->on_chat(hear, nullptr));
    CHECK(logged("on_chat refused"));
    note_chat_line({0xA1, 0x40, "nobody listens"});  // dropped at once: no listener
    g_chat_on = true;
    REQUIRE(api_table()->on_chat(hear, nullptr));
    CHECK_FALSE(api_table()->on_chat(nullptr, nullptr));
    f.state.entity_by_account = [](uint64_t a) { return a == 0xA1 ? 3u : 0u; };
    note_chat_line({0xA1, 0x40, "hello"});
    note_chat_line({0xC3, 0x44, "who am i"});
    CHECK(g_heard.empty());  // not before the server world's step
    pump_chat_lines(f.w());
    REQUIRE(g_heard.size() == 2);
    CHECK(g_heard[0].world == f.w());
    CHECK(g_heard[0].account == 0xA1);
    CHECK(g_heard[0].entity == 3u);
    CHECK(g_heard[0].text == "hello");
    CHECK(g_heard[1].entity == 0u);  // could not be told
    pump_chat_lines(f.w());
    CHECK(g_heard.size() == 2);  // each line once
}

TEST_CASE("on_chat: at most 256 lines wait for the step (the rest dropped, logged); a faulting listener is switched off") {
    Fixture f;
    g_heard.clear();
    REQUIRE(api_table()->on_chat(faulting_listener, nullptr));
    REQUIRE(api_table()->on_chat(hear, nullptr));
    for (int i = 0; i < 300; ++i) note_chat_line({0xA1, 0x40, "spam " + std::to_string(i)});
    CHECK(logged("more than 256"));
    pump_chat_lines(f.w());
    CHECK(g_heard.size() == 256);
    CHECK(logged("faulted and was switched off"));
    note_chat_line({0xA1, 0x40, "after"});
    pump_chat_lines(f.w());
    CHECK(g_heard.back().text == "after");
}

TEST_CASE("request_save: off without the game's function; needs this session's instance; runs on the server thread") {
    Fixture f;
    CHECK_FALSE(api_table()->request_save());
    CHECK(logged("RequestServerSave signature is not usable"));
    f.state.request_save = fake_save;
    f.in_callback([](EmfWorld*) { CHECK_FALSE(api_table()->request_save()); });  // no load seen yet
    CHECK(logged("no world save was loaded in this session"));
    int inst = 0;
    f.state.save_instance = &inst;
    f.state.save_instance_session = f.state.server_session.load();
    f.in_callback([](EmfWorld*) { CHECK(api_table()->request_save()); });
    REQUIRE(g_saves.size() == 1);
    CHECK(g_saves[0] == &inst);
    CHECK(logged("the game was asked to save"));
    // from another thread (a command file, the console): after the server world's next step
    CHECK(api_table()->request_save());
    CHECK(g_saves.size() == 1);
    f.step();
    CHECK(g_saves.size() == 2);
    // an instance from an earlier session is never used
    ++f.state.server_session;
    f.in_callback([](EmfWorld*) { CHECK_FALSE(api_table()->request_save()); });
    CHECK(g_saves.size() == 2);
}

TEST_CASE("request_save: a fault in the game's function is caught and logged; no server world refuses") {
    Fixture f;
    int inst = 0;
    f.state.request_save = faulting_save;
    f.state.save_instance = &inst;
    f.state.save_instance_session = f.state.server_session.load();
    f.in_callback([](EmfWorld*) { CHECK_FALSE(api_table()->request_save()); });
    CHECK(logged("faulted"));
    CoreState none;
    none.log_line = [](const std::string& s) { g_lines.push_back(s); };
    none.now_ms = [] { return g_now; };
    none.request_save = fake_save;
    set_core_state(&none);
    CHECK_FALSE(api_table()->request_save());
    CHECK(logged("this game hosts no world"));
    set_core_state(&f.state);
}

TEST_CASE("emf.hpp v19 helpers: tell, broadcast, player_name, on_chat, request_save through the table") {
    Fixture f;
    f.player(1, 0x40, 0xA1);
    REQUIRE(emf::init(api_table()));
    CHECK(emf::has_v19());
    f.in_callback([](EmfWorld* w) { CHECK(emf::tell(emf::World(w), 1, "100% sure")); });  // no format: '%' stays
    REQUIRE(g_tells.size() == 1);
    CHECK(std::get<2>(g_tells[0]) == "100% sure");
    CHECK(emf::broadcast("all"));
    CHECK_FALSE(emf::player_name(emf::World(f.w()), 1).has_value());  // no connection records here
    g_heard.clear();
    std::vector<std::string> said;
    CHECK(emf::on_chat("test", [&said](emf::World, uint64_t, uint32_t, const std::string& t) { said.push_back(t); }));
    note_chat_line({0xA1, 0x40, "hi"});
    pump_chat_lines(f.w());
    CHECK(said == std::vector<std::string>{"hi"});
    CHECK_FALSE(emf::request_save());
}

TEST_CASE("player_name: the name in the player's connection record; false without records, for a non-player or a client world") {
    Fixture f;
    f.player(2, 0x41, 0xB2);
    alignas(8) static const char ember[8] = "Ember";
    static std::vector<uint8_t> records;
    records.assign(static_cast<size_t>(server_layout::kConnectionSize) * server_layout::kConnectionCount, 0);
    uint8_t* rec = records.data() + 1 * server_layout::kConnectionSize;  // handle 0x41: record 1
    const uint32_t handle = 0x41, peer = 0x82;
    const uint64_t account = 0xB2, ptr = reinterpret_cast<uint64_t>(ember), len = 5;
    std::memcpy(rec, &handle, 4);
    std::memcpy(rec + server_layout::kConnectionPeer, &peer, 4);
    std::memcpy(rec + server_layout::kConnectionAccount, &account, 8);
    std::memcpy(rec + server_layout::kConnectionName, &ptr, 8);
    std::memcpy(rec + server_layout::kConnectionNameLength, &len, 8);
    f.state.read_connections = [](std::vector<uint8_t>* out) {
        *out = records;
        return true;
    };
    char buf[64] = "junk";
    CHECK(api_table()->player_name(f.w(), 2, buf, sizeof buf));
    CHECK(std::string(buf) == "Ember");
    char tiny[4];
    CHECK(api_table()->player_name(f.w(), 2, tiny, sizeof tiny));
    CHECK(std::string(tiny) == "Emb");  // cut to size - 1
    CHECK_FALSE(api_table()->player_name(f.w(), 3, buf, sizeof buf));  // no such player
    CHECK(buf[0] == '\0');
    CHECK_FALSE(api_table()->player_name(f.c(), 2, buf, sizeof buf));  // a client world
    f.state.read_connections = [](std::vector<uint8_t>*) { return false; };  // players' chat is off: no records
    CHECK_FALSE(api_table()->player_name(f.w(), 2, buf, sizeof buf));
}

TEST_CASE("chat_broadcast on a joined client (this game hosts no server world): false, and no note") {
    Fixture f(false);
    f.host_player(1);
    g_now += 5000;  // the server world is gone (someone else's game); only the client world steps
    f.state.tracker.on_tick(f.c(), EMF_WORLD_CLIENT, g_now);
    CHECK_FALSE(api_table()->chat_broadcast("not from a client"));
    CHECK(g_notes.empty());
    CHECK(g_everyone.empty());
    CHECK(logged("this game hosts no world"));
}
