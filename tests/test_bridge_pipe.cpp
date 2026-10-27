// The debug bridge's pipe: its security descriptor (only this user and SYSTEM, no network logons, a mandatory label),
// and the server in this process with real clients: hello and an error case, the 4-client limit, an over-long line,
// one owner per pipe name, and a stop that does not hang.
#include <windows.h>

#include <aclapi.h>
#include <sddl.h>

#include <doctest.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <new>
#include <string>

#include "core/bridge/pipe_server.h"
#include "core/bridge/protocol.h"
#include "core/bridge/service.h"
#include "core/json.h"

using namespace emf::core;
using namespace emf::core::bridge;

namespace {

std::wstring sid_string(PSID sid) {
    LPWSTR s = nullptr;
    if (!ConvertSidToStringSidW(sid, &s)) return L"";
    std::wstring out(s);
    LocalFree(s);
    return out;
}

std::wstring unique_pipe_name(const wchar_t* tag) {
    static int n = 0;
    return L"\\\\.\\pipe\\emf-bridge-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + tag + L"-" +
           std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++n);
}

// A client with a read timeout (overlapped handle), so a missing reply fails the test instead of hanging it.
class TestClient {
public:
    explicit TestClient(const std::wstring& name) {
        h_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
                         nullptr);
        error_ = h_ == INVALID_HANDLE_VALUE ? GetLastError() : 0;
        ev_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    ~TestClient() {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
        CloseHandle(ev_);
    }
    bool ok() const { return h_ != INVALID_HANDLE_VALUE; }
    DWORD error() const { return error_; }
    bool write(const std::string& data) {
        OVERLAPPED ov{};
        ov.hEvent = ev_;
        ResetEvent(ev_);
        DWORD n = 0;
        if (!WriteFile(h_, data.data(), static_cast<DWORD>(data.size()), nullptr, &ov) &&
            GetLastError() != ERROR_IO_PENDING)
            return false;
        if (WaitForSingleObject(ev_, 3000) != WAIT_OBJECT_0) {
            CancelIoEx(h_, &ov);
            GetOverlappedResult(h_, &ov, &n, TRUE);
            return false;
        }
        return GetOverlappedResult(h_, &ov, &n, FALSE) && n == data.size();
    }
    // The next line, or "" after `ms` without one (or when the pipe broke: broken() then says so).
    std::string read_line(DWORD ms = 3000) {
        const ULONGLONG until = GetTickCount64() + ms;
        for (;;) {
            const size_t nl = buf_.find('\n');
            if (nl != std::string::npos) {
                std::string line = buf_.substr(0, nl);
                buf_.erase(0, nl + 1);
                return line;
            }
            const ULONGLONG now = GetTickCount64();
            if (now >= until) return "";
            char chunk[4096];
            OVERLAPPED ov{};
            ov.hEvent = ev_;
            ResetEvent(ev_);
            DWORD n = 0;
            if (!ReadFile(h_, chunk, sizeof chunk, nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) {
                broken_ = true;
                return "";
            }
            if (WaitForSingleObject(ev_, static_cast<DWORD>(until - now)) != WAIT_OBJECT_0) {
                CancelIoEx(h_, &ov);
                GetOverlappedResult(h_, &ov, &n, TRUE);
                return "";
            }
            if (!GetOverlappedResult(h_, &ov, &n, FALSE)) {
                broken_ = true;
                return "";
            }
            buf_.append(chunk, n);
        }
    }
    bool broken() const { return broken_; }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    HANDLE ev_ = nullptr;
    DWORD error_ = 0;
    std::string buf_;
    bool broken_ = false;
};

// A pipe server wired to a service with fake sources, the way the core wires them.
struct Rig {
    std::wstring name;
    PipeServer pipe;
    std::unique_ptr<Service> service;
    bool started = false;
    std::string why;
    explicit Rig(const wchar_t* tag) : name(unique_pipe_name(tag)) {
        Sources s;
        s.hello = HelloInfo{"9.9.9", 16, 0, false, GetCurrentProcessId()};
        Output o;
        o.send = [this](ClientId c, std::string line, bool droppable) { pipe.send(c, std::move(line), droppable); };
        o.disconnect = [this](ClientId c) { pipe.disconnect(c); };
        service = std::make_unique<Service>(s, o);
        PipeHandler h;
        h.connected = [this](ClientId c) { service->on_connect(c); };
        h.line = [this](ClientId c, std::string_view l) { service->on_line(c, l); };
        h.too_long = [this](ClientId c) { service->on_too_long(c); };
        h.disconnected = [this](ClientId c) { service->on_disconnect(c); };
        h.poll = [this] { service->poll(); };
        started = pipe.start(name, pipe_sddl(current_user_sid(), current_integrity_sid()), h, kMaxLineIn, kMaxClients,
                             &why);
    }
};

json::Value parse(const std::string& line) {
    auto r = json::parse(line);
    REQUIRE_MESSAGE(r.value, line);
    return *r.value;
}

}  // namespace

