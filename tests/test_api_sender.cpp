// API v15: command_sender (who typed the running command: a player in chat, or the console).
#include <doctest.h>

#include <string>

#include "core/api.h"
#include "core/api_sizes.h"
#include "core/commands.h"
#include "emf/emf.hpp"

using namespace emf::core;

namespace {
bool g_ok = false;
EmfCommandSender g_seen{};
void me(EmfWorld*, int, const char* const*, void*) {
    EmfCommandSender s{};
    s.size = sizeof s;
    g_ok = api_table()->command_sender(&s);
    g_seen = s;
}
EmfWorld* const kWorld = reinterpret_cast<EmfWorld*>(0x4000);
}  // namespace

TEST_CASE("api v15: command_sender tells who typed the running command; false outside commands; has_v15") {
    const EmfApi* a = api_table();
    REQUIRE(a->version >= 15u);
    EmfCommandSender outside{};
    outside.size = sizeof outside;
    CHECK_FALSE(a->command_sender(&outside));
    CommandCenter c;
    REQUIRE(c.add("me", "", me, nullptr, nullptr));
    CommandSender from;
    from.from_chat = true;
    from.account = 0xABCDEF0123456789ull;
    from.handle = 0x81;
    from.entity = 7;
    c.enqueue("me", 1000, from);
    c.run_pending(kWorld, nullptr, 1000);
    CHECK(g_ok);
    CHECK(g_seen.from_chat == 1);
    CHECK(g_seen.account == 0xABCDEF0123456789ull);
    CHECK(g_seen.entity == 7u);
    c.enqueue("me", 1000);  // typed into the console
    c.run_pending(kWorld, nullptr, 1000);
    CHECK(g_ok);
    CHECK(g_seen.from_chat == 0);
    CHECK(g_seen.entity == 0u);
    EmfCommandSender tiny{};
    tiny.size = 4;
    CHECK_FALSE(a->command_sender(&tiny));

    emf::api_ref() = a;
    CHECK(emf::has_v15());
    EmfApi old = *a;
    old.size = static_cast<uint32_t>(offsetof(EmfApi, command_sender));
    emf::api_ref() = &old;
    CHECK_FALSE(emf::has_v15());
    emf::api_ref() = nullptr;
}

namespace {
uint32_t g_size = 0;
bool g_sized_ok = false;
void sized(EmfWorld*, int, const char* const*, void*) {
    EmfCommandSender s{};
    s.size = g_size;
    g_sized_ok = api_table()->command_sender(&s);
}
}  // namespace

TEST_CASE("api sizes: command_sender takes the v15 size; less is refused") {
    CommandCenter c;
    REQUIRE(c.add("sized", "", sized, nullptr, nullptr));
    g_size = kCommandSenderV15Size;
    c.enqueue("sized", 1000);
    c.run_pending(kWorld, nullptr, 1000);
    CHECK(g_sized_ok);
    g_size = kCommandSenderV15Size - 1;
    c.enqueue("sized", 1000);
    c.run_pending(kWorld, nullptr, 1000);
    CHECK_FALSE(g_sized_ok);
}
