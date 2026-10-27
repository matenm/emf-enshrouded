// dinput8-chain.ini (shared with XHL-Core and Auto Loot Plus) lists DLL mods that one dinput8.dll starts for the
// others. EMF's loader reads it leniently: other tools may put sections, comments or settings in it.
#include <doctest.h>

#include <string>
#include <vector>

#include "loader/chain_list.h"

using emf::loader::parse_chain_list;
using Names = std::vector<std::wstring>;

TEST_CASE("chain list: one DLL per line, in order") {
    CHECK(parse_chain_list("creative.dll\r\nstatsliders.dll\n") == Names{L"creative.dll", L"statsliders.dll"});
}

TEST_CASE("chain list: blank lines and comments are skipped; only [Chain] and lines before any section count") {
    const char* text =
        "\xEF\xBB\xBF; settings of another tool\n"
        "early.dll\n"
        "[Chain]\n"
        "\n"
        "  # a comment\n"
        "// another comment\n"
        "  atlas.dll  \n"
        "[Disabled]\n"
        "old.dll\n"
        "[chain]\n"
        "EML.dll\n";
    CHECK(parse_chain_list(text) == Names{L"early.dll", L"atlas.dll", L"EML.dll"});
}

TEST_CASE("chain list: a file saved as UTF-16 (PowerShell 5.1's > does that) is read too") {
    const std::wstring w = L"[Chain]\r\nwide.dll\r\n";
    std::string bytes = "\xFF\xFE";
    bytes.append(reinterpret_cast<const char*>(w.data()), w.size() * sizeof(wchar_t));
    CHECK(parse_chain_list(bytes) == Names{L"wide.dll"});
}

TEST_CASE("chain list: at most 256 entries (a huge file must not hold the game's start)") {
    std::string text = "[Chain]\n";
    for (int i = 0; i < 300; ++i) text += "m" + std::to_string(i) + ".dll\n";
    const Names names = parse_chain_list(text);
    CHECK(names.size() == emf::loader::kMaxChainEntries);
    CHECK(names.back() == L"m255.dll");
}

TEST_CASE("chain list: key=value lines give their value; settings that are not DLLs are ignored") {
    CHECK(parse_chain_list("delay=500\nmod1=creative.dll\nmod2 = \"no-intro.dll\"\nenabled=1\n") ==
          Names{L"creative.dll", L"no-intro.dll"});
}

TEST_CASE("chain list: a comment after the name is dropped") {
    CHECK(parse_chain_list("creative.dll ; Creative Mode\nxhl.dll # XHL-Core\n") == Names{L"creative.dll", L"xhl.dll"});
}

TEST_CASE("chain list: a name is listed once, whatever its case") {
    CHECK(parse_chain_list("a.dll\nA.DLL\nb.dll\na.dll\n") == Names{L"a.dll", L"b.dll"});
}

TEST_CASE("chain list: only DLLs inside the game folder: absolute paths, drives, '..' and UNC paths are refused") {
    CHECK(parse_chain_list("C:\\Windows\\evil.dll\n\\\\server\\share\\x.dll\n..\\up.dll\nmods\\..\\..\\x.dll\n"
                           "/abs.dll\nd:rel.dll\nmods\\ok.dll\nsub/also.dll\n") ==
          Names{L"mods\\ok.dll", L"sub\\also.dll"});  // '/' becomes a backslash: with '/' the DLL's own folder is not searched
}

TEST_CASE("chain list: EMF's own dinput8.dll is never chained (it would be loading itself)") {
    CHECK(parse_chain_list("dinput8.dll\nDINPUT8.DLL\nother.dll\n") == Names{L"other.dll"});
}

TEST_CASE("chain list: a non-ASCII name in UTF-8 is read as UTF-8") {
    CHECK(parse_chain_list("m\xC3\xB6" "d.dll\n") == Names{L"m\u00F6d.dll"});
}
