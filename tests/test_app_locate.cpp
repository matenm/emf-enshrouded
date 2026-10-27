// EMF.exe finds the game through Steam like the installer does, knows when the game runs from the folder it manages,
// and opens only Nexus links in the browser.
#include <doctest.h>

#include <string>
#include <vector>

#include "app/locate.h"
#include "app/text.h"
#include "app_test_files.h"

using namespace emf::app;
using app_test::TempGame;

TEST_CASE("app steam: library paths from libraryfolders.vdf, with escaped backslashes") {
    const char* vdf =
        "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\n"
        "\t\t\"label\"\t\t\"\"\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"D:\\\\SteamLibrary\"\n\t}\n}\n";
    CHECK(steam_library_paths(vdf) == std::vector<std::wstring>{L"C:\\Program Files (x86)\\Steam", L"D:\\SteamLibrary"});
    CHECK(steam_library_paths("").empty());
}

TEST_CASE("app steam: installdir from the app manifest") {
    CHECK(acf_installdir("\"AppState\"\n{\n\t\"appid\"\t\t\"1203620\"\n\t\"installdir\"\t\t\"Enshrouded\"\n}\n") == "Enshrouded");
    CHECK(acf_installdir("\"AppState\" { }").empty());
}

TEST_CASE("app steam: the game is found in whichever library has its app manifest") {
    TempGame steam(L"steam", false);
    TempGame lib(L"steamlib", false);
    std::string libpath;  // as Steam writes it: UTF-8, each backslash doubled
    for (char c : emf::app::utf8(lib.dir)) libpath += c == '\\' ? std::string("\\\\") : std::string(1, c);
    steam.write(L"steamapps\\libraryfolders.vdf", "\"libraryfolders\"\n{\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"" + libpath + "\"\n\t}\n}\n");
    CHECK(find_game_in_steam(steam.dir).empty());  // no app manifest anywhere yet
    lib.write(L"steamapps\\appmanifest_1203620.acf", "\"AppState\"\n{\n\t\"installdir\"\t\t\"Enshrouded Game\"\n}\n");
    CHECK(find_game_in_steam(steam.dir).empty());  // manifest but no exe
    lib.write(L"steamapps\\common\\Enshrouded Game\\enshrouded.exe", "fake");
    CHECK(find_game_in_steam(steam.dir) == lib.dir + L"\\steamapps\\common\\Enshrouded Game");
    CHECK(find_game_in_steam(L"").empty());
}

TEST_CASE("app running: a game process inside the folder makes it busy; one elsewhere does not") {
    const std::wstring game = L"C:\\Games\\Enshrouded";
    CHECK(game_running_reason(game, {}).empty());
    CHECK(game_running_reason(game, {{L"enshrouded.exe", L"D:\\Other\\enshrouded.exe", 7}}).empty());
    CHECK(game_running_reason(game, {{L"enshrouded.exe", L"C:\\Games\\Enshrouded2\\enshrouded.exe", 7}}).empty());
    const std::string r = game_running_reason(game, {{L"enshrouded.exe", L"c:\\games\\ENSHROUDED\\enshrouded.exe", 42}});
    CHECK(r.find("enshrouded.exe") != std::string::npos);
    CHECK(r.find("42") != std::string::npos);
    CHECK_FALSE(game_running_reason(game + L"\\", {{L"enshrouded_server.exe", L"C:\\Games\\Enshrouded\\enshrouded_server.exe", 1}}).empty());
}

TEST_CASE("app running: a game folder reached through a junction still matches the game's real path") {
    TempGame real(L"junction_real");
    wchar_t base[MAX_PATH];
    GetTempPathW(MAX_PATH, base);
    const std::wstring link = std::wstring(base) + L"emf_app_test_junction_" + std::to_wstring(GetCurrentProcessId());
    RemoveDirectoryW(link.c_str());
    const std::wstring cmd = L"cmd /c mklink /J \"" + link + L"\" \"" + real.dir + L"\" >nul";
    REQUIRE(_wsystem(cmd.c_str()) == 0);
    const std::string r = game_running_reason(link, {{L"enshrouded.exe", real.dir + L"\\enshrouded.exe", 11}});
    RemoveDirectoryW(link.c_str());  // the junction only, not what it points at
    CHECK(r.find("11") != std::string::npos);
}

TEST_CASE("app running: a process whose folder Windows hides counts as running from it") {
    const std::string r = game_running_reason(L"C:\\Games\\Enshrouded", {{L"enshrouded.exe", L"", 9}});
    CHECK(r.find("administrator") != std::string::npos);
}

TEST_CASE("app links: only https links to nexusmods.com open in the browser") {
    CHECK(nexus_url_allowed("https://www.nexusmods.com/enshrouded/mods/123"));
    CHECK(nexus_url_allowed("https://nexusmods.com/enshrouded"));
    CHECK(nexus_url_allowed("https://www.NexusMods.com/enshrouded/mods/1?tab=files"));
    CHECK_FALSE(nexus_url_allowed("http://www.nexusmods.com/enshrouded"));
    CHECK_FALSE(nexus_url_allowed("https://nexusmods.com.evil.com/x"));
    CHECK_FALSE(nexus_url_allowed("https://evilnexusmods.com/x"));
    CHECK_FALSE(nexus_url_allowed("https://www.nexusmods.com@evil.com/x"));
    CHECK_FALSE(nexus_url_allowed("https://www.nexusmods.com:8080/x"));
    CHECK_FALSE(nexus_url_allowed("https://www.nexusmods.com/a b"));
    CHECK_FALSE(nexus_url_allowed("https://www.nexusmods.com/\"x"));
    CHECK_FALSE(nexus_url_allowed("file:///C:/Windows/notepad.exe"));
    CHECK_FALSE(nexus_url_allowed("steam://rungameid/1"));
    CHECK_FALSE(nexus_url_allowed(""));
}

TEST_CASE("app paths: inside the folder, case ignored, '..' resolved first") {
    CHECK(path_inside(L"C:\\Games\\E", L"C:\\Games\\E"));
    CHECK(path_inside(L"C:\\Games\\E", L"c:\\games\\e\\mods\\x"));
    CHECK(path_inside(L"C:\\Games\\E\\", L"C:\\Games\\E\\mods"));
    CHECK_FALSE(path_inside(L"C:\\Games\\E", L"C:\\Games\\Else"));
    CHECK_FALSE(path_inside(L"C:\\Games\\E", L"C:\\Games\\E\\..\\Other"));
    CHECK_FALSE(path_inside(L"C:\\Games\\E", L"C:\\Windows"));
    CHECK_FALSE(path_inside(L"", L"C:\\Games"));
}

TEST_CASE("app paths: a path Windows cannot make full fails closed") {
    const std::wstring huge = L"C:\\Games\\E\\" + std::wstring(40000, L'a');
    CHECK_FALSE(path_inside(L"C:\\Games\\E", huge));
    CHECK_FALSE(path_inside(huge, L"C:\\Games\\E\\x"));
    // and the running check then says it cannot tell, rather than letting switches through
    CHECK_FALSE(game_running_reason(huge, {{L"enshrouded.exe", L"C:\\Games\\E\\enshrouded.exe", 3}}).empty());
}
