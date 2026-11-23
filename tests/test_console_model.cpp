#include <windows.h>

#include <doctest.h>

#include <string>
#include <vector>

#include "core/overlay/console_model.h"

using namespace emf::core;

TEST_CASE("classify_reply: errors red, successes green, the rest grey") {
    CHECK(classify_reply("usage: give <item name> [count]") == LineKind::Error);
    CHECK(classify_reply("unknown item 'Nothing' (items <part of a name> lists names)") == LineKind::Error);
    CHECK(classify_reply("not run: load a world you host first") == LineKind::Error);
    CHECK(classify_reply("command 'x' crashed and was disabled") == LineKind::Error);
    CHECK(classify_reply("Enemy01BehaviorState is 400 B in this game but 2504 B in EMF's headers (a game update changed it): refused") ==
          LineKind::Error);
    CHECK(classify_reply("give failed (see the log)") == LineKind::Error);
    CHECK(classify_reply("could not write the template data (see the log)") == LineKind::Error);
    CHECK(classify_reply("gave 3 x Material_T1_Wood_Logs to player 1") == LineKind::Success);
    CHECK(classify_reply("spawned 2: 6855 6856") == LineKind::Success);
    CHECK(classify_reply("Material_T1_Wood_Logs: stack 250 -> 1000 (until the game restarts)") == LineKind::Success);
    CHECK(classify_reply("time of day is now 12.00 h (was 15.35 h)") == LineKind::Success);
    CHECK(classify_reply("player 1 at (3704.2, 905.0, 1171.0) m") == LineKind::Normal);
    CHECK(classify_reply("> pos") == LineKind::Command);
}

TEST_CASE("ConsoleModel keeps at most max_lines, newest last, with a version that bumps on every line") {
    ConsoleModel m(2000);
    const uint64_t v0 = m.version();
    for (int i = 0; i < 2500; ++i) m.add_line("line " + std::to_string(i));
    CHECK(m.size() == 2000);
    CHECK(m.version() == v0 + 2500);
    const auto last = m.snapshot(1999, 5);
    REQUIRE(last.size() == 1);
    CHECK(last[0].second == "line 2499");
    const auto first = m.snapshot(0, 2);
    REQUIRE(first.size() == 2);
    CHECK(first[0].second == "line 500");
    CHECK(m.snapshot(5000, 3).empty());
    m.add_line("> help");
    CHECK(m.snapshot(m.size() - 1, 1)[0].first == LineKind::Command);
}

TEST_CASE("ConsoleModel history: no empty lines or repeats of the last one, 100 kept, Up/Down walk") {
    ConsoleModel m;
    m.push_history("help");
    m.push_history("help");
    m.push_history("");
    m.push_history("pos");
    CHECK(m.history_prev() == "pos");
    CHECK(m.history_prev() == "help");
    CHECK(m.history_prev() == "help");  // stays at the oldest
    CHECK(m.history_next() == "pos");
    CHECK(m.history_next() == "");  // past the newest: an empty line
    for (int i = 0; i < 150; ++i) m.push_history("cmd " + std::to_string(i));
    std::string oldest;
    for (int i = 0; i < 200; ++i) oldest = m.history_prev();
    CHECK(oldest == "cmd 50");  // 100 kept
}

TEST_CASE("ConsoleModel history survives a save and load") {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    const std::wstring path = std::wstring(dir) + L"emf_test_history_" + std::to_wstring(GetCurrentProcessId()) + L".txt";
    ConsoleModel a;
    a.push_history("give Material_T1_Wood_Logs 5");
    a.push_history("time 12");
    REQUIRE(a.save_history(path));
    ConsoleModel b;
    REQUIRE(b.load_history(path));
    CHECK(b.history_prev() == "time 12");
    CHECK(b.history_prev() == "give Material_T1_Wood_Logs 5");
    DeleteFileW(path.c_str());
    ConsoleModel c;
    CHECK_FALSE(c.load_history(path));  // missing file: nothing loaded, no crash
}

TEST_CASE("complete: commands, then item names after give/stack, component names after tpl/tplset") {
    CompletionSources src;
    src.commands = {"entities", "give", "help", "items", "spawn", "stack", "time", "tpl", "tplset"};
    src.items = {"Material_T1_Wood_Logs", "Material_T1_Wood_Logs_Dead", "Material_T4_Wood_Logs_Palm", "Tool_Torch01"};
    src.components = {"ActiveNpcState", "Enemy01BehaviorState", "Locomotion"};
    CHECK(complete("gi", src) == std::vector<std::string>{"give"});
    CHECK(complete("T", src) == std::vector<std::string>{"time", "tpl", "tplset"});
    const auto wood = complete("give material_t1_wood", src);
    REQUIRE(wood.size() == 2);
    CHECK(wood[0] == "give Material_T1_Wood_Logs");
    CHECK(wood[1] == "give Material_T1_Wood_Logs_Dead");
    const auto logs = complete("stack Wood_Logs", src);  // substring match
    CHECK(logs.size() == 3);
    CHECK(complete("tpl 1025 activenpc", src) == std::vector<std::string>{"tpl 1025 ActiveNpcState"});
    CHECK(complete("tplset 6820 enemy", src) == std::vector<std::string>{"tplset 6820 Enemy01BehaviorState"});
    CHECK(complete("nosuch x", src).empty());
    CHECK(complete("", src).empty());
    std::vector<std::string> many;
    for (int i = 0; i < 20; ++i) many.push_back("cmd" + std::to_string(i));
    src.commands = many;
    CHECK(complete("cmd", src).size() == 8);
}

