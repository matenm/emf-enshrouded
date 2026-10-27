#include "log.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <string>

#include "line_queue.h"
#include "log_ring.h"

namespace emf {
namespace {
SRWLOCK g_lock = SRWLOCK_INIT;
FILE* g_file = nullptr;
bool g_console = false;  // also write each line to the process console (the dedicated server's window)
// Console lines go through a queue to their own thread: writing to a console blocks while its window is paused (a
// QuickEdit selection), and that must never stall the game threads that log.
LineQueue g_console_lines(2000);
HANDLE g_console_wake = nullptr;
std::atomic<LogRing*> g_ring{nullptr};  // the debug bridge's ring (null while off; never freed)

void write_console(const char* line, size_t n) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);  // asked each time: the server may set up its console after EMF
    if (!h || h == INVALID_HANDLE_VALUE) return;
    DWORD done = 0;
    if (!WriteConsoleA(h, line, static_cast<DWORD>(n), &done, nullptr))
        WriteFile(h, line, static_cast<DWORD>(n), &done, nullptr);  // redirected to a file or pipe
}
DWORD WINAPI console_writer(void*) {
    for (;;) {
        WaitForSingleObject(g_console_wake, INFINITE);
        auto batch = g_console_lines.take();
        if (batch.dropped) {
            const std::string note =
                "[EMF] (" + std::to_string(batch.dropped) + " lines skipped in this window; the log file has them)\r\n";
            write_console(note.data(), note.size());
        }
        for (const auto& line : batch.lines) write_console(line.data(), line.size());
    }
}
}  // namespace

void log_to_console(bool on) {
    AcquireSRWLockExclusive(&g_lock);
    if (on && !g_console_wake) {
        g_console_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto-reset
        HANDLE t = g_console_wake ? CreateThread(nullptr, 0, console_writer, nullptr, 0, nullptr) : nullptr;
        if (t) CloseHandle(t);
        else on = false;
    }
    g_console = on && g_console_wake;
    ReleaseSRWLockExclusive(&g_lock);
}

void log_open(const std::wstring& path) {
    AcquireSRWLockExclusive(&g_lock);
    if (g_file) fclose(g_file);
    g_file = _wfsopen(path.c_str(), L"wb", _SH_DENYWR);
    ReleaseSRWLockExclusive(&g_lock);
}

void log_close() {
    AcquireSRWLockExclusive(&g_lock);
    if (g_file) fclose(g_file);
    g_file = nullptr;
    ReleaseSRWLockExclusive(&g_lock);
}

void log_ring_enable(size_t capacity) {
    AcquireSRWLockExclusive(&g_lock);
    if (!g_ring.load()) g_ring.store(new LogRing(capacity), std::memory_order_release);
    ReleaseSRWLockExclusive(&g_lock);
}

LogRing* log_ring() { return g_ring.load(std::memory_order_acquire); }

void logf(const char* fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[2112];
    std::snprintf(line, sizeof(line), "[%02u:%02u:%02u.%03u t%05lu] %s", t.wHour, t.wMinute, t.wSecond,
                  t.wMilliseconds, GetCurrentThreadId(), msg);
    AcquireSRWLockExclusive(&g_lock);
    if (g_file) {
        fputs(line, g_file);
        fputc('\n', g_file);
        fflush(g_file);
    }
    const bool console = g_console;
    ReleaseSRWLockExclusive(&g_lock);
    if (LogRing* ring = g_ring.load(std::memory_order_acquire)) ring->push(line);  // its own short lock
    if (console) {
        g_console_lines.push(std::string("[EMF] ") + msg + "\r\n");
        SetEvent(g_console_wake);
    }
}

}  // namespace emf