TEST_CASE("bridge pipe: the security descriptor allows this user and SYSTEM only, denies network logons, keeps a label") {
    const std::wstring user = current_user_sid();
    REQUIRE(user.rfind(L"S-1-5-", 0) == 0);
    const std::wstring il = current_integrity_sid();
    REQUIRE(il.rfind(L"S-1-16-", 0) == 0);
    const std::wstring sddl = pipe_sddl(user, il);
    CHECK(sddl.find(user) != std::wstring::npos);
    CHECK(sddl.find(L"D:P") == 0);  // protected: nothing inherited
    CHECK(sddl.find(L"WD") == std::wstring::npos);  // no Everyone
    CHECK(sddl.find(L"AN") == std::wstring::npos);  // no anonymous
    CHECK(sddl.find(L"AU") == std::wstring::npos);  // no "authenticated users"
    CHECK(sddl.find(L"S:(ML;;NWNR;;;" + il + L")") != std::wstring::npos);

    PSECURITY_DESCRIPTOR sd = nullptr;
    REQUIRE(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr));
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    REQUIRE(GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted));
    REQUIRE(present);
    REQUIRE(dacl);
    REQUIRE(dacl->AceCount == 3);
    void* ace = nullptr;
    REQUIRE(GetAce(dacl, 0, &ace));  // the deny comes first
    CHECK(static_cast<ACE_HEADER*>(ace)->AceType == ACCESS_DENIED_ACE_TYPE);
    CHECK(sid_string(&static_cast<ACCESS_DENIED_ACE*>(ace)->SidStart) == L"S-1-5-2");  // NETWORK
    REQUIRE(GetAce(dacl, 1, &ace));
    CHECK(static_cast<ACE_HEADER*>(ace)->AceType == ACCESS_ALLOWED_ACE_TYPE);
    CHECK(sid_string(&static_cast<ACCESS_ALLOWED_ACE*>(ace)->SidStart) == user);
    REQUIRE(GetAce(dacl, 2, &ace));
    CHECK(static_cast<ACE_HEADER*>(ace)->AceType == ACCESS_ALLOWED_ACE_TYPE);
    CHECK(sid_string(&static_cast<ACCESS_ALLOWED_ACE*>(ace)->SidStart) == L"S-1-5-18");  // SYSTEM
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    REQUIRE(GetSecurityDescriptorControl(sd, &control, &revision));
    CHECK((control & SE_DACL_PROTECTED) != 0);
    LocalFree(sd);
}

TEST_CASE("bridge pipe: the pipe the server creates carries that descriptor") {
    Rig rig(L"acl");
    REQUIRE_MESSAGE(rig.started, rig.why);
    TestClient c(rig.name);
    REQUIRE(c.ok());
    // read the DACL back from the live pipe
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int tries = 0; tries < 100 && h == INVALID_HANDLE_VALUE; ++tries) {  // the next instance comes right after
        h = CreateFileW(rig.name.c_str(), READ_CONTROL, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) Sleep(10);
    }
    REQUIRE_MESSAGE(h != INVALID_HANDLE_VALUE, "could not open the live pipe to read its descriptor");
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    REQUIRE(GetSecurityInfo(h, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &sd) ==
            ERROR_SUCCESS);
    REQUIRE(dacl);
    CHECK(dacl->AceCount == 3);
    LocalFree(sd);
    CloseHandle(h);
    CHECK(rig.pipe.stop(3000));
}

