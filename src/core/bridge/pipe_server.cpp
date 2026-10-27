#include "pipe_server.h"

#include <sddl.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <tuple>
#include <vector>

namespace emf::core::bridge {

namespace {

// A token's user SID (TokenUser) or integrity SID (TokenIntegrityLevel) as text, and the integrity level's RID.
std::wstring sid_of(HANDLE token, TOKEN_INFORMATION_CLASS what, uint32_t* rid = nullptr) {
    DWORD size = 0;
    GetTokenInformation(token, what, nullptr, 0, &size);
    std::vector<BYTE> buf(size ? size : 1);
    std::wstring out;
    if (size && GetTokenInformation(token, what, buf.data(), size, &size)) {
        // TOKEN_USER and TOKEN_MANDATORY_LABEL both start with a SID_AND_ATTRIBUTES
        PSID sid = reinterpret_cast<SID_AND_ATTRIBUTES*>(buf.data())->Sid;
        LPWSTR text = nullptr;
        if (ConvertSidToStringSidW(sid, &text)) {
            out = text;
            LocalFree(text);
        }
        if (rid && IsValidSid(sid) && *GetSidSubAuthorityCount(sid))
            *rid = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1u);
    }
    return out;
}

std::wstring token_sid(TOKEN_INFORMATION_CLASS what, uint32_t* rid = nullptr) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return L"";
    std::wstring out = sid_of(token, what, rid);
    CloseHandle(token);
    return out;
}

std::string win_error(const char* what, DWORD e) {
    return std::string(what) + " (error " + std::to_string(e) + ")";
}

}  // namespace

std::wstring current_user_sid() { return token_sid(TokenUser); }
std::wstring current_integrity_sid() { return token_sid(TokenIntegrityLevel); }

bool client_identity_ok(const std::wstring& client_user, uint32_t client_integrity, const std::wstring& our_user,
                        uint32_t our_integrity) {
    return !client_user.empty() && client_user == our_user && client_integrity >= our_integrity;
}

std::string check_pipe_client(HANDLE pipe) {
    ULONG pid = 0;
    if (!GetNamedPipeClientProcessId(pipe, &pid)) return "its process is unknown";
    const std::string who = "process " + std::to_string(pid);
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return who + " cannot be opened (another user?)";
    HANDLE token = nullptr;
    const bool opened = OpenProcessToken(proc, TOKEN_QUERY, &token) != FALSE;
    CloseHandle(proc);
    if (!opened) return who + ": its token cannot be read (run the client at the game's level)";
    uint32_t client_il = 0, our_il = 0;
    const std::wstring client_user = sid_of(token, TokenUser);
    sid_of(token, TokenIntegrityLevel, &client_il);
    CloseHandle(token);
    const std::wstring our_user = token_sid(TokenUser);
    token_sid(TokenIntegrityLevel, &our_il);
    if (!client_identity_ok(client_user, client_il, our_user, our_il))
        return who + " runs as another user or below the game's integrity level";
    return "";
}

std::wstring pipe_sddl(const std::wstring& user_sid, const std::wstring& integrity_sid) {
    std::wstring s = L"D:P(D;;GA;;;NU)(A;;GA;;;" + user_sid + L")(A;;GA;;;SY)";
    if (!integrity_sid.empty()) s += L"S:(ML;;NWNR;;;" + integrity_sid + L")";
    return s;
}

struct PipeServer::State {
    struct Slot {
        HANDLE pipe = INVALID_HANDLE_VALUE;
        ClientId id = 0;
        OVERLAPPED rd{}, wr{};
        HANDLE rd_ev = nullptr, wr_ev = nullptr;
        bool reading = false, writing = false;
        char buf[16384];
        std::string in;           // the line being received
        bool discarding = false;  // inside an over-long line, until its newline
        std::deque<std::string> out;
        size_t out_bytes = 0;  // queued and being written
        std::string now_writing;
        size_t dropped = 0;    // droppable lines dropped since the last notice
        bool closing = false;  // close once everything queued is written, or at close_by
        bool graceful = false;  // closing after its last lines: the client may still read them
        ULONGLONG close_by = 0;
        ULONGLONG partial_since = 0;  // when the unfinished line began (0 = none)
    };

