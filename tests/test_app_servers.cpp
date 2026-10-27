// EMF.exe and dedicated servers (docs/app.md "Servers"): the folders it keeps, server folders found through
// Steam, the server's enshrouded_server.json (passwords never leave the parser except as "set"), EMF's chat settings in
// emf\emf_core.ini, where a mod runs ("side" in mod.json), log redaction and the EMF package that installs a server.
#include <doctest.h>

#include <string>
#include <vector>

#include "app/archive.h"
#include "app/install.h"
#include "app/servers.h"
#include "app/text.h"
#include "app_test_files.h"
#include "core/json.h"
#include "core/server_chat.h"

using namespace emf::app;
using app_test::TempGame;

namespace {

std::string vdf_path(const std::wstring& dir) {  // as Steam writes it: UTF-8, each backslash doubled
    std::string out;
    for (char c : utf8(dir)) out += c == '\\' ? std::string("\\\\") : std::string(1, c);
    return out;
}

// An enshrouded_server.json with invented values, shaped like the one the server writes (update 2 and later).
const char* kServerJson = R"({
	"name": "Ember Test Server",
	"saveDirectory": "./savegame",
	"logDirectory": "./logs",
	"ip": "0.0.0.0",
	"queryPort": 15637,
	"slotCount": 8,
	"enableTextChat": true,
	"gameSettingsPreset": "Default",
	"gameSettings": {"playerHealthFactor": 1},
	"userGroups": [
		{"name": "Admin", "password": "Adm1nSecretXyz", "canKickBan": true, "reservedSlots": 1},
		{"name": "Friend", "password": "FriendSecret42", "canKickBan": false, "reservedSlots": 0},
		{"name": "Guest", "password": "", "canKickBan": false, "reservedSlots": 0}
	],
	"bannedAccounts": []
})";

}  // namespace

// ---- the folder list ----

TEST_CASE("app servers: a folder is a game (enshrouded.exe), a server (enshrouded_server.exe only) or neither") {
    TempGame game(L"kind_game");
    TempGame server(L"kind_server", false);
    server.write(L"enshrouded_server.exe", "fake");
    TempGame both(L"kind_both");
    both.write(L"enshrouded_server.exe", "fake");
    TempGame none(L"kind_none", false);
    CHECK(folder_kind(game.dir) == "game");
    CHECK(folder_kind(server.dir) == "server");
    CHECK(folder_kind(both.dir) == "game");  // the game's folder with a server exe next to it is still the game
    CHECK(folder_kind(none.dir).empty());
    CHECK(folder_kind(L"").empty());
}

TEST_CASE("app servers: the folder list keeps one game and each server once, and survives a round trip") {
    FolderList list;
    CHECK(remember_folder(list, L"C:\\Games\\Enshrouded", "game"));
    CHECK(remember_folder(list, L"D:\\Servers\\One", "server"));
    CHECK_FALSE(remember_folder(list, L"d:\\servers\\ONE\\", "server"));  // the same folder: case and slash ignored
    CHECK(remember_folder(list, L"D:\\Servers\\Two", "server"));
    CHECK(remember_folder(list, L"E:\\Steam\\Enshrouded", "game"));  // a new game folder replaces the old one
    REQUIRE(list.folders.size() == 3);
    CHECK(list.folders[0].kind == "game");
    CHECK(list.folders[0].path == L"E:\\Steam\\Enshrouded");
    CHECK(list.folders[1].path == L"D:\\Servers\\One");
    CHECK_FALSE(list.folders[1].stop_on_close);  // the default: closing EMF.exe leaves a server running
    list.folders[2].stop_on_close = true;

    const FolderList back = parse_folder_list(folder_list_text(list));
    REQUIRE(back.folders.size() == 3);
    CHECK(back.folders[0].kind == "game");
    CHECK(back.folders[1].path == L"D:\\Servers\\One");
    CHECK(back.folders[2].path == L"D:\\Servers\\Two");
    CHECK(back.folders[2].stop_on_close);
    CHECK(find_folder(back, L"d:\\servers\\two") == &back.folders[2]);
    CHECK(find_folder(back, L"D:\\Servers\\Three") == nullptr);

    FolderList less = back;
    CHECK(forget_folder(less, L"D:\\SERVERS\\ONE"));
    CHECK_FALSE(forget_folder(less, L"D:\\Servers\\One"));
    CHECK(less.folders.size() == 2);
}

TEST_CASE("app servers: folders.json is written whole and read back; a broken one is an empty list") {
    TempGame app(L"folders_json", false);
    const std::wstring file = app.at(L"folders.json");
    CHECK(read_folder_list(file).folders.empty());  // no file yet
    FolderList list;
    remember_folder(list, L"C:\\Games\\Enshrouded", "game");
    remember_folder(list, L"D:\\Servers\\Ünïcode server", "server");
    REQUIRE(write_folder_list(file, list).ok);
    const FolderList back = read_folder_list(file);
    REQUIRE(back.folders.size() == 2);
    CHECK(back.folders[1].path == L"D:\\Servers\\Ünïcode server");
    CHECK_FALSE(app.has(L"folders.json.emf-new"));

    app.write(L"folders.json", "{not json");
    CHECK(read_folder_list(file).folders.empty());
    // entries that are not one of the two kinds, without a path or with a relative path are dropped
    const FolderList odd = parse_folder_list(
        R"({"version":1,"folders":[{"kind":"server","path":"relative\\dir"},{"kind":"other","path":"C:\\x"},)"
        R"({"kind":"server"},{"kind":"server","path":"C:\\Good"},{"kind":"game","path":"C:\\G1"},{"kind":"game","path":"C:\\G2"}]})");
    REQUIRE(odd.folders.size() == 2);
    CHECK(odd.folders[0].kind == "game");  // the game first, and only one
    CHECK(odd.folders[0].path == L"C:\\G1");
    CHECK(odd.folders[1].path == L"C:\\Good");
}

