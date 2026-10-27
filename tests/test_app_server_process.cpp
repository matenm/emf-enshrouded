// EMF.exe starts and stops a dedicated server: against a stand-in enshrouded_server.exe
// (tools/standin_server) copied into a temp server folder. The stop signal is sent by EMF.exe itself (EMF.exe
// --stop-server <pid>, a process of its own, as in the app), so this also checks that path of the real EMF.exe.
#include <doctest.h>

#include <windows.h>

#include <filesystem>
#include <functional>
#include <string>

#include "app/args.h"
#include "app/locate.h"
#include "app/server_process.h"
#include "app/text.h"
#include "app_test_files.h"

using namespace emf::app;
using app_test::TempGame;

namespace {

std::wstring build_dir() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    const std::wstring exe(buf, n);
    return exe.substr(0, exe.find_last_of(L'\\'));
}

std::wstring emf_exe() { return build_dir() + L"\\app\\EMF.exe"; }

// A temp server folder with the stand-in as enshrouded_server.exe (and standin.txt when given).
struct TempServer : TempGame {
    explicit TempServer(const wchar_t* name, const std::string& standin = {}) : TempGame(name, false) {
        std::filesystem::copy_file(build_dir() + L"\\standin_server\\enshrouded_server.exe", at(L"enshrouded_server.exe"));
        if (!standin.empty()) write(L"standin.txt", standin);
    }
};

bool wait_for(const std::function<bool()>& cond, unsigned ms) {
    const ULONGLONG until = GetTickCount64() + ms;
    while (GetTickCount64() < until) {
        if (cond()) return true;
        Sleep(50);
    }
    return cond();
}

bool alive(unsigned long pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return false;
    const bool running = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return running;
}

bool ended_within(unsigned long pid, unsigned ms) { return wait_for([&] { return !alive(pid); }, ms); }

// Ends a stand-in a test left running.
struct Reaper {
    unsigned long pid = 0;
    ~Reaper() {
        if (pid && alive(pid)) kill_server(pid);
    }
};

StopOptions quick(const TempServer& srv) {
    StopOptions o;
    o.helper = emf_exe();
    o.core_log = srv.at(L"emf\\emf_core.log");
    o.exit_wait_ms = 10000;
    o.autosave_wait_ms = 6000;
    return o;
}

}  // namespace

TEST_CASE("app server process: the build has the stand-in server and EMF.exe for the stop signal") {
    REQUIRE(exists(build_dir() + L"\\standin_server\\enshrouded_server.exe"));
    REQUIRE(exists(emf_exe()));
}

TEST_CASE("app server process: a server starts from its folder, shows with its pid and stops cleanly on Ctrl+C") {
    TempServer srv(L"proc_clean");
    SetConsoleCtrlHandler(nullptr, TRUE);  // as a shell or launcher may have started EMF.exe: ignoring Ctrl+C is inherited
    ServerRunner runner;
    const ServerStart st = runner.start(srv.dir, false, ConsoleShow::Hidden);
    REQUIRE_MESSAGE(st.ok, st.error);
    Reaper reap{st.pid};
    CHECK(st.pid != 0);
    REQUIRE(wait_for([&] { return srv.read(L"logs\\enshrouded_server.log").find("Run after") != std::string::npos; }, 10000));
    CHECK(server_pid(srv.dir, game_processes()) == st.pid);
    CHECK(runner.started_here(st.pid));
    CHECK(server_pid(srv.dir + L"_other", game_processes()) == 0);

    std::vector<std::string> steps;
    StopOptions o = quick(srv);
    o.progress = [&](const std::string& s) { steps.push_back(s); };
    const StopOutcome out = stop_server(st.pid, o);
    CHECK_MESSAGE(out.stopped, out.text);
    CHECK(out.how == "clean");
    CHECK(ended_within(st.pid, 2000));
    CHECK(srv.read(L"stopped.txt") == "Ctrl_C");  // the server's own shutdown ran
    CHECK(srv.read(L"logs\\enshrouded_server.log").find("[app] Trigger gameflow shutdown, exit: Ctrl_C") != std::string::npos);
    CHECK_FALSE(steps.empty());
    CHECK_FALSE(runner.started_here(st.pid));
    CHECK(server_pid(srv.dir, game_processes()) == 0);
}