TEST_CASE("bridge pipe: a process of this user at low integrity cannot open the pipe") {
    Rig rig(L"lowil");
    REQUIRE_MESSAGE(rig.started, rig.why);
    HANDLE self = nullptr, low = nullptr;
    REQUIRE(OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &self));
    REQUIRE(DuplicateTokenEx(self, TOKEN_QUERY | TOKEN_IMPERSONATE | TOKEN_ADJUST_DEFAULT, nullptr, SecurityImpersonation,
                             TokenImpersonation, &low));
    PSID low_sid = nullptr;
    REQUIRE(ConvertStringSidToSidW(L"S-1-16-4096", &low_sid));  // low integrity
    TOKEN_MANDATORY_LABEL label{};
    label.Label.Attributes = SE_GROUP_INTEGRITY;
    label.Label.Sid = low_sid;
    REQUIRE(SetTokenInformation(low, TokenIntegrityLevel, &label, sizeof label + GetLengthSid(low_sid)));
    REQUIRE(SetThreadToken(nullptr, low));
    HANDLE h = CreateFileW(rig.name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    const DWORD e = GetLastError();
    RevertToSelf();
    CHECK(h == INVALID_HANDLE_VALUE);
    CHECK(e == ERROR_ACCESS_DENIED);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    LocalFree(low_sid);
    CloseHandle(low);
    CloseHandle(self);
    TestClient normal(rig.name);  // this thread at its own level again: allowed
    CHECK(normal.ok());
    CHECK(rig.pipe.stop(3000));
}

TEST_CASE("bridge pipe: who may stay: the same user at the same or a higher integrity level") {
    const std::wstring me = L"S-1-5-21-1-2-3-1001";
    CHECK(client_identity_ok(me, 0x2000, me, 0x2000));
    CHECK(client_identity_ok(me, 0x3000, me, 0x2000));  // an elevated client
    CHECK_FALSE(client_identity_ok(me, 0x1000, me, 0x2000));  // low integrity
    CHECK_FALSE(client_identity_ok(L"S-1-5-21-1-2-3-1002", 0x2000, me, 0x2000));  // another user
    CHECK_FALSE(client_identity_ok(L"", 0x2000, me, 0x2000));
}

TEST_CASE("bridge pipe: the real client check lets a client of this process in; a refused client is closed at once") {
    {
        Rig rig(L"check");
        REQUIRE_MESSAGE(rig.started, rig.why);
        TestClient c(rig.name);  // the default check: the client's token is ours
        REQUIRE(c.ok());
        REQUIRE(c.write("{\"id\":1,\"cmd\":\"hello\"}\n"));
        CHECK(parse(c.read_line()).get("ok")->b);
        CHECK(rig.pipe.stop(3000));
    }
    PipeServer pipe;
    std::atomic<int> connected{0};
    std::string refused;
    std::mutex m;
    pipe.set_client_check([](HANDLE) { return std::string("another user"); });
    PipeHandler h;
    h.connected = [&](ClientId) { ++connected; };
    h.refused = [&](const std::string& why) {
        const std::lock_guard<std::mutex> l(m);
        refused = why;
    };
    std::string why;
    const std::wstring name = unique_pipe_name(L"refuse");
    REQUIRE(pipe.start(name, pipe_sddl(current_user_sid(), current_integrity_sid()), h, kMaxLineIn, 8, &why));
    TestClient c(name);
    REQUIRE(c.ok());
    CHECK(c.read_line(2000).empty());
    CHECK(c.broken());
    CHECK(connected.load() == 0);
    {
        const std::lock_guard<std::mutex> l(m);
        CHECK(refused == "another user");
    }
    CHECK(pipe.stop(3000));
}

TEST_CASE("bridge pipe: when the last instance closed and another process took the name, the server stops, never joins") {
    PipeServer pipe;
    const std::wstring name = unique_pipe_name(L"lapse");
    HANDLE squatter = INVALID_HANDLE_VALUE;
    std::string stopped;
    std::mutex m;
    PipeHandler h;
    h.disconnected = [&](ClientId) {  // the only instance just closed: another process creates the name now
        squatter = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                    PIPE_TYPE_BYTE | PIPE_WAIT, 4, 4096, 4096, 0, nullptr);
    };
    h.stopped = [&](const std::string& why) {
        const std::lock_guard<std::mutex> l(m);
        stopped = why;
    };
    std::string why;
    REQUIRE(pipe.start(name, pipe_sddl(current_user_sid(), current_integrity_sid()), h, kMaxLineIn, 1, &why));
    {
        TestClient c(name);  // with one client allowed, it holds the only instance
        REQUIRE(c.ok());
    }  // it leaves
    for (int i = 0; i < 300 && pipe.running(); ++i) Sleep(10);
    CHECK_FALSE(pipe.running());
    {
        const std::lock_guard<std::mutex> l(m);
        CHECK(stopped.find("taken") != std::string::npos);
    }
    REQUIRE(squatter != INVALID_HANDLE_VALUE);
    CloseHandle(squatter);
    CHECK(pipe.stop(3000));
}