// ---- server folders through Steam ----

TEST_CASE("app servers: the dedicated server (app 2278520) is found in whichever Steam library has its manifest") {
    TempGame steam(L"srv_steam", false);
    TempGame lib(L"srv_steamlib", false);
    steam.write(L"steamapps\\libraryfolders.vdf",
                "\"libraryfolders\"\n{\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"" + vdf_path(lib.dir) + "\"\n\t}\n}\n");
    CHECK(find_servers_in_steam(steam.dir).empty());
    lib.write(L"steamapps\\appmanifest_2278520.acf", "\"AppState\"\n{\n\t\"installdir\"\t\t\"EnshroudedServer\"\n}\n");
    CHECK(find_servers_in_steam(steam.dir).empty());  // the manifest, but no exe
    lib.write(L"steamapps\\common\\EnshroudedServer\\enshrouded_server.exe", "fake");
    // the game's manifest in the Steam folder is not a server
    steam.write(L"steamapps\\appmanifest_1203620.acf", "\"AppState\"\n{\n\t\"installdir\"\t\t\"Enshrouded\"\n}\n");
    steam.write(L"steamapps\\common\\Enshrouded\\enshrouded.exe", "fake");
    const auto found = find_servers_in_steam(steam.dir);
    REQUIRE(found.size() == 1);
    CHECK(found[0] == lib.dir + L"\\steamapps\\common\\EnshroudedServer");
    // no installdir: Steam's default folder name
    TempGame steam2(L"srv_steam2", false);
    steam2.write(L"steamapps\\appmanifest_2278520.acf", "\"AppState\"\n{\n}\n");
    steam2.write(L"steamapps\\common\\EnshroudedServer\\enshrouded_server.exe", "fake");
    CHECK(find_servers_in_steam(steam2.dir).size() == 1);
    CHECK(find_servers_in_steam(L"").empty());
}

// ---- enshrouded_server.json ----

TEST_CASE("app servers: enshrouded_server.json gives name, slots, port and groups; passwords only as set or not") {
    std::vector<std::string> secrets;
    const ServerConfig c = parse_server_config(kServerJson, &secrets);
    CHECK(c.error.empty());
    CHECK(c.name == "Ember Test Server");
    CHECK(c.slots == 8);
    CHECK(c.query_port == 15637);
    CHECK(c.game_port == 0);
    CHECK(c.text_chat_known);
    CHECK(c.text_chat);
    CHECK(c.log_dir == "./logs");
    REQUIRE(c.groups.size() == 3);
    CHECK(c.groups[0].name == "Admin");
    CHECK(c.groups[0].password_set);
    CHECK(c.groups[0].can_kick_ban);
    CHECK(c.groups[0].reserved_slots == 1);
    CHECK(c.groups[1].password_set);
    CHECK_FALSE(c.groups[2].password_set);  // an empty password lets anyone in: the page says so
    CHECK(secrets == std::vector<std::string>{"Adm1nSecretXyz", "FriendSecret42"});

    // what goes to the page never carries a password
    const std::string page = server_config_json(c);
    CHECK(page.find("Adm1nSecretXyz") == std::string::npos);
    CHECK(page.find("FriendSecret42") == std::string::npos);
    CHECK(page.find("\"password_set\":true") != std::string::npos);
    CHECK(page.find("Ember Test Server") != std::string::npos);
    // without the secrets vector, nothing keeps them
    CHECK(parse_server_config(kServerJson).groups.size() == 3);
}

TEST_CASE("app servers: an old enshrouded_server.json (one password, game port) and broken ones") {
    const ServerConfig old = parse_server_config(
        R"({"name": "Old", "password": "OldSecret99", "gamePort": 15636, "queryPort": 15637, "slotCount": 16})");
    CHECK(old.legacy_password_set);
    CHECK(old.game_port == 15636);
    CHECK(old.groups.empty());
    CHECK_FALSE(old.text_chat_known);
    CHECK(server_config_json(old).find("OldSecret99") == std::string::npos);

    const ServerConfig bad = parse_server_config("{\"name\": ");
    CHECK_FALSE(bad.error.empty());
    CHECK(parse_server_config("[1,2]").error.find("object") != std::string::npos);
    // a password that is not text still counts as set, and never shows
    const ServerConfig odd = parse_server_config(R"({"userGroups": [{"name": "X", "password": 12345}]})");
    REQUIRE(odd.groups.size() == 1);
    CHECK(odd.groups[0].password_set);
}

TEST_CASE("app servers: the server's config and log are read from its folder; the log stays inside it") {
    TempGame srv(L"srv_config", false);
    srv.write(L"enshrouded_server.exe", "fake");
    ServerConfig none = read_server_config(srv.dir);
    CHECK_FALSE(none.found);
    CHECK(server_log_path(srv.dir, none) == srv.dir + L"\\logs\\enshrouded_server.log");  // the server's default
    srv.write(L"enshrouded_server.json", "\xEF\xBB\xBF" + std::string(kServerJson));  // with a BOM
    std::vector<std::string> secrets;
    const ServerConfig c = read_server_config(srv.dir, &secrets);
    CHECK(c.found);
    CHECK(c.name == "Ember Test Server");
    CHECK(secrets.size() == 2);
    ServerConfig moved = c;
    moved.log_dir = "./serverlogs";
    CHECK(server_log_path(srv.dir, moved) == srv.dir + L"\\serverlogs\\enshrouded_server.log");
    moved.log_dir = "../elsewhere";  // outside the folder: the default instead
    CHECK(server_log_path(srv.dir, moved) == srv.dir + L"\\logs\\enshrouded_server.log");
    moved.log_dir = "C:\\Windows";
    CHECK(server_log_path(srv.dir, moved) == srv.dir + L"\\logs\\enshrouded_server.log");
}

