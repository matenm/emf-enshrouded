// The engine side of players' chat commands (server_chat.h): two hooks and nothing else touches the game's chat.
//   receive(Server*)  the server's network receive loop. While it runs, this thread is "receiving"; afterwards the
//                     replies waiting in the outbox are added to the server's chat system (*(Server+0x60)) on this
//                     same thread, each marked for its sender's reader only.
//   chatLogAdd(...)   adds a chat line to a chat system. Called from the receive loop for each line a player sends; a
//                     "/command" line is taken there and never added (no one sees it).
#include <windows.h>

#include <cstring>
#include <map>
#include <memory>

#include "api.h"
#include "chat_commands.h"
#include "common/seh.h"
#include "net/net_line.h"
#include "server_chat.h"

namespace emf::core {
namespace {

constexpr size_t kServerChatSystem = 0x60;  // Server: its ChatSystem* (both exes of 1273043)

ChatReceiveFn o_receive = nullptr;
ChatLogAddFn o_log_add = nullptr;
ClientFlushFn o_flush = nullptr;
ChatClientSendFn o_client_send = nullptr;
ClientChatDeps g_client;
std::atomic<uint8_t*> g_last_game_client{nullptr};  // the GameClient of the last flush (a new one: a new session)
std::atomic<uint8_t*> g_client_sys{nullptr};  // the client's ChatSystem (from the flush hook's GameClient)
std::atomic<bool> g_client_on{false};
ServerChatDeps g_deps;
ChatOutbox g_outbox;
ChatRateLimit g_limit;  // players who are no admins: 5 command lines per 10 s
thread_local uint8_t* t_server = nullptr;  // the server whose receive loop runs on this thread
std::atomic<uint8_t*> g_server{nullptr};   // the server the receive loop last ran for (v19: chat.players, tells)
std::atomic<bool> g_on{false};             // the hooks are in place
std::atomic<bool> g_spoof_logged{false};   // a dropped spoofed line was logged (once per session)

bool copy_text(const ChatText* t, char* out, size_t cap, size_t* n) {
    __try {
        if (!t || !t->data || t->size == 0 || t->size > cap) return false;
        std::memcpy(out, t->data, static_cast<size_t>(t->size));
        *n = static_cast<size_t>(t->size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The game's chatLogAdd for a reply line, spoken by the player it answers (players' games do not show a line without a
// known speaker: confirmed in game); kind 4 (kChatKindSession, tags 0) shows its text with no name. False if it faulted.
bool add_reply(uint8_t* sys, const ChatText* text, uint32_t handle, uint64_t account, uint8_t* result, uint8_t kind) {
    struct {
        uint64_t time;
        uint64_t has;  // low byte: 0 = no time given
    } when{0, 0};
    __try {
        o_log_add(result, sys, kind, text, 0, 0, handle, account, &when);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Narrows the newest line to `reader` if it is the reply just added (not a line another thread added in between).
bool narrow_reply(uint8_t* sys, const char* text, size_t len, uint32_t reader) {
    __try {
        return newest_line_is(sys, std::string_view(text, len)) && restrict_line(sys, reader);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A copy of the chat system's first bytes and its newest line (SEH-guarded), the tail pointer aimed at the copy.
bool snapshot_chat(const uint8_t* sys, uint8_t* sys_copy, uint8_t* node_copy) {
    __try {
        std::memcpy(sys_copy, sys, kChatDumpSysBytes);
        const uint8_t* node = nullptr;
        std::memcpy(&node, sys + chat_layout::kTail, sizeof node);
        const uint8_t* aim = nullptr;
        if (node) {
            std::memcpy(node_copy, node, kChatDumpNodeBytes);
            aim = node_copy;
        }
        std::memcpy(sys_copy + chat_layout::kTail, &aim, sizeof aim);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// [chat] debug=1: logs the chat system (newest line and readers) with a tag.
void debug_chat(const char* tag, const uint8_t* sys) {
    if (!g_deps.debug || !g_deps.log || !sys) return;
    uint8_t sys_copy[kChatDumpSysBytes];
    uint8_t node_copy[kChatDumpNodeBytes];
    if (!snapshot_chat(sys, sys_copy, node_copy)) {
        g_deps.log(std::string("chat debug ") + tag + ": the chat system could not be read");
        return;
    }
    g_deps.log(std::string("chat debug ") + tag + ": " + describe_chat_system(sys_copy));
}

// The chat reader of the player on connection `handle` with `account` (their connection record), or nullopt.
std::optional<uint32_t> sender_reader(const uint8_t* server, uint32_t handle, uint64_t account) {
    const auto at = connection_record_offset(handle);
    if (!server || !at) return std::nullopt;
    uint8_t record[server_layout::kConnectionAccount + 8];
    if (!seh_read(server + *at, record, sizeof record)) return std::nullopt;
    return reader_from_connection(record, handle, account);
}

std::string hex(uint32_t v) {
    char h[16];
    std::snprintf(h, sizeof h, "%x", v);
    return h;
}

// True if EMF took the line (a command, run or refused): the game must not add it.
bool take_line(const ChatText* text, uint8_t kind, uint32_t handle, uint64_t account) {
    if (g_deps.net_line && kind == kChatKindTyped) {  // API v27 mod messages: before commands and on_chat
        char nbuf[kChatMaxBytes];
        size_t nn = 0;
        if (copy_text(text, nbuf, sizeof nbuf, &nn) && net::is_marked(std::string_view(nbuf, nn)))
            return g_deps.net_line(handle, account, std::string_view(nbuf, nn));
    }
    if (kind != kChatKindTyped || ((!g_deps.run || !g_deps.is_command) && !g_deps.on_line)) return false;
    char buf[kChatMaxBytes];
    size_t n = 0;
    if (!copy_text(text, buf, sizeof buf, &n)) return false;
    const std::string_view typed(buf, n);
    std::string line;
    const ChatAction action = g_deps.run && g_deps.is_command
                                  ? judge_chat(typed, account, g_deps.policy, g_deps.is_command, &line, g_deps.for_players,
                                               handle)
                                  : ChatAction::Pass;
    if (action != ChatAction::Pass && g_deps.debug) g_deps.debug(t_server, handle, account);
    if (action != ChatAction::Pass && !g_deps.policy.allows(account, handle)) {
        const uint64_t now = g_deps.now_ms ? g_deps.now_ms() : GetTickCount64();
        switch (g_limit.check(account, now)) {
            case ChatRateLimit::Verdict::Ok: break;
            case ChatRateLimit::Verdict::Warn:
                g_outbox.add(handle, account, "not run: too many commands; wait a few seconds");
                if (g_deps.log)
                    g_deps.log("chat commands from account " + account_text(account) +
                               ": more than 5 in 10 s; the rest of this window is dropped");
                return true;
            case ChatRateLimit::Verdict::Drop: return true;
        }
    }
    switch (action) {
        case ChatAction::Pass:
            if (g_deps.on_line) {  // v19 on_chat: a player's ordinary line, read-only
                ChatLine l;
                l.account = account;
                l.handle = handle;
                l.text = printable(typed);
                g_deps.on_line(std::move(l));
            }
            return false;
        case ChatAction::Refuse:
            g_outbox.add(handle, account,
                         "not run: your account (" + account_text(account) +
                             ") may not run that command here (/help lists the ones you may run)");
            if (g_deps.log)
                g_deps.log("chat command refused from account " + account_text(account) + ": " + printable(typed) +
                           " (to allow it, add the account to [chat] admins in emf\\emf_core.ini)");
            return true;
        case ChatAction::Run: {
            CommandSender s;
            s.from_chat = true;
            s.account = account;
            s.handle = handle;  // the entity is looked up when the command runs, on the world's thread
            s.restricted = !g_deps.policy.allows(account, handle);  // a players' command: checked again when it runs
            if (g_deps.log)
                g_deps.log("chat command from account " + account_text(account) + " (handle 0x" + hex(handle) +
                           "): " + printable(line));
            auto limit = std::make_shared<ReplyLimiter>(kMaxChatReplyLines);  // one command's lines in the chat
            g_deps.run(line, s, [handle, account, limit](const std::string& r) {
                if (auto out = limit->next(r)) g_outbox.add(handle, account, *out);
            });
            return true;
        }
    }
    return false;
}

// A copy of the server's connection records (SEH-guarded); false if they could not be read.
bool read_connections(const uint8_t* server, std::vector<uint8_t>* out) {
    using namespace server_layout;
    out->assign(static_cast<size_t>(kConnectionSize) * kConnectionCount, 0);
    return server && seh_read(server + kConnections, out->data(), out->size());
}

// Adds `text` of `kind` spoken by (handle, account) and narrows it to `reader`. False when adding faulted (stop sending).
bool send_line(uint8_t* sys, const std::string& text, uint32_t handle, uint64_t account, uint32_t reader, uint8_t kind) {
    std::string t = text;
    if (t.size() > kChatMaxBytes) t.resize(kChatMaxBytes);
    const ChatText view{t.data(), t.size()};
    uint8_t result[16] = {};
    debug_chat("before reply", sys);
    if (!add_reply(sys, &view, handle, account, result, kind)) {
        if (g_deps.log) g_deps.log("chat replies: adding a line faulted; replies stop here");
        return false;
    }
    const bool narrowed = result[0] == 0 && narrow_reply(sys, t.data(), t.size(), reader);
    if (result[0] == 0 && !narrowed && g_deps.log)
        g_deps.log("chat replies: a reply could not be limited to its sender (the newest chat line is not the reply: "
                   "another line came in between, or the game changed its text); other players may see it: " +
                   printable(text));
    if (g_deps.debug && g_deps.log) {
        char note[96];
        std::snprintf(note, sizeof note, "chat debug: reply result %u, reader %u, narrowed %d", result[0], reader,
                      narrowed ? 1 : 0);
        g_deps.log(note);
        debug_chat("reply narrowed", sys);
    }
    return true;
}

// The bytes the chat reader `reader` still has to send (-1 when unreadable).
int32_t reader_pending(const uint8_t* sys, uint32_t reader) {
    int32_t pending = 0;
    if (reader >= static_cast<uint32_t>(chat_layout::kReaderCount) ||
        !seh_read(sys + chat_layout::kReaders + chat_layout::kReaderStride * reader + chat_layout::kReaderPending, &pending,
                  sizeof pending))
        return -1;
    return pending;
}

void flush_replies(uint8_t* server) {
    auto items = g_outbox.take();
    if (items.empty()) return;
    std::vector<ChatReply> deferred;  // mod message lines for players whose reader is behind: next run
    struct Defer {
        std::vector<ChatReply>* d;
        ~Defer() { g_outbox.put_back(std::move(*d)); }
    } defer{&deferred};
    uint8_t* sys = nullptr;
    if (!seh_read(server + kServerChatSystem, &sys, sizeof sys) || !sys) return;
    std::vector<uint8_t> records;  // read once, when a mod's line needs them
    bool records_read = false;
    auto connections = [&]() -> const uint8_t* {
        if (!records_read) records_read = read_connections(server, &records) || (records.clear(), true);
        return records.empty() ? nullptr : records.data();
    };
    int sent = 0;  // lines added in this run (kMaxChatLinesPerRun)
    const uint64_t now = g_deps.now_ms ? g_deps.now_ms() : GetTickCount64();
    std::map<uint32_t, size_t> held;  // readers with a mod line waiting in this run, and how many wait
    size_t stale = 0, over = 0;       // mod lines dropped: waited too long / too many for one player
    struct Report {
        size_t* stale;
        size_t* over;
        ~Report() {
            if ((*stale || *over) && g_deps.log)
                g_deps.log("mod messages: " + std::to_string(*stale) + " line(s) for lagging players waited too long and " +
                           std::to_string(*over) + " more than a player may have waiting: dropped");
        }
    } report{&stale, &over};
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& reply = items[i];
        if (reply.everyone) {  // v19 chat_broadcast: each connected player gets the line as its own speaker
            if (!connections()) {
                if (g_deps.log) g_deps.log("chat: the server's connection records could not be read; line dropped: " +
                                           printable(reply.text));
                continue;
            }
            const auto players = connected_players(connections());
            if (sent && sent + static_cast<int>(players.size()) > kMaxChatLinesPerRun) {  // whole, in the next run
                g_outbox.put_back(std::vector<ChatReply>(items.begin() + static_cast<std::ptrdiff_t>(i), items.end()));
                return;
            }
            for (const auto& c : players)
                if (!send_line(sys, reply.text, c.handle, c.account, c.reader,
                               reply.system ? kChatKindSession : kChatKindTyped))
                    return;
            sent += static_cast<int>(players.size());
            continue;
        }
        if (sent >= kMaxChatLinesPerRun) {
            g_outbox.put_back(std::vector<ChatReply>(items.begin() + static_cast<std::ptrdiff_t>(i), items.end()));
            return;
        }
        ++sent;
        // The reader first: a line that cannot be limited to its player is never added (it would show to every player
        // under that player's name).
        uint32_t handle = reply.handle;
        auto reader = sender_reader(server, handle, reply.account);
        // v19 chat_tell: the handle came from BasicPlayerInfos; find the account's record. Never for a mod line: it is
        // numbered for its own connection, another one of the account would read it as a gap
        if (!reader && reply.raw && !reply.net) {
            if (const auto c = connection_of(connections(), reply.account)) {
                handle = c->handle;
                reader = c->reader;
                if (g_deps.debug && g_deps.log)
                    g_deps.log("chat debug: tell for account " + account_text(reply.account) + ": handle hint 0x" +
                               hex(reply.handle) + " did not match its connection record; used 0x" + hex(handle));
            }
        }
        if (!reader && reply.net) continue;  // the player left (or reconnected): the line was for that connection
        if (!reader) {
            if (g_deps.log)
                g_deps.log("chat replies: no chat reader for connection 0x" + hex(reply.handle) +
                           " (the player left?); reply dropped: " + printable(reply.text));
            continue;
        }
        // a lagging player: later, not now (and once one of its lines waits, the rest of this run waits behind it)
        if (reply.net && (held.count(*reader) || reader_pending(sys, *reader) > kMaxNetPendingBytes)) {
            --sent;
            if (reply.queued_ms && now > reply.queued_ms + kNetMaxWaitMs) {
                ++stale;
                continue;
            }
            if (++held[*reader] > kMaxNetWaitingLines) {
                ++over;
                continue;
            }
            deferred.push_back(reply);
            continue;
        }
        // a command's reply and a mod's system line show without a speaker's name; a mod message line stays typed text
        const uint8_t kind = !reply.net && (reply.system || !reply.raw) ? kChatKindSession : kChatKindTyped;
        if (!send_line(sys, reply.raw ? reply.text : "[EMF] " + reply.text, handle, reply.account, *reader, kind)) return;
    }
}

void __fastcall hk_receive(uint8_t* server) {
    uint8_t* const previous = t_server;
    t_server = server;
    g_server = server;
    o_receive(server);
    t_server = previous;
    try {
        if (g_deps.net_tick) {
            std::vector<uint8_t> records;
            if (read_connections(server, &records)) g_deps.net_tick(connected_players(records.data()));
        }
        flush_replies(server);
    } catch (...) {  // nothing may unwind into the game
    }
}

// The client's own chat got a line: true when it is a mod message line EMF takes (never added).
bool client_takes(uint8_t* sys, uint8_t kind, const ChatText* text, uint64_t speaker) {
    if (!g_client_on || kind != kChatKindTyped || !g_client.line || !sys) return false;
    char buf[kChatMaxBytes];
    size_t n = 0;
    if (!copy_text(text, buf, sizeof buf, &n) || !net::is_marked(std::string_view(buf, n))) return false;
    if (sys != g_client_sys.load()) {  // not the chat the client's outbox belongs to: left to the game
        static std::atomic<bool> told{false};
        if (!told.exchange(true) && g_client.log) {
            char t[96];
            std::snprintf(t, sizeof t, "%p (the client's: %p)", static_cast<void*>(sys),
                          static_cast<void*>(g_client_sys.load()));
            g_client.log(std::string("mod messages: a marked chat line reached a chat system that is not the client's ") +
                         t + "; left to the game (logged once)");
        }
        return false;
    }
    return g_client.line(std::string_view(buf, n), speaker);
}

bool guarded_client_send(uint8_t* sys, const ChatText* text, uint8_t* result) {
    __try {
        o_client_send(result, sys, kChatKindTyped, text, 0, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Before the game flushes the client's outbox: this frame's mod message lines go in (the GameClient's own thread).
void client_pump(uint8_t* game_client) {
    uint8_t* sys = nullptr;
    if (!game_client || !seh_read(game_client + kGameClientChat, &sys, sizeof sys) || !sys) return;
    if (game_client != g_last_game_client.exchange(game_client) && g_client.reset) g_client.reset();
    g_client_sys = sys;
    if (!g_client.take || !o_client_send) return;
    auto lines = g_client.take(kClientLinesPerFrame);
    for (size_t i = 0; i < lines.size(); ++i) {
        const ChatText view{lines[i].data(), lines[i].size()};
        uint8_t result[16] = {};
        if (!guarded_client_send(sys, &view, result)) {
            if (g_client.log) g_client.log("mod messages: sending a chat line faulted; the lines of this frame are dropped");
            return;
        }
        if (result[0] == 0) continue;
        if (result[0] == 2 && g_client.put_back) {  // the chat pool is full: this line and the rest, next frame
            g_client.put_back(std::vector<std::string>(lines.begin() + static_cast<std::ptrdiff_t>(i), lines.end()));
            return;
        }
        if (g_client.log) g_client.log("mod messages: the game refused a chat line (code " + std::to_string(result[0]) + ")");
    }
}

void __fastcall hk_flush(uint8_t* game_client) {
    try {
        client_pump(game_client);
    } catch (...) {  // nothing may unwind into the game
    }
    o_flush(game_client);
}

uint8_t* __fastcall hk_log_add(uint8_t* out, uint8_t* sys, uint8_t kind, const ChatText* text, uint32_t tag1,
                               uint32_t tag2, uint32_t handle, uint64_t account, const void* time) {
    if (!t_server && out && g_client_on) {  // the client's own chat: a mod message line is taken, never shown
        bool taken = false;
        try {
            taken = client_takes(sys, kind, text, account);
        } catch (...) {
        }
        if (taken) {
            out[0] = 0;
            std::memset(out + 4, 0, 4);
            return out;
        }
    }
    if (t_server && out) {
        bool taken = false;
        if (spoofed_line(kind, tag1)) {  // no unmodified game sends it: it would look like EMF's lines without a speaker
            taken = true;
            if (!g_spoof_logged.exchange(true) && g_deps.log)
                g_deps.log("chat: a line of kind " + std::to_string(kind) + (tag1 ? "" : " without a text id") +
                           " from account " + account_text(account) + " was dropped (no unmodified game sends it; said once)");
        }
        try {
            if (!taken) taken = take_line(text, kind, handle, account);
        } catch (...) {
        }
        if (taken) {  // as if added: the receive loop does not read the result
            out[0] = 0;
            std::memset(out + 4, 0, 4);
            return out;
        }
    }
    uint8_t* const r = o_log_add(out, sys, kind, text, tag1, tag2, handle, account, time);
    if (t_server && kind == kChatKindTyped) {
        try {
            debug_chat("player line added", sys);
        } catch (...) {
        }
    }
    return r;
}

}  // namespace

bool install_server_chat(uintptr_t receive, uintptr_t chat_log_add, ServerChatDeps deps) {
    if (!receive || !chat_log_add || o_receive) return false;
    g_deps = std::move(deps);
    // chatLogAdd first: the receive hook only marks the thread, so either order is safe while the game runs
    if (!install_emf_hook("chatLogAdd (players' chat commands)", chat_log_add, reinterpret_cast<void*>(&hk_log_add),
                          reinterpret_cast<void**>(&o_log_add)))
        return false;
    const bool ok = install_emf_hook("server receive loop (chat replies)", receive, reinterpret_cast<void*>(&hk_receive),
                                     reinterpret_cast<void**>(&o_receive));
    g_on = ok;
    return ok;
}

bool install_client_chat(uintptr_t flush, uintptr_t client_send, ClientChatDeps deps) {
    if (!flush || !client_send || !o_log_add || o_flush) return false;
    g_client = std::move(deps);
    o_client_send = reinterpret_cast<ChatClientSendFn>(client_send);
    if (!install_emf_hook("client chat outbox flush (mod messages)", flush, reinterpret_cast<void*>(&hk_flush),
                          reinterpret_cast<void**>(&o_flush)))
        return false;
    g_client_on = true;
    return true;
}

ClientChatDetours client_chat_for_test(ClientFlushFn game_flush, ChatClientSendFn game_send, ChatLogAddFn game_log_add,
                                       ClientChatDeps deps) {
    o_flush = game_flush;
    o_client_send = game_send;
    o_log_add = game_log_add;
    g_client = std::move(deps);
    g_client_sys = nullptr;
    g_last_game_client = nullptr;
    g_client_on = true;
    g_deps = ServerChatDeps{};
    g_server = nullptr;
    return {&hk_flush, &hk_log_add};
}

ServerChatDetours server_chat_for_test(ChatReceiveFn game_receive, ChatLogAddFn game_log_add, ServerChatDeps deps) {
    g_limit.reset();
    g_spoof_logged = false;
    o_receive = game_receive;
    o_log_add = game_log_add;
    g_deps = std::move(deps);
    g_outbox.take();
    g_server = nullptr;
    g_on = true;
    g_client_on = false;
    g_client_sys = nullptr;
    o_flush = nullptr;
    o_client_send = nullptr;
    return {&hk_receive, &hk_log_add};
}

bool server_chat_on() { return g_on.load(); }

bool queue_chat_tell(uint32_t handle_hint, uint64_t account, std::string line, bool system) {
    return g_on.load() && g_outbox.add(handle_hint, account, std::move(line), true, false, system);
}

bool queue_chat_everyone(std::string line, bool system) {
    return g_on.load() && g_outbox.add_everyone(std::move(line), system);
}

bool queue_net_line(uint32_t handle, uint64_t account, std::string line) {
    return queue_net_lines(handle, account, {std::move(line)});
}

bool queue_net_lines(uint32_t handle, uint64_t account, const std::vector<std::string>& lines) {
    const uint64_t now = g_deps.now_ms ? g_deps.now_ms() : GetTickCount64();
    return g_on.load() && g_outbox.add_net_lines(handle, account, lines, now);
}

size_t chat_outbox_room() {
    const size_t n = g_outbox.size();
    return n >= kMaxChatOutbox ? 0 : kMaxChatOutbox - n;
}

uint8_t* chat_server() { return g_server.load(); }

}  // namespace emf::core
