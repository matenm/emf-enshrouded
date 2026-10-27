// The tests' own scratch helpers (test_scratch.h): folders a test makes in %TEMP% are removed when the run ends.
#include <windows.h>

#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "test_scratch.h"

namespace {
bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
}  // namespace

TEST_CASE("test scratch: a folder given to an ExitCleanup is removed with it, contents and all, and kept until then") {
    const std::wstring dir = test_scratch::temp_path(L"emf_test_cleanup");
    const std::wstring other = test_scratch::temp_path(L"emf_test_cleanup_other");
    std::filesystem::create_directories(dir + L"\\sub");
    std::filesystem::create_directories(other);
    std::ofstream(dir + L"\\sub\\file.txt") << "x";
    {
        test_scratch::ExitCleanup cleanup;
        cleanup.add(dir);
        cleanup.add(dir);  // twice: still one folder to remove
        CHECK(exists(dir + L"\\sub\\file.txt"));  // a later test of the run may open it again
    }
    CHECK_FALSE(exists(dir));
    CHECK(exists(other));  // only what was given
    std::error_code ec;
    std::filesystem::remove_all(other, ec);
}

TEST_CASE("test scratch: remove_at_exit hands the folder to the run's ExitCleanup, which leaves it until the run ends") {
    const std::wstring dir = test_scratch::temp_path(L"emf_test_cleanup_at_exit");
    std::filesystem::create_directories(dir);
    test_scratch::remove_at_exit(dir);
    CHECK(exists(dir));
    CHECK(test_scratch::exit_cleanup().holds(dir));
}
