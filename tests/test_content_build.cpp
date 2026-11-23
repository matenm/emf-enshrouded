// The call into emf_content.dll (src/loader/content_build.h): load it, check its ABI, build, check that no thread of
// its own still runs, unload it, and turn every outcome into a code and a message. The DLL here is the stand-in
// (tools/standin_content), which does what <game_dir>\standin.txt says.
#include <doctest.h>

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "content_fixtures.h"
#include "loader/content_build.h"
#include "loader/content_splash.h"
#include "test_scratch.h"

using content_fixtures::file_text;
using content_fixtures::put_file;

namespace {
std::wstring exe_dir() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    const std::wstring exe(buf, n);
    return exe.substr(0, exe.find_last_of(L'\\'));
}
std::wstring standin_dll() { return exe_dir() + L"\\standin_content\\emf_content.dll"; }

// A temp folder the stand-in builds in, with standin.txt saying what it does.
struct BuildFolder {
    std::wstring dir;
    BuildFolder(const wchar_t* name, const std::string& word) : dir(test_scratch::temp_path(name)) {
        test_scratch::remove_at_exit(dir);
        put_file(dir + L"\\standin.txt", word + "\r\n");
        put_file(dir + L"\\enshrouded.kfc", "vanilla kfc\r\n");
        put_file(dir + L"\\enshrouded.kfc_resources", "vanilla resources\r\n");
        put_file(dir + L"\\mods\\beasts\\mod.json", R"({"id": "beasts", "name": "Beasts", "version": "1.2.0"})");
        put_file(dir + L"\\mods\\aaa\\mod.json", R"({"id": "aaa", "name": "A", "version": "0.1.0"})");
    }
    ~BuildFolder() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    std::string request() const { return emf::loader::content_request(dir, "enshrouded", false); }
};
}  // namespace

TEST_CASE("content build: the request") {
    CHECK(emf::loader::content_request(L"C:\\Games\\B\u00e4r \"x\"", "enshrouded", false) ==
          "{\"game_dir\": \"C:\\\\Games\\\\B\xC3\xA4r \\\"x\\\"\", \"file_name\": \"enshrouded\", \"force\": true, "
          "\"server\": false}");
    CHECK(emf::loader::content_request(L"D:\\S", "enshrouded_server", true).find("\"server\": true") != std::string::npos);
}

TEST_CASE("content build: ok") {
    REQUIRE(std::filesystem::exists(standin_dll()));
    BuildFolder f(L"emf_content_build_ok", "ok");
    const auto out = emf::loader::run_content_build(standin_dll(), f.request(), nullptr);
    CHECK(out.code == 0);
    CHECK(out.unloaded);
    CHECK_FALSE(out.truncated);
    CHECK(out.usable());
    CHECK(out.mods == std::vector<std::string>{"aaa 0.1.0", "beasts 1.2.0"});
    CHECK(out.errors.empty());
    CHECK(GetModuleHandleW(standin_dll().c_str()) == nullptr);
    CHECK(file_text(f.dir + L"\\built.txt").find("seq=1 ") == 0);
    CHECK(file_text(f.dir + L"\\enshrouded.kfc") == "built kfc 1\r\n");
    CHECK(file_text(f.dir + L"\\enshrouded.kfc.bak") == "vanilla kfc\r\n");
}

TEST_CASE("content build: fail") {
    BuildFolder f(L"emf_content_build_fail", "fail");
    const auto out = emf::loader::run_content_build(standin_dll(), f.request(), nullptr);
    CHECK(out.code == 2);
    CHECK(out.message == "standin: the build failed on purpose");
    CHECK(out.unloaded);
    CHECK_FALSE(out.usable());
}

TEST_CASE("content build: a mod that could not be read") {
    BuildFolder f(L"emf_content_build_modfail", "mods_failed");
    const auto out = emf::loader::run_content_build(standin_dll(), f.request(), nullptr);
    CHECK(out.code == 1);
    REQUIRE(out.errors.size() == 1);
    CHECK(out.errors[0].find("broken on purpose") != std::string::npos);
    CHECK(out.errors[0].find("mods\\broken") != std::string::npos);
}

TEST_CASE("content build: big reply is cut, and a cut reply's result counts as unknown") {
    BuildFolder f(L"emf_content_build_big", "big");
    auto out = emf::loader::run_content_build(standin_dll(), f.request(), nullptr);
    CHECK(out.code == 0);  // the real code survives the cut
    CHECK(out.truncated);
    CHECK(out.mods.empty());
    CHECK_FALSE(out.usable());
    CHECK(out.unloaded);
    // a buffer too small even for the cut reply: REPLY_TOO_SMALL, nothing written
    BuildFolder g(L"emf_content_build_tiny", "ok");
    out = emf::loader::run_content_build(standin_dll(), g.request(), nullptr, 16);
    CHECK(out.code == emf::loader::content_abi::EMF_CONTENT_REPLY_TOO_SMALL);
    CHECK(out.message.find("unknown") != std::string::npos);
    CHECK_FALSE(out.usable());
}

