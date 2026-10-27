#pragma once
// Command input: emf/commands.txt (always) and an optional console window (emf/emf_core.ini [console] window=1).
#include <windows.h>

#include <deque>
#include <functional>
#include <string>

#include "commands.h"

namespace emf::core {

// Reads `path` once per poll: each non-empty line that does not start with '#' is queued once, then the lines are
// appended to `done_path` and the file is removed. The file is first renamed to <path>.processing, so lines written
// while it is being read land in a new file; a file another program holds open is left for the next poll.
class CommandFile {
public:
    CommandFile(std::wstring path, std::wstring done_path) : path_(std::move(path)), done_(std::move(done_path)) {}
    // Returns the lines queued. *problem (if given) gets a one-line reason when a file could not be used.
    size_t poll(CommandCenter& center, std::string* problem = nullptr);

private:
    std::wstring path_, done_;
};

// Hands lines to `sink` on its own thread, so push never waits (a console in selection mode blocks writes). At most
// `cap` lines wait; older ones are dropped and counted ("(N lines dropped ...)").
class LineWriter {
public:
    explicit LineWriter(std::function<void(const std::string&)> sink, size_t cap = 2000);
    ~LineWriter();
    LineWriter(const LineWriter&) = delete;
    LineWriter& operator=(const LineWriter&) = delete;
    void push(std::string line);

private:
    static DWORD WINAPI thread_main(void* self);
    std::function<void(const std::string&)> sink_;
    size_t cap_;
    SRWLOCK lock_ = SRWLOCK_INIT;
    CONDITION_VARIABLE wake_ = CONDITION_VARIABLE_INIT;
    std::deque<std::string> lines_;
    size_t dropped_ = 0;
    bool stop_ = false;
    HANDLE thread_ = nullptr;
};

// A console window for typing commands (and reading their replies). It has no close button, and Ctrl+C is ignored,
// because closing a console window would end the game. QuickEdit is off, so a click does not freeze its output.
class ConsoleWindow {
public:
    // `submit` gets every line typed (on the window's reader thread).
    bool open(std::function<void(const std::string&)> submit, const std::wstring& title);
    bool is_open() const { return writer_ != nullptr; }
    void write_line(const std::string& line);  // never waits: a LineWriter thread does the console write

private:
    LineWriter* writer_ = nullptr;  // lives for the process once the window is open
};

}  // namespace emf::core
