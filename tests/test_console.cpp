#include <windows.h>

#include <doctest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/commands.h"
#include "core/console.h"
#include "test_scratch.h"

using namespace emf::core;

namespace {
// %TEMP%\emf_test_console_<pid>_<ms>, removed when the run ends (two tests in the same millisecond share it).
std::wstring temp_dir() {
    wchar_t base[MAX_PATH];
    GetTempPathW(MAX_PATH, base);
    std::wstring dir = std::wstring(base) + L"emf_test_console_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                       std::to_wstring(GetTickCount64());
    CreateDirectoryW(dir.c_str(), nullptr);
    test_scratch::remove_at_exit(dir);
    return dir;
}
void write_file(const std::wstring& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary);
    f << text;
}
std::string read_file(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
}  // namespace

TEST_CASE("command file: each line is queued once, then moved to the done file") {
    const std::wstring dir = temp_dir();
    CommandFile file(dir + L"\\commands.txt", dir + L"\\commands.done.txt");
    CommandCenter c;
    CHECK(file.poll(c) == 0);  // no file yet
    write_file(dir + L"\\commands.txt", "give Torch 2\r\n\r\n# a comment\ntime 12\n");
    CHECK(file.poll(c) == 2);
    CHECK(c.pending() == 2);
    CHECK_FALSE(exists(dir + L"\\commands.txt"));
    const std::string done = read_file(dir + L"\\commands.done.txt");
    CHECK(done.find("give Torch 2") != std::string::npos);
    CHECK(done.find("time 12") != std::string::npos);
    CHECK(file.poll(c) == 0);
    CHECK(c.pending() == 2);
    write_file(dir + L"\\commands.txt", "pos");  // no trailing newline
    CHECK(file.poll(c) == 1);
    CHECK(read_file(dir + L"\\commands.done.txt").find("pos") != std::string::npos);
}

TEST_CASE("command file: a file another program holds open is left alone until it is free") {
    const std::wstring dir = temp_dir();
    const std::wstring path = dir + L"\\commands.txt";
    write_file(path, "help\n");
    CommandFile file(path, dir + L"\\commands.done.txt");
    CommandCenter c;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, 0 /* no sharing */, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    CHECK(file.poll(c) == 0);
    CHECK(c.pending() == 0);
    CloseHandle(h);
    CHECK(file.poll(c) == 1);
}

// ---- edge cases ----
namespace {
std::vector<std::string> g_ran;
void ran_cmd(EmfWorld*, int argc, const char* const* argv, void*) {
    std::string line;
    for (int i = 0; i < argc; ++i) line += (i ? " " : "") + std::string(argv[i]);
    g_ran.push_back(line);
}
void remove_dir(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            SetFileAttributesW((dir + L"\\" + name).c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW((dir + L"\\" + name).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}
}  // namespace

TEST_CASE("command file: a read-only file runs once, not on every poll") {
    const std::wstring dir = temp_dir();
    const std::wstring path = dir + L"\\commands.txt";
    write_file(path, "give Torch 1\n");
    REQUIRE(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY));
    CommandFile file(path, dir + L"\\commands.done.txt");
    CommandCenter c;
    CHECK(file.poll(c) == 1);
    for (int i = 0; i < 4; ++i) CHECK(file.poll(c) == 0);
    CHECK(c.pending() == 1);
    remove_dir(dir);
}

TEST_CASE("command file: a processing file that cannot be removed is never run, and does not block new files") {
    const std::wstring dir = temp_dir();
    const std::wstring path = dir + L"\\commands.txt";
    write_file(path + L".processing", "spawn x 20\n");  // left over (e.g. the game crashed mid-poll)
    HANDLE h = CreateFileW((path + L".processing").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    CommandFile file(path, dir + L"\\commands.done.txt");
    CommandCenter c;
    std::string problem;
    CHECK(file.poll(c, &problem) == 0);
    CHECK_FALSE(problem.empty());
    for (int i = 0; i < 3; ++i) CHECK(file.poll(c) == 0);
    CHECK(c.pending() == 0);
    CloseHandle(h);
    CHECK(file.poll(c) == 0);  // now removed, still not run
    CHECK_FALSE(exists(path + L".processing"));
    write_file(path, "help\n");
    CHECK(file.poll(c) == 1);
    remove_dir(dir);
}

TEST_CASE("command file: a file of 1 MB or more is removed and reported instead of wedging the input") {
    const std::wstring dir = temp_dir();
    const std::wstring path = dir + L"\\commands.txt";
    write_file(path, std::string(1100000, 'x'));
    CommandFile file(path, dir + L"\\commands.done.txt");
    CommandCenter c;
    std::string problem;
    CHECK(file.poll(c, &problem) == 0);
    CHECK(problem.find("too big") != std::string::npos);
    CHECK_FALSE(exists(path));
    CHECK_FALSE(exists(path + L".processing"));
    write_file(path, "help\n");
    CHECK(file.poll(c) == 1);
    remove_dir(dir);
}

TEST_CASE("command file: a UTF-8 BOM and UTF-16LE text (PowerShell 5.1) are read as plain commands") {
    const std::wstring dir = temp_dir();
    const std::wstring path = dir + L"\\commands.txt";
    CommandFile file(path, dir + L"\\commands.done.txt");
    CommandCenter c;
    REQUIRE(c.add("give", "", ran_cmd, nullptr, nullptr));
    g_ran.clear();
    write_file(path, "\xEF\xBB\xBFgive Torch 1\r\n");
    CHECK(file.poll(c) == 1);
    const std::wstring wide = L"give Torch 2\r\ngive Torch 3\r\n";
    std::string utf16 = "\xFF\xFE";
    utf16.append(reinterpret_cast<const char*>(wide.data()), wide.size() * 2);
    write_file(path, utf16);
    CHECK(file.poll(c) == 2);
    c.run_pending(nullptr, nullptr);
    CHECK(g_ran == std::vector<std::string>{"give Torch 1", "give Torch 2", "give Torch 3"});
    remove_dir(dir);
}

TEST_CASE("LineWriter: push never waits for a stuck sink; lines arrive in order; overflow is counted") {
    HANDLE gate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::vector<std::string> got;
    SRWLOCK lock = SRWLOCK_INIT;
    {
        LineWriter w(
            [&](const std::string& s) {
                WaitForSingleObject(gate, 5000);
                AcquireSRWLockExclusive(&lock);
                got.push_back(s);
                ReleaseSRWLockExclusive(&lock);
            },
            10);
        const ULONGLONG t0 = GetTickCount64();
        for (int i = 0; i < 25; ++i) w.push("line " + std::to_string(i));
        CHECK(GetTickCount64() - t0 < 1000);
        SetEvent(gate);
        for (int i = 0; i < 200; ++i) {
            AcquireSRWLockShared(&lock);
            const bool done = !got.empty() && got.back() == "line 24";
            ReleaseSRWLockShared(&lock);
            if (done) break;
            Sleep(10);
        }
    }
    CloseHandle(gate);
    REQUIRE_FALSE(got.empty());
    CHECK(got.back() == "line 24");
    bool dropped = false;
    for (const auto& s : got) dropped |= s.find("dropped") != std::string::npos;
    CHECK(dropped);
    CHECK(got.size() <= 25);
}
