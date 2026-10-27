#include "server_process.h"

#include <windows.h>

#include <cstdio>
#include <thread>

#include "servers.h"
#include "text.h"

namespace emf::app {

namespace {

std::wstring exe_name_of(HANDLE process) {
    wchar_t buf[4096];
    DWORD n = 4096;
    if (!QueryFullProcessImageNameW(process, 0, buf, &n)) return {};
    const std::wstring path(buf, n);
    return path.substr(path.find_last_of(L'\\') + 1);
}

bool still_running(HANDLE process) { return process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT; }

BOOL WINAPI swallow_ctrl(DWORD) { return TRUE; }  // the signal reaches the sender too: it must not end it

std::string now_clock() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char b[16];
    std::snprintf(b, sizeof b, "%02u:%02u:%02u", t.wHour, t.wMinute, t.wSecond);
    return b;
}

long long file_size(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(lp(path).c_str(), GetFileExInfoStandard, &d)) return -1;
    return (static_cast<long long>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
}

// The bytes of a growing log from `offset` on (at most 1 MB).
std::string read_from(const std::wstring& path, long long offset) {
    std::string out;
    FILE* f = open_shared(path);
    if (!f) return out;
    if (_fseeki64(f, offset, SEEK_SET) == 0) {
        char buf[65536];
        size_t n = 0;
        while (out.size() < (1u << 20) && (n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    }
    fclose(f);
    return out;
}

// A log that grows: what was added since the watch began, searched a part at a time (the end of the part before is
// kept, so a line written in two goes is still found).
struct LogWatch {
    std::wstring path;
    long long offset = 0;
    std::string tail;
    explicit LogWatch(std::wstring p) : path(std::move(p)) {
        const long long size = path.empty() ? 0 : file_size(path);
        offset = size > 0 ? size : 0;
    }
    bool saw(const std::string& needle) {
        if (path.empty()) return false;
        const long long size = file_size(path);
        if (size < 0) return false;
        if (size < offset) {  // a new log: read it from the start
            offset = 0;
            tail.clear();
        }
        if (size == offset) return false;
        const std::string part = read_from(path, offset);
        offset += static_cast<long long>(part.size());
        const std::string text = tail + part;
        tail = text.size() > 256 ? text.substr(text.size() - 256) : text;
        return text.find(needle) != std::string::npos;
    }
};

// Waits up to `ms` for the process to end, giving up early when cancelled. True when it ended.
bool wait_exit(HANDLE process, unsigned ms, const std::atomic<bool>* cancel) {
    const ULONGLONG until = GetTickCount64() + ms;
    while (true) {
        if (WaitForSingleObject(process, 100) != WAIT_TIMEOUT) return true;
        if ((cancel && *cancel) || GetTickCount64() >= until) return false;
    }
}

}  // namespace

ServerRunner::~ServerRunner() {
    for (auto& s : started_) {  // nothing is ended here: the job has no kill-on-close flag
        if (s.process) CloseHandle(s.process);
        if (s.job) CloseHandle(s.job);
    }
}

ServerStart ServerRunner::start(const std::wstring& folder_in, bool stop_on_close, ConsoleShow show) {
    ServerStart r;
    std::wstring folder = folder_in;
    while (folder.size() > 3 && (folder.back() == L'\\' || folder.back() == L'/')) folder.pop_back();
    const std::wstring exe = folder + L"\\enshrouded_server.exe";
    if (folder.empty() || !exists(exe)) {
        r.error = "There is no enshrouded_server.exe in " + utf8(folder) + ".";
        return r;
    }
    HANDLE job = CreateJobObjectW(nullptr, nullptr);  // groups its processes; never ends them
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = static_cast<WORD>(show == ConsoleShow::Hidden ? SW_HIDE : SW_SHOWMINNOACTIVE);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = command_line_arg(exe);
    // The server must take Ctrl+C: whether a process ignores it is passed on to its children, and EMF.exe may have been
    // started with it switched off (by a shell or a launcher). EMF.exe itself has no console, so this changes nothing
    // for it.
    SetConsoleCtrlHandler(nullptr, FALSE);
    // its own console (so the stop signal reaches only it), suspended until it is in the job; out of a job EMF.exe may
    // run in (a launcher's), so closing that does not end the server
    const DWORD flags = CREATE_NEW_CONSOLE | CREATE_SUSPENDED;
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, flags | CREATE_BREAKAWAY_FROM_JOB, nullptr,
                             folder.c_str(), &si, &pi);
    if (!ok && GetLastError() == ERROR_ACCESS_DENIED)  // EMF.exe's own job does not allow breaking away
        ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, flags, nullptr, folder.c_str(), &si, &pi);
    if (!ok) {
        const DWORD e = GetLastError();
        if (job) CloseHandle(job);
        r.error = "Windows did not start enshrouded_server.exe: " + windows_error_text(e);
        return r;
    }
    if (job && !AssignProcessToJobObject(job, pi.hProcess)) {
        CloseHandle(job);
        job = nullptr;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    std::lock_guard lock(m_);
    started_.push_back(Started{folder, job, pi.hProcess, pi.dwProcessId, stop_on_close});
    r.ok = true;
    r.pid = pi.dwProcessId;
    return r;
}

bool ServerRunner::set_stop_on_close(const std::wstring& folder, bool on) {
    std::lock_guard lock(m_);
    bool found = false;
    for (auto& s : started_)
        if (still_running(s.process) && same_place(s.folder, folder)) {
            s.stop_on_close = on;
            found = true;
        }
    return found;
}

bool ServerRunner::started_here(unsigned long pid) const {
    std::lock_guard lock(m_);
    for (const auto& s : started_)
        if (s.pid == pid && still_running(s.process)) return true;
    return false;
}

std::vector<unsigned long> ServerRunner::running(bool only_stop_on_close) const {
    std::vector<unsigned long> out;
    for (const auto& r : running_servers(only_stop_on_close)) out.push_back(r.pid);
    return out;
}

std::vector<ServerRunner::Running> ServerRunner::running_servers(bool only_stop_on_close) const {
    std::lock_guard lock(m_);
    std::vector<Running> out;
    for (const auto& s : started_)
        if (still_running(s.process) && (!only_stop_on_close || s.stop_on_close)) out.push_back(Running{s.pid, s.folder});
    return out;
}

unsigned long server_pid(const std::wstring& folder, const std::vector<ProcessInfo>& processes) {
    for (const auto& p : processes) {
        if (!iequals(p.name, L"enshrouded_server.exe") || p.path.empty()) continue;
        const std::wstring dir = p.path.substr(0, p.path.find_last_of(L'\\'));
        if (same_place(dir, folder)) return p.pid;  // the exe in the folder itself (also through a junction or link)
    }
    return 0;
}

int send_stop_signal(unsigned long pid, bool ctrl_break, bool shared_ok) {
    if (pid == 0) return 1;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return 1;
    const bool server = iequals(exe_name_of(h), L"enshrouded_server.exe");
    CloseHandle(h);
    if (!server) return 1;
    FreeConsole();  // this process exists only to send the signal: a console of its own would keep it from attaching
    if (!AttachConsole(pid)) return 2;
    // who else is on that console: the signal reaches every one of them
    DWORD ids[64];
    const DWORD n = GetConsoleProcessList(ids, 64);
    int others = 0;
    bool other_server = false;
    for (DWORD i = 0; i < n && i < 64; ++i) {
        if (ids[i] == pid || ids[i] == GetCurrentProcessId()) continue;
        ++others;
        if (HANDLE o = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ids[i])) {
            other_server = other_server || iequals(exe_name_of(o), L"enshrouded_server.exe");
            CloseHandle(o);
        }
    }
    if (n > 64) others = static_cast<int>(n);
    if (others > 0 && !shared_ok) {
        FreeConsole();
        return other_server ? 5 : 4;
    }
    SetConsoleCtrlHandler(swallow_ctrl, TRUE);
    const BOOL sent = GenerateConsoleCtrlEvent(ctrl_break ? CTRL_BREAK_EVENT : CTRL_C_EVENT, 0);  // all on its console
    Sleep(100);  // the console delivers it on its own thread; stay attached a moment
    FreeConsole();
    return sent ? 0 : 3;
}

