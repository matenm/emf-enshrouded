#pragma once
// Developer commands: lines from the console window or emf/commands.txt are queued from any thread and run on the
// server world's tick (inside the dispatcher scope, so commands may spawn, destroy and give items).
#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "emf/emf_api.h"

namespace emf::core {

using CommandFn = void (*)(EmfWorld* world, int argc, const char* const* argv, void* user);
using ReplyFn = std::function<void(const std::string&)>;

// What became of a command line.
enum class CommandOutcome {
    Ran,         // its command ran
    Unknown,     // no command with that name
    Refused,     // the command is disabled (it crashed earlier), or not for this player (a non-admin in chat, v17)
    NeedsWorld,  // it needs a world you host (main menu, someone else's game)
    TooOld,      // typed too long ago, while no world you host was running
    Crashed,     // it crashed now and was disabled
    Empty,       // no words
};
// "ran", "unknown", "refused", "needs_world", "too_old", "crashed", "empty"
const char* command_outcome_name(CommandOutcome o);
using DoneFn = std::function<void(CommandOutcome)>;

// Splits a command line into words; "double quotes" group words, \" is a literal quote.
std::vector<std::string> split_command(std::string_view line);

// One line of output for the command running on this thread (dropped outside a command).
void command_reply_line(const std::string& text);
// A copy of the command's reply (empty outside a command), for lines that come after it returned (put's result one
// server step later); it stays usable afterwards and answers the same place (a chat command: its sender too).
ReplyFn current_reply();

// Replies that must not go to emf_core.log (the player's character and world names, their folders): while a
// PrivateReplies lives on a thread, the lines sent on it are private, and the core's sink shows them in the console but
// does not log them. private_reply(r) makes every line sent through r private, also from another thread later.
bool reply_is_private();
class PrivateReplies {
public:
    PrivateReplies();
    ~PrivateReplies();
    PrivateReplies(const PrivateReplies&) = delete;
    PrivateReplies& operator=(const PrivateReplies&) = delete;
};
ReplyFn private_reply(ReplyFn reply);

// Who typed a command line: a player in the game's chat (on a dedicated server or a game you host), or nobody in
// particular (the console, emf/commands.txt, the overlay).
struct CommandSender {
    bool from_chat = false;
    uint64_t account = 0;     // the player's account hash (chat)
    uint32_t handle = 0;      // the player's connection handle on the server (chat)
    uint32_t entity = 0;      // the player's entity in the server world, 0 if unknown
    bool restricted = false;  // chat: the account is no admin ([chat] policy): only commands for players run
};
// The sender of the command running on this thread, or nullptr outside a command.
const CommandSender* current_sender();
// The player a command means by "you": a chat sender's entity (0 if unknown: never someone else), else local_player.
uint32_t command_player(uint32_t local_player);

// What a world's tick does with queued commands: the server world runs them; a client world (someone else's game)
// answers "needs the host" only while no server world exists, so a host's client tick never steals them.
enum class CommandRoute { None, Run, Reject };
CommandRoute command_route(uint8_t world_kind, bool server_world_exists);

// Command names from a setting such as [chat] deny ("a.b, c.d e"): lower case, split at commas and spaces.
std::vector<std::string> parse_command_names(std::string_view text);

// Command flags (API v17 EmfCommandDef.flags).
constexpr uint32_t kCommandForPlayers = 0x01;  // EMF_COMMAND_PLAYERS: any player may run it from the game's chat
constexpr uint32_t kCommandKnownFlags = kCommandForPlayers;

struct CommandEntry {
    std::string name;  // lower case
    std::string help;
    bool players = false;  // kCommandForPlayers, and not taken away by [chat] deny
};

class CommandCenter {
public:
    static constexpr size_t kMaxChatLines = 256;
    // Names: 1..32 of [A-Za-z0-9_.-], unique ignoring case. False (*why) otherwise or for a null fn. `anywhere`
    // (built-ins only, e.g. help): the command also runs without a world (world == nullptr). `flags`: kCommand*.
    // `update_same_owner`: a name that exists with the same fn and user gets the new help and flags (a script host
    // re-declaring its command after a reload) instead of being refused as a duplicate.
    bool add(const std::string& name, const std::string& help, CommandFn fn, void* user, std::string* why,
             bool anywhere = false, uint32_t flags = 0, bool update_same_owner = false);
    // Any thread. `sender`: who typed it; `reply` (may be empty): also gets this line's echo and replies (a chat line
    // answers its sender). `done` (may be empty): called once with the outcome when the line was handled, after its
    // replies, on the thread that handled it (the debug bridge answers then). At most kMaxChatLines chat lines wait;
    // more are answered through `reply` and dropped (outcome Refused).
    void enqueue(std::string line, uint64_t now_ms = GetTickCount64(), CommandSender sender = {}, ReplyFn reply = {},
                 DoneFn done = {});
    size_t pending() const;
    // Runs every queued line (echoed as "> line") with `world`; replies go to `reply`. Lines queued more than
    // max_age_ms before now_ms (typed while no world you host was running) are answered, not run. Returns commands run.
    size_t run_pending(EmfWorld* world, const ReplyFn& reply, uint64_t now_ms = GetTickCount64(),
                       uint64_t max_age_ms = 30000);
    // For a line typed while no world you host is running: echoes it, runs it now if its command is `anywhere`,
    // otherwise answers `reason` (an unknown command gets the usual "unknown command"). Returns commands run (0/1);
    // *outcome (optional) gets what became of the line.
    size_t run_without_world(const std::string& line, const ReplyFn& reply, const std::string& reason,
                             CommandOutcome* outcome = nullptr);
    // Answers every queued line with `reason` and runs none (outcome NeedsWorld). Returns the lines answered.
    size_t reject_pending(const std::string& reason, const ReplyFn& reply);
    std::vector<std::pair<std::string, std::string>> list() const;  // (name, help), sorted by name
    std::vector<CommandEntry> entries() const;                         // with the flags, sorted by name
    // True if the command (a lower-case name) exists and any player may run it from chat (kCommandForPlayers), and the
    // server owner did not take it away from players ([chat] deny).
    bool for_players(std::string_view lower_name) const;
    // [chat] deny: commands for players that players who are no admins may not run here after all (lower-case names).
    void set_denied_for_players(std::vector<std::string> names);
    // Called in run_pending (the world's thread) for each chat line before it runs, to fill in the sender's entity.
    void set_sender_resolver(std::function<void(CommandSender&)> resolve);
    void reset();  // tests only

private:
    struct Cmd {
        std::string name;  // lower case
        std::string help;
        CommandFn fn;
        void* user;
        bool enabled = true;
        bool anywhere = false;
        bool players = false;
    };
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    std::vector<Cmd> cmds_;
    struct Line {
        std::string text;
        uint64_t queued_ms;
        CommandSender sender;
        ReplyFn reply;
        DoneFn done;
    };
    std::vector<Line> queue_;
    std::function<void(CommandSender&)> resolve_;
    std::vector<std::string> denied_;  // [chat] deny
};

}  // namespace emf::core
