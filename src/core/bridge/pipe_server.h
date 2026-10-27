#pragma once
// The debug bridge's transport: a named pipe only the current user (and SYSTEM) can open, never from another machine
// (PIPE_REJECT_REMOTE_CLIENTS, and network logons are denied), with up to 8 clients at once. Each client's process is
// checked again on connect (same user, integrity level at least the game's). One thread with overlapped I/O hands each
// complete line to a handler and writes back what is sent; a fault or exception in it stops the bridge, never the
// game. It wakes on stop; stop() never waits longer than asked (a stuck thread is left behind, holding its own state).
#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace emf::core::bridge {

using ClientId = uint64_t;

// The SID of the user this process runs as ("S-1-5-21-..."), or "" on a failure.
std::wstring current_user_sid();
// The SID of this process's integrity level ("S-1-16-8192" = medium), or "" on a failure.
std::wstring current_integrity_sid();
// The pipe's security descriptor: network logons denied, `user_sid` and SYSTEM allowed, nobody else (a protected DACL:
// nothing inherited); with `integrity_sid`, a mandatory label that keeps lower-integrity processes out even when they
// run as the same user (no write up, no read up).
std::wstring pipe_sddl(const std::wstring& user_sid, const std::wstring& integrity_sid);

// Whether a client may stay: its user is ours and its integrity level (the RID of S-1-16-<rid>) is at least ours.
bool client_identity_ok(const std::wstring& client_user, uint32_t client_integrity, const std::wstring& our_user,
                        uint32_t our_integrity);
// The real check for a connected pipe: the client process's token (GetNamedPipeClientProcessId). "" = allowed, else
// why not.
std::string check_pipe_client(HANDLE pipe);

struct PipeHandler {
    std::function<void(ClientId)> connected;
    std::function<void(ClientId, std::string_view)> line;  // one line, without its newline
    std::function<void(ClientId)> too_long;                // a line over the limit was dropped
    std::function<void(ClientId)> disconnected;
    std::function<void()> poll;  // every 50 ms or so, and after each wake-up
    // A connecting client was refused by the client check (it never gets a ClientId).
    std::function<void(const std::string&)> refused;
    // The thread stopped by itself (a fault or exception in it, or the pipe name taken by another process).
    std::function<void(const std::string&)> stopped;
};

class PipeServer {
public:
    static constexpr size_t kDropAbove = 2u << 20;        // droppable lines go while a client has this much waiting
    static constexpr size_t kDisconnectAbove = 8u << 20;  // a client with this much waiting is disconnected
    static constexpr uint32_t kPartialLineMs = 30000;     // a line not finished within this ends the connection

    PipeServer();
    ~PipeServer();  // stop(2000)
    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    // How connecting clients are checked (default check_pipe_client; tests replace it). Set before start().
    void set_client_check(std::function<std::string(HANDLE pipe)> check);
    void set_partial_line_ms(uint32_t ms);  // tests

    // Creates the pipe (its first instance: if another process already owns the name, this fails) and starts the
    // thread. Whenever no instance is left, the next one is again created as a first instance, so the name is never
    // joined after another process took it (the thread then stops). `max_line` is the longest request line. False and
    // *why on a failure.
    bool start(const std::wstring& name, const std::wstring& sddl, PipeHandler handler, size_t max_line,
               int max_clients, std::string* why);
    // Any thread: queues a line for a client (the newline is added). Droppable lines are dropped while the client has
    // more than kDropAbove bytes waiting; it gets {"event":"dropped","lines":N} when it catches up.
    void send(ClientId c, std::string line, bool droppable);
    // Any thread: closes the client's pipe once what was sent to it is written (2 s at most).
    void disconnect(ClientId c);
    void wake();  // any thread: poll soon
    // Stops the thread and closes the pipes, waiting at most timeout_ms. True if the thread ended.
    bool stop(uint32_t timeout_ms);
    HANDLE wake_event() const;  // e.g. for the log ring's notify
    size_t client_count() const;
    bool running() const;  // false once stopped (also when it stopped by itself)

    struct State;

private:
    std::shared_ptr<State> s_;
};

}  // namespace emf::core::bridge
