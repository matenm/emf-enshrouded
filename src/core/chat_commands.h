#pragma once
// /commands in the game's own chat: a chat line "/<EMF command> ..." runs as that command instead of being sent.
// The hook point is the chat box's queueChatMessage (docs/engine_parts/re_chat.md). It runs once for each line the
// local player sends, before anything goes to the network. Returning true without calling the game leaves the chat
// box as after a send (input cleared, chat closed) with nothing queued, so the line reaches no one.
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace emf::core {

struct ChatText {  // the game's string view: UTF-8 bytes, not NUL-terminated
    const char* data;
    uint64_t size;
};
constexpr uint8_t kChatKindTyped = 0;      // a line the player typed (the other kinds are pings with no text)
constexpr uint64_t kChatMaxBytes = 0x400;  // the game's limit for one chat message

struct ChatDeps {
    std::function<bool(std::string_view lower_word)> is_command;  // an EMF command with that (lower case) name exists
    std::function<void(const std::string& line)> run;              // runs the line as if typed into the console
    // true while this client hosts the game (or plays alone); in someone else's game the line goes on to the server,
    // which may run EMF itself and answer (empty: always ours)
    std::function<bool()> ours;
};

// True if `text` is "/<word> ..." and <word> names an EMF command; *line = the text without the slash, trimmed.
// Anything else (plain chat, "/" alone, "/ x", "//x", a word EMF does not know) stays chat for the game.
bool chat_line_command(std::string_view text, const std::function<bool(std::string_view)>& is_command,
                       std::string* line);

// The hook's decision for one queueChatMessage call: true = EMF ran the line (the game must not send it).
bool take_chat_message(const ChatText* text, uint8_t kind, const ChatDeps& deps);

// Hooks queueChatMessage at `target` (0 = not found on this build). True if the hook is in place.
bool install_chat_commands(uintptr_t target, ChatDeps deps);

}  // namespace emf::core
