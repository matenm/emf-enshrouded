// API v17: commands any player may run from the game's chat (EMF_COMMAND_PLAYERS), the chat policy that decides who
// may run what, and the run-time check behind it.
#include <doctest.h>

#include <string>
#include <vector>

#include "core/api.h"
#include "core/api_sizes.h"
#include "core/bridge/console_runner.h"
#include "core/commands.h"
#include "core/server_chat.h"
#include "emf/emf.hpp"
#include "emf/query.hpp"

using namespace emf::core;

namespace {
EmfWorld* const kWorld = reinterpret_cast<EmfWorld*>(0x4000);
std::vector<std::string> g_ran;
void ran(EmfWorld*, int, const char* const* argv, void*) { g_ran.emplace_back(argv[0]); }
void other(EmfWorld*, int, const char* const* argv, void*) { g_ran.emplace_back(std::string("other ") + argv[0]); }

bool known(std::string_view w) { return w == "give" || w == "help" || w == "raids.status" || w == "loot.toggle"; }
bool players(std::string_view w) { return w == "raids.status" || w == "loot.toggle" || w == "help"; }
}  // namespace

TEST_CASE("chat policy matrix: admins run everything, other players only commands marked for players") {
    const auto admins = parse_chat_policy("0xaa", false);
    const auto everyone = parse_chat_policy("", true);
    const auto nobody = parse_chat_policy("", false);
    std::string line;
    struct Row {
        const ChatPolicy* policy;
        uint64_t account;
        const char* text;
        ChatAction want;
    };
    const Row rows[] = {
        {&admins, 0xaa, "/give Wood 5", ChatAction::Run},          // an admin: an admin command
        {&admins, 0xaa, "/raids.status", ChatAction::Run},         // an admin: a players' command
        {&admins, 0xbb, "/give Wood 5", ChatAction::Refuse},       // another player: an admin command
        {&admins, 0xbb, "/raids.status", ChatAction::Run},         // another player: a players' command
        {&admins, 0xbb, "/LOOT.Toggle off", ChatAction::Run},      // the name ignores case
        {&nobody, 0xbb, "/loot.toggle", ChatAction::Run},          // no admins at all: players' commands still run
        {&nobody, 0xbb, "/give Wood 5", ChatAction::Refuse},
        {&everyone, 0xbb, "/give Wood 5", ChatAction::Run},        // everyone=1 opens every command
        {&admins, 0xbb, "/dance", ChatAction::Pass},               // not an EMF command: chat
        {&admins, 0xbb, "raids.status", ChatAction::Pass},         // no slash: chat
        {&admins, 0xbb, "/ raids.status", ChatAction::Pass},
    };
    for (const auto& r : rows) {
        CAPTURE(r.text);
        CAPTURE(r.account);
        CHECK(judge_chat(r.text, r.account, *r.policy, known, &line, players) == r.want);
    }
    CHECK(judge_chat("/LOOT.Toggle off", 0xbb, admins, known, &line, players) == ChatAction::Run);
    CHECK(line == "LOOT.Toggle off");
    // without the players' list (an older caller): only the account counts, as before v17
    CHECK(judge_chat("/raids.status", 0xbb, admins, known, &line) == ChatAction::Refuse);
}

TEST_CASE("commands: a command can be marked for players; for_players tells by name, ignoring case") {
    CommandCenter c;
    std::string why;
    REQUIRE(c.add("raids.status", "", ran, nullptr, &why, false, kCommandForPlayers));
    REQUIRE(c.add("raids.start", "", ran, nullptr, &why));
    CHECK(c.for_players("raids.status"));
    CHECK_FALSE(c.for_players("raids.start"));
    CHECK_FALSE(c.for_players("nope"));
    const auto all = c.entries();
    REQUIRE(all.size() == 2);
    CHECK(all[0].name == "raids.start");  // sorted by name
    CHECK_FALSE(all[0].players);
    CHECK(all[1].players);
}

TEST_CASE("commands: re-declaring a command with the same callback and user updates its help and flags; anything else is a duplicate") {
    CommandCenter c;
    std::string why;
    int owner = 0;
    REQUIRE(c.add("lua_mod.where", "old", ran, &owner, &why, false, kCommandForPlayers, true));
    CHECK(c.for_players("lua_mod.where"));
    // the same owner again (a script host after a reload), now without the players' mark
    CHECK(c.add("Lua_Mod.Where", "new", ran, &owner, &why, false, 0, true));
    CHECK_FALSE(c.for_players("lua_mod.where"));
    REQUIRE(c.list().size() == 1);
    CHECK(c.list()[0].second == "new");
    // another callback, or another user, may not take it over
    CHECK_FALSE(c.add("lua_mod.where", "x", other, &owner, &why, false, kCommandForPlayers, true));
    CHECK(why.find("already") != std::string::npos);
    int someone = 0;
    CHECK_FALSE(c.add("lua_mod.where", "x", ran, &someone, &why, false, kCommandForPlayers, true));
    // without the update permission (register_command, v6) the same owner is a duplicate too
    CHECK_FALSE(c.add("lua_mod.where", "x", ran, &owner, &why));
    CHECK_FALSE(c.for_players("lua_mod.where"));
}

