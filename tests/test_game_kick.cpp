// Refusing a join: the game's kick (handleAccountAction with a Kick event) and the connection's admin bit
// (src/core/game_kick.h).
#include <doctest.h>

#include <cstring>
#include <vector>

#include "core/game_kick.h"
#include "core/server_chat.h"

namespace {
uint8_t* g_seen_server = nullptr;
uint8_t g_seen_event[24];
int g_calls = 0;
void __fastcall fake_handler(uint8_t* server, const uint8_t* ev) {
    g_seen_server = server;
    std::memcpy(g_seen_event, ev, sizeof g_seen_event);
    ++g_calls;
}
void __fastcall faulting_handler(uint8_t*, const uint8_t*) {
    volatile int* p = nullptr;
    *p = 1;
}
}  // namespace

TEST_CASE("game kick: the event is the game's 24-byte Kick for the account") {
    const auto ev = emf::core::kick_event(0x1122334455667788ull);
    REQUIRE(ev.size() == 24);
    uint64_t account = 0;
    std::memcpy(&account, ev.data() + 8, 8);
    CHECK(account == 0x1122334455667788ull);
    uint32_t group = 1;
    std::memcpy(&group, ev.data() + 0x10, 4);
    CHECK(group == 0);
    CHECK(ev[0x14] == 0);  // Kick; never Ban (1) or UnBan (2)
    for (size_t i = 0; i < 8; ++i) CHECK(ev[i] == 0);
}

TEST_CASE("game kick: the handler gets the server and the event; nothing without a server; a fault is false") {
    uint8_t server[64] = {};
    g_calls = 0;
    CHECK(emf::core::kick_account(&fake_handler, server, 0xABCDull));
    CHECK(g_calls == 1);
    CHECK(g_seen_server == server);
    uint64_t account = 0;
    std::memcpy(&account, g_seen_event + 8, 8);
    CHECK(account == 0xABCDull);
    CHECK(g_seen_event[0x14] == 0);
    CHECK_FALSE(emf::core::kick_account(&fake_handler, nullptr, 0xABCDull));
    CHECK_FALSE(emf::core::kick_account(&fake_handler, server, 0));
    CHECK_FALSE(emf::core::kick_account(nullptr, server, 0xABCDull));
    CHECK(g_calls == 1);
    CHECK_FALSE(emf::core::kick_account(&faulting_handler, server, 0xABCDull));
}

namespace {
struct Records {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(emf::core::server_layout::kConnectionSize *
                                                          emf::core::server_layout::kConnectionCount,
                                                      0);
    void put(uint32_t i, uint32_t handle, uint64_t account, uint8_t perms) {
        using namespace emf::core::server_layout;
        uint8_t* r = bytes.data() + i * kConnectionSize;
        std::memcpy(r, &handle, 4);
        std::memcpy(r + kConnectionAccount, &account, 8);
        r[kConnectionPerms] = perms;
    }
};
}  // namespace

TEST_CASE("game kick: the admin bit is read from the record that holds that connection only") {
    using emf::core::connection_is_admin;
    Records rs;
    rs.put(1, 0x41, 0xAA, 0x00);  // a player
    rs.put(2, 0x42, 0xBB, 0x3F);  // an admin (CanKickBan and the rest)
    rs.put(3, 0x45, 0xCC, 0x01);  // a stale record: its handle names record 5, not 3
    const uint8_t* r = rs.bytes.data();
    CHECK(connection_is_admin(r, 0x41, 0xAA) == std::optional<bool>(false));
    CHECK(connection_is_admin(r, 0x42, 0xBB) == std::optional<bool>(true));
    CHECK(connection_is_admin(r, 0, 0xAA) == std::optional<bool>(false));  // the handle not known: by the account
    CHECK(connection_is_admin(r, 0, 0xBB) == std::optional<bool>(true));
    CHECK_FALSE(connection_is_admin(r, 0x45, 0xCC).has_value());
    CHECK_FALSE(connection_is_admin(r, 0, 0xCC).has_value());
    CHECK_FALSE(connection_is_admin(r, 0, 0xDD).has_value());
    CHECK_FALSE(connection_is_admin(r, 0x41, 0).has_value());
    CHECK_FALSE(connection_is_admin(r, 0, 0).has_value());
    CHECK_FALSE(connection_is_admin(nullptr, 0x41, 0xAA).has_value());
}

TEST_CASE("game kick: after a quick reconnect the admin bit is the new connection's, never the old record's") {
    using emf::core::connection_is_admin;
    Records rs;
    const uint8_t* r = rs.bytes.data();
    // the old connection (record 1, an admin then) is not cleared yet; the player is back on record 4, demoted
    rs.put(1, 0x41, 0xAA, 0x3F);
    rs.put(4, 0x44, 0xAA, 0x2E);
    CHECK(connection_is_admin(r, 0x44, 0xAA) == std::optional<bool>(false));  // the line's own connection
    CHECK(connection_is_admin(r, 0x41, 0xAA) == std::optional<bool>(true));   // the old one, when asked for it
    // without a handle the two records disagree: not known (an admin check says no), never the first record's bit
    CHECK_FALSE(connection_is_admin(r, 0, 0xAA).has_value());
    // a handle whose record holds no such connection: by the account (the same two records)
    CHECK_FALSE(connection_is_admin(r, 0x47, 0xAA).has_value());
    // promoted on the new connection instead: both records say admin
    rs.put(4, 0x44, 0xAA, 0x3F);
    CHECK(connection_is_admin(r, 0, 0xAA) == std::optional<bool>(true));
    // the old record cleared: the new one alone
    rs.put(1, 0, 0, 0);
    rs.put(4, 0x44, 0xAA, 0x2E);
    CHECK(connection_is_admin(r, 0, 0xAA) == std::optional<bool>(false));
    CHECK(connection_is_admin(r, 0x41, 0xAA) == std::optional<bool>(false));  // the old handle: by the account
}
