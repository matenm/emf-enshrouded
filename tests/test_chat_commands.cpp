// /commands typed into the game's chat: which lines EMF takes, and the queueChatMessage hook's decision.
#include <doctest.h>

#include <string>
#include <vector>

#include "core/chat_commands.h"

using namespace emf::core;

namespace {
bool known(std::string_view word) { return word == "give" || word == "time" || word == "help"; }  // lower case

struct Rig {
    std::vector<std::string> ran;
    ChatDeps deps;
    Rig() {
        deps.is_command = known;
        deps.run = [this](const std::string& line) { ran.push_back(line); };
    }
};
}  // namespace

TEST_CASE("chat commands: a '/' line naming an EMF command is taken, without the slash") {
    std::string line;
    CHECK(chat_line_command("/give Material_T1_Wood_Logs 5", known, &line));
    CHECK(line == "give Material_T1_Wood_Logs 5");
    CHECK(chat_line_command("  /TIME 12 \r\n", known, &line));  // any case; spaces and line ends trimmed
    CHECK(line == "TIME 12");
    CHECK(chat_line_command("/help", known, &line));
    CHECK(line == "help");
}

TEST_CASE("chat commands: everything else stays chat") {
    std::string line = "untouched";
    CHECK_FALSE(chat_line_command("hello /give x", known, &line));
    CHECK_FALSE(chat_line_command("", known, &line));
    CHECK_FALSE(chat_line_command("/", known, &line));
    CHECK_FALSE(chat_line_command("/ give x", known, &line));
    CHECK_FALSE(chat_line_command("//give x", known, &line));
    CHECK_FALSE(chat_line_command("/wave", known, &line));  // not EMF's: left to the game (its own / lines, emotes)
    CHECK_FALSE(chat_line_command("/givex 5", known, &line));  // whole words only
    CHECK(line == "untouched");
}

TEST_CASE("chat hook: typed text naming a command runs it and is not sent") {
    Rig r;
    const char text[] = "/give Material_T1_Wood_Logs 5";
    const ChatText t{text, sizeof(text) - 1};
    CHECK(take_chat_message(&t, kChatKindTyped, r.deps));
    REQUIRE(r.ran.size() == 1);
    CHECK(r.ran[0] == "give Material_T1_Wood_Logs 5");
}

TEST_CASE("chat hook: the text is read by its length (no terminator needed)") {
    Rig r;
    const char text[] = "/time 12XXXXXXXX";
    const ChatText t{text, 8};
    CHECK(take_chat_message(&t, kChatKindTyped, r.deps));
    REQUIRE(r.ran.size() == 1);
    CHECK(r.ran[0] == "time 12");
}

TEST_CASE("chat hook: other kinds, plain chat and odd input go to the game untouched") {
    Rig r;
    const char cmd[] = "/give x 5";
    const ChatText c{cmd, sizeof(cmd) - 1};
    CHECK_FALSE(take_chat_message(&c, 2, r.deps));  // a ping line, not typed text
    const char hi[] = "hello everyone";
    const ChatText h{hi, sizeof(hi) - 1};
    CHECK_FALSE(take_chat_message(&h, kChatKindTyped, r.deps));
    CHECK_FALSE(take_chat_message(nullptr, kChatKindTyped, r.deps));
    const ChatText empty{cmd, 0};
    CHECK_FALSE(take_chat_message(&empty, kChatKindTyped, r.deps));
    const ChatText too_long{cmd, 0x401};  // more than the game's chat limit: not a line the chat box made
    CHECK_FALSE(take_chat_message(&too_long, kChatKindTyped, r.deps));
    const ChatText bad{reinterpret_cast<const char*>(uintptr_t(0x10)), 5};  // unreadable: the game decides
    CHECK_FALSE(take_chat_message(&bad, kChatKindTyped, r.deps));
    CHECK(r.ran.empty());
}

TEST_CASE("chat hook: without a runner nothing is taken") {
    ChatDeps none;
    const char cmd[] = "/give x 5";
    const ChatText c{cmd, sizeof(cmd) - 1};
    CHECK_FALSE(take_chat_message(&c, kChatKindTyped, none));
}

TEST_CASE("chat hook: in someone else's game the line goes on to the server (it may run EMF and answer)") {
    Rig r;
    bool ours = false;
    r.deps.ours = [&] { return ours; };
    const char text[] = "/give Material_T1_Wood_Logs 5";
    const ChatText t{text, sizeof(text) - 1};
    CHECK_FALSE(take_chat_message(&t, kChatKindTyped, r.deps));  // a joined player: sent as chat
    CHECK(r.ran.empty());
    ours = true;  // a game this client hosts (or plays alone)
    CHECK(take_chat_message(&t, kChatKindTyped, r.deps));
    CHECK(r.ran.size() == 1);
}