TEST_CASE("commands: a restricted chat sender runs players' commands only, checked again when the line runs") {
    CommandCenter c;
    REQUIRE(c.add("raids.status", "", ran, nullptr, nullptr, false, kCommandForPlayers));
    REQUIRE(c.add("raids.start", "", ran, nullptr, nullptr));
    g_ran.clear();
    std::vector<std::string> told;
    CommandSender guest;
    guest.from_chat = true;
    guest.account = 0xbb;
    guest.restricted = true;
    c.enqueue("raids.status", 1000, guest, [&](const std::string& s) { told.push_back(s); });
    c.enqueue("raids.start", 1000, guest, [&](const std::string& s) { told.push_back(s); });
    CHECK(c.run_pending(kWorld, nullptr, 1000) == 1);
    CHECK(g_ran == std::vector<std::string>{"raids.status"});
    REQUIRE_FALSE(told.empty());
    CHECK(told.back().find("not run") == 0);
    CHECK(told.back().find("raids.start") != std::string::npos);
    CommandSender admin = guest;
    admin.restricted = false;
    c.enqueue("raids.start", 1000, admin);
    c.enqueue("raids.start", 1000);  // the console
    CHECK(c.run_pending(kWorld, nullptr, 1000) == 2);
}

TEST_CASE("api v17: register_command_ex adds a command; EMF_COMMAND_PLAYERS marks it for players; bad input refused") {
    CoreState state;
    std::vector<std::string> lines;
    state.log_line = [&](const std::string& s) { lines.push_back(s); };
    set_core_state(&state);
    const EmfApi* a = api_table();
    REQUIRE(a->version >= 17u);
    REQUIRE(a->register_command_ex);
    EmfCommandDef d{};
    d.size = sizeof d;
    d.name = "raids.status";
    d.help = ": the raid";
    d.cb = ran;
    d.flags = EMF_COMMAND_PLAYERS;
    CHECK(a->register_command_ex(&d));
    CHECK(state.commands.for_players("raids.status"));
    d.name = "raids.start";
    d.flags = 0;
    CHECK(a->register_command_ex(&d));
    CHECK_FALSE(state.commands.for_players("raids.start"));
    d.name = "raids.weird";
    d.flags = 0x80;  // a flag this core does not know (a newer mod)
    CHECK_FALSE(a->register_command_ex(&d));
    CHECK_FALSE(state.commands.for_players("raids.weird"));
    d.flags = 0;
    d.size = kCommandDefV17Size - 1;
    CHECK_FALSE(a->register_command_ex(&d));
    CHECK_FALSE(a->register_command_ex(nullptr));
    bool said = false;
    for (const auto& l : lines) said |= l.find("register_command_ex") != std::string::npos && l.find("flags") != std::string::npos;
    CHECK(said);
    // the same callback and user again: an update (a script host re-declaring after a reload)
    d.size = sizeof d;
    d.name = "raids.status";
    d.flags = 0;
    CHECK(a->register_command_ex(&d));
    CHECK_FALSE(state.commands.for_players("raids.status"));
    d.cb = other;  // someone else
    CHECK_FALSE(a->register_command_ex(&d));
    set_core_state(nullptr);
}

TEST_CASE("api sizes: EmfCommandDef (v17) is 40 bytes; register_command_ex takes that size") {
    CHECK(kCommandDefV17Size == 40u);
    CHECK(sizeof(EmfCommandDef) == 40u);
}

namespace {
std::vector<std::pair<std::string, uint32_t>> g_registered;
bool fake_register_ex(const EmfCommandDef* d) {
    g_registered.emplace_back(d->name, d->flags);
    return true;
}
bool fake_register(const char* name, const char*, EmfCommandCallback, void*) {
    g_registered.emplace_back(name, 0xFFFFFFFFu);
    return true;
}
std::vector<std::string> g_logged;
void fake_log(const char*, const char* fmt, ...) { g_logged.emplace_back(fmt); }
}  // namespace