    // Closes the slot once what is queued is written, or after grace_ms at most (a client that never reads).
    static void close_after(Slot& s, ULONGLONG grace_ms) {
        const ULONGLONG by = GetTickCount64() + grace_ms;
        if (!s.closing) s.graceful = true;
        if (!s.closing || by < s.close_by) s.close_by = by;
        s.closing = true;
    }
    // Closes the slot at once and drops what is queued (the client went away, or it does not read).
    static void close_now(Slot& s) {
        s.closing = true;
        s.graceful = false;
        s.close_by = 0;
        s.out.clear();
        s.out_bytes = 0;
    }

    std::wstring name;
    PSECURITY_DESCRIPTOR sd = nullptr;
    PipeHandler handler;
    size_t max_line = 64 * 1024;
    int max_clients = 4;
    HANDLE stop_ev = nullptr, wake_ev = nullptr, conn_ev = nullptr;
    HANDLE thread = nullptr;
    std::atomic<bool> running{false};
    std::atomic<size_t> count{0};
    std::function<std::string(HANDLE)> client_check = check_pipe_client;
    uint32_t partial_ms = kPartialLineMs;
    std::string stop_reason;  // why the thread stopped by itself ("" = asked to)

    // listening instance
    HANDLE listen_pipe = INVALID_HANDLE_VALUE;
    OVERLAPPED conn{};
    bool listening = false;
    bool connected_now = false;
    ULONGLONG listen_retry_ms = 0;

    std::vector<std::unique_ptr<Slot>> slots;  // pipe thread only (OVERLAPPED must not move)
    ClientId next_id = 1;

    std::mutex outbox_lock;  // any thread -> pipe thread
    std::vector<std::tuple<ClientId, std::string, bool>> outbox;
    std::vector<ClientId> kicks;

    ~State() {
        for (HANDLE h : {stop_ev, wake_ev, conn_ev})
            if (h) CloseHandle(h);
        if (sd) LocalFree(sd);
    }

    HANDLE create_instance(bool first, DWORD* error) {
        SECURITY_ATTRIBUTES sa{sizeof sa, sd, FALSE};
        HANDLE p = CreateNamedPipeW(name.c_str(),
                                    PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                    static_cast<DWORD>(max_clients), 64 * 1024, 64 * 1024, 0, &sa);
        if (error) *error = p == INVALID_HANDLE_VALUE ? GetLastError() : 0;
        return p;
    }

    // Waits for a client on `p` (the first instance from start(), or a new one).
    void listen(HANDLE p) {
        listen_pipe = p;
        std::memset(&conn, 0, sizeof conn);
        conn.hEvent = conn_ev;
        ResetEvent(conn_ev);
        connected_now = false;
        listening = true;
        if (ConnectNamedPipe(p, &conn)) {
            connected_now = true;
        } else {
            const DWORD e = GetLastError();
            if (e == ERROR_PIPE_CONNECTED) {
                connected_now = true;  // a client came between create and connect
            } else if (e != ERROR_IO_PENDING) {
                CloseHandle(p);
                listen_pipe = INVALID_HANDLE_VALUE;
                listening = false;
                listen_retry_ms = GetTickCount64() + 1000;
                return;
            }
        }
        if (connected_now) SetEvent(conn_ev);
    }

    // False when the bridge must stop: no instance was left and another process took the name meanwhile.
    bool maybe_listen() {
        if (listening || slots.size() >= static_cast<size_t>(max_clients)) return true;
        if (GetTickCount64() < listen_retry_ms) return true;
        // With no instance left the pipe is gone: the next one must be a first instance again, or it could join a pipe
        // another process made under the name in between (with its own, open, security).
        const bool first = slots.empty();
        DWORD e = 0;
        HANDLE p = create_instance(first, &e);
        if (p == INVALID_HANDLE_VALUE) {
            if (first && e == ERROR_ACCESS_DENIED) {
                stop_reason = "the pipe name was taken by another process";
                return false;
            }
            listen_retry_ms = GetTickCount64() + 1000;  // e.g. every instance is in use: try again later
            return true;
        }
        listen(p);
        return true;
    }

    void start_read(Slot& s) {
        if (s.closing) return;
        std::memset(&s.rd, 0, sizeof s.rd);
        s.rd.hEvent = s.rd_ev;
        ResetEvent(s.rd_ev);
        if (ReadFile(s.pipe, s.buf, sizeof s.buf, nullptr, &s.rd) || GetLastError() == ERROR_IO_PENDING) {
            s.reading = true;
            return;
        }
        close_now(s);  // broken
    }