TEST_CASE("content build: missing dll") {
    const std::wstring dll = test_scratch::temp_path(L"emf_no_such_dir") + L"\\emf_content.dll";
    const auto out = emf::loader::run_content_build(dll, "{}", nullptr);
    CHECK(out.code == emf::loader::kBuildNoDll);
    CHECK(out.message.find("emf_content.dll") != std::string::npos);
    CHECK(out.unloaded);
    CHECK_FALSE(out.usable());
}

TEST_CASE("content build: abi mismatch") {
    const std::wstring dll = exe_dir() + L"\\standin_content_abi2\\emf_content.dll";
    REQUIRE(std::filesystem::exists(dll));
    BuildFolder f(L"emf_content_build_abi", "ok");
    const auto out = emf::loader::run_content_build(dll, f.request(), nullptr);
    CHECK(out.code == emf::loader::kBuildWrongAbi);
    CHECK(out.message.find("ABI 2") != std::string::npos);
    CHECK(out.unloaded);
    CHECK(GetModuleHandleW(dll.c_str()) == nullptr);
    CHECK_FALSE(std::filesystem::exists(f.dir + L"\\built.txt"));  // never asked to build
}

TEST_CASE("content build: a leftover thread keeps it loaded") {
    // its own copy: the module stays in this process until the test lets the thread go
    BuildFolder f(L"emf_content_build_thread", "thread");
    const std::wstring dll = f.dir + L"\\dll\\emf_content.dll";
    std::filesystem::create_directories(f.dir + L"\\dll");
    std::filesystem::copy_file(standin_dll(), dll);
    const auto out = emf::loader::run_content_build(dll, f.request(), nullptr);
    CHECK(out.code == 0);
    CHECK_FALSE(out.unloaded);
    CHECK_FALSE(out.usable());
    CHECK(out.message.find("thread") != std::string::npos);
    HMODULE still = GetModuleHandleW(dll.c_str());
    CHECK(still != nullptr);
    // let the stand-in's thread end (it unloads its own reference), then drop the loader's
    const std::wstring stop = L"Local\\emf_standin_content_stop_" + std::to_wstring(GetCurrentProcessId());
    if (HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, stop.c_str())) {
        SetEvent(e);
        CloseHandle(e);
    }
    if (still) FreeLibrary(still);
    for (int i = 0; i < 400 && GetModuleHandleW(dll.c_str()); ++i) Sleep(50);  // up to 20 s: a busy PC ends the thread late
    CHECK(GetModuleHandleW(dll.c_str()) == nullptr);
}

TEST_CASE("content build: log lines reach the callback, and one that throws stays inside") {
    BuildFolder f(L"emf_content_build_log", "ok");
    std::vector<std::pair<int, std::string>> lines;
    auto out = emf::loader::run_content_build(standin_dll(), f.request(),
                                              [&](int level, const std::string& line) { lines.emplace_back(level, line); });
    CHECK(out.code == 0);
    bool info = false, warning = false;
    for (const auto& [level, line] : lines) {
        info = info || (level == 2 && line.find("standin: building ") == 0);
        warning = warning || (level == 1 && line == "standin: a warning for the test");
    }
    CHECK(info);
    CHECK(warning);
    out = emf::loader::run_content_build(standin_dll(), f.request(),
                                         [](int, const std::string&) { throw std::runtime_error("the log is full"); });
    CHECK(out.code == 0);
    CHECK(out.unloaded);
}

TEST_CASE("content build: a build given up on (its time was up) never loads the DLL") {
    BuildFolder f(L"emf_content_build_abandoned", "ok");
    const std::atomic<bool> abandoned{true};
    const auto out = emf::loader::run_content_build(standin_dll(), f.request(), nullptr, emf::loader::kReplyCapacity,
                                                    &abandoned);
    CHECK(out.code == emf::loader::kBuildAbandoned);
    CHECK(out.unloaded);
    CHECK_FALSE(std::filesystem::exists(f.dir + L"\\built.txt"));
}

TEST_CASE("content build: a build that needs a deep stack (EML's Lua on big files) runs on the loader's worker") {
    // the stand-in recurses through about 64 MB of stack: past a thread's usual 1 MB, inside the worker's reserve
    BuildFolder f(L"emf_content_build_deep", "deep");
    emf::loader::BuildOutcome out;
    const bool done = emf::loader::run_with_splash(
        nullptr, [&] { out = emf::loader::run_content_build(standin_dll(), f.request(), nullptr); }, 60000);
    CHECK(done);
    CHECK(out.code == 0);
    CHECK(out.message.find("deep") != std::string::npos);
}
