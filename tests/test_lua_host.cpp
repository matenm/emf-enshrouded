// The Lua host driven through a fake EmfApi with real Lua scripts in a temporary mods folder.
#include <windows.h>

#include <doctest.h>

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "lua_host/host.h"

using emf::lua::Host;

#include "lua_test_fake.h"

TEST_CASE("lua host: main.lua runs with emf.id; emf.log and print reach the log; notes and HUD lines pass through") {
    Rig r(L"basic");
    add_script(r.dir, "hello", "emf.log('hi from', emf.id, 42)\nprint('printed')\nemf.notify('ready')\n"
                               "emf.hud('line', 'text', 2)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    CHECK(host.script_count() == 1);
    CHECK(host.status("hello") == "running");
    CHECK(logged("[hello] hi from hello 42"));
    CHECK(logged("[hello] printed"));
    REQUIRE(r.fake.notes.size() == 1);
    CHECK(r.fake.notes[0] == "ready");
    CHECK(r.fake.hud.count("hello.line") == 1);  // HUD ids are prefixed with the mod's id
}

TEST_CASE("lua host: an error in main.lua pauses the mod with file and line; others still run") {
    Rig r(L"error");
    add_script(r.dir, "broken", "local x = 1\nerror('boom')\n");
    add_script(r.dir, "fine", "emf.log('fine ran')\n");
    Host host(&r.api, r.dir);
    host.load_all();
    CHECK(host.status("broken").find("paused") == 0);
    CHECK(logged("broken/main.lua:2: boom"));
    CHECK(host.status("fine") == "running");
    CHECK(logged("fine ran"));
}

TEST_CASE("lua host: sandbox: no os.exit, require only finds the mod's own files, endless loops are stopped") {
    Rig r(L"sandbox");
    add_script(r.dir, "safe", "emf.log('exit is', tostring(os.exit))\nlocal u = require('util')\nemf.log('util says', u.word)\n"
                              "emf.log('loadlib is', tostring(package.loadlib))\n");
    write(r.dir + L"\\safe\\util.lua", "return { word = 'hello' }\n");
    add_script(r.dir, "looper", "while true do end\n");
    Host host(&r.api, r.dir);
    const uint64_t t0 = GetTickCount64();
    host.load_all();
    const uint64_t took = GetTickCount64() - t0;
    CHECK(logged("exit is nil"));
    CHECK(logged("util says hello"));
    CHECK(logged("loadlib is nil"));
    CHECK(host.status("looper").find("paused") == 0);
    CHECK(logged("longer than 250 ms"));
    CHECK(took < 3000);
}

TEST_CASE("lua host: on_tick callbacks run on tick; a callback error pauses the mod") {
    Rig r(L"ticks");
    add_script(r.dir, "ticker", "count = 0\nemf.on_tick(function(w)\n  count = count + 1\n"
                                "  if w:is_server() then emf.hud('n', tostring(count)) end\n"
                                "  if count == 3 then error('third tick') end\nend)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    host.tick(kServer);
    CHECK(r.fake.hud["ticker.n"] == "1");
    host.tick(kClient);  // is_client: no hud update, still counted
    CHECK(r.fake.hud["ticker.n"] == "1");
    host.tick(kServer);  // count 3: the line shows 3, then the error
    CHECK(r.fake.hud["ticker.n"] == "3");
    CHECK(host.status("ticker").find("paused") == 0);
    CHECK(logged("third tick"));
    host.tick(kServer);  // paused: not called again
    CHECK(r.fake.hud["ticker.n"] == "3");
}

TEST_CASE("lua host: commands are prefixed with the mod id and answer through emf.reply") {
    Rig r(L"commands");
    add_script(r.dir, "cmds", "emf.command('greet', '<name>: say hi', function(w, args)\n"
                              "  emf.reply('hi ' .. (args[1] or '?'))\nend)\n"
                              "emf.command('cmds.full', ': already prefixed', function() emf.reply('ok') end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    REQUIRE(r.fake.commands.size() == 2);
    CHECK(r.fake.commands[0].name == "cmds.greet");
    CHECK(r.fake.commands[1].name == "cmds.full");
    r.run_command("cmds.greet Ann");
    REQUIRE(r.fake.replies.size() == 1);
    CHECK(r.fake.replies[0] == "hi Ann");
}

TEST_CASE("lua host: a command name the console cannot take is logged and not added; the mod keeps running") {
    Rig r(L"longcmd");
    add_script(r.dir, "cmds",
               "emf.log('long added', tostring(emf.command('a_name_that_is_far_too_long_for_it', '', function() end)))\n"
               "emf.log('space added', tostring(emf.command('has space', '', function() end)))\n"
               "emf.log('ok added', tostring(emf.command('ok', '', function() emf.reply('fine') end)))\n");
    Host host(&r.api, r.dir);
    host.load_all();
    CHECK(host.status("cmds") == "running");
    CHECK(logged("long added false"));
    CHECK(logged("space added false"));
    CHECK(logged("ok added true"));
    CHECK(logged("cmds.a_name_that_is_far_too_long_for_it"));  // the log line names it
    CHECK(logged("(39 characters) is not added"));
    REQUIRE(r.fake.commands.size() == 1);
    CHECK(r.fake.commands[0].name == "cmds.ok");
    r.run_command("cmds.ok");
    REQUIRE_FALSE(r.fake.replies.empty());
    CHECK(r.fake.replies.back() == "fine");
}

TEST_CASE("lua host: a command whose name only changes case on a reload keeps its registration") {
    Rig r(L"cmdcase");
    add_script(r.dir, "cmds", "emf.command('greet', '', function() emf.reply('v1') end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    REQUIRE(r.fake.commands.size() == 1);
    Sleep(20);
    write(r.dir + L"\\cmds\\main.lua", "emf.command('Greet', '', function() emf.reply('v2') end)\n");
    host.check_reload_now();
    CHECK(host.status("cmds") == "running");
    CHECK(r.fake.commands.size() == 1);
    r.run_command("cmds.greet");  // the console's own spelling (it ignores case)
    REQUIRE_FALSE(r.fake.replies.empty());
    CHECK(r.fake.replies.back() == "v2");
}

TEST_CASE("lua host: a script that needs more memory than its limit is paused with a clear error; others run") {
    Rig r(L"memory");
    add_script(r.dir, "hog", "emf.log('small ok', #string.rep('x', 1000))\n"
                             "local big = string.rep('x', 64 * 1024 * 1024)\nemf.log('big made', #big)\n");
    add_script(r.dir, "grower", "emf.command('grow', '', function() local t = {} for i = 1, 1e7 do t[i] = i end emf.reply('grown') end)\n");
    add_script(r.dir, "fine", "emf.log('fine ran')\n");
    Host host(&r.api, r.dir);
    host.set_memory_limit(8u << 20);
    host.load_all();
    CHECK(logged("small ok 1000"));
    CHECK_FALSE(logged("big made"));
    CHECK(host.status("hog").find("paused: not enough memory") == 0);
    CHECK(host.status("hog").find("8 MB") != std::string::npos);
    CHECK(host.status("fine") == "running");
    CHECK(host.status("grower") == "running");
    r.run_command("grower.grow");  // many small allocations from a callback
    CHECK(host.status("grower").find("paused: not enough memory") == 0);
    for (const auto& reply : r.fake.replies) CHECK(reply != "grown");
    CHECK(emf::lua::kMemoryLimit == 256u << 20);
}

TEST_CASE("lua host: settings are typed from mod.json; set_setting and change callbacks") {
    Rig r(L"settings");
    add_script(r.dir, "conf",
               "emf.log('speed', math.type(emf.setting('speed')), emf.setting('speed'))\n"
               "emf.log('on', type(emf.setting('on')), tostring(emf.setting('on')))\n"
               "emf.log('name', type(emf.setting('name')), emf.setting('name'))\n"
               "emf.log('ratio', math.type(emf.setting('ratio')))\n"
               "emf.on_setting_changed(function(key) emf.log('changed', key, emf.setting(key)) end)\n"
               "emf.set_setting('speed', 7)\n",
               R"([{"key": "speed", "type": "int", "default": 3}, {"key": "on", "type": "bool", "default": true},
                  {"key": "name", "type": "text", "default": "Bob"}, {"key": "ratio", "type": "float", "default": 0.5}])");
    r.fake.settings = {{"conf.speed", "3"}, {"conf.on", "1"}, {"conf.name", "Bob"}, {"conf.ratio", "0.5"}};
    Host host(&r.api, r.dir);
    host.load_all();
    CHECK(logged("speed integer 3"));
    CHECK(logged("on boolean true"));
    CHECK(logged("name string Bob"));
    CHECK(logged("ratio float"));
    CHECK(r.fake.settings["conf.speed"] == "7");
    REQUIRE(r.fake.setting_watchers.size() == 1);
    r.fake.setting_watchers[0].second.first(kServer, "conf", "speed", r.fake.setting_watchers[0].second.second);
    CHECK(logged("changed speed 7"));
}

TEST_CASE("lua host: a changed file reloads the mod: on_unload runs, its HUD lines and callbacks go") {
    Rig r(L"reload");
    add_script(r.dir, "live", "emf.hud('v', 'one')\nemf.on_tick(function() emf.hud('t', 'old tick') end)\n"
                              "function on_unload() emf.log('unloading') end\n");
    Host host(&r.api, r.dir);
    host.load_all();
    host.tick(kServer);
    CHECK(r.fake.hud["live.t"] == "old tick");
    Sleep(20);
    write(r.dir + L"\\live\\main.lua", "emf.hud('v', 'two')\nemf.on_tick(function() emf.hud('t', 'new tick') end)\n");
    host.check_reload_now();
    CHECK(logged("unloading"));
    CHECK(r.fake.hud["live.v"] == "two");
    CHECK(r.fake.hud.count("live.t") == 0);  // removed with the old state
    host.tick(kServer);
    CHECK(r.fake.hud["live.t"] == "new tick");
    CHECK(logged("reloaded"));
}

TEST_CASE("lua host: a paused mod runs again when its file is fixed") {
    Rig r(L"fixit");
    add_script(r.dir, "fix", "error('not yet')\n");
    Host host(&r.api, r.dir);
    host.load_all();
    CHECK(host.status("fix").find("paused") == 0);
    Sleep(20);
    write(r.dir + L"\\fix\\main.lua", "emf.log('fixed now')\n");
    host.check_reload_now();
    CHECK(host.status("fix") == "running");
    CHECK(logged("fixed now"));
}

TEST_CASE("lua host: a mod with strict globals (an error for unknown names) reloads without ending the game") {
    Rig r(L"strict");
    add_script(r.dir, "strict", "setmetatable(_G, {__index = function(_, k) error('undefined ' .. k) end})\n"
                                "emf.log('strict v1')\n");
    Host host(&r.api, r.dir);
    host.load_all();
    CHECK(host.status("strict") == "running");
    Sleep(20);
    write(r.dir + L"\\strict\\main.lua", "emf.log('strict v2')\n");
    host.check_reload_now();  // looks up on_unload, which this mod does not have
    CHECK(host.status("strict") == "running");
    CHECK(logged("strict v2"));
}

TEST_CASE("lua host: the time limit holds against pcall and the debug library; nothing launches programs or loads bytecode") {
    Rig r(L"sandbox2");
    add_script(r.dir, "spinner", "while true do pcall(function() while true do end end) end\n");
    add_script(r.dir, "xspinner", "while true do xpcall(function() while true do end end, function(e) return e end) end\n");
    add_script(r.dir, "cospinner", "while true do coroutine.resume(coroutine.create(function() while true do end end)) end\n");
    add_script(r.dir, "probe",
               "emf.log('sethook is', tostring(debug.sethook), 'getregistry is', tostring(debug.getregistry))\n"
               "emf.log('traceback is', type(debug.traceback))\n"
               "emf.log('execute is', tostring(os.execute), 'popen is', tostring(io.popen))\n"
               "local f, err = load(string.dump(function() return 1 end))\n"
               "emf.log('bytecode load gives', tostring(f))\n"
               "emf.log('text load gives', load('return 7')())\n"
               "local ok = pcall(require, 'outside')\n"
               "emf.log('require outside', tostring(ok))\n");
    write(r.dir + L"\\outside.lua", "return 1\n");  // next to the mod folders, not inside one
    Host host(&r.api, r.dir);
    const uint64_t t0 = GetTickCount64();
    host.load_all();
    const uint64_t took = GetTickCount64() - t0;
    CHECK(host.status("spinner").find("paused") == 0);
    CHECK(host.status("xspinner").find("paused") == 0);
    CHECK(host.status("cospinner").find("paused") == 0);
    CHECK(logged("spinner/main.lua:1: the script ran longer than 250 ms"));  // where it looped
    CHECK(took < 5000);
    CHECK(logged("sethook is nil getregistry is nil"));
    CHECK(logged("traceback is function"));
    CHECK(logged("execute is nil popen is nil"));
    CHECK(logged("bytecode load gives nil"));
    CHECK(logged("text load gives 7"));
    CHECK(logged("require outside false"));
}

TEST_CASE("lua host: a mod the core did not leave as a script (a dependency failed later) stops on the first tick") {
    Rig r(L"recheck");
    add_script(r.dir, "late", "emf.on_tick(function() emf.log('late ticked') end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    CHECK(host.status("late") == "running");
    r.fake.mods[0].status = EMF_MOD_REJECTED;  // the core re-checked its dependencies after every DLL loaded
    host.tick(kServer);
    CHECK_FALSE(logged("late ticked"));
    CHECK(host.status("late").find("not run") == 0);
}

TEST_CASE("lua host: a mod in a folder with non-ASCII characters loads, requires its modules and opens its own files") {
    Rig r(L"utf8_\u00e6\u00f8\u00e5");  // the game's own path has them too
    const std::wstring folder = r.dir + L"\\mod_\u00e6\u00f8\u00e5";
    CreateDirectoryW(folder.c_str(), nullptr);
    write(folder + L"\\mod.json", R"({"id": "utf8mod", "version": "1", "main": "main.lua"})");
    write(folder + L"\\main.lua",
          "local u = require('util')\n"
          "emf.log('util says', u.word)\n"
          "local f = assert(io.open(emf.dir .. '\\\\data.txt', 'w'))\n"
          "f:write('kept')\n"
          "f:close()\n"
          "local g = assert(io.open(emf.dir .. '\\\\data.txt'))\n"
          "emf.log('read back', g:read('a'))\n"
          "g:close()\n"
          "emf.log('loadfile', loadfile(emf.dir .. '\\\\extra.lua')())\n"
          "emf.log('dofile', dofile(emf.dir .. '\\\\extra.lua'))\n"
          "emf.log('dir has the name', emf.dir:find('mod_\\xc3\\xa6\\xc3\\xb8\\xc3\\xa5', 1, true) ~= nil)\n");
    write(folder + L"\\util.lua", "return { word = 'hello' }\n");
    write(folder + L"\\extra.lua", "return 'extra ran'\n");
    EmfModInfo info{};
    info.size = sizeof info;
    info.status = EMF_MOD_SCRIPT;
    std::snprintf(info.id, sizeof info.id, "utf8mod");
    std::snprintf(info.source, sizeof info.source, "mod_\xc3\xa6\xc3\xb8\xc3\xa5\\");  // the core gives UTF-8
    std::snprintf(info.main, sizeof info.main, "main.lua");
    r.fake.mods.push_back(info);
    Host host(&r.api, r.dir);
    host.load_all();
    CHECK(host.status("utf8mod") == "running");
    CHECK(logged("util says hello"));
    CHECK(logged("read back kept"));
    CHECK(logged("loadfile extra ran"));
    CHECK(logged("dofile extra ran"));
    CHECK(logged("dir has the name true"));
    std::ifstream data(folder + L"\\data.txt", std::ios::binary);
    CHECK(std::string((std::istreambuf_iterator<char>(data)), std::istreambuf_iterator<char>()) == "kept");
    data.close();
    Sleep(20);
    write(folder + L"\\util.lua", "return { word = 'changed' }\n");
    host.check_reload_now();  // the reload check sees files there too
    CHECK(logged("util says changed"));
}

TEST_CASE("lua host: emf.dir is the mod's folder (for its own data files)") {
    Rig r(L"dirmod");
    add_script(r.dir, "keeper", "emf.log('dir ends', emf.dir:sub(-6))\n");
    Host host(&r.api, r.dir);
    host.load_all();
    CHECK(logged("dir ends keeper"));
}

namespace {
std::string read_file(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
const char* kKeeper =
    "emf.on_world_data(function(w, text)\n"
    "  emf.hud('got', '[' .. text .. ']')\n"
    "  emf.hud('kind', w:is_server() and 'server' or 'client')\n"
    "end)\n"
    "emf.command('save', '<text>', function(w, args) emf.reply(tostring(emf.save_world_data(args[1] or ''))) end)\n";
}  // namespace

TEST_CASE("lua host: world data: on_world_data runs on a server tick after the host's world loaded, with what was saved") {
    Rig r(L"worlddata");
    add_script(r.dir, "keeper", kKeeper);
    const std::wstring file = r.dir + L"\\..\\config\\keeper\\0001.txt";
    DeleteFileW(file.c_str());
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("keeper.save early");
    REQUIRE(!r.fake.replies.empty());
    CHECK(r.fake.replies.back() == "false");  // no world loaded yet: nothing is written
    CHECK(GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES);
    host.tick(kServer);
    CHECK(r.fake.hud.count("keeper.got") == 0);
    fire_load(1);
    host.tick(kClient);
    CHECK(r.fake.hud.count("keeper.got") == 0);  // server ticks only
    host.tick(kServer);
    CHECK(r.fake.hud["keeper.got"] == "[]");
    CHECK(r.fake.hud["keeper.kind"] == "server");
    host.tick(kServer);  // once per session
    r.run_command("keeper.save kills=3");
    CHECK(r.fake.replies.back() == "true");
    CHECK(read_file(file) == "kills=3");
    DeleteFileW(file.c_str());
}

TEST_CASE("lua host: world data: a reloaded script gets the saved text again; another world gets its own file") {
    Rig r(L"worlddata2");
    add_script(r.dir, "keeper", kKeeper);
    const std::wstring one = r.dir + L"\\..\\config\\keeper\\0001.txt";
    const std::wstring two = r.dir + L"\\..\\config\\keeper\\0002.txt";
    DeleteFileW(one.c_str());
    DeleteFileW(two.c_str());
    Host host(&r.api, r.dir);
    host.load_all();
    fire_load(1);
    host.tick(kServer);
    r.run_command("keeper.save kills=3");
    REQUIRE(r.fake.replies.back() == "true");
    Sleep(20);
    write(r.dir + L"\\keeper\\main.lua", std::string(kKeeper) + "-- changed\n");
    host.check_reload_now();
    CHECK(r.fake.hud.count("keeper.got") == 0);  // the old state's HUD lines went with it
    host.tick(kServer);
    CHECK(r.fake.hud["keeper.got"] == "[kills=3]");
    r.fake.session = 2;  // another hosted session, world slot 2
    host.tick(kServer);  // before its save loaded: nothing
    CHECK(r.fake.hud["keeper.got"] == "[kills=3]");
    r.run_command("keeper.save too-early");
    CHECK(r.fake.replies.back() == "false");
    fire_load(2);
    host.tick(kServer);
    CHECK(r.fake.hud["keeper.got"] == "[]");
    r.run_command("keeper.save kills=9");
    CHECK(read_file(two) == "kills=9");
    CHECK(read_file(one) == "kills=3");  // untouched
    DeleteFileW(one.c_str());
    DeleteFileW(two.c_str());
}

TEST_CASE("lua host: world data: after a reload save_world_data answers false until on_world_data has run again") {
    Rig r(L"worlddata3");
    auto code = [](const char* state) {
        return std::string("emf.on_world_data(function(w, text) emf.hud('got', text) end)\n"
                           "emf.on_tick(function(w) emf.hud('saved', tostring(emf.save_world_data('") +
               state + "'))) end)\n";
    };
    add_script(r.dir, "keeper", code("state v1"));
    const std::wstring file = r.dir + L"\\..\\config\\keeper\\0001.txt";
    CreateDirectoryW((r.dir + L"\\..\\config").c_str(), nullptr);
    CreateDirectoryW((r.dir + L"\\..\\config\\keeper").c_str(), nullptr);
    write(file, "old data");
    auto disk = [&] {
        std::ifstream f(file, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };
    Host host(&r.api, r.dir);
    host.load_all();
    fire_load(1);
    host.tick(kServer);
    CHECK(r.fake.hud["keeper.got"] == "old data");
    CHECK(r.fake.hud["keeper.saved"] == "true");
    CHECK(disk() == "state v1");
    Sleep(20);
    write(r.dir + L"\\keeper\\main.lua", code("state v2"));  // the new code starts from its defaults
    host.check_reload_now();
    host.tick(kClient);  // the reloaded script ticks before its data came back
    CHECK(r.fake.hud["keeper.saved"] == "false");
    CHECK(disk() == "state v1");  // its defaults did not overwrite the world's data
    host.tick(kServer);
    CHECK(r.fake.hud["keeper.got"] == "state v1");
    CHECK(r.fake.hud["keeper.saved"] == "true");
    CHECK(disk() == "state v2");
    DeleteFileW(file.c_str());
}

TEST_CASE("lua host: world data: on_world_data registered after the load's data was read gets it on the next server tick") {
    Rig r(L"worlddata4");
    add_script(r.dir, "late",
               "emf.command('watch', '', function()\n"
               "  emf.on_world_data(function(w, text) calls = (calls or 0) + 1 emf.hud('got', calls .. ' [' .. text .. ']') end)\n"
               "end)\n"
               "emf.command('save', '<text>', function(w, args) emf.reply(tostring(emf.save_world_data(args[1] or ''))) end)\n");
    const std::wstring file = r.dir + L"\\..\\config\\late\\0001.txt";
    CreateDirectoryW((r.dir + L"\\..\\config").c_str(), nullptr);
    CreateDirectoryW((r.dir + L"\\..\\config\\late").c_str(), nullptr);
    write(file, "kills=4");
    Host host(&r.api, r.dir);
    host.load_all();
    fire_load(1);
    host.tick(kServer);  // the world's data is read; no callback yet
    r.run_command("late.save kills=5");
    REQUIRE_FALSE(r.fake.replies.empty());
    CHECK(r.fake.replies.back() == "true");  // saving works as before
    r.run_command("late.watch");
    CHECK(r.fake.hud.count("late.got") == 0);
    host.tick(kClient);  // server ticks only
    CHECK(r.fake.hud.count("late.got") == 0);
    host.tick(kServer);
    CHECK(r.fake.hud["late.got"] == "1 [kills=5]");  // what the file holds now
    host.tick(kServer);
    CHECK(r.fake.hud["late.got"] == "1 [kills=5]");  // once
    DeleteFileW(file.c_str());
}

TEST_CASE("lua host: world data: a handler registered again on every tick gets the text once, not every tick") {
    Rig r(L"worlddata_lazy");
    add_script(r.dir, "lazy",
               "calls = 0\n"
               "emf.on_tick(function(w)\n"
               "  emf.on_world_data(function(w2, text) calls = calls + 1; emf.hud('calls', tostring(calls)) end)\n"
               "end)\n");
    const std::wstring file = r.dir + L"\\..\\config\\lazy\\0001.txt";
    DeleteFileW(file.c_str());
    Host host(&r.api, r.dir);
    host.load_all();
    fire_load(1);
    for (int i = 0; i < 6; ++i) host.tick(kServer);
    CHECK(r.fake.hud["lazy.calls"] == "1");
}

TEST_CASE("lua host: a script mod whose main file is written Main.LUA runs") {
    Rig r(L"mixedcase");
    add_script(r.dir, "mixed", "emf.log('mixed case runs')\n");
    std::snprintf(r.fake.mods.back().main, sizeof r.fake.mods.back().main, "Main.LUA");
    Host host(&r.api, r.dir);
    host.load_all();
    CHECK(host.status("mixed") == "running");
    CHECK(logged("mixed case runs"));
}

TEST_CASE("lua host: emf.hud takes a select setting's corner name and a color setting's #rrggbb") {
    Rig r(L"hud_style");
    add_script(r.dir, "styled",
               "emf.hud('a', 'A', 'bottom_left', '#ffd070')\n"
               "emf.hud('b', 'B', 'Bottom Right', '#11223344')\n"
               "emf.hud('c', 'C', 0, 0x80FF80FF)\n"
               "emf.hud('d', 'D')\n"
               "emf.hud('f', 'F', 2, '112233')\n"
               "emf.hud('g', 'G', 1, '0x80FF80FF')\n"
               "emf.hud('h', 'H', 1, '0XffD070ff')\n"
               "emf.log('bad corner', pcall(emf.hud, 'e', 'E', 'middle'))\n"
               "emf.log('bad colour', pcall(emf.hud, 'e', 'E', 0, 'red'))\n");
    Host host(&r.api, r.dir);
    host.load_all();
    REQUIRE(host.status("styled") == "running");
    CHECK(r.fake.hud_style["styled.a"] == std::make_pair(uint32_t(EMF_HUD_BOTTOM_LEFT), 0xFFD070FFu));
    CHECK(r.fake.hud_style["styled.b"] == std::make_pair(uint32_t(EMF_HUD_BOTTOM_RIGHT), 0x11223344u));
    CHECK(r.fake.hud_style["styled.c"] == std::make_pair(uint32_t(EMF_HUD_TOP_RIGHT), 0x80FF80FFu));
    CHECK(r.fake.hud_style["styled.d"] == std::make_pair(uint32_t(EMF_HUD_TOP_RIGHT), 0u));
    CHECK(r.fake.hud_style["styled.f"] == std::make_pair(uint32_t(EMF_HUD_BOTTOM_LEFT), 0x112233FFu));  // text is a colour
    CHECK(r.fake.hud_style["styled.g"] == std::make_pair(uint32_t(EMF_HUD_BOTTOM_RIGHT), 0x80FF80FFu));  // "0x..." as in 1.2
    CHECK(r.fake.hud_style["styled.h"] == std::make_pair(uint32_t(EMF_HUD_BOTTOM_RIGHT), 0xFFD070FFu));
    CHECK(logged("bad corner false"));
    CHECK(logged("the corner is \"top_right\", \"bottom_right\" or \"bottom_left\""));
    CHECK(logged("a colour is \"#rrggbb\""));
}

namespace {
std::map<uint32_t, bool> g_keys;  // the fake keyboard: vk -> held
bool fake_key(uint32_t vk) { return g_keys[vk]; }
}  // namespace

TEST_CASE("lua host: emf.on_key runs once per press, on the server world when this game hosts one, else the client's") {
    Rig r(L"on_key");
    g_keys.clear();
    add_script(r.dir, "keys",
               "emf.on_key('F7', function(world) emf.log('F7 on', world:is_server() and 'server' or 'client') end)\n"
               "emf.on_key('G', function(world) emf.log('G on', world:is_server() and 'server' or 'client') end, 'client')\n"
               "emf.command('down', '', function() emf.reply(tostring(emf.key_down('F7'))) end)\n");
    Host host(&r.api, r.dir);
    host.set_key_reader(fake_key);
    host.load_all();
    REQUIRE(host.status("keys") == "running");
    host.tick(kClient);  // a client of someone else's game: no server world steps
    g_keys[VK_F7] = true;
    host.tick(kClient);
    host.tick(kClient);  // still held: not another press
    g_keys[VK_F7] = false;
    host.tick(kClient);
    int presses = 0;
    for (const auto& l : r.fake.log) presses += l.find("F7 on client") != std::string::npos;
    CHECK(presses == 1);
    r.fake.log.clear();
    host.tick(kServer);  // now this game hosts: a press goes to the server world, whichever world saw it first
    g_keys[VK_F7] = true;
    host.tick(kClient);
    CHECK_FALSE(logged("F7 on"));
    host.tick(kServer);
    CHECK(logged("F7 on server"));
    g_keys['G'] = true;  // asked for the client world
    host.tick(kServer);
    CHECK_FALSE(logged("G on"));
    host.tick(kClient);
    CHECK(logged("G on client"));
    r.fake.replies.clear();
    r.run_command("keys.down");
    REQUIRE_FALSE(r.fake.replies.empty());
    CHECK(r.fake.replies.back() == "true");
}

TEST_CASE("lua host: emf.on_key with a keybind setting follows the player's choice at once; an empty one never fires") {
    Rig r(L"on_key_setting");
    g_keys.clear();
    add_script(r.dir, "binds", "emf.on_key('toggle', function() emf.log('toggled') end)\n",
               R"([{"key": "toggle", "type": "keybind", "default": "F7"}])");
    r.fake.settings["binds.toggle"] = "F7";
    Host host(&r.api, r.dir);
    host.set_key_reader(fake_key);
    host.load_all();
    REQUIRE(host.status("binds") == "running");
    g_keys[VK_F7] = true;
    host.tick(kClient);
    CHECK(logged("toggled"));
    g_keys[VK_F7] = false;
    host.tick(kClient);
    r.fake.log.clear();
    r.fake.settings["binds.toggle"] = "H";  // changed in the Mods tab
    g_keys[VK_F7] = true;
    host.tick(kClient);
    CHECK_FALSE(logged("toggled"));
    g_keys['H'] = true;
    host.tick(kClient);
    CHECK(logged("toggled"));
    g_keys['H'] = false;
    host.tick(kClient);
    r.fake.log.clear();
    r.fake.settings["binds.toggle"] = "";  // cleared: no key
    g_keys['H'] = true;
    host.tick(kClient);
    CHECK_FALSE(logged("toggled"));
}

TEST_CASE("lua host: a key held while the mod loads is not a press; an unknown key is an error naming it") {
    Rig r(L"on_key_held");
    g_keys.clear();
    g_keys[VK_F7] = true;
    add_script(r.dir, "held", "emf.on_key('F7', function() emf.log('pressed') end)\n");
    add_script(r.dir, "typo", "emf.on_key('Mouse9', function() end)\n");
    Host host(&r.api, r.dir);
    host.set_key_reader(fake_key);
    host.load_all();
    host.tick(kClient);
    CHECK_FALSE(logged("pressed"));
    CHECK(host.status("typo").find("Mouse9 is not a key or one of your keybind settings") != std::string::npos);
    CHECK(host.status("held") == "running");
}

TEST_CASE("lua host: a mod.json saved with a mistake while the game runs pauses the mod with that mistake; fixed, it runs") {
    Rig r(L"bad_manifest");
    add_script(r.dir, "edited", "emf.log('running')\n");
    Host host(&r.api, r.dir);
    host.load_all();
    REQUIRE(host.status("edited") == "running");
    const std::wstring manifest = r.dir + L"\\edited\\mod.json";
    write(manifest, R"({"id": "edited", "version": "1", "main": "main.lua", "settings": [{"key": "a" "type": "bool"}]})");
    // a later write time, so the reload check sees the change
    HANDLE h = CreateFileW(manifest.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ft.dwHighDateTime += 1;
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
    host.check_reload_now();
    CHECK(host.status("edited").rfind("paused: mod.json: mod.json is not valid JSON", 0) == 0);
    CHECK(host.status("edited").find("line 1") != std::string::npos);
    REQUIRE_FALSE(r.fake.notes.empty());
    CHECK(r.fake.notes.back().find("Lua mod edited: mod.json:") == 0);
    write(manifest, R"({"id": "edited", "version": "1", "main": "main.lua"})");
    h = CreateFileW(manifest.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    ft.dwHighDateTime += 1;
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
    host.check_reload_now();
    CHECK(host.status("edited") == "running");
}

TEST_CASE("lua host: the settings example in lua.md runs as written") {
    std::ifstream f(EMF_SOURCE_DIR "/docs/modding/lua.md", std::ios::binary);
    const std::string doc((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const size_t at = doc.find("```lua\n-- mod.json: {\"key\": \"toggle_key\"");
    const size_t at_crlf = doc.find("```lua\r\n-- mod.json: {\"key\": \"toggle_key\"");
    const size_t start = at != std::string::npos ? at : at_crlf;
    REQUIRE(start != std::string::npos);
    const size_t body = doc.find('\n', start) + 1;
    const std::string code = doc.substr(body, doc.find("```", body) - body);
    Rig r(L"lua_md_settings");
    g_keys.clear();
    add_script(r.dir, "doc", code,
               R"([{"key": "toggle_key", "type": "keybind", "default": "F7", "label": "Toggle", "live": true},
                  {"key": "tint", "type": "color", "default": "#ffd070", "live": true}])");
    r.fake.settings = {{"doc.toggle_key", "F7"}, {"doc.tint", "#ffd070"}};
    Host host(&r.api, r.dir);
    host.set_key_reader(fake_key);
    host.load_all();
    REQUIRE(host.status("doc") == "running");
    host.tick(kClient);
    CHECK(r.fake.hud["doc.line"] == "Hello");
    CHECK(r.fake.hud_style["doc.line"] == std::make_pair(uint32_t(EMF_HUD_BOTTOM_LEFT), 0xFFD070FFu));
    g_keys[VK_F7] = true;
    host.tick(kClient);
    CHECK(r.fake.hud.count("doc.line") == 0);
}

namespace {
// Rewrites a script mod's main.lua with a later write time, so the reload check sees it.
void rewrite_main(const std::wstring& dir, const char* id, const std::string& code, int step) {
    const std::wstring path = dir + L"\\" + std::wstring(id, id + std::strlen(id)) + L"\\main.lua";
    write(path, code);
    HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ft.dwHighDateTime += static_cast<DWORD>(step);
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
}
}  // namespace

TEST_CASE("lua host: emf.on_key across a reload: the new function only, once; a reload without on_key stops the old one") {
    Rig r(L"on_key_reload");
    g_keys.clear();
    add_script(r.dir, "rl", "emf.on_key('F7', function() emf.log('v1') end)\n");
    Host host(&r.api, r.dir);
    host.set_key_reader(fake_key);
    host.load_all();
    REQUIRE(host.status("rl") == "running");
    rewrite_main(r.dir, "rl", "emf.on_key('F7', function() emf.log('v2') end)\n", 1);
    host.check_reload_now();
    REQUIRE(host.status("rl") == "running");
    g_keys[VK_F7] = true;
    host.tick(kClient);
    g_keys[VK_F7] = false;
    host.tick(kClient);
    int v1 = 0, v2 = 0;
    for (const auto& l : r.fake.log) {
        v1 += l == "[rl] v1";
        v2 += l == "[rl] v2";
    }
    CHECK(v1 == 0);  // the old function's reference went with the old state
    CHECK(v2 == 1);
    rewrite_main(r.dir, "rl", "emf.log('no keys now')\n", 2);
    host.check_reload_now();
    r.fake.log.clear();
    g_keys[VK_F7] = true;
    host.tick(kClient);
    CHECK(r.fake.log.empty());  // nothing is watched any more (and nothing stale is called)
    CHECK(host.status("rl") == "running");
}