    void check_connect() {
        if (!listening) return;
        if (!connected_now) {
            if (!HasOverlappedIoCompleted(&conn)) return;
            DWORD n = 0;
            if (!GetOverlappedResult(listen_pipe, &conn, &n, FALSE)) {
                CloseHandle(listen_pipe);
                listen_pipe = INVALID_HANDLE_VALUE;
                listening = false;
                return;
            }
        }
        const std::string refused = client_check ? client_check(listen_pipe) : std::string();
        if (!refused.empty()) {
            if (handler.refused) handler.refused(refused);
            DisconnectNamedPipe(listen_pipe);
            CloseHandle(listen_pipe);
            listen_pipe = INVALID_HANDLE_VALUE;
            listening = false;
            return;
        }
        auto s = std::make_unique<Slot>();
        s->pipe = listen_pipe;
        s->id = next_id++;
        s->rd_ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        s->wr_ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        listen_pipe = INVALID_HANDLE_VALUE;
        listening = false;
        if (!s->rd_ev || !s->wr_ev) {
            close_handles(*s);
            return;
        }
        Slot& ref = *s;
        slots.push_back(std::move(s));
        count = slots.size();
        if (handler.connected) handler.connected(ref.id);
        start_read(ref);
    }

    void feed(Slot& s, const char* data, size_t n) {
        feed_lines(s, data, n);
        if (s.in.empty() && !s.discarding)
            s.partial_since = 0;
        else if (!s.partial_since)
            s.partial_since = GetTickCount64();
    }

    void feed_lines(Slot& s, const char* data, size_t n) {
        while (n && !s.closing) {
            const char* nl = static_cast<const char*>(std::memchr(data, '\n', n));
            const size_t take = nl ? static_cast<size_t>(nl - data) : n;
            if (!s.discarding) {
                if (s.in.size() + take > max_line) {
                    s.in.clear();
                    s.discarding = true;
                    if (handler.too_long) handler.too_long(s.id);
                } else {
                    s.in.append(data, take);
                }
            }
            if (!nl) return;
            if (s.discarding) {
                s.discarding = false;  // the over-long line ends here
            } else {
                std::string line;
                line.swap(s.in);
                if (handler.line) handler.line(s.id, line);
            }
            data = nl + 1;
            n -= take + 1;
        }
    }

    void check_read(Slot& s) {
        if (!s.reading || !HasOverlappedIoCompleted(&s.rd)) return;
        DWORD n = 0;
        const BOOL ok = GetOverlappedResult(s.pipe, &s.rd, &n, FALSE);
        s.reading = false;
        if (!ok) {  // the client went away
            close_now(s);
            return;
        }
        feed(s, s.buf, n);
        start_read(s);
    }

    void pump_write(Slot& s) {
        if (s.writing || s.out.empty() || s.pipe == INVALID_HANDLE_VALUE) return;
        s.now_writing = std::move(s.out.front());
        s.out.pop_front();
        std::memset(&s.wr, 0, sizeof s.wr);
        s.wr.hEvent = s.wr_ev;
        ResetEvent(s.wr_ev);
        if (WriteFile(s.pipe, s.now_writing.data(), static_cast<DWORD>(s.now_writing.size()), nullptr, &s.wr) ||
            GetLastError() == ERROR_IO_PENDING) {
            s.writing = true;
        } else {
            close_now(s);
        }
    }

    void check_write(Slot& s) {
        if (!s.writing || !HasOverlappedIoCompleted(&s.wr)) return;
        DWORD n = 0;
        const BOOL ok = GetOverlappedResult(s.pipe, &s.wr, &n, FALSE);
        s.writing = false;
        if (!ok) {
            close_now(s);
            return;
        }
        const size_t size = s.now_writing.size();
        s.out_bytes -= (std::min)(s.out_bytes, static_cast<size_t>(n));
        if (n < size) s.out.push_front(s.now_writing.substr(n));  // the rest goes next
        s.now_writing.clear();
        if (s.dropped && s.out_bytes < (1u << 20)) {
            std::string note = "{\"event\":\"dropped\",\"lines\":" + std::to_string(s.dropped) + "}\n";
            s.out_bytes += note.size();
            s.out.push_back(std::move(note));
            s.dropped = 0;
        }
    }