TEST_CASE("app server process: closing EMF.exe stops the servers set to stop, cleanly; it never kills one") {
    TempServer left(L"proc_left");
    TempServer stopped(L"proc_stopped");
    Reaper r1, r2;
    {
        ServerRunner runner;
        const ServerStart a = runner.start(left.dir, false, ConsoleShow::Hidden);
        const ServerStart b = runner.start(stopped.dir, false, ConsoleShow::Hidden);
        REQUIRE(a.ok);
        REQUIRE(b.ok);
        r1.pid = a.pid;
        r2.pid = b.pid;
        CHECK(runner.set_stop_on_close(stopped.dir, true));  // chosen after the start
        CHECK_FALSE(runner.set_stop_on_close(left.dir + L"_none", true));
        CHECK(runner.running(true) == std::vector<unsigned long>{b.pid});
        CHECK(runner.running(false).size() == 2);
        REQUIRE(wait_for([&] { return stopped.has(L"logs\\enshrouded_server.log"); }, 10000));
        // what the window's close does: the servers set to stop, together, as Stop does
        const std::vector<StopOutcome> outs = stop_servers_on_close(runner, emf_exe());
        REQUIRE(outs.size() == 1);
        CHECK_MESSAGE(outs[0].stopped, outs[0].text);
        CHECK(outs[0].how == "clean");
    }  // the runner (EMF.exe) goes: its job handles close, and nothing is ended by that
    CHECK(stopped.read(L"stopped.txt") == "Ctrl_C");
    Sleep(500);
    CHECK(alive(r1.pid));  // left running (an EMF.exe that crashed or was ended leaves its servers running too)
    const StopOutcome out = stop_server(r1.pid, quick(left));  // a server started by an earlier EMF.exe stops the same way
    CHECK(out.stopped);
    CHECK(left.read(L"stopped.txt") == "Ctrl_C");
}

TEST_CASE("app server process: a server that ignores Ctrl+C (started with it switched off) gets Ctrl+Break") {
    TempServer srv(L"proc_break", "ignore_ctrl_c\n");
    ServerRunner runner;
    const ServerStart st = runner.start(srv.dir, false, ConsoleShow::Hidden);
    REQUIRE(st.ok);
    Reaper reap{st.pid};
    REQUIRE(wait_for([&] { return srv.has(L"logs\\enshrouded_server.log"); }, 10000));
    StopOptions o = quick(srv);
    o.break_after_ms = 1500;
    std::vector<std::string> steps;
    o.progress = [&](const std::string& s) { steps.push_back(s); };
    const StopOutcome out = stop_server(st.pid, o);
    CHECK_MESSAGE(out.stopped, out.text);
    CHECK(out.how == "clean");
    CHECK(srv.read(L"stopped.txt") == "Ctrl_Break");
    std::string all;
    for (const auto& s : steps) all += s + "\n";
    CHECK(all.find("Ctrl+Break") != std::string::npos);
}

TEST_CASE("app server process: a server that ignores the signal is stopped after its next autosave line") {
    TempServer srv(L"proc_autosave", "ignore_ctrl\nautosave_ms=1500\n");
    ServerRunner runner;
    const ServerStart st = runner.start(srv.dir, false, ConsoleShow::Hidden);
    REQUIRE(st.ok);
    Reaper reap{st.pid};
    REQUIRE(wait_for([&] { return srv.has(L"logs\\enshrouded_server.log"); }, 10000));
    StopOptions o = quick(srv);
    o.exit_wait_ms = 1500;
    const StopOutcome out = stop_server(st.pid, o);
    CHECK_MESSAGE(out.stopped, out.text);
    CHECK(out.how == "after_autosave");
    CHECK(out.text.find("autosave") != std::string::npos);
    CHECK(ended_within(st.pid, 3000));
    CHECK_FALSE(srv.has(L"stopped.txt"));  // its own shutdown never ran
}