TEST_CASE("emf::add_command with flags registers through register_command_ex; has_v17") {
    EmfApi api{};
    api.version = EMF_API_VERSION;
    api.size = sizeof api;
    api.register_command = fake_register;
    api.register_command_ex = fake_register_ex;
    api.log = fake_log;
    emf::api_ref() = &api;
    g_registered.clear();
    CHECK(emf::has_v17());
    CHECK(emf::add_command("raids.status", ": status", [](emf::World, const std::vector<std::string>&) {},
                           EMF_COMMAND_PLAYERS));
    CHECK(emf::add_command("raids.start", ": start", [](emf::World, const std::vector<std::string>&) {}));
    REQUIRE(g_registered.size() == 2);
    CHECK(g_registered[0] == std::make_pair(std::string("raids.status"), EMF_COMMAND_PLAYERS));
    CHECK(g_registered[1].second == 0xFFFFFFFFu);  // the plain v6 path
    // an older core: the command is still added, for admins only, and the log says so
    EmfApi old = api;
    old.size = static_cast<uint32_t>(offsetof(EmfApi, register_command_ex));
    emf::api_ref() = &old;
    CHECK_FALSE(emf::has_v17());
    g_logged.clear();
    CHECK(emf::add_command("raids.where", "", [](emf::World, const std::vector<std::string>&) {}, EMF_COMMAND_PLAYERS));
    CHECK(g_registered.back() == std::make_pair(std::string("raids.where"), 0xFFFFFFFFu));
    CHECK_FALSE(g_logged.empty());
    emf::api_ref() = nullptr;
}

TEST_CASE("cookbook: the command-for-every-player recipe compiles and registers with EMF_COMMAND_PLAYERS") {
    EmfApi api{};
    api.version = EMF_API_VERSION;
    api.size = sizeof api;
    api.register_command = fake_register;
    api.register_command_ex = fake_register_ex;
    api.log = fake_log;
    emf::api_ref() = &api;
    g_registered.clear();
    // docs/modding/cookbook.md, "A command every player may run from the chat"
    emf::add_command("mymod.where", ": where am I?", [](emf::World w, const std::vector<std::string>&) {
        EmfCommandSender s{};
        const uint32_t me = emf::command_sender(&s) && s.from_chat ? s.entity : emf::local_player();  // "you" = the sender
        if (const auto p = emf::position(w, me)) EMF_REPLY("you are at %.0f, %.0f, %.0f", p->x, p->y, p->z);
    }, EMF_COMMAND_PLAYERS);
    REQUIRE(g_registered.size() == 1);
    CHECK(g_registered[0] == std::make_pair(std::string("mymod.where"), EMF_COMMAND_PLAYERS));
    emf::api_ref() = nullptr;
}

TEST_CASE("commands: at most 256 chat lines wait; more are answered, not queued (the console is not limited)") {
    CommandCenter c;
    REQUIRE(c.add("raids.status", "", ran, nullptr, nullptr, false, kCommandForPlayers));
    CommandSender p;
    p.from_chat = true;
    p.restricted = true;
    int told = 0;
    for (int i = 0; i < 300; ++i) c.enqueue("raids.status", 1000, p, [&](const std::string& s) { told += s.find("too many") != std::string::npos; });
    CHECK(c.pending() == 256u);
    CHECK(told == 44);
    for (int i = 0; i < 10; ++i) c.enqueue("raids.status", 1000);  // the console and commands.txt
    CHECK(c.pending() == 266u);
}

// ---- with the debug bridge: what became of a line, for commands for players ----
TEST_CASE("bridge console: a command for players runs like any console line (ran); the console is no restricted player") {
    CommandCenter c;
    REQUIRE(c.add("raids.status", "", ran, nullptr, nullptr, false, kCommandForPlayers));
    REQUIRE(c.add("raids.start", "", ran, nullptr, nullptr));
    g_ran.clear();
    std::vector<std::string> outcomes, lines;
    const auto done = [&](const std::string& o) { outcomes.push_back(o); };
    const auto line = [&](const std::string& l) { lines.push_back(l); };
    bridge::run_console_line(c, HostState::Hosting, "raids.status", line, done, nullptr, 1000);
    bridge::run_console_line(c, HostState::Hosting, "raids.start", line, done, nullptr, 1000);
    CHECK(outcomes.empty());  // queued for the server tick
    c.run_pending(kWorld, nullptr, 1000);
    CHECK(outcomes == std::vector<std::string>{"ran", "ran"});
    CHECK(g_ran == std::vector<std::string>{"raids.status", "raids.start"});
    // with no world hosted it needs one, marked for players or not
    outcomes.clear();
    bridge::run_console_line(c, HostState::None, "raids.status", line, done, nullptr, 1000);
    CHECK(outcomes == std::vector<std::string>{"needs_world"});
}