// ---- log redaction ----

TEST_CASE("app servers: anything that looks like a password is stripped from a log") {
    const std::vector<std::string> secrets{"Adm1nSecretXyz", "abc"};
    const std::string log =
        "[I 00:00:00,450] \"password\": \"hunter22\",\n"
        "[I 00:00:00,451] password=letmein ok\n"
        "[I 00:00:00,452] Password: s3cr3t\n"
        "[I 00:00:01,000] someone typed Adm1nSecretXyz into the chat\n"
        "[I 00:00:01,001] the abc of servers\n"  // too short to strip by value: it would wreck the log
        "[I 00:00:02,000] [server] Player 'Ember' logged in\n";
    const std::string r = redact_secrets(log, secrets);
    CHECK(r.find("hunter22") == std::string::npos);
    CHECK(r.find("letmein") == std::string::npos);
    CHECK(r.find("s3cr3t") == std::string::npos);
    CHECK(r.find("Adm1nSecretXyz") == std::string::npos);
    CHECK(r.find("the abc of servers") != std::string::npos);
    CHECK(r.find("[server] Player 'Ember' logged in") != std::string::npos);
    CHECK(r.find("password\": \"******\"") != std::string::npos);
    CHECK(r.find("password=******\n") != std::string::npos);  // an unquoted value: to the end of the line
    CHECK(redact_secrets("no secrets here", {}) == "no secrets here");
    CHECK(redact_secrets("passwords are set for 4 groups", {}) == "passwords are set for 4 groups");  // no value to hide
}

// ---- EMF's chat settings in emf\emf_core.ini ----

TEST_CASE("app servers: [chat] players, everyone and admins are read as the core reads them") {
    const ChatSettings s = parse_chat_settings(
        "[console]\r\nwindow=0\r\n[chat]\r\n; who may\r\nadmins=0x0123456789abcdef, 0xFEDCBA9876543210 ,,\r\neveryone=1\r\nplayers=0\r\n");
    CHECK(s.everyone);
    CHECK_FALSE(s.players);
    CHECK(s.admins == std::vector<std::string>{"0x0123456789abcdef", "0xFEDCBA9876543210"});
    const ChatSettings none = parse_chat_settings("[console]\nwindow=1\n");
    CHECK_FALSE(none.everyone);
    CHECK(none.admins.empty());
    // keys outside [chat] do not count, and section names ignore case like Windows does
    CHECK(parse_chat_settings("everyone=1\n[CHAT]\nplayers=1\n").players);
    CHECK_FALSE(parse_chat_settings("everyone=1\n[CHAT]\nplayers=1\n").everyone);
}

TEST_CASE("app servers: one ini value changes and every other byte stays") {
    const std::string text = "; EMF settings\r\n[console]\r\nwindow=0\r\n\r\n[chat]\r\n; account hashes\r\nadmins=0x1\r\ndebug=0\r\n\r\n[other]\r\nx=1\r\n";
    CHECK(set_ini_value(text, "chat", "admins", "0x2, 0x3") ==
          "; EMF settings\r\n[console]\r\nwindow=0\r\n\r\n[chat]\r\n; account hashes\r\nadmins=0x2, 0x3\r\ndebug=0\r\n\r\n[other]\r\nx=1\r\n");
    // a new key goes at the end of its section, before the section's blank lines
    CHECK(set_ini_value(text, "chat", "everyone", "1") ==
          "; EMF settings\r\n[console]\r\nwindow=0\r\n\r\n[chat]\r\n; account hashes\r\nadmins=0x1\r\ndebug=0\r\neveryone=1\r\n\r\n[other]\r\nx=1\r\n");
    // a new section at the end, with the file's own line breaks
    CHECK(set_ini_value("[console]\nwindow=0\n", "chat", "admins", "0x9") == "[console]\nwindow=0\n\n[chat]\nadmins=0x9\n");
    CHECK(set_ini_value("", "chat", "admins", "0x9") == "[chat]\r\nadmins=0x9\r\n");
    CHECK(set_ini_value("[chat]\r\nadmins = 0x1\r\n", "Chat", "ADMINS", "0x1") == "[chat]\r\nadmins = 0x1\r\n");  // unchanged
    // a file without a final line break
    CHECK(set_ini_value("[chat]\r\nadmins=0x1", "chat", "everyone", "0") == "[chat]\r\nadmins=0x1\r\neveryone=0\r\n");
}

TEST_CASE("app servers: an admin entry is what the core takes as an account hash (same answer as parse_chat_policy)") {
    for (const char* entry : {"0x0123456789abcdef", "0XABC", "abc", "1", "ffffffffffffffff"}) {
        CHECK(valid_account_hash(entry));
        CHECK(emf::core::parse_chat_policy(entry, false).admins.size() == 1);
    }
    for (const char* entry : {"76561198000000000x", "0x", "0x0123456789abcdef0", "zz", "0x12 34", "-1", "12345678901234567"}) {
        CHECK_FALSE(valid_account_hash(entry));
        CHECK(emf::core::parse_chat_policy(entry, false).admins.empty());
    }
    const AdminList a = parse_admin_lines("0x0123456789abcdef\r\n\r\n  0xfedcba9876543210  \n76561198012345678901\n0x0123456789ABCDEF\n");
    CHECK(a.ok == std::vector<std::string>{"0x0123456789abcdef", "0xfedcba9876543210"});  // the same account once
    CHECK(a.bad == std::vector<std::string>{"76561198012345678901"});
    // commas from a pasted ini line separate too
    CHECK(parse_admin_lines("0x1, 0x2").ok.size() == 2);
}