TEST_CASE("complete: item names after put too") {
    CompletionSources src;
    src.commands = {"put"};
    src.items = {"Material_T1_Wood_Logs", "Torch"};
    const auto r = complete("put To", src);
    REQUIRE(r.size() == 1);
    CHECK(r[0] == "put Torch");
}

TEST_CASE("console completion: settings and set complete mod ids, then a mod's setting keys") {
    CompletionSources src;
    src.commands = {"set", "settings"};
    src.mod_settings = {{"clock", {"place", "h24"}}, {"companion", {"key", "faction"}}};
    CHECK(complete("settings c", src) == std::vector<std::string>{"settings clock", "settings companion"});
    CHECK(complete("set clo", src) == std::vector<std::string>{"set clock"});
    CHECK(complete("set clock p", src) == std::vector<std::string>{"set clock place"});
    CHECK(complete("set clock ", src) == std::vector<std::string>{"set clock enabled", "set clock h24", "set clock place"});
    CHECK(complete("set nobody p", src).empty());
}

// ---- a time on each line, clear, copy, find (GitHub #61, #11, #60) ----

TEST_CASE("console: each line keeps the time it came, shown as hh:mm:ss") {
    ConsoleModel m(10);
    m.add_line("> pos", 21 * 3600 + 4 * 60 + 17);
    m.add_line("player 1 at (1, 2, 3) m", 21 * 3600 + 4 * 60 + 18);
    const auto lines = m.lines(0, 5);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].kind == LineKind::Command);
    CHECK(lines[0].text == "> pos");
    CHECK(ConsoleModel::stamp_text(lines[0].seconds) == "21:04:17");
    CHECK(ConsoleModel::stamp_text(lines[1].seconds) == "21:04:18");
    CHECK(ConsoleModel::stamp_text(0) == "00:00:00");
    CHECK(ConsoleModel::stamp_text(86399) == "23:59:59");
    m.add_line("now");  // the clock's time of day
    CHECK(m.lines(2, 1).at(0).seconds < 86400u);
    CHECK(m.snapshot(0, 1).at(0).second == "> pos");  // snapshot as before
}

TEST_CASE("console: clear empties the scrollback; the version still grows and says where the clear was") {
    ConsoleModel m(10);
    m.add_line("a");
    m.add_line("b");
    const uint64_t v = m.version();
    m.clear();
    CHECK(m.size() == 0);
    CHECK(m.version() > v);
    CHECK(m.cleared_version() == m.version());
    m.add_line("c");
    CHECK(m.size() == 1);
    CHECK(m.snapshot(0, 1).at(0).second == "c");
    CHECK(is_console_clear("clear"));
    CHECK(is_console_clear("  CLS "));
    CHECK(is_console_clear("Clear"));
    CHECK_FALSE(is_console_clear("clear now"));
    CHECK_FALSE(is_console_clear("cleared"));
    CHECK_FALSE(is_console_clear("/clear"));
}

TEST_CASE("console: the scrollback as text for the clipboard: one line each, CRLF, the times when they show") {
    ConsoleModel m(2000);
    CHECK(m.text(0, 10, false).empty());
    m.add_line("> items wood", 3600);
    m.add_line("Material_T1_Wood_Logs", 3601);
    CHECK(m.text(0, 10, false) == "> items wood\r\nMaterial_T1_Wood_Logs");
    CHECK(m.text(0, 10, true) == "01:00:00 > items wood\r\n01:00:01 Material_T1_Wood_Logs");
    CHECK(m.text(1, 1, false) == "Material_T1_Wood_Logs");
    for (int i = 0; i < 2500; ++i) m.add_line("line " + std::to_string(i));
    const std::string all = m.text(0, m.size(), false);
    size_t breaks = 0;
    for (size_t at = all.find("\r\n"); at != std::string::npos; at = all.find("\r\n", at + 2)) ++breaks;
    CHECK(breaks == 1999);  // 2000 lines, the cap
    CHECK(all.rfind("line 2499") == all.size() - 9);
}

TEST_CASE("console: find lists the lines that hold the text, any case, oldest first") {
    ConsoleModel m(100);
    m.add_line("> items wood");
    m.add_line("Material_T1_Wood_Logs");
    m.add_line("Material_T1_Stone");
    m.add_line("WOODEN thing");
    CHECK(m.find_all("wood") == std::vector<size_t>{0, 1, 3});
    CHECK(m.find_all("STONE") == std::vector<size_t>{2});
    CHECK(m.find_all("nothing").empty());
    CHECK(m.find_all("").empty());
}

TEST_CASE("console: the scrollback holds as many lines as it was made with") {
    ConsoleModel m(500);
    for (int i = 0; i < 600; ++i) m.add_line(std::to_string(i));
    CHECK(m.size() == 500);
    CHECK(m.snapshot(0, 1).at(0).second == "100");
}

