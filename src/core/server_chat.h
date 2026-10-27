#pragma once
// Players' /commands on the server side: a dedicated server, or a game you host (docs/modding/server.md).
// The server's receive loop calls chatLogAdd once per chat line a player sends; there EMF turns "/<command> ..." from an
// allowed player into a queued command (with the player as its sender) and drops the line, so no one sees it. Replies
// are added after the receive loop, on the same thread, and marked for the sender's reader only.
// Engine facts: docs/engine_parts/re_chat.md ("Server side").
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "chat_commands.h"
#include "commands.h"

namespace emf::core {

// Who may run commands from chat ([chat] in emf_core.ini).
struct ChatPolicy {
    bool everyone = false;         // everyone=1: any player
    std::vector<uint64_t> admins;  // admins=0x...,0x...: these accounts
    bool allows(uint64_t account) const;
};
// `admins`: comma-separated account hashes in hex ("0x" optional); anything else is skipped and, when `skipped` is
// given, added to it (trimmed; empty entries are not).
ChatPolicy parse_chat_policy(std::string_view admins, bool everyone, std::vector<std::string>* skipped = nullptr);
// The start-up warning for an admin entry parse_chat_policy skipped.
std::string skipped_admin_warning(std::string_view entry);
// An account hash as the log prints it and the admin list takes it: "0x" + 16 hex digits.
std::string account_text(uint64_t account);

enum class ChatAction { Pass, Run, Refuse };
// One player's chat line: Pass = ordinary chat (also "/word" that is no EMF command); Run = an allowed player's
// command (*line = the command without the slash); Refuse = a command from a player who may not. The policy's accounts
// may run every command; `for_players` (lower-case name; API v17 EMF_COMMAND_PLAYERS) names those any player may run.
ChatAction judge_chat(std::string_view text, uint64_t account, const ChatPolicy& policy,
                      const std::function<bool(std::string_view)>& is_command, std::string* line,
                      const std::function<bool(std::string_view)>& for_players = {});

// Since any player may run some commands (v17), a player who is no admin gets at most `max` command lines (run or
// refused) per `window_ms`: the first line over it is answered once (Warn), the others are dropped (no reply, no log
// line), so nobody can flood the server's queue, its log or a mod's file writes. Admins are not limited. Thread-safe.
class ChatRateLimit {
public:
    enum class Verdict { Ok, Warn, Drop };
    explicit ChatRateLimit(int max = 5, uint64_t window_ms = 10000) : max_(max), window_ms_(window_ms) {}
    Verdict check(uint64_t account, uint64_t now_ms);
    void reset() {
        std::lock_guard<std::mutex> l(m_);
        windows_.clear();
    }

private:
    struct Window {
        uint64_t start = 0;
        int count = 0;
    };
    std::mutex m_;
    std::map<uint64_t, Window> windows_;
    int max_;
    uint64_t window_ms_;
};

// A line for the chat. For one player: their connection handle and account (the line's speaker, so their game shows it).
// A command's reply gets "[EMF] " in front; a mod's line (API v19 chat_tell, raw) is sent as given, and its handle is
// only a hint (from BasicPlayerInfos): the player's connection is found by the account when the hint is wrong.
// everyone: for every connected player (chat_broadcast), each one as its speaker.
struct ChatReply {
    uint32_t handle = 0;
    uint64_t account = 0;
    std::string text;
    bool raw = false;
    bool everyone = false;
};
// Lines waiting for the receive loop's thread (commands and mods run on the server world's thread). Any thread. At most
// kMaxChatOutbox lines wait: add refuses more (false).
constexpr size_t kMaxChatOutbox = 1024;
// Lines added to the game's chat system per receive loop run (its line pool evicts the oldest lines when it is full,
// which could drop lines not sent yet); the rest wait for the next run, a frame later.
constexpr int kMaxChatLinesPerRun = 32;
class ChatOutbox {
public:
    bool add(uint32_t handle, uint64_t account, std::string text, bool raw = false);
    bool add_everyone(std::string text);
    std::vector<ChatReply> take();
    // Puts lines that were taken but not sent yet back in front of the others (their order kept).
    void put_back(std::vector<ChatReply> lines);
    size_t size();

private:
    std::mutex m_;
    std::vector<ChatReply> items_;
};

// ---- API v19: lines from mods ----
// The chat lines of a mod's text: '\n' splits (a '\r' before it goes, a last empty line too, empty lines are skipped),
// control characters become '?', each line is cut at kChatMaxBytes bytes (never inside a UTF-8 character). At most
// `max_lines`: one more becomes "...", the rest are dropped (*dropped, when given, counts them).
std::vector<std::string> chat_lines(std::string_view text, size_t max_lines = 10, size_t* dropped = nullptr);
// Each mod (by name) may send at most `max` chat lines per `window_ms`. take() counts `lines` and says yes, or says no and
// counts nothing (the caller refuses the whole call). Thread-safe.
// The chat lines a mod (its DLL name) may send in 10 s: 30; the Lua host ("lua", which sends for every Lua mod and
// holds each script to 30 itself) 120.
int chat_line_limit(std::string_view module);
class ChatLineBudget {
public:
    explicit ChatLineBudget(int max = 30, uint64_t window_ms = 10000) : max_(max), window_ms_(window_ms) {}
    // max < 0: the budget's own max
    bool take(const std::string& mod, int lines, uint64_t now_ms, int max = -1);
    // Lines taken that were not sent after all (refused, a full outbox, a world that went): they count no more.
    void give_back(const std::string& mod, int lines);

private:
    struct Window {
        uint64_t start = 0;
        int lines = 0;
    };
    std::mutex m_;
    std::map<std::string, Window> windows_;
    int max_;
    uint64_t window_ms_;
};
// One chat line a player sent that is no EMF command (on_chat), from the receive loop to the server world's thread.
struct ChatLine {
    uint64_t account = 0;
    uint32_t handle = 0;
    std::string text;  // printable()
};
// At most kMaxChatInbox lines wait; push refuses more (false) and counts them in dropped(). Thread-safe.
constexpr size_t kMaxChatInbox = 256;
class ChatInbox {
public:
    bool push(ChatLine line);
    std::vector<ChatLine> take();
    uint64_t dropped() const { return dropped_.load(); }

private:
    std::mutex m_;
    std::vector<ChatLine> items_;
    std::atomic<uint64_t> dropped_{0};
};

// The parts of the game's chat system EMF touches (the same in the client and the server of 1273043).
namespace chat_layout {
constexpr size_t kTail = 0x1C8;        // ChatSystem: the newest line
constexpr size_t kReaders = 0x0C;      // ChatSystem: reader i at kReaders + kReaderStride * i
constexpr size_t kReaderStride = 12;
constexpr size_t kReaderPending = 8;   // reader: i32 bytes of lines still to send (u8 "active" at +0)
constexpr int kReaderCount = 32;
constexpr size_t kNodeLen = 8;         // line: u32 text length
constexpr size_t kNodeHandle = 0x14;   // line: u32 the speaker's player handle (0: none)
constexpr size_t kNodeAccount = 0x18;  // line: u64 the speaker's account hash
constexpr size_t kNodeText = 0x40;     // line: the text (not NUL-terminated)
constexpr size_t kNodeMask = 0x20;     // line: u32, bit i set = reader i is done with it
}  // namespace chat_layout

// `text` for a log or console line: control characters (CR, LF, ESC...) become '?' so typed chat cannot forge lines
// or send escape codes to the server window. UTF-8 bytes stay.
std::string printable(std::string_view text);

// At most `max` reply lines of one command reach the chat (long outputs would push players' lines out of the game's
// chat list); the next line becomes one note, the rest are dropped (the log has them all).
class ReplyLimiter {
public:
    explicit ReplyLimiter(int max) : max_(max) {}
    std::optional<std::string> next(const std::string& line);

private:
    int max_;
    int sent_ = 0;
};
constexpr int kMaxChatReplyLines = 10;

// For [chat] debug=1: the newest line (length, speaker, reader mask) and every reader that is active or has bytes
// left to send ("r<i> on|off <pending>"). Reads `chat_system` as laid out in chat_layout (a copy is fine: the newest
// line is read through the tail pointer).
std::string describe_chat_system(const uint8_t* chat_system);
// The bytes describe_chat_system reads from a chat system and from its newest line.
constexpr size_t kChatDumpSysBytes = 0x1D0;
constexpr size_t kChatDumpNodeBytes = 0x28;

// True if the newest line of the chat system has exactly `text` (the reply EMF just added, not a line another thread
// added in between).
bool newest_line_is(const uint8_t* chat_system, std::string_view text);

// The server's connection records (both exes of 1273043): 16 records of 0x148 bytes at Server+0x1E0, index = connection
// handle & 0x3F; +0 the connection handle that chat lines carry, +4 the player's peer handle. The send loop marks chat
// lines per reader = peer & 0x7F (confirmed in game: connection 0x40, peer 0x81, reader 1).
namespace server_layout {
constexpr size_t kConnections = 0x1E0;
constexpr size_t kConnectionSize = 0x148;
constexpr uint32_t kConnectionCount = 16;
constexpr size_t kConnectionPeer = 4;
constexpr size_t kConnectionAccount = 0x48;  // u64 the player's account hash
constexpr size_t kConnectionName = 0x20;        // char* the player's name (confirmed in game: their platform name)
constexpr size_t kConnectionNameLength = 0x28;  // u64 its length in bytes (no NUL needed)
}  // namespace server_layout
// Where the record of connection `handle` starts in the server; nullopt past the 16 records.
std::optional<size_t> connection_record_offset(uint32_t handle);
// The chat reader of the player on connection `handle` with `account`, from a copy of its record (at least
// kConnectionAccount + 8 bytes); nullopt when the record holds another connection or account (the player left and the
// record was reused or cleared), handle 0 (no connection), no peer, or a reader the line mask cannot name (32 or more).
std::optional<uint32_t> reader_from_connection(const uint8_t* record, uint32_t handle, uint64_t account);

// A player's connection: its handle (the speaker a line for them carries), account and chat reader.
struct ChatConnection {
    uint32_t handle = 0;
    uint64_t account = 0;
    uint32_t reader = 0;
};
// Every connected player in a copy of the server's connection records (kConnectionCount records of kConnectionSize
// bytes from Server+kConnections): record i counts when its handle is not 0 and names record i (handle & 0x3F == i),
// it holds an account, and reader_from_connection gives a reader for it.
std::vector<ChatConnection> connected_players(const uint8_t* records);
// The connection whose record holds `account` (the first; nullopt when none).
std::optional<ChatConnection> connection_of(const uint8_t* records, uint64_t account);

// Makes the newest line go to `reader` only: every other active reader whose bit is clear is marked done and its
// pending count lowered by the line's size (as chatLogAdd added it). False (nothing changed) for a reader the mask
// cannot name (>= 32) or no line.
bool restrict_line(uint8_t* chat_system, uint32_t reader);

// BasicPlayerInfos (a server world singleton): one entry per player entity, at index entity - 1, its first u32 the
// entity id (docs/engine_parts/inventory_crafting.md, isAdmin).
constexpr size_t kPlayerInfoSize = 0xA0;
// The entity whose player info entry holds `account` (an 8-byte aligned field anywhere in the entry), or 0 when none
// or more than one does. `alive` (may be empty) drops entries whose entity no longer exists (a player who reconnected
// leaves an old entry). `infos` points at `count` entries in memory EMF may read.
uint32_t entity_by_account(const uint8_t* infos, size_t count, uint64_t account,
                           const std::function<bool(uint32_t entity)>& alive = {});
constexpr size_t kPlayerInfoCount = 0x3ff;  // entities 1..1023 are players
// An entry's fields, confirmed on the dedicated server of 1273043: +0 the entity id, +4 the player's connection
// handle (0x40 for the first player: the handle their chat lines carry), +8 the account hash.
constexpr size_t kPlayerInfoHandle = 4;
constexpr size_t kPlayerInfoAccount = 8;
struct PlayerInfo {
    uint32_t entity = 0;
    uint32_t handle = 0;
    uint64_t account = 0;
};
// The entry (kPlayerInfoSize bytes) of player `entity`, if it is theirs (its id is `entity`) and holds an account.
std::optional<PlayerInfo> read_player_info(const uint8_t* entry, uint32_t entity);
// Reads n bytes of game memory at src into dst; false when it cannot (a guarded copy in the core, a fake in tests).
using MemoryRead = std::function<bool(const void* src, void* dst, size_t n)>;
// API v19 player_name: the name the player's connection carries, from a copy of the server's connection records (all
// kConnectionCount of them): the record of `handle` (else the one that holds `account`) points at the text at
// kConnectionName with its length at kConnectionNameLength. Confirmed in game (a game hosted with [chat]
// players=1, and a dump of the dedicated server): the player's platform (Steam) name, not the character's.
// BasicPlayerInfos holds no name (checked in game). nullopt when no record is theirs or the text is not 1-64 bytes without
// control characters.
std::optional<std::string> connection_name(const uint8_t* records, uint32_t handle, uint64_t account,
                                           const MemoryRead& read);

// ---- engine glue (server_chat_hooks.cpp) ----
struct ServerChatDeps {
    std::function<bool(std::string_view lower_word)> is_command;
    std::function<bool(std::string_view lower_word)> for_players;  // any player may run it (v17); empty: none
    ChatPolicy policy;
    // queues `line` as a command typed by `sender`; `reply` gets its echo and replies (any thread)
    std::function<void(const std::string& line, const CommandSender& sender, ReplyFn reply)> run;
    std::function<void(const std::string& text)> log;
    std::function<uint64_t()> now_ms;  // the clock for the players' rate limit (empty: GetTickCount64)
    // [chat] debug=1: called (on the receive thread) for each command line with the server object, for research dumps
    std::function<void(uint8_t* server, uint32_t handle, uint64_t account)> debug;
    // API v19 on_chat: each line a player sent that is no EMF command (on the receive thread; empty: none)
    std::function<void(ChatLine line)> on_line;
};
// Hooks the server's receive loop and chatLogAdd (both found by signature; 0 = not found). True if both are in place.
bool install_server_chat(uintptr_t receive, uintptr_t chat_log_add, ServerChatDeps deps);

// ---- API v19: lines from mods into the server's chat (sent by the receive loop's thread after its next run) ----
bool server_chat_on();  // the hooks are in place (install_server_chat, or server_chat_for_test)
// A mod's line for one player: `handle_hint` from their BasicPlayerInfos entry, checked against the connection record
// (else the account's record is used). False when the hooks are not in place or the outbox is full.
bool queue_chat_tell(uint32_t handle_hint, uint64_t account, std::string line);
// A mod's line for every connected player. False as queue_chat_tell.
bool queue_chat_everyone(std::string line);
// The server object the receive loop last ran for (its connection records), or nullptr before the first run.
uint8_t* chat_server();

// Tests: the two detours over stand-ins for the game's functions (nothing is hooked; the outbox starts empty).
using ChatReceiveFn = void(__fastcall*)(uint8_t* server);
using ChatLogAddFn = uint8_t*(__fastcall*)(uint8_t* out, uint8_t* sys, uint8_t kind, const ChatText* text, uint32_t tag1,
                                           uint32_t tag2, uint32_t handle, uint64_t account, const void* time);
struct ServerChatDetours {
    ChatReceiveFn receive;
    ChatLogAddFn log_add;
};
ServerChatDetours server_chat_for_test(ChatReceiveFn game_receive, ChatLogAddFn game_log_add, ServerChatDeps deps);

}  // namespace emf::core