TEST_CASE("app servers: chat settings are saved into emf\\emf_core.ini keeping its other keys and comments") {
    TempGame srv(L"chat_save", false);
    srv.write(L"enshrouded_server.exe", "fake");
    // no ini yet: the file is made
    ChatSettings s;
    s.admins = {"0x0123456789abcdef"};
    REQUIRE(save_chat_settings(srv.dir, s).ok);
    CHECK(srv.read(L"emf\\emf_core.ini") == "[chat]\r\nadmins=0x0123456789abcdef\r\neveryone=0\r\n");
    // an ini the core wrote, with comments and other sections
    srv.write(L"emf\\emf_core.ini", "[chat]\r\n; account hashes of the players who may run EMF commands\r\nadmins=\r\neveryone=0\r\ndebug=0\r\n[console]\r\nwindow=0\r\n");
    s.admins = {"0x0123456789abcdef", "0xfedcba9876543210"};
    s.everyone = true;
    REQUIRE(save_chat_settings(srv.dir, s).ok);
    CHECK(srv.read(L"emf\\emf_core.ini") ==
          "[chat]\r\n; account hashes of the players who may run EMF commands\r\nadmins=0x0123456789abcdef, "
          "0xfedcba9876543210\r\neveryone=1\r\ndebug=0\r\n[console]\r\nwindow=0\r\n");
    const ChatSettings back = read_chat_settings(srv.dir);
    CHECK(back.everyone);
    CHECK(back.admins.size() == 2);
    // players is written only when the file has it or it is on (a dedicated server ignores it)
    s.players = true;
    REQUIRE(save_chat_settings(srv.dir, s).ok);
    CHECK(srv.read(L"emf\\emf_core.ini").find("players=1") != std::string::npos);
    // what the core reads from the saved file: the same accounts
    wchar_t buf[2048] = {};
    GetPrivateProfileStringW(L"chat", L"admins", L"", buf, 2048, srv.at(L"emf\\emf_core.ini").c_str());
    CHECK(emf::core::parse_chat_policy(utf8(buf), false).admins.size() == 2);
    // a UTF-16 ini stays UTF-16
    const std::wstring w16 = L"\xFEFF[chat]\r\neveryone=1\r\n";
    srv.write(L"emf\\emf_core.ini", std::string(reinterpret_cast<const char*>(w16.data()), w16.size() * 2));
    CHECK(read_chat_settings(srv.dir).everyone);
    s.everyone = false;
    REQUIRE(save_chat_settings(srv.dir, s).ok);
    const std::string after = srv.read(L"emf\\emf_core.ini");
    REQUIRE(after.size() > 2);
    CHECK(after.substr(0, 2) == "\xFF\xFE");
    CHECK_FALSE(read_chat_settings(srv.dir).everyone);
}

TEST_CASE("app servers: accounts the server refused are listed from emf_core.log, each once, newest last") {
    const std::string log =
        "[11:05:41.520 t1] players' chat commands: on, for 0 admin account(s)\n"
        "[11:06:10.000 t2] chat command refused from account 0x00ab00cd00ef0011: help (to allow it, add the account to [chat] admins in emf\\emf_core.ini)\n"
        "[11:06:20.000 t2] chat command refused from account 0x1111222233334444: give x (to allow it, add the account to [chat] admins in emf\\emf_core.ini)\n"
        "[11:06:30.000 t2] chat command refused from account 0x00ab00cd00ef0011: pos (to allow it, add the account to [chat] admins in emf\\emf_core.ini)\n"
        "[11:06:40.000 t2] chat command refused from account 0xNOTHEX: forged\n";
    const auto r = refused_accounts(log);
    REQUIRE(r.size() == 2);
    CHECK(r[0].account == "0x1111222233334444");
    CHECK(r[0].time == "11:06:20");
    CHECK(r[0].command == "give x");
    CHECK(r[1].account == "0x00ab00cd00ef0011");
    CHECK(r[1].command == "pos");  // the newest line of that account
    CHECK(refused_accounts("").empty());
}

// ---- where a mod runs ----

TEST_CASE("app servers: mod.json's side is server, client or both; without it a mod counts as both (unknown)") {
    namespace json = emf::core::json;
    const auto side_of = [](const char* text) { return read_mod_side(*json::parse(text).value); };
    CHECK(side_of(R"({"side": "server"})").side == "server");
    CHECK(side_of(R"({"side": "Client"})").side == "client");
    CHECK(side_of(R"({"side": "both"})").declared);
    const ModSide none = side_of(R"({"id": "x"})");
    CHECK(none.side == "both");
    CHECK_FALSE(none.declared);
    const ModSide bad = side_of(R"({"side": "host"})");
    CHECK(bad.side == "both");
    CHECK_FALSE(bad.declared);
    CHECK(bad.error.find("\"side\"") != std::string::npos);
    CHECK_FALSE(side_of(R"({"side": 3})").error.empty());

    // the notes a server folder's Mods tab shows
    CHECK(server_side_note("both", true).text == "Its mod.json says players need it too.");
    CHECK(server_side_note("both", false).text.find("may need") != std::string::npos);
    CHECK(server_side_note("client", true).text == "Its mod.json says it does nothing on a server.");
    CHECK(server_side_note("client", true).level == "warn");
    CHECK(server_side_note("server", true).text.find("players do not need it") != std::string::npos);
}

// ---- a server folder's Mods tab and install plan ----

