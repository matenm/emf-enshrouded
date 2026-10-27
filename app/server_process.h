#pragma once
// Starting and stopping a dedicated server from EMF.exe (docs/app.md "Servers").
//
// Start: enshrouded_server.exe runs from its folder in a console window of its own (minimized), inside a job object of
// EMF.exe (which only groups its processes: nothing is ended when the job closes). What closing EMF.exe does is the
// user's choice: by default nothing (the server keeps running); with "stop it first" EMF.exe stops it cleanly, as Stop
// does, before its window closes (stop_servers_on_close). A server is never ended because EMF.exe ends: a cut save
// cannot be repaired, a server left running can be stopped from the next EMF.exe.
//
// Stop: the server handles Ctrl+C and Ctrl+Break as a clean shutdown: it saves the world and tells the players, then
// exits (its console handler, build 1273043: 0x140575fb0, logs "[app] Trigger gameflow shutdown, exit: Ctrl_C"; on
// a real server its shutdown steps and a last "world saved" followed). Closing its console window is
// not the same: Windows ends the process about 5 s after the close event, so a longer save is cut (use Stop).
// A program without the server's console cannot press those keys, and Ctrl+C typed into some shells never becomes the
// console's signal. So a process of its own (EMF.exe --stop-server <pid>) attaches to the server's console and sends
// Ctrl+C, as a person would press it there. Every process on that console gets it: when another program shares the
// console (the command window the server was started from, another server), it sends nothing and EMF asks the user
// first (--shared-ok). A process can be started with Ctrl+C switched off; when the server has not exited after a few
// seconds and its log does not say it shuts down, EMF.exe --stop-server-break sends Ctrl+Break, which no one can switch
// off. When neither reaches the server, or it does not end in time, EMF waits for the next autosave line in
// emf\emf_core.log ("world saved", every 30 s), waits a few seconds more and then ends the process, so nothing of the
// world is lost. It never does that once the server's log says it shuts down (its shutdown save writes that line too):
// then it only waits. Without such a line the server is left running and the user is told (Stop now ends it at once).
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "locate.h"

namespace emf::app {

struct ServerStart {
    bool ok = false;
    std::string error;
    unsigned long pid = 0;
};

enum class ConsoleShow { Minimized, Hidden };  // the server's console window: minimized (the app), hidden (tests)

class ServerRunner {
public:
    ServerRunner() = default;
    ServerRunner(const ServerRunner&) = delete;
    ServerRunner& operator=(const ServerRunner&) = delete;
    // Closes its handles: every server keeps running (stop_servers_on_close stops those set to stop, before).
    ~ServerRunner();

    // Runs <folder>\enshrouded_server.exe with the folder as its working folder.
    ServerStart start(const std::wstring& folder, bool stop_on_close, ConsoleShow show = ConsoleShow::Minimized);
    // Whether the server started from `folder` is stopped when EMF.exe closes; false when none started here runs.
    bool set_stop_on_close(const std::wstring& folder, bool on);
    // `pid` is a server this runner started and it still runs.
    bool started_here(unsigned long pid) const;
    // The pids of the servers started here that still run (with `only_stop_on_close`: those set to stop on close).
    std::vector<unsigned long> running(bool only_stop_on_close) const;
    struct Running {
        unsigned long pid = 0;
        std::wstring folder;  // the folder it was started from, as given
    };
    std::vector<Running> running_servers(bool only_stop_on_close) const;

private:
    struct Started {
        std::wstring folder;
        void* job = nullptr;
        void* process = nullptr;
        unsigned long pid = 0;
        bool stop_on_close = false;
    };
    mutable std::mutex m_;  // a stop runs on a worker thread
    std::vector<Started> started_;
};

// The enshrouded_server.exe running from `folder` (its pid), 0 when none. The folder may be reached through a junction,
// a symbolic link or a subst drive, on either side.
unsigned long server_pid(const std::wstring& folder, const std::vector<ProcessInfo>& processes);

// The stop signal, sent from a process that exists only for that (EMF.exe --stop-server <pid>): it leaves its own
// console, attaches to the server's and sends Ctrl+C (Ctrl+Break with `ctrl_break`). 0 = sent; 1 = `pid` is not
// enshrouded_server.exe; 2 = its console could not be reached (a server started without one); 3 = Windows refused;
// 4 = another program shares the console, 5 = another enshrouded_server.exe does (nothing sent unless `shared_ok`).
int send_stop_signal(unsigned long pid, bool ctrl_break, bool shared_ok = false);
// Runs `helper` --stop-server <pid> (--stop-server-break with `ctrl_break`, --shared-ok with `shared_ok`) and returns
// its exit code; -1 when it could not run or did not end in time.
int run_stop_helper(const std::wstring& helper, unsigned long pid, bool ctrl_break, unsigned timeout_ms = 15000,
                    bool shared_ok = false);

struct StopOptions {
    std::wstring helper;              // the exe that sends the signal: EMF.exe itself
    std::wstring core_log;            // the server's emf\emf_core.log, for the autosave line
    std::wstring server_log;          // the server's own log: Ctrl+Break only when it does not say it shuts down
    unsigned exit_wait_ms = 60000;    // how long the server may take to save and exit after the signal
    unsigned break_after_ms = 10000;  // Ctrl+Break follows Ctrl+C when the server still runs this long after it
    unsigned after_autosave_ms = 3000;  // after the autosave line, before EMF ends it (its files are written whole)
    bool shared_ok = false;           // the user agreed to Ctrl+C reaching the other programs on the server's console
    unsigned autosave_wait_ms = 45000;  // then how long to wait for an autosave line (they come every 30 s)
    std::function<void(const std::string&)> progress;  // a line for the page (any thread)
    const std::atomic<bool>* cancel = nullptr;
};
struct StopOutcome {
    bool stopped = false;
    std::string how;   // "clean", "after_autosave", "not_stopped", "shared_console" (asks first), "gone" (not running)
    std::string text;  // what happened, for the player
};
StopOutcome stop_server(unsigned long pid, const StopOptions& o);
// What EMF.exe's window does when it closes: the servers `runner` started that are set to stop, stopped together as Stop
// does (their logs from the folder each was started from); the outcome of each.
std::vector<StopOutcome> stop_servers_on_close(ServerRunner& runner, const std::wstring& helper);
// Ends the server at once ("Stop now"): what changed since its last autosave (up to 30 s) is lost.
bool kill_server(unsigned long pid);
// A line EMF's core writes after each world save ("world saved (slot 0x0001)").
bool autosave_line_in(const std::string& text);

}  // namespace emf::app
