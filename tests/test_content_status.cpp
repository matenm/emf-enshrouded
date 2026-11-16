// What EMF's core tells the player about EML content at this start (src/core/content_status.h), from what the loader
// told it (EMF_CONTENT_STATUS, src/loader/content_status_env.h).
#include <doctest.h>

#include <windows.h>

#include <optional>
#include <string>

#include "core/content_status.h"

using emf::core::content_lines;
using emf::loader::ContentStatus;

namespace {
ContentStatus status(const char* kind, size_t mods, uint64_t ms, const char* message) {
    ContentStatus s;
    s.kind = kind;
    s.mods = mods;
    s.ms = ms;
    s.message = message;
    return s;
}
}  // namespace

TEST_CASE("content status: failed build line") {
    auto l = content_lines(status("failed", 0, 4000, "EML could not patch the bear"), true);
    CHECK(l.console == "EML content could not be built: EML could not patch the bear. The game runs without it. "
                       "Your saves were backed up.");
    CHECK(l.notice == l.console);  // also on screen once a world loads
    CHECK(l.backup);
    // with save backups off, it does not promise one
    l = content_lines(status("failed", 0, 4000, "EML could not patch the bear."), false);
    CHECK(l.console == "EML content could not be built: EML could not patch the bear. The game runs without it.");
    CHECK(l.backup);  // asked for all the same: the backup's own setting decides
}

TEST_CASE("content status: a Lua error says which mod and what, without the stack traceback") {
    const auto l = content_lines(
        status("failed", 0, 4000,
               "runtime error: [string \"mods.emf_broken_test\"]:1: boom: a deliberately broken test mod stack "
               "traceback: [C]: in ? [C]: in function 'error' [string \"mods.emf_broken_test\"]:1: in main chunk"),
        true);
    CHECK(l.console == "EML content could not be built: the mod emf_broken_test: line 1: boom: a deliberately broken "
                       "test mod. The game runs without it. Your saves were backed up.");
}

TEST_CASE("content status: built line") {
    const auto l = content_lines(status("built", 3, 28000, ""), true);
    CHECK(l.console == "EML content: 3 mods (built in 28 s)");
    CHECK(l.notice.empty());  // the console only
    CHECK_FALSE(l.backup);
    CHECK(content_lines(status("built", 1, 1500, ""), true).console == "EML content: 1 mod (built in 1.5 s)");
}

TEST_CASE("content status: current line") {
    const auto l = content_lines(status("current", 3, 28000, ""), true);
    CHECK(l.console == "EML content: 3 mods (unchanged since the last build)");
    CHECK(l.notice.empty());
    CHECK_FALSE(l.backup);
}

// A creature pack caught its own failure: the build was good, but the bear was not added. The player hears of it.
TEST_CASE("content status: a good build whose mods warned: the console line and a notice say which and what") {
    auto l = content_lines(status("built", 3, 28000, "emf_beastmaster_bear: Bear: not added: out of references"), true);
    CHECK(l.console == "EML content: 3 mods (built in 28 s); a mod reported a problem: emf_beastmaster_bear: Bear: not "
                       "added: out of references");
    CHECK(l.notice == "An EML mod reported a problem while its content was built: emf_beastmaster_bear: Bear: not added: "
                      "out of references. The rest of the content is in; emf\\content.log has every line. Your saves were "
                      "backed up.");
    // a part not added can be a placed object's template: the world drops those when it loads, so a backup first
    CHECK(l.backup);
    CHECK(std::string(l.backup_label) == "before a start with part of the EML content missing");
    CHECK(content_lines(status("built", 3, 28000, "a: b"), false).notice.find("backed up") == std::string::npos);
    l = content_lines(status("current", 3, 28000, "a: b (and 1 more)"), true);
    CHECK(l.console == "EML content: 3 mods (unchanged since the last build); a mod reported a problem: a: b (and 1 more)");
    CHECK(l.notice == "An EML mod reported a problem while its content was built: a: b (and 1 more). The rest of the "
                      "content is in; emf\\content.log has every line.");
    CHECK_FALSE(l.backup);  // the same content as the last start, which made its backup (as failed_again)
}

TEST_CASE("content status: off line") {
    // the loader says "off" only with [content] mode=on; without a status (mode=auto, no emf_content.dll) nothing
    auto l = content_lines(status("off", 0, 0, "off (emf_content.dll is not installed)"), true);
    CHECK(l.console == "EML content is off (emf_content.dll is not installed)");
    CHECK(l.notice.empty());
    CHECK_FALSE(l.backup);
    l = content_lines(std::nullopt, true);
    CHECK(l.console.empty());
    CHECK(l.notice.empty());
    CHECK_FALSE(l.backup);
    CHECK(content_lines(status("something new", 0, 0, "x"), true).console.empty());
}

TEST_CASE("content status: read from the variable's text") {
    CHECK(emf::core::content_status_from(std::wstring(L"built\t2\t30000\t")).has_value());
    CHECK_FALSE(emf::core::content_status_from(std::wstring()).has_value());
    CHECK(emf::core::content_status_from(std::wstring(L"failed\t0\t1\tBär")).value().message == "B\xC3\xA4r");
}

TEST_CASE("content status: the same failure again: the warning, without a second backup") {
    const auto l = content_lines(status("failed_again", 0, 0, "EML could not patch the bear"), true);
    CHECK(l.console.find("EML content could not be built: EML could not patch the bear. The game runs without it.") == 0);
    CHECK(l.console.find("backed up") == std::string::npos);
    CHECK(l.console.find("changes") != std::string::npos);  // when EMF tries again
    CHECK(l.notice == l.console);
    CHECK_FALSE(l.backup);
}

TEST_CASE("content status: a long message is cut when the loader writes it, read whole, and the variable cleared") {
    emf::loader::ContentStatus st;
    st.kind = "failed";
    st.message = std::string(3000, 'x') + "\xC3\xA4";
    const std::string text = emf::loader::content_status_text(st);
    CHECK(text.size() < 600);  // at most 512 bytes of message
    const auto back = emf::loader::parse_content_status(text);
    REQUIRE(back.has_value());
    CHECK(back->message.size() <= 512);
    // a message near the limit ending in a two-byte letter is never cut inside it
    st.message = std::string(511, 'y') + "\xC3\xA4";
    CHECK(emf::loader::parse_content_status(emf::loader::content_status_text(st))->message == std::string(511, 'y'));
    // read whole, whatever its length, and cleared so nothing started later sees it
    const std::wstring big = L"failed\t0\t0\t" + std::wstring(6000, L'z');
    REQUIRE(SetEnvironmentVariableW(L"EMF_CONTENT_STATUS", big.c_str()));
    const auto read = emf::core::read_content_status();
    REQUIRE(read.has_value());
    CHECK(read->message.size() == 6000);
    CHECK(GetEnvironmentVariableW(L"EMF_CONTENT_STATUS", nullptr, 0) == 0);
}