namespace {

void fake_mods(const TempGame& g) {
    g.write(L"dinput8.dll", "EMF-LOADER-7d3c9f");
    g.write(L"emf\\emf_core.dll", std::string("emf_core 0.3.0\0", 15));
    g.write(L"emf\\mods\\clock\\mod.json", R"({"id":"clock","name":"Clock","version":"1.1.0","side":"client"})");
    g.write(L"emf\\mods\\clock\\clock.dll", "dll");
    g.write(L"emf\\mods\\loot\\mod.json", R"({"id":"loot","name":"Loot","version":"1.0.0","side":"server"})");
    g.write(L"emf\\mods\\loot\\loot.dll", "dll");
    g.write(L"emf\\mods\\stacks\\mod.json", R"({"id":"stacks","name":"Stacks","version":"1.0.0","side":"both"})");
    g.write(L"emf\\mods\\stacks\\stacks.dll", "dll");
    g.write(L"emf\\mods\\plain\\mod.json", R"({"id":"plain","name":"Plain","version":"1.0.0"})");
    g.write(L"emf\\mods\\plain\\plain.dll", "dll");
    g.write(L"emf\\mods\\odd\\mod.json", R"({"id":"odd","name":"Odd","version":"1.0.0","side":"host"})");
    g.write(L"emf\\mods\\odd\\odd.dll", "dll");
    g.write(L"mods\\content\\mod.json", R"({"id":"content","name":"Content","version":"1.0.0","side":"both"})");
}

const ModEntry* mod_named(const FolderState& s, const std::string& name) {
    for (const auto& m : s.mods)
        if (m.name == name) return &m;
    return nullptr;
}

bool warned(const FolderState& s, const std::string& text) {
    for (const auto& w : s.warnings)
        if (w.text.find(text) != std::string::npos) return true;
    return false;
}

}  // namespace

TEST_CASE("app servers: a server folder's Mods tab says where each mod runs and flags what does nothing there") {
    TempGame srv(L"side_server", false);
    srv.write(L"enshrouded_server.exe", "fake");
    fake_mods(srv);
    const FolderState s = scan_game_folder(srv.dir);
    REQUIRE(s.server);
    const ModEntry* clock = mod_named(s, "Clock");
    REQUIRE(clock);
    CHECK(clock->side == "client");
    CHECK(clock->side_declared);
    CHECK(clock->server_note == "Its mod.json says it does nothing on a server.");
    CHECK(clock->server_level == "warn");
    CHECK(mod_named(s, "Loot")->server_note.find("players do not need it") != std::string::npos);
    CHECK(mod_named(s, "Stacks")->server_note == "Its mod.json says players need it too.");
    CHECK(mod_named(s, "Plain")->server_note.find("may need") != std::string::npos);
    CHECK_FALSE(mod_named(s, "Plain")->side_declared);
    CHECK(mod_named(s, "Odd")->side == "both");
    CHECK(mod_named(s, "Content")->server_note == "Its mod.json says players need it too.");  // an EML mod's mod.json may say it too
    CHECK(warned(s, "Clock does nothing on a server"));
    CHECK(warned(s, "Odd: \"side\" must be"));
    // switched off, a client mod is no warning any more
    REQUIRE(set_mod_enabled(srv.dir, "emf", "clock", false).ok);
    CHECK_FALSE(warned(scan_game_folder(srv.dir), "Clock does nothing on a server"));

    // the game's folder: no server notes
    TempGame game(L"side_game");
    fake_mods(game);
    const FolderState g = scan_game_folder(game.dir);
    REQUIRE_FALSE(g.server);
    CHECK(mod_named(g, "Clock")->side == "client");
    CHECK(mod_named(g, "Clock")->server_note.empty());
    CHECK_FALSE(warned(g, "does nothing on a server"));
}

TEST_CASE("app servers: installing a downloaded mod into a server folder says where it runs") {
    TempGame srv(L"side_plan", false);
    srv.write(L"enshrouded_server.exe", "fake");
    TempGame arc(L"side_plan_archive", false);
    arc.write(L"emf\\mods\\hud\\mod.json", R"({"id":"hud","name":"Hud","version":"1.0.0","side":"client"})");
    arc.write(L"emf\\mods\\hud\\hud.dll", "dll");
    PlanInput in;
    in.game = srv.dir;
    in.folder = arc.dir;
    uint64_t bytes = 0;
    std::string why;
    REQUIRE(list_tree(arc.dir, in.files, bytes, why));
    in.archive = "Hud-1-0.zip";
    in.emf_installed = true;
    const InstallPlan p = build_plan(in);
    REQUIRE(p.parts.size() == 1);
    CHECK(p.installable());  // still installable: the player decides
    std::string lines;
    for (const auto& l : p.parts[0].lines) lines += l + "\n";
    CHECK(lines.find("does nothing on a server.") != std::string::npos);
    std::string notes;
    for (const auto& n : p.notes) notes += n + "\n";
    CHECK(notes.find("Hud does nothing on a server") != std::string::npos);

    // the same archive for the game's folder: no server lines
    TempGame game(L"side_plan_game");
    in.game = game.dir;
    const InstallPlan q = build_plan(in);
    REQUIRE(q.parts.size() == 1);
    for (const auto& l : q.parts[0].lines) CHECK(l.find("server") == std::string::npos);
}

// ---- installing EMF into a server folder ----

namespace {

std::string repo_file(const char* rel) {
    std::string s;
    read_file(wide(std::string(EMF_SOURCE_DIR) + "/" + rel), s);
    return s;
}

// What package.ps1 puts into game\: EMF's loader and core (stand-ins), a mod, EMF.exe and its setup\ scripts.
void package_tree(const TempGame& t, const std::wstring& game_rel) {
    const std::wstring g = game_rel.empty() ? L"" : game_rel + L"\\";
    t.write(g + L"dinput8.dll", "EMF-LOADER-7d3c9f stand-in loader");
    t.write(g + L"emf\\emf_core.dll", std::string("emf_core 9.8.7\0", 15));
    t.write(g + L"emf\\known_tools.txt", repo_file("src/core/known_tools.txt"));
    t.write(g + L"emf\\mods\\clock\\mod.json", repo_file("mods/clock/mod.json"));
    t.write(g + L"emf\\mods\\clock\\clock.dll", "dll");
    t.write(g + L"emf\\EMF.exe", "exe");
    t.write(g + L"emf\\setup\\install.ps1", repo_file("package/install.ps1"));
    t.write(g + L"emf\\setup\\emf_common.ps1", repo_file("package/emf_common.ps1"));
}

}  // namespace