TEST_CASE("app server process: a server that neither stops nor autosaves keeps running, and Stop now ends it") {
    TempServer srv(L"proc_stuck", "ignore_ctrl\n");
    ServerRunner runner;
    const ServerStart st = runner.start(srv.dir, false, ConsoleShow::Hidden);
    REQUIRE(st.ok);
    Reaper reap{st.pid};
    REQUIRE(wait_for([&] { return srv.has(L"logs\\enshrouded_server.log"); }, 10000));
    StopOptions o = quick(srv);
    o.exit_wait_ms = 800;
    o.autosave_wait_ms = 1200;
    const StopOutcome out = stop_server(st.pid, o);
    CHECK_FALSE(out.stopped);
    CHECK(out.how == "not_stopped");
    CHECK(alive(st.pid));
    CHECK(kill_server(st.pid));
    CHECK(ended_within(st.pid, 3000));
}

TEST_CASE("app server process: the stop signal goes to enshrouded_server.exe only, and start needs the exe") {
    // EMF.exe --stop-server with a process that is not a server: refused (exit 1), nothing is sent
    CHECK(run_stop_helper(emf_exe(), GetCurrentProcessId(), false) == 1);
    CHECK(run_stop_helper(emf_exe(), GetCurrentProcessId(), true) == 1);
    CHECK(run_stop_helper(emf_exe(), 0, false) == 1);
    CHECK(run_stop_helper(L"C:\\nowhere\\EMF.exe", 1234, false) == -1);
    TempGame empty(L"proc_noexe", false);
    ServerRunner runner;
    const ServerStart st = runner.start(empty.dir, false, ConsoleShow::Hidden);
    CHECK_FALSE(st.ok);
    CHECK(st.error.find("enshrouded_server.exe") != std::string::npos);
}

TEST_CASE("app server process: the autosave line is EMF's world saved line") {
    CHECK(autosave_line_in("[11:06:02.065 t1] world saved (slot 0x0001)\r\n"));
    CHECK_FALSE(autosave_line_in("[11:05:52.382 t1] world loaded (slot 0x0001)\r\n"));
    CHECK_FALSE(autosave_line_in(""));
}

TEST_CASE("app server process: EMF.exe --stop-server <pid> is read from the command line, and a link drops it") {
    AppArgs a = parse_app_args({L"--stop-server", L"4242"});
    finalize_args(a, false);
    CHECK(a.stop_server);
    CHECK(a.stop_pid == 4242);
    CHECK_FALSE(a.stop_break);
    AppArgs brk = parse_app_args({L"--stop-server-break", L"4242"});
    finalize_args(brk, false);
    CHECK(brk.stop_server);
    CHECK(brk.stop_break);
    CHECK(brk.stop_pid == 4242);
    AppArgs bad = parse_app_args({L"--stop-server", L"12x"});
    CHECK(bad.stop_server);
    CHECK(bad.stop_pid == 0);
    AppArgs shared = parse_app_args({L"--stop-server", L"4242", L"--shared-ok"});
    finalize_args(shared, false);
    CHECK(shared.stop_shared_ok);
    CHECK_FALSE(a.stop_shared_ok);
    AppArgs link = parse_app_args({L"--nxm", L"nxm://enshrouded/mods/1/files/2", L"--stop-server", L"4242"});
    finalize_args(link, false);
    CHECK_FALSE(link.stop_server);
}

TEST_CASE("app server process: no Ctrl+Break while the server's log says it is shutting down (a long final save)") {
    TempServer srv(L"proc_slow", "slow_shutdown_ms=3000\n");
    ServerRunner runner;
    const ServerStart st = runner.start(srv.dir, false, ConsoleShow::Hidden);
    REQUIRE(st.ok);
    Reaper reap{st.pid};
    REQUIRE(wait_for([&] { return srv.read(L"logs\\enshrouded_server.log").find("Run after") != std::string::npos; }, 10000));
    StopOptions o = quick(srv);
    o.server_log = srv.at(L"logs\\enshrouded_server.log");
    o.break_after_ms = 1000;
    const StopOutcome out = stop_server(st.pid, o);
    CHECK_MESSAGE(out.stopped, out.text);
    CHECK(srv.read(L"stopped.txt") == "Ctrl_C");
    CHECK_FALSE(srv.has(L"second_signal.txt"));
}