    static void close_handles(Slot& s) {
        if (s.pipe != INVALID_HANDLE_VALUE) {
            // After its last lines were written, only closing our end lets the client still read them (a disconnect
            // would throw away what it has not read yet).
            const bool keep_data = s.graceful && !s.writing && s.out.empty();
            CancelIoEx(s.pipe, nullptr);
            DWORD n = 0;
            if (s.reading) GetOverlappedResult(s.pipe, &s.rd, &n, TRUE);
            if (s.writing) GetOverlappedResult(s.pipe, &s.wr, &n, TRUE);
            s.reading = s.writing = false;
            if (!keep_data) DisconnectNamedPipe(s.pipe);
            CloseHandle(s.pipe);
            s.pipe = INVALID_HANDLE_VALUE;
        }
        if (s.rd_ev) CloseHandle(s.rd_ev);
        if (s.wr_ev) CloseHandle(s.wr_ev);
        s.rd_ev = s.wr_ev = nullptr;
    }

    void close_finished() {
        for (size_t i = 0; i < slots.size();) {
            Slot& s = *slots[i];
            const bool done = s.closing && ((!s.writing && s.out.empty()) || GetTickCount64() >= s.close_by);
            if (!done) {
                ++i;
                continue;
            }
            const ClientId id = s.id;
            close_handles(s);
            slots.erase(slots.begin() + static_cast<std::ptrdiff_t>(i));
            count = slots.size();
            if (handler.disconnected) handler.disconnected(id);
        }
    }

    Slot* find(ClientId id) {
        for (auto& s : slots)
            if (s->id == id) return s.get();
        return nullptr;
    }

    void drain_outbox() {
        std::vector<std::tuple<ClientId, std::string, bool>> items;
        std::vector<ClientId> kicked;
        {
            const std::lock_guard<std::mutex> l(outbox_lock);
            items.swap(outbox);
            kicked.swap(kicks);
        }
        for (auto& [id, line, droppable] : items) {
            Slot* s = find(id);
            if (!s || s->closing) continue;  // going: nothing more for it
            if (droppable && s->out_bytes > kDropAbove) {
                ++s->dropped;
                continue;
            }
            if (s->out_bytes > kDisconnectAbove) {  // it does not read: let it go
                close_now(*s);
                continue;
            }
            line += '\n';
            s->out_bytes += line.size();
            s->out.push_back(std::move(line));
        }
        for (ClientId id : kicked)
            if (Slot* s = find(id)) close_after(*s, 2000);  // its last reply first, if it reads
    }

    void run() {
        for (;;) {
            HANDLE hs[3 + 2 * 16];
            DWORD n = 0;
            hs[n++] = stop_ev;
            hs[n++] = wake_ev;
            if (listening) hs[n++] = conn_ev;
            for (auto& s : slots) {
                if (n + 2 > sizeof hs / sizeof hs[0]) break;
                if (s->reading) hs[n++] = s->rd_ev;
                if (s->writing) hs[n++] = s->wr_ev;
            }
            const DWORD r = WaitForMultipleObjects(n, hs, FALSE, 50);
            if (r == WAIT_OBJECT_0) break;
            check_connect();
            for (size_t i = 0; i < slots.size(); ++i) check_read(*slots[i]);  // handlers may send: slots stay put
            drain_outbox();
            for (auto& s : slots) {
                check_write(*s);
                pump_write(*s);
            }
            if (handler.poll) handler.poll();
            drain_outbox();
            for (auto& s : slots) pump_write(*s);
            const ULONGLONG now = GetTickCount64();
            for (auto& s : slots)
                if (s->partial_since && now - s->partial_since > partial_ms) close_now(*s);  // a line never finished
            close_finished();
            if (!maybe_listen()) break;
        }
        for (auto& s : slots) {
            const ClientId id = s->id;
            close_handles(*s);
            if (handler.disconnected) handler.disconnected(id);
        }
        slots.clear();
        count = 0;
        if (listen_pipe != INVALID_HANDLE_VALUE) {
            CancelIoEx(listen_pipe, nullptr);
            DWORD n = 0;
            if (!connected_now) GetOverlappedResult(listen_pipe, &conn, &n, TRUE);
            CloseHandle(listen_pipe);
            listen_pipe = INVALID_HANDLE_VALUE;
        }
    }

