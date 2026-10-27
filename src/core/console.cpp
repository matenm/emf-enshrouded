#include "console.h"

#include <string>
#include <vector>

namespace emf::core {
namespace {

constexpr LONGLONG kMaxFileBytes = 1 << 20;

bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Reads the whole file. False if it cannot be read or is 1 MB or more (*too_big then).
bool read_all(const std::wstring& p, std::string& out, bool* too_big) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) != 0;
    if (ok && size.QuadPart >= kMaxFileBytes) {
        *too_big = true;
        ok = false;
    }
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = out.empty() || (ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size());
    }
    CloseHandle(h);
    return ok;
}

// Deletes a file, clearing a read-only attribute if needed. True once it is gone.
bool remove_file(const std::wstring& p) {
    if (DeleteFileW(p.c_str())) return true;
    SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
    return DeleteFileW(p.c_str()) || !exists(p);
}

// Command text as UTF-8: a UTF-8 BOM is dropped, UTF-16LE (BOM FF FE, e.g. Windows PowerShell 5.1 "> file") converted.
std::string to_utf8(const std::string& raw) {
    if (raw.size() >= 3 && static_cast<uint8_t>(raw[0]) == 0xEF && static_cast<uint8_t>(raw[1]) == 0xBB &&
        static_cast<uint8_t>(raw[2]) == 0xBF)
        return raw.substr(3);
    if (raw.size() >= 2 && static_cast<uint8_t>(raw[0]) == 0xFF && static_cast<uint8_t>(raw[1]) == 0xFE) {
        const auto* w = reinterpret_cast<const wchar_t*>(raw.data() + 2);
        const int n = static_cast<int>((raw.size() - 2) / 2);
        const int len = WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
        std::string s(len > 0 ? len : 0, '\0');
        if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w, n, s.data(), len, nullptr, nullptr);
        return s;
    }
    return raw;
}

void append(const std::wstring& p, const std::string& text) {
    HANDLE h = CreateFileW(p.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr);
    CloseHandle(h);
}

struct ReaderArgs {
    std::function<void(const std::string&)> submit;
    HANDLE in;
};

DWORD WINAPI reader_thread(void* p) {
    auto* a = static_cast<ReaderArgs*>(p);
    std::string line;
    char buf[512];
    for (;;) {
        DWORD got = 0;
        if (!ReadConsoleA(a->in, buf, sizeof(buf), &got, nullptr)) {
            Sleep(200);
            continue;
        }
        for (DWORD i = 0; i < got; ++i) {
            if (buf[i] == '\r') continue;
            if (buf[i] == '\n') {
                if (!line.empty()) a->submit(line);
                line.clear();
            } else {
                line += buf[i];
            }
        }
    }
}

BOOL WINAPI ignore_ctrl(DWORD type) { return type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT; }

}  // namespace

// ---- LineWriter ----

LineWriter::LineWriter(std::function<void(const std::string&)> sink, size_t cap)
    : sink_(std::move(sink)), cap_(cap ? cap : 1) {
    thread_ = CreateThread(nullptr, 0, thread_main, this, 0, nullptr);
}

LineWriter::~LineWriter() {
    AcquireSRWLockExclusive(&lock_);
    stop_ = true;
    ReleaseSRWLockExclusive(&lock_);
    WakeConditionVariable(&wake_);
    if (thread_) {
        WaitForSingleObject(thread_, 5000);
        CloseHandle(thread_);
    }
}

void LineWriter::push(std::string line) {
    if (!thread_) {  // no writer thread: write directly
        if (sink_) sink_(line);
        return;
    }
    AcquireSRWLockExclusive(&lock_);
    if (lines_.size() >= cap_) {
        lines_.pop_front();
        ++dropped_;
    }
    lines_.push_back(std::move(line));
    ReleaseSRWLockExclusive(&lock_);
    WakeConditionVariable(&wake_);
}