TEST_CASE("app server process: a server reached through a junction is found, and found through the junction") {
    TempServer srv(L"proc_junction");
    wchar_t base[MAX_PATH];
    GetTempPathW(MAX_PATH, base);
    const std::wstring link = std::wstring(base) + L"emf_app_test_srvjunction_" + std::to_wstring(GetCurrentProcessId());
    RemoveDirectoryW(link.c_str());
    REQUIRE(_wsystem((L"cmd /c mklink /J \"" + link + L"\" \"" + srv.dir + L"\" >nul").c_str()) == 0);
    ServerRunner runner;
    const ServerStart st = runner.start(link, false, ConsoleShow::Hidden);  // started through the junction
    REQUIRE(st.ok);
    Reaper reap{st.pid};
    REQUIRE(wait_for([&] { return srv.has(L"logs\\enshrouded_server.log"); }, 10000));
    CHECK(server_pid(link, game_processes()) == st.pid);
    CHECK(server_pid(srv.dir, game_processes()) == st.pid);  // the real folder, whichever path the process shows
    kill_server(st.pid);
    RemoveDirectoryW(link.c_str());  // the junction only
}

TEST_CASE("app server process: no autosave kill once the server says it shuts down (its shutdown save writes that line)") {
    // its final save takes 9 s and writes its "world saved" line half-way: later than the 3 s grace after that line
    TempServer srv(L"proc_ack", "slow_shutdown_ms=9000\nautosave_ms=300\n");
    ServerRunner runner;
    const ServerStart st = runner.start(srv.dir, false, ConsoleShow::Hidden);
    REQUIRE(st.ok);
    Reaper reap{st.pid};
    REQUIRE(wait_for([&] { return srv.read(L"logs\\enshrouded_server.log").find("Run after") != std::string::npos; }, 10000));
    StopOptions o = quick(srv);
    o.server_log = srv.at(L"logs\\enshrouded_server.log");
    o.exit_wait_ms = 1500;  // shorter than its final save
    o.autosave_wait_ms = 12000;
    const StopOutcome out = stop_server(st.pid, o);
    CHECK_MESSAGE(out.stopped, out.text);
    CHECK(out.how == "clean");
    CHECK(srv.read(L"stopped.txt") == "Ctrl_C");  // its own shutdown finished: it was never ended half-way
}

TEST_CASE("app server process: the autosave kill waits a few seconds after the line, and not when it shuts down then") {
    TempServer srv(L"proc_after", "ignore_ctrl\nautosave_ms=800\n");
    ServerRunner runner;
    const ServerStart st = runner.start(srv.dir, false, ConsoleShow::Hidden);
    REQUIRE(st.ok);
    Reaper reap{st.pid};
    REQUIRE(wait_for([&] { return srv.has(L"logs\\enshrouded_server.log"); }, 10000));
    StopOptions o = quick(srv);
    o.exit_wait_ms = 800;
    o.after_autosave_ms = 1500;
    const ULONGLONG t0 = GetTickCount64();
    const StopOutcome out = stop_server(st.pid, o);
    CHECK(out.how == "after_autosave");
    CHECK(GetTickCount64() - t0 >= 800 + 1500);
}

TEST_CASE("app server process: Ctrl+C is not sent when another program shares the server's console, unless allowed") {
    TempServer srv(L"proc_twin", "spawn_twin\n");
    ServerRunner runner;
    const ServerStart st = runner.start(srv.dir, false, ConsoleShow::Hidden);
    REQUIRE(st.ok);
    Reaper reap{st.pid};
    REQUIRE(wait_for([&] { return srv.has(L"logs\\enshrouded_server.log"); }, 10000));
    Sleep(500);  // the twin attaches to the console
    StopOptions o = quick(srv);
    StopOutcome out = stop_server(st.pid, o);
    CHECK_FALSE(out.stopped);
    CHECK(out.how == "shared_console");
    CHECK(out.text.find("enshrouded_server.exe") != std::string::npos);  // another server shares it
    CHECK(alive(st.pid));
    CHECK_FALSE(srv.has(L"twin_stopped.txt"));
    o.shared_ok = true;  // the user said yes
    out = stop_server(st.pid, o);
    CHECK_MESSAGE(out.stopped, out.text);
    CHECK(srv.read(L"stopped.txt") == "Ctrl_C");
    CHECK(wait_for([&] { return srv.has(L"twin_stopped.txt"); }, 5000));  // it reached the twin too, as said
}