TEST_CASE("bridge pipe: an exception or a fault in a handler stops the bridge, not the process") {
    for (int kind = 0; kind < 2; ++kind) {
        PipeServer pipe;
        std::string stopped;
        std::mutex m;
        PipeHandler h;
        h.line = [kind](ClientId, std::string_view) {
            if (kind == 0) throw std::bad_alloc();
            *static_cast<volatile int*>(nullptr) = 1;  // a bug
        };
        h.stopped = [&](const std::string& why) {
            const std::lock_guard<std::mutex> l(m);
            stopped = why;
        };
        std::string why;
        const std::wstring name = unique_pipe_name(kind ? L"fault" : L"throw");
        REQUIRE(pipe.start(name, pipe_sddl(current_user_sid(), current_integrity_sid()), h, kMaxLineIn, 8, &why));
        TestClient c(name);
        REQUIRE(c.ok());
        REQUIRE(c.write("{\"id\":1,\"cmd\":\"hello\"}\n"));
        for (int i = 0; i < 300 && pipe.running(); ++i) Sleep(10);
        CHECK_FALSE(pipe.running());
        {
            const std::lock_guard<std::mutex> l(m);
            CHECK_FALSE(stopped.empty());
        }
        CHECK(pipe.stop(3000));
    }
}

TEST_CASE("bridge pipe: a line left unfinished too long ends the connection") {
    PipeServer pipe;
    pipe.set_partial_line_ms(300);
    std::atomic<int> left{0};
    PipeHandler h;
    h.disconnected = [&](ClientId) { ++left; };
    std::string why;
    const std::wstring name = unique_pipe_name(L"partial");
    REQUIRE(pipe.start(name, pipe_sddl(current_user_sid(), current_integrity_sid()), h, kMaxLineIn, 8, &why));
    TestClient c(name);
    REQUIRE(c.ok());
    REQUIRE(c.write("{\"id\":1,\"cmd\""));  // and nothing more
    for (int i = 0; i < 200 && left.load() == 0; ++i) Sleep(10);
    CHECK(left.load() == 1);
    CHECK(pipe.stop(3000));
}

TEST_CASE("bridge pipe: a client exchanges hello and an error case; stop ends the thread and the client sees it") {
    Rig rig(L"hello");
    REQUIRE_MESSAGE(rig.started, rig.why);
    TestClient c(rig.name);
    REQUIRE(c.ok());
    REQUIRE(c.write("{\"id\":1,\"cmd\":\"hello\"}\n"));
    auto r = parse(c.read_line());
    CHECK(r.get("id")->n == 1);
    CHECK(r.get("ok")->b);
    CHECK(r.get("emf")->s == "9.9.9");
    CHECK(r.get("protocol")->n == 1);

    REQUIRE(c.write("not json\r\n{\"id\":\"two\",\"cmd\":\"nope\"}\n"));  // two lines in one write, CRLF too
    r = parse(c.read_line());
    CHECK_FALSE(r.get("ok")->b);
    CHECK(r.get("error")->s.find("not JSON") == 0);
    r = parse(c.read_line());
    CHECK(r.get("id")->s == "two");
    CHECK(r.get("error")->s.find("unknown command") == 0);
    CHECK(rig.pipe.client_count() == 1);

    const ULONGLONG t0 = GetTickCount64();
    CHECK(rig.pipe.stop(3000));
    CHECK(GetTickCount64() - t0 < 3000);
    CHECK(c.read_line(1000).empty());
    CHECK(c.broken());
}