DWORD WINAPI LineWriter::thread_main(void* p) {
    auto* self = static_cast<LineWriter*>(p);
    for (;;) {
        std::string line;
        size_t dropped = 0;
        AcquireSRWLockExclusive(&self->lock_);
        while (self->lines_.empty() && self->dropped_ == 0 && !self->stop_)
            SleepConditionVariableSRW(&self->wake_, &self->lock_, INFINITE, 0);
        if (self->lines_.empty() && self->dropped_ == 0) {  // stopping and drained
            ReleaseSRWLockExclusive(&self->lock_);
            return 0;
        }
        dropped = self->dropped_;
        self->dropped_ = 0;
        if (dropped == 0) {
            line = std::move(self->lines_.front());
            self->lines_.pop_front();
        }
        ReleaseSRWLockExclusive(&self->lock_);
        if (!self->sink_) continue;
        if (dropped)
            self->sink_("(" + std::to_string(dropped) + " line(s) dropped: the console was busy)");
        else
            self->sink_(line);
    }
}

// ---- CommandFile ----

size_t CommandFile::poll(CommandCenter& center, std::string* problem) {
    auto fail = [problem](const char* why) {
        if (problem) *problem = why;
        return size_t{0};
    };
    const std::wstring processing = path_ + L".processing";
    // A processing file only survives a poll when it could not be removed (or the game stopped mid-poll). Its lines
    // may have run already, so it is removed without running them.
    if (exists(processing) && !remove_file(processing))
        return fail("emf\\commands.txt.processing could not be removed; its commands were not run (delete it)");
    if (!MoveFileExW(path_.c_str(), processing.c_str(), 0)) return 0;  // no file, or another program holds it open
    std::string raw;
    bool too_big = false;
    const bool read = read_all(processing, raw, &too_big);
    // Remove first, queue after: a file that stays would run again on every poll.
    if (!remove_file(processing))
        return fail("emf\\commands.txt could not be removed after reading; its commands were not run");
    if (too_big) return fail("emf\\commands.txt is too big (1 MB or more); it was removed without running it");
    if (!read) return fail("emf\\commands.txt could not be read; it was removed without running it");
    const std::string text = to_utf8(raw);
    size_t queued = 0;
    std::string done;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t' || line.back() == '\0'))
            line.pop_back();
        size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos && line[first] != '#') {
            center.enqueue(line.substr(first));
            done += line.substr(first) + "\r\n";
            ++queued;
        }
        start = end + 1;
    }
    if (!done.empty()) append(done_, done);
    return queued;
}

// ---- ConsoleWindow ----

bool ConsoleWindow::open(std::function<void(const std::string&)> submit, const std::wstring& title) {
    if (is_open() || !submit) return is_open();
    if (!AllocConsole()) return false;
    SetConsoleTitleW(title.c_str());
    SetConsoleCtrlHandler(ignore_ctrl, TRUE);
    if (HWND w = GetConsoleWindow()) DeleteMenu(GetSystemMenu(w, FALSE), SC_CLOSE, MF_BYCOMMAND);
    HANDLE out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    HANDLE in = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (out == INVALID_HANDLE_VALUE || in == INVALID_HANDLE_VALUE) {
        if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
        if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
        return false;
    }
    // QuickEdit: a click would put the window in selection mode, which blocks every write until it ends.
    DWORD mode = 0;
    if (GetConsoleMode(in, &mode)) SetConsoleMode(in, (mode | ENABLE_EXTENDED_FLAGS) & ~DWORD(ENABLE_QUICK_EDIT_MODE));
    writer_ = new LineWriter([out](const std::string& line) {  // lives for the process
        const std::string text = line + "\r\n";
        DWORD wrote = 0;
        WriteConsoleA(out, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr);
    });
    auto* args = new ReaderArgs{std::move(submit), in};  // lives for the process
    HANDLE t = CreateThread(nullptr, 0, reader_thread, args, 0, nullptr);
    if (t) CloseHandle(t);
    write_line("EMF console: type a command and press Enter (help lists them). Do not close this window.");
    return true;
}

void ConsoleWindow::write_line(const std::string& line) {
    if (writer_) writer_->push(line);
}

}  // namespace emf::core