    // run() with every C++ exception caught; the reason, or nullptr.
    const char* run_caught() {
        try {
            run();
            return nullptr;
        } catch (const std::bad_alloc&) {
            return "out of memory in the bridge's pipe thread";
        } catch (...) {
            return "an exception in the bridge's pipe thread";
        }
    }
    // And hardware faults (a bug): the bridge stops, the game goes on.
    static const char* run_guarded(State* st) {
        __try {
            return st->run_caught();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return "a fault in the bridge's pipe thread";
        }
    }
    static void close_all_guarded(State* st) {
        __try {
            st->close_all();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    // After a fault: closes what can be closed, without the handler (its state may be broken).
    void close_all() {
        for (auto& s : slots) close_handles(*s);
        slots.clear();
        count = 0;
        if (listen_pipe != INVALID_HANDLE_VALUE) {
            CancelIoEx(listen_pipe, nullptr);
            CloseHandle(listen_pipe);
            listen_pipe = INVALID_HANDLE_VALUE;
        }
    }

    static DWORD WINAPI thread_main(void* p) {
        auto* keep = static_cast<std::shared_ptr<State>*>(p);
        std::shared_ptr<State> self = *keep;  // the state lives as long as the thread, even if the server is gone
        delete keep;
        if (const char* fault = run_guarded(self.get())) {
            close_all_guarded(self.get());
            self->stop_reason = fault;
        }
        self->running = false;
        if (!self->stop_reason.empty() && self->handler.stopped) {
            try {
                self->handler.stopped(self->stop_reason);
            } catch (...) {
            }
        }
        return 0;
    }
};

PipeServer::PipeServer() : s_(std::make_shared<State>()) {}

PipeServer::~PipeServer() { stop(2000); }

bool PipeServer::start(const std::wstring& name, const std::wstring& sddl, PipeHandler handler, size_t max_line,
                       int max_clients, std::string* why) {
    auto fail = [why](std::string text) {
        if (why) *why = std::move(text);
        return false;
    };
    State& s = *s_;
    if (s.running) return fail("the pipe server is already running");
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &s.sd, nullptr))
        return fail(win_error("the pipe's security descriptor could not be built", GetLastError()));
    s.name = name;
    s.handler = std::move(handler);
    s.max_line = max_line;
    s.max_clients = max_clients < 1 ? 1 : max_clients;
    s.stop_ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    s.wake_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s.conn_ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!s.stop_ev || !s.wake_ev || !s.conn_ev) return fail(win_error("could not create events", GetLastError()));
    DWORD e = 0;
    HANDLE first = s.create_instance(true, &e);
    if (first == INVALID_HANDLE_VALUE)
        return fail(e == ERROR_ACCESS_DENIED ? "the pipe name is already in use by another process"
                                              : win_error("could not create the pipe", e));
    s.listen(first);
    auto* keep = new std::shared_ptr<State>(s_);
    s.running = true;
    s.thread = CreateThread(nullptr, 0, State::thread_main, keep, 0, nullptr);
    if (!s.thread) {
        s.running = false;
        delete keep;
        CloseHandle(first);
        s.listen_pipe = INVALID_HANDLE_VALUE;
        s.listening = false;
        return fail(win_error("could not start the pipe thread", GetLastError()));
    }
    return true;
}

void PipeServer::send(ClientId c, std::string line, bool droppable) {
    {
        const std::lock_guard<std::mutex> l(s_->outbox_lock);
        s_->outbox.emplace_back(c, std::move(line), droppable);
    }
    if (s_->wake_ev) SetEvent(s_->wake_ev);
}

void PipeServer::disconnect(ClientId c) {
    {
        const std::lock_guard<std::mutex> l(s_->outbox_lock);
        s_->kicks.push_back(c);
    }
    if (s_->wake_ev) SetEvent(s_->wake_ev);
}

void PipeServer::wake() {
    if (s_->wake_ev) SetEvent(s_->wake_ev);
}

bool PipeServer::stop(uint32_t timeout_ms) {
    State& s = *s_;
    if (!s.thread) return true;
    SetEvent(s.stop_ev);
    if (WaitForSingleObject(s.thread, timeout_ms) != WAIT_OBJECT_0) return false;  // left behind, with its state
    CloseHandle(s.thread);
    s.thread = nullptr;
    return true;
}

HANDLE PipeServer::wake_event() const { return s_->wake_ev; }

void PipeServer::set_client_check(std::function<std::string(HANDLE)> check) { s_->client_check = std::move(check); }

void PipeServer::set_partial_line_ms(uint32_t ms) { s_->partial_ms = ms; }

bool PipeServer::running() const { return s_->running.load(); }

size_t PipeServer::client_count() const { return s_->count.load(); }

}  // namespace emf::core::bridge
