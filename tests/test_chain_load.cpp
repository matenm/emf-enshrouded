// The loader starts the chained DLLs in list order; one that is missing or fails is reported and skipped, never
// stops the game.
#include <doctest.h>

#include <string>
#include <vector>

#include "loader/chain_load.h"

using emf::loader::ChainResult;
using emf::loader::load_chain;

TEST_CASE("chain load: every DLL in order; missing and failing ones are reported and the rest still load") {
    std::vector<std::wstring> tried;
    const auto results = load_chain(
        {L"a.dll", L"gone.dll", L"bad.dll", L"b.dll"}, [](const std::wstring& n) { return n != L"gone.dll"; },
        [&](const std::wstring& n, unsigned long* error) {
            tried.push_back(n);
            if (n == L"bad.dll") {
                *error = 193;  // ERROR_BAD_EXE_FORMAT: a 32-bit DLL
                return false;
            }
            return true;
        });
    CHECK(tried == std::vector<std::wstring>{L"a.dll", L"bad.dll", L"b.dll"});
    REQUIRE(results.size() == 4);
    CHECK(results[0].kind == ChainResult::Loaded);
    CHECK(results[1].kind == ChainResult::Missing);
    CHECK(results[2].kind == ChainResult::Failed);
    CHECK(results[2].error == 193);
    CHECK(results[3].kind == ChainResult::Loaded);
}

TEST_CASE("chain load: the log line names the DLL and what happened") {
    CHECK(emf::loader::chain_line({L"creative.dll", ChainResult::Loaded, 0}) == "chain: started creative.dll");
    CHECK(emf::loader::chain_line({L"gone.dll", ChainResult::Missing, 0}) ==
          "chain: gone.dll is listed but not in the game folder; skipped");
    CHECK(emf::loader::chain_line({L"bad.dll", ChainResult::Failed, 193}) ==
          "chain: bad.dll did not load (Windows error 193); skipped");
}

TEST_CASE("chain load: the status file has one tab-separated line per DLL for EMF's manager") {
    const std::string status = emf::loader::chain_status_text(
        {{L"a.dll", ChainResult::Loaded, 0}, {L"gone.dll", ChainResult::Missing, 0}, {L"bad.dll", ChainResult::Failed, 5}});
    CHECK(status == "a.dll\tstarted\r\ngone.dll\tmissing\r\nbad.dll\tfailed 5\r\n");
}

TEST_CASE("chain load: a non-ASCII name is written in UTF-8") {
    CHECK(emf::loader::chain_line({L"möd.dll", ChainResult::Loaded, 0}) == "chain: started m\xC3\xB6" "d.dll");
}
