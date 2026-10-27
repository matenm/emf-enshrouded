// The Lua host and emf.take: a take whose script cannot hear the answer any more (it was paused by an error or the
// 250 ms budget, or reloaded, between the take and its answer) would leave the taken items nowhere; the host puts them
// back into the entity they came from (and drops what does not fit there), and logs it.
#include <windows.h>

#include <doctest.h>

#include "lua_host/host.h"
#include "lua_test_fake.h"

using emf::lua::Host;

namespace {
const char* const kTaker =
    "emf.command('take', '', function(w)\n"
    "  emf.take(w, 500, 'Wood', 5, function(w2, taken) emf.put(w2, 1, 'Wood', taken) end)\n"
    "end)\n"
    "emf.command('boom', '', function(w) error('on purpose') end)\n";
}  // namespace

TEST_CASE("lua host: a take answered after its script was paused puts the items back into where they came from") {
    Rig r(L"take_back_paused");
    add_script(r.dir, "taker", kTaker);
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("taker.take");
    REQUIRE(r.fake.takes.size() == 1);
    r.run_command("taker.boom");  // an error pauses the script before the answer comes
    REQUIRE(host.status("taker").find("paused") == 0);
    const EmfItemsRequest take = r.fake.takes[0];
    take.cb(kServer, take.entity, take.item, take.count, 5, take.user);  // 5 were taken
    REQUIRE(r.fake.puts.size() == 1);  // back into chest 500, not lost
    CHECK(r.fake.puts[0].entity == 500u);
    CHECK(r.fake.puts[0].item == 0x11u);
    CHECK(r.fake.puts[0].count == 5u);
    CHECK(logged("put back into 500"));
    // only 3 fit back: the other 2 are dropped there
    const EmfItemsRequest back = r.fake.puts[0];
    back.cb(kServer, back.entity, back.item, back.count, 3, back.user);
    REQUIRE(r.fake.gives.size() == 1);
    CHECK(r.fake.gives[0].player == 500u);
    CHECK(r.fake.gives[0].count == 2u);
}

TEST_CASE("lua host: a take answered after its script was reloaded puts them back too; a take that took nothing does nothing") {
    Rig r(L"take_back_reloaded");
    add_script(r.dir, "taker", kTaker);
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("taker.take");
    r.run_command("taker.take");
    REQUIRE(r.fake.takes.size() == 2);
    // the script is edited and reloads before the answers come
    HANDLE h = CreateFileW((r.dir + L"\\taker\\main.lua").c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ft.dwHighDateTime += 1;
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
    host.check_reload_now();
    REQUIRE(host.status("taker") == "running");
    const EmfItemsRequest a = r.fake.takes[0], b = r.fake.takes[1];
    a.cb(kServer, a.entity, a.item, a.count, 0, a.user);  // nothing was taken: nothing to give back
    CHECK(r.fake.puts.empty());
    b.cb(kServer, b.entity, b.item, b.count, 5, b.user);
    REQUIRE(r.fake.puts.size() == 1);
    CHECK(r.fake.puts[0].entity == 500u);
}

TEST_CASE("lua host: a put back refused at once drops the taken items at the entity they came from") {
    Rig r(L"take_back_refused");
    add_script(r.dir, "taker", kTaker);
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("taker.take");
    r.run_command("taker.boom");
    g_put_ok = false;
    const EmfItemsRequest c = r.fake.takes[0];
    c.cb(kServer, c.entity, c.item, c.count, 5, c.user);
    g_put_ok = true;
    REQUIRE(r.fake.gives.size() == 1);
    CHECK(r.fake.gives[0].player == 500u);
    CHECK(r.fake.gives[0].count == 5u);
    CHECK(logged("dropped at 500"));
}

TEST_CASE("lua host: a take whose script hears the answer is the script's business (nothing put back)") {
    Rig r(L"take_back_normal");
    add_script(r.dir, "taker", kTaker);
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("taker.take");
    const EmfItemsRequest take = r.fake.takes[0];
    take.cb(kServer, take.entity, take.item, take.count, 5, take.user);
    REQUIRE(r.fake.puts.size() == 1);
    CHECK(r.fake.puts[0].entity == 1u);  // the script's own put into the player
}