TEST_CASE("app servers: the EMF to install into a server is the package or game this EMF.exe came with") {
    // EMF.exe in an unpacked package: <package>\game\emf\EMF.exe
    TempGame pkg(L"emfpkg_package", false);
    package_tree(pkg, L"game");
    const EmfPackage a = find_emf_package(pkg.at(L"game\\emf"));
    REQUIRE_MESSAGE(a.ok, a.error);
    CHECK(a.tree == pkg.at(L"game"));
    CHECK(a.installed_in.empty());
    CHECK(a.scripts == pkg.at(L"game\\emf\\setup"));
    CHECK(a.version == "9.8.7");

    // EMF.exe installed in the game: <game>\emf\EMF.exe, EMF's files listed in emf\installed.txt
    TempGame game(L"emfpkg_game");
    package_tree(game, L"");
    game.write(L"emf\\installed.txt",
               "dinput8.dll\r\nemf\\emf_core.dll\r\nemf\\known_tools.txt\r\nemf\\mods\\clock\\mod.json\r\nemf\\mods\\clock\\clock.dll\r\n"
               "emf\\EMF.exe\r\nemf\\setup\\install.ps1\r\nemf\\setup\\emf_common.ps1\r\n");
    game.write(L"emf\\config\\clock.ini", "[clock]\r\nplace=0\r\n");  // the player's settings stay out
    const EmfPackage b = find_emf_package(game.at(L"emf"));
    REQUIRE_MESSAGE(b.ok, b.error);
    CHECK(b.tree.empty());
    CHECK(b.installed_in == game.dir);
    CHECK(b.version == "9.8.7");

    TempGame stage(L"emfpkg_stage", false);
    REQUIRE(stage_installed_emf(game.dir, stage.at(L"tree")).empty());
    CHECK(stage.has(L"tree\\dinput8.dll"));
    CHECK(stage.has(L"tree\\emf\\mods\\clock\\clock.dll"));
    CHECK(stage.has(L"tree\\emf\\setup\\install.ps1"));
    CHECK_FALSE(stage.has(L"tree\\emf\\config\\clock.ini"));
    CHECK_FALSE(stage.has(L"tree\\enshrouded.exe"));

    // a record that names a path outside the folder or the game's exe: not staged
    game.write(L"emf\\installed.txt", "dinput8.dll\r\nemf\\emf_core.dll\r\n..\\outside.txt\r\nC:\\Windows\\win.ini\r\nenshrouded.exe\r\n");
    TempGame stage2(L"emfpkg_stage2", false);
    REQUIRE(stage_installed_emf(game.dir, stage2.at(L"tree")).empty());
    CHECK(stage2.has(L"tree\\emf\\emf_core.dll"));
    CHECK_FALSE(stage2.has(L"tree\\enshrouded.exe"));
    CHECK_FALSE(stage2.has(L"outside.txt"));

    // a dev build, or a package without its setup scripts: none, with the reason
    TempGame bare(L"emfpkg_bare", false);
    const EmfPackage c = find_emf_package(bare.at(L"app"));
    CHECK_FALSE(c.ok);
    CHECK(c.error.find("install.cmd") != std::string::npos);
}

TEST_CASE("app servers: EMF installs into a server folder with the package's own install.ps1 (rename, chain, record)") {
    TempGame pkg(L"emfinst_package", false);
    package_tree(pkg, L"game");
    const EmfPackage p = find_emf_package(pkg.at(L"game\\emf"));
    REQUIRE(p.ok);
    TempGame srv(L"emfinst_server", false);
    srv.write(L"enshrouded_server.exe", "fake");
    srv.write(L"dinput8.dll", "MZ creative-mode-marker another tool");  // a foreign dinput8.dll
    srv.write(L"emf\\config\\clock.ini", "[clock]\r\nplace=2\r\n");  // the server owner's settings
    const ScriptRun r = install_emf_into(p, srv.dir);
    std::string out;
    for (const auto& l : r.lines) out += l + "\n";
    const std::string why = r.error + "\n" + out;
    REQUIRE_MESSAGE(r.ok, why);
    CHECK(srv.read(L"dinput8.dll").find("EMF-LOADER-7d3c9f") != std::string::npos);
    CHECK(srv.read(L"emf\\emf_core.dll").find("emf_core 9.8.7") != std::string::npos);
    CHECK(srv.has(L"emf\\mods\\clock\\clock.dll"));
    CHECK(srv.read(L"emf\\config\\clock.ini") == "[clock]\r\nplace=2\r\n");  // never overwritten
    // the foreign dinput8.dll: renamed, chained and recorded, the installer's own rules
    CHECK(srv.read(L"dinput8-chain.ini").find(".dll") != std::string::npos);
    CHECK(srv.read(L"emf\\moved.txt").find("moved\t") != std::string::npos);
    CHECK(srv.read(L"emf\\installed.txt").find("emf\\emf_core.dll") != std::string::npos);
    CHECK(out.find("Start the server normally") != std::string::npos);

    // the installer's refusals reach the player: here, a folder that is no game or server
    TempGame none(L"emfinst_none", false);
    const ScriptRun bad = install_emf_into(p, none.dir);
    CHECK_FALSE(bad.ok);
    std::string bad_out = bad.error;
    for (const auto& l : bad.lines) bad_out += l + "\n";
    CHECK(bad_out.find("enshrouded_server.exe") != std::string::npos);
}