TEST_CASE("bridge pipe: eight clients at once; a ninth finds the pipe busy until one leaves") {
    Rig rig(L"eight");
    REQUIRE_MESSAGE(rig.started, rig.why);
    std::unique_ptr<TestClient> clients[8];
    for (auto& cl : clients) {
        for (int tries = 0; tries < 100; ++tries) {  // the server opens the next instance right after a connect
            cl = std::make_unique<TestClient>(rig.name);
            if (cl->ok()) break;
            Sleep(10);
        }
        REQUIRE(cl->ok());
    }
    for (int i = 0; i < 8; ++i) {
        REQUIRE(clients[i]->write("{\"id\":" + std::to_string(i) + ",\"cmd\":\"hello\"}\n"));
        CHECK(parse(clients[i]->read_line()).get("id")->n == i);
    }
    TestClient ninth(rig.name);
    CHECK_FALSE(ninth.ok());
    CHECK(ninth.error() == ERROR_PIPE_BUSY);
    clients[2].reset();
    bool joined = false;
    for (int tries = 0; tries < 100 && !joined; ++tries) {
        TestClient again(rig.name);
        if (again.ok()) {
            joined = again.write("{\"id\":9,\"cmd\":\"hello\"}\n") && parse(again.read_line()).get("id")->n == 9;
            break;
        }
        Sleep(20);
    }
    CHECK(joined);
    CHECK(rig.pipe.stop(3000));
}

TEST_CASE("bridge pipe: a line over 64 KiB is dropped with an error; the next line works") {
    Rig rig(L"long");
    REQUIRE_MESSAGE(rig.started, rig.why);
    TestClient c(rig.name);
    REQUIRE(c.ok());
    REQUIRE(c.write(std::string(70 * 1024, 'x') + "\n{\"id\":2,\"cmd\":\"hello\"}\n"));
    auto r = parse(c.read_line());
    CHECK(r.get("error")->s == "the line is longer than 64 KiB");
    r = parse(c.read_line());
    CHECK(r.get("id")->n == 2);
    CHECK(r.get("ok")->b);
    CHECK(rig.pipe.stop(3000));
}

TEST_CASE("bridge pipe: a client sending garbage five times is disconnected") {
    Rig rig(L"garbage");
    REQUIRE_MESSAGE(rig.started, rig.why);
    TestClient c(rig.name);
    REQUIRE(c.ok());
    REQUIRE(c.write("a\nb\nc\nd\ne\n"));
    for (int i = 0; i < 5; ++i) CHECK_FALSE(c.read_line().empty());
    CHECK(c.read_line(2000).empty());
    CHECK(c.broken());
    CHECK(rig.pipe.stop(3000));
}

TEST_CASE("bridge pipe: a client that stops reading is closed: when kicked, and when too much waits for it") {
    PipeServer pipe;
    const std::wstring name = unique_pipe_name(L"stuck");
    std::atomic<int> left{0};
    PipeHandler h;
    h.connected = [&](ClientId c) {
        if (c == 1) {  // fills the pipe (it holds 64 KiB), then is told to go: its write never completes
            for (int i = 0; i < 4; ++i) pipe.send(c, std::string(256 * 1024, 'x'), false);
            pipe.disconnect(c);
        } else {  // more than 8 MiB waiting: closed at once
            for (int i = 0; i < 12; ++i) pipe.send(c, std::string(1024 * 1024, 'y'), false);
        }
    };
    h.disconnected = [&](ClientId) { ++left; };
    std::string why;
    REQUIRE(pipe.start(name, pipe_sddl(current_user_sid(), current_integrity_sid()), h, kMaxLineIn, 4, &why));
    TestClient kicked(name);  // never reads
    REQUIRE(kicked.ok());
    for (int i = 0; i < 400 && left.load() < 1; ++i) Sleep(10);
    CHECK(left.load() == 1);  // closed within about 2 s although its write was stuck
    TestClient flooded(name);
    REQUIRE(flooded.ok());
    for (int i = 0; i < 400 && left.load() < 2; ++i) Sleep(10);
    CHECK(left.load() == 2);
    CHECK(pipe.client_count() == 0);
    CHECK(pipe.stop(3000));
}

TEST_CASE("bridge pipe: the pipe's name and emf\\bridge.txt name the process") {
    CHECK(bridge_pipe_name(1234) == L"\\\\.\\pipe\\emf-bridge-1234");
    CHECK(bridge_info_text(bridge_pipe_name(1234), 1234) == "pipe=\\\\.\\pipe\\emf-bridge-1234\r\npid=1234\r\nprotocol=1\r\n");
}

TEST_CASE("bridge pipe: a second server cannot take a pipe name that is in use") {
    Rig first(L"owner");
    REQUIRE_MESSAGE(first.started, first.why);
    PipeServer second;
    std::string why;
    CHECK_FALSE(second.start(first.name, pipe_sddl(current_user_sid(), L""), PipeHandler{}, kMaxLineIn, 4, &why));
    CHECK(why.find("pipe") != std::string::npos);
    CHECK(first.pipe.stop(3000));
}
