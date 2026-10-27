#pragma once
#include <string>

namespace emf {

class LogRing;

// Opens (truncates) the log file. Safe to call once at startup.
void log_open(const std::wstring& path);
// Also write every line to the process console, prefixed "[EMF] " (on the dedicated server, its window).
void log_to_console(bool on);
void log_close();
// printf-style; adds a timestamp and thread id, flushes every line. No-op before log_open (except for the ring).
void logf(const char* fmt, ...);
// Keeps the last `capacity` lines in memory with sequence numbers, for the debug bridge. Off until the first call (a
// later call keeps the first ring); the ring lives for the process.
void log_ring_enable(size_t capacity);
LogRing* log_ring();  // null while off

}  // namespace emf