TEST_CASE("app servers: redaction takes the passwords out first, then whole values after a password word") {
    const std::vector<std::string> secrets{"Adm1nSecretXyz"};
    // a secret is taken out before the password rule masks part of it
    CHECK(redact_secrets("password=xx,Adm1nSecretXyz", secrets).find("Adm1nSecretXyz") == std::string::npos);
    // unquoted values to the end of the line, so a value with spaces or commas goes whole
    const std::string r = redact_secrets("password=ab,cdefgh\nPassword: my secret phrase\nnext line\n", {});
    CHECK(r.find("cdefgh") == std::string::npos);
    CHECK(r.find("secret phrase") == std::string::npos);
    CHECK(r.find("next line") != std::string::npos);
    // a quoted value with an escaped quote in it goes whole
    const std::string q = redact_secrets("{\"password\": \"ab\\\"cd\", \"name\": \"x\"}", {});
    CHECK(q.find("cd") == std::string::npos);
    CHECK(q.find("\"name\": \"x\"") != std::string::npos);
}

TEST_CASE("app servers: a number password and the passwords of a broken enshrouded_server.json still go to redaction") {
    std::vector<std::string> secrets;
    parse_server_config(R"({"userGroups": [{"name": "X", "password": 4815162342}]})", &secrets);
    CHECK(secrets == std::vector<std::string>{"4815162342"});
    std::vector<std::string> broken;
    const ServerConfig c = parse_server_config("{\"name\": \"x\", \"userGroups\": [{\"password\": \"BrokenJsonSecret\", ", &broken);
    CHECK_FALSE(c.error.empty());
    CHECK(c.error.find("BrokenJsonSecret") == std::string::npos);
    CHECK(broken == std::vector<std::string>{"BrokenJsonSecret"});
}

TEST_CASE("app servers: a command-line argument keeps a trailing backslash (a drive root) out of its closing quote") {
    CHECK(command_line_arg(L"C:\\Games\\Server one") == L"\"C:\\Games\\Server one\"");
    CHECK(command_line_arg(L"D:\\") == L"\"D:\\\\\"");
    CHECK(command_line_arg(L"D:\\x\\\\") == L"\"D:\\x\\\\\\\\\"");
}

// ---- edge cases ----

TEST_CASE("app servers: a refused account a player types into the chat is not listed (one entry per line, anchored)") {
    const std::string log =
        // a refused player typed the core's own words: only their own account counts
        "[11:06:10.000 t2] chat command refused from account 0x00ab00cd00ef0011: help chat command refused from account "
        "0x1337133713371337 x (to allow it, add the account to [chat] admins in emf\\emf_core.ini)\n"
        // an admin typed them: their command ran, nobody was refused
        "[11:06:11.000 t2] chat command from account 0x2222333344445555 (handle 0x40): help chat command refused from "
        "account 0x1337133713371337 y\n"
        // words that are not where the core writes them
        "[11:06:12.000 t2] [mod] note: chat command refused from account 0x1337133713371337: z (to allow it, add the account to [chat] admins in emf\\emf_core.ini)\n";
    const auto r = refused_accounts(log);
    REQUIRE(r.size() == 1);
    CHECK(r[0].account == "0x00ab00cd00ef0011");
    CHECK(r[0].command == "help chat command refused from account 0x1337133713371337 x");
}

TEST_CASE("app servers: [chat] is read exactly as Windows reads it (first section, spaces and one pair of quotes dropped)") {
    TempGame t(L"ini_parity", false);
    const std::vector<std::string> cases = {
        "[chat]\r\nadmins=0x1\r\n[core]\r\nx=1\r\n",
        "\xEF\xBB\xBF[chat]\r\nadmins=0x1\r\neveryone=1\r\n",
        "[chat]\r\neveryone=0\r\n[other]\r\na=1\r\n[chat]\r\nadmins=0x2\r\neveryone=1\r\n",
        "[chat]\r\nadmins=\"0x3, 0x4\"\r\n",
        "[chat]\r\nadmins='0x5'\r\n",
        "[chat]\r\nadmins=\"0x6\r\n",
        "[chat]\r\nadmins=0x7 ; me\r\n",
        "[chat]\r\n  admins  =   0x8  \r\n",
        "[ chat ]\r\nadmins=0x9\r\n",
        "[CHAT]\r\nADMINS=0xa\r\nEveryone=2\r\n",
        "[chat]\r\nadmins=0xb\r\nadmins=0xc\r\n",
        "[chat]\nadmins=0xd\n",
        "[chat]\r\n;admins=0xe\r\n",
    };
    int n = 0;
    for (const auto& text : cases) {
        const std::wstring file = t.at(L"case" + std::to_wstring(n++) + L".ini");
        t.write(file.substr(t.dir.size() + 1), text);
        wchar_t buf[2048] = {};
        GetPrivateProfileStringW(L"chat", L"admins", L"<none>", buf, 2048, file.c_str());
        const std::string windows = utf8(buf);
        const auto ours = ini_value(text, "chat", "admins");
        CAPTURE(text);
        CHECK((ours ? *ours : std::string("<none>")) == windows);
        const UINT everyone = GetPrivateProfileIntW(L"chat", L"everyone", 0, file.c_str());
        CHECK(parse_chat_settings(text).everyone == (everyone != 0));
    }
}