TEST_CASE("bridge outcomes: a player's line refused by the v17 check, or dropped past the chat queue's limit, is refused, not ran") {
    CommandCenter c;
    REQUIRE(c.add("raids.status", "", ran, nullptr, nullptr, false, kCommandForPlayers));
    REQUIRE(c.add("raids.start", "", ran, nullptr, nullptr));
    g_ran.clear();
    CommandSender guest;
    guest.from_chat = true;
    guest.account = 0xbb;
    guest.restricted = true;
    std::vector<CommandOutcome> outcomes;
    const auto done = [&](CommandOutcome o) { outcomes.push_back(o); };
    c.enqueue("raids.start", 1000, guest, nullptr, done);
    c.enqueue("raids.status", 1000, guest, nullptr, done);
    c.run_pending(kWorld, nullptr, 1000);
    REQUIRE(outcomes.size() == 2);
    CHECK(outcomes[0] == CommandOutcome::Refused);
    CHECK(outcomes[1] == CommandOutcome::Ran);
    CHECK(g_ran == std::vector<std::string>{"raids.status"});
    outcomes.clear();
    for (size_t i = 0; i < CommandCenter::kMaxChatLines; ++i) c.enqueue("raids.status", 1000, guest);
    c.enqueue("raids.status", 1000, guest, nullptr, done);  // one too many: answered and refused at once
    REQUIRE(outcomes.size() == 1);
    CHECK(outcomes[0] == CommandOutcome::Refused);
    CHECK(c.pending() == CommandCenter::kMaxChatLines);
}

// ---- edge cases ----
namespace {
EmfCommandSender g_who{};
void who_cmd(EmfWorld*, int, const char* const*, void*) {
    g_who = EmfCommandSender{};
    g_who.size = sizeof g_who;
    api_table()->command_sender(&g_who);
}
}  // namespace

TEST_CASE("api v17: command_sender says whether the sender may run every command (admin, in the old pad's first byte)") {
    CHECK(offsetof(EmfCommandSender, admin) == 17u);
    CHECK(sizeof(EmfCommandSender) == 24u);
    CommandCenter c;
    REQUIRE(c.add("who", "", who_cmd, nullptr, nullptr, false, kCommandForPlayers));
    c.enqueue("who", 1000);  // the console
    c.run_pending(kWorld, nullptr, 1000);
    CHECK(g_who.admin == 1);
    CHECK(g_who.from_chat == 0);
    CommandSender p;
    p.from_chat = true;
    p.restricted = true;  // a player who is no admin
    c.enqueue("who", 1000, p);
    c.run_pending(kWorld, nullptr, 1000);
    CHECK(g_who.from_chat == 1);
    CHECK(g_who.admin == 0);
    p.restricted = false;  // an admin in the chat
    c.enqueue("who", 1000, p);
    c.run_pending(kWorld, nullptr, 1000);
    CHECK(g_who.admin == 1);
}

TEST_CASE("chat: [chat] deny takes listed players' commands away from players who are no admins") {
    CommandCenter c;
    REQUIRE(c.add("raids.status", "", ran, nullptr, nullptr, false, kCommandForPlayers));
    REQUIRE(c.add("loot.toggle", "", ran, nullptr, nullptr, false, kCommandForPlayers));
    CommandSender guest;
    guest.from_chat = true;
    guest.restricted = true;
    c.enqueue("loot.toggle", 1000, guest);  // queued before the server's deny list is read: checked again when it runs
    c.set_denied_for_players(parse_command_names("Loot.Toggle, nope"));
    CHECK_FALSE(c.for_players("loot.toggle"));
    CHECK(c.for_players("raids.status"));
    g_ran.clear();
    std::vector<CommandOutcome> outcomes;
    c.run_pending(kWorld, nullptr, 1000);
    CHECK(g_ran.empty());
    const auto admins = parse_chat_policy("0xaa", false);
    std::string line;
    CHECK(judge_chat("/loot.toggle", 0xbb, admins, known, &line, [&](std::string_view w) { return c.for_players(w); }) ==
          ChatAction::Refuse);
    CHECK(judge_chat("/loot.toggle", 0xaa, admins, known, &line, [&](std::string_view w) { return c.for_players(w); }) ==
          ChatAction::Run);  // admins keep it
    CHECK(parse_command_names(" a.b ,C.D  e ") == std::vector<std::string>{"a.b", "c.d", "e"});
}