int run_stop_helper(const std::wstring& helper, unsigned long pid, bool ctrl_break, unsigned timeout_ms, bool shared_ok) {
    if (helper.empty() || !exists(helper)) return -1;
    std::wstring cmd = command_line_arg(helper) + (ctrl_break ? L" --stop-server-break " : L" --stop-server ") + std::to_wstring(pid) +
                       (shared_ok ? L" --shared-ok" : L"");
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(helper.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;
    CloseHandle(pi.hThread);
    int code = -1;
    if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_OBJECT_0) {
        DWORD c = 0;
        if (GetExitCodeProcess(pi.hProcess, &c)) code = static_cast<int>(c);
    } else {
        TerminateProcess(pi.hProcess, 1);
    }
    CloseHandle(pi.hProcess);
    return code;
}

bool autosave_line_in(const std::string& text) { return text.find("world saved (slot") != std::string::npos; }

bool kill_server(unsigned long pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    bool ok = iequals(exe_name_of(h), L"enshrouded_server.exe") && TerminateProcess(h, 1);
    if (ok) WaitForSingleObject(h, 5000);
    CloseHandle(h);
    return ok;
}

StopOutcome stop_server(unsigned long pid, const StopOptions& o) {
    StopOutcome r;
    const auto say = [&](const std::string& s) {
        if (o.progress) o.progress(s);
    };
    const auto cancelled = [&] { return o.cancel && *o.cancel; };
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h && GetLastError() == ERROR_ACCESS_DENIED) {  // it runs, but EMF.exe may not touch it
        r.how = "not_stopped";
        r.text = "Windows does not let EMF.exe reach the server (process " + std::to_string(pid) +
                 "; it probably runs as administrator). Stop it in its own window, or run EMF.exe as administrator.";
        return r;
    }
    if (!h || !still_running(h)) {
        if (h) CloseHandle(h);
        r.stopped = true;
        r.how = "gone";
        r.text = "The server is not running.";
        return r;
    }
    if (!iequals(exe_name_of(h), L"enshrouded_server.exe")) {
        CloseHandle(h);
        r.how = "not_stopped";
        r.text = "Process " + std::to_string(pid) + " is not enshrouded_server.exe.";
        return r;
    }
    LogWatch shutdown_line(o.server_log);  // its own log, from here on
    bool shutting_down = false;            // the server said "Trigger gameflow shutdown": it saves, then exits
    const auto watch = [&] {
        if (!shutting_down && shutdown_line.saw("Trigger gameflow shutdown")) shutting_down = true;
        return shutting_down;
    };
    const auto clean = [&] {
        CloseHandle(h);
        r.stopped = true;
        r.how = "clean";
        r.text = "The server stopped cleanly at " + now_clock() + ": it saved the world and shut down.";
        return r;
    };
    say("Asking the server to stop: Ctrl+C in its window, as if pressed there. It saves the world first.");
    int sent = run_stop_helper(o.helper, pid, false, 15000, o.shared_ok);
    if (sent == 4 || sent == 5) {  // nothing was sent: the user decides
        CloseHandle(h);
        r.how = "shared_console";
        r.text = sent == 5 ? "Another enshrouded_server.exe shares this server's console window, so Ctrl+C would stop it "
                             "too. Stop both?"
                           : "Another program shares this server's console window (the command window it was started from, "
                             "say), and Ctrl+C reaches it too, as if pressed there. Send it anyway?";
        return r;
    }
    std::string why;
    const unsigned secs = o.exit_wait_ms / 1000;
    bool ended = false;
    if (sent == 0) {
        say("Waiting for the server to save and exit (up to " + std::to_string(secs ? secs : 1) + " s)...");
        const unsigned first = o.break_after_ms < o.exit_wait_ms ? o.break_after_ms : o.exit_wait_ms;
        ended = wait_exit(h, first, o.cancel);
        if (!ended && first < o.exit_wait_ms && !cancelled()) {
            if (watch()) {  // it took Ctrl+C and is saving: no second signal
                say("The server is saving and shutting down...");
            } else {  // Ctrl+C may be switched off for it
                say("The server is still running: sending Ctrl+Break, which it treats the same way.");
                run_stop_helper(o.helper, pid, true, 15000, o.shared_ok);
            }
            ended = wait_exit(h, o.exit_wait_ms - first, o.cancel);
        }
    } else if (sent != 2) {  // Ctrl+C was refused: Ctrl+Break is the other way in
        sent = run_stop_helper(o.helper, pid, true, 15000, o.shared_ok);
        if (sent == 0) ended = wait_exit(h, o.exit_wait_ms, o.cancel);
    }
    if (ended) return clean();
    if (sent == 0) {
        why = "The server did not exit within " + std::to_string(secs ? secs : 1) + " s of the signal.";
    } else if (sent == 2) {
        why = "EMF could not reach the server's console (it was started without one), so it cannot send Ctrl+C.";
    } else {
        why = "EMF could not send the stop signal (" + std::string(sent < 0 ? "the helper did not run" : "Windows refused it") + ").";
    }
    if (cancelled()) {
        CloseHandle(h);
        r.how = "not_stopped";
        r.text = why + " Stopping was cancelled; the server is still running.";
        return r;
    }
    const unsigned asecs = o.autosave_wait_ms / 1000;
    if (watch()) {  // it is shutting down: its shutdown save writes "world saved" too, so EMF never ends it now
        say("The server is still saving and shutting down: waiting up to " + std::to_string(asecs ? asecs : 1) + " s more.");
        if (wait_exit(h, o.autosave_wait_ms, o.cancel)) return clean();
        CloseHandle(h);
        r.how = "not_stopped";
        r.text = "The server is still shutting down (its log says so) after " + std::to_string(secs + asecs) +
                 " s, so EMF leaves it to finish. Stop now ends it at once and may cut its last save.";
        return r;
    }

    // the fallback: end it a few seconds after its next autosave, so nothing of the world is lost
    say(why + " Waiting for its next autosave line in emf_core.log (up to " + std::to_string(asecs ? asecs : 1) +
        " s), then EMF ends it.");
    LogWatch autosave(o.core_log);
    const ULONGLONG until = GetTickCount64() + o.autosave_wait_ms;
    bool saved = false;
    while (GetTickCount64() < until && !cancelled()) {
        if (WaitForSingleObject(h, 250) != WAIT_TIMEOUT) return clean();  // it ended by itself after all
        if (watch()) break;  // it shuts down after all: no autosave kill
        if (autosave.saw("world saved (slot")) {
            saved = true;
            break;
        }
    }
    if (saved) {  // its files are being written whole: a few seconds more, and never while it shuts down
        const ULONGLONG grace = GetTickCount64() + o.after_autosave_ms;
        while (GetTickCount64() < grace && !watch() && !cancelled())
            if (WaitForSingleObject(h, 100) != WAIT_TIMEOUT) return clean();
    }
    if (shutting_down) {
        if (wait_exit(h, o.autosave_wait_ms, o.cancel)) return clean();
        CloseHandle(h);
        r.how = "not_stopped";
        r.text = why + " It is shutting down now (its log says so), so EMF leaves it to finish.";
        return r;
    }
    if (saved && !cancelled()) {
        HANDLE t = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        const bool killed = t && TerminateProcess(t, 1) && WaitForSingleObject(h, 5000) == WAIT_OBJECT_0;
        if (t) CloseHandle(t);
        CloseHandle(h);
        r.stopped = killed;
        r.how = killed ? "after_autosave" : "not_stopped";
        r.text = killed ? why + " EMF ended it at " + now_clock() + ", a few seconds after its autosave (emf_core.log: world "
                                "saved), so the world is saved; its own shutdown did not run."
                        : why + " EMF could not end it after its autosave (Windows refused).";
        return r;
    }
    CloseHandle(h);
    r.how = "not_stopped";
    r.text = why + " It wrote no autosave line within " + std::to_string(asecs ? asecs : 1) +
             " s (emf_core.log: world saved; a server without EMF writes none), so EMF left it running. Stop now ends it "
             "at once and loses what changed since its last save (up to 30 s).";
    return r;
}

std::vector<StopOutcome> stop_servers_on_close(ServerRunner& runner, const std::wstring& helper) {
    const auto servers = runner.running_servers(true);
    std::vector<StopOutcome> outs(servers.size());
    std::vector<std::thread> stops;
    for (size_t i = 0; i < servers.size(); ++i)  // together: each may take a minute
        stops.emplace_back([&outs, &servers, &helper, i] {
            StopOptions o;
            o.helper = helper;
            o.core_log = servers[i].folder + L"\\emf\\emf_core.log";
            o.server_log = server_log_path(servers[i].folder, read_server_config(servers[i].folder));
            outs[i] = stop_server(servers[i].pid, o);
        });
    for (auto& t : stops) t.join();
    return outs;
}

}  // namespace emf::app