TEST_CASE("app servers: passwords are taken out of the raw log, whatever its bytes, and each line decoded on its own") {
    const std::string secret = "Gl\xC3\xBC" "ckspiel24";  // UTF-8, as the JSON parser gives it
    // a log whose end is half a UTF-8 character (the server was writing): before, the whole text went through ANSI first
    const std::string cut = "[I 00:00:01] someone pasted " + secret + " into chat\n[I 00:00:02] Player 'J\xC3";
    const std::string a = redact_log(cut, {secret});
    CHECK(a.find("ckspiel24") == std::string::npos);
    CHECK(a.find("someone pasted ******") != std::string::npos);
    // the same password as the ANSI code page writes it
    const std::string ansi = "[I 00:00:01] typed Gl\xFC" "ckspiel24 here\n";
    CHECK(redact_log(ansi, {secret}).find("ckspiel24") == std::string::npos);
    // JSON-escaped, and "password is x"
    CHECK(redact_log("cfg: {\"pw\":\"ab\\\"cdefg\"}\n", {"ab\"cdefg"}).find("cdefg") == std::string::npos);
    CHECK(redact_secrets("the server password is hunter22\n", {}).find("hunter22") == std::string::npos);
    CHECK(redact_secrets("server password = hunter22\n", {}).find("hunter22") == std::string::npos);
    // a good UTF-8 line next to a bad one keeps its characters
    const std::string mixed = "caf\xC3\xA9 ok\nbad \xE9 line\n";
    const std::string m = redact_log(mixed, {});
    CHECK(m.find("caf\xC3\xA9 ok") != std::string::npos);
}

TEST_CASE("app servers: two password keys in one group both go to redaction") {
    std::vector<std::string> secrets;
    parse_server_config(R"({"userGroups":[{"name":"A","password":"FirstPassw0rd","password":"SecondPassw0rd"}]})", &secrets);
    CHECK(std::find(secrets.begin(), secrets.end(), "FirstPassw0rd") != secrets.end());
    CHECK(std::find(secrets.begin(), secrets.end(), "SecondPassw0rd") != secrets.end());
}

TEST_CASE("app servers: only EMF's own files are staged from installed.txt (an allowlist)") {
    TempGame game(L"stage_allow");
    package_tree(game, L"");
    game.write(L"steam_api64.dll", "the game's");
    game.write(L"savegame\\world.sav", "a save");
    game.write(L"emf\\config\\clock.ini", "[clock]");
    game.write(L"emf\\installed_mods\\x.json", "{}");
    game.write(L"emf\\moved.txt", "moved\tx.dll");
    game.write(L"emf\\emf_core.ini", "[chat]");
    game.write(L"emf\\backup\\old\\x.dll", "x");
    game.write(L"emf\\installed.txt",
               "dinput8.dll\r\nemf\\emf_core.dll\r\nemf\\known_tools.txt\r\nemf\\mods\\clock\\mod.json\r\nemf\\EMF.exe\r\n"
               "emf\\setup\\install.ps1\r\nemf\\setup\\emf_common.ps1\r\nsteam_api64.dll\r\nsavegame\\world.sav\r\n"
               "emf\\config\\clock.ini\r\nemf\\installed_mods\\x.json\r\nemf\\moved.txt\r\nemf\\emf_core.ini\r\n"
               "emf\\backup\\old\\x.dll\r\nemf\\installed.txt\r\n");
    TempGame stage(L"stage_allow_out", false);
    REQUIRE(stage_installed_emf(game.dir, stage.at(L"tree")).empty());
    for (const wchar_t* in : {L"dinput8.dll", L"emf\\emf_core.dll", L"emf\\known_tools.txt", L"emf\\mods\\clock\\mod.json",
                              L"emf\\EMF.exe", L"emf\\setup\\install.ps1"}) {
        CAPTURE(utf8(in));
        CHECK(stage.has(std::wstring(L"tree\\") + in));
    }
    for (const wchar_t* out : {L"steam_api64.dll", L"savegame\\world.sav", L"emf\\config\\clock.ini", L"emf\\installed_mods\\x.json",
                               L"emf\\moved.txt", L"emf\\emf_core.ini", L"emf\\backup\\old\\x.dll", L"emf\\installed.txt"}) {
        CAPTURE(utf8(out));
        CHECK_FALSE(stage.has(std::wstring(L"tree\\") + out));
    }
}

TEST_CASE("app servers: the admins line must fit the 2047 characters the core reads") {
    std::vector<std::string> admins;
    while (admins_value(admins).size() + 20 <= kAdminsMaxChars) admins.push_back("0x0123456789abcdef");
    CHECK(admins_value(admins).size() <= kAdminsMaxChars);
    admins.push_back("0x0123456789abcdef");
    CHECK(admins_value(admins).size() > kAdminsMaxChars);
    CHECK(admins_value({"0x1", "0x2"}) == "0x1, 0x2");
}

TEST_CASE("app servers: the EMF package names the mods it brings and whether each starts switched on") {
    TempGame pkg(L"emfpkg_mods", false);
    package_tree(pkg, L"game");
    pkg.write(L"game\\emf\\mods\\big\\mod.json", R"({"id":"big","name":"Big","version":"1.0.0","enabled":false})");
    const EmfPackage p = find_emf_package(pkg.at(L"game\\emf"));
    REQUIRE(p.ok);
    REQUIRE(p.mods.size() == 2);
    CHECK(p.mods[0].id == "big");
    CHECK_FALSE(p.mods[0].on);
    CHECK(p.mods[1].id == "clock");
    CHECK(p.mods[1].on);
    CHECK(p.mods[1].side == "client");
}

TEST_CASE("app servers: the install plan names the folder it installs into") {
    InstallPlan p;
    p.game = L"C:\\Servers\\Ember";
    p.name = "Mod";
    CHECK(plan_json(p, "p1", "").find("\"game\":\"C:\\\\Servers\\\\Ember\"") != std::string::npos);
}
