// Lua 1.14: E prompts (API v25) from Lua: emf.offer, emf.spawn_offer, emf.offer_remove; the options reach the core,
// accepts reach fn, one script's entity is not another's, a reload removes the script's offers on the next server step.
#include <doctest.h>

#include <string>
#include <vector>

#include "lua_host/host.h"
#include "lua_test_fake.h"

using emf::lua::Host;

namespace {
const char* kStable =
    "emf.command('go', '', function(w, args)\n"
    "  local id = tonumber(args[1])\n"
    "  emf.reply(tostring(emf.offer(w, id, {verb = 'Open', sequence = 'button', range = 12, low_priority = true},\n"
    "    function(w2, player, entity) emf.log('accept ' .. player .. ' ' .. entity .. ' ' .. tostring(w2 ~= nil)) end)))\n"
    "end)\n"
    "emf.command('words', '', function(w, args)\n"
    "  emf.reply(tostring(emf.offer(w, tonumber(args[1]), {text = 'Beasts'}, function() end)))\n"
    "end)\n"
    "emf.command('spawn', '', function(w)\n"
    "  emf.reply(tostring(emf.spawn_offer(w, '503fa1d1-dce2-45a1-85a5-185a22666d33', 1, 2, 3, {verb = 0x0C2E4515})))\n"
    "end)\n"
    "emf.command('off', '', function(w, args) emf.reply(tostring(emf.offer_remove(w, tonumber(args[1])))) end)\n"
    "emf.command('chest', '', function(w, args)\n"
    "  emf.reply(tostring(emf.offer(w, tonumber(args[1]), {verb = 'Open', sequence = 'chest'}, function() end)))\n"
    "  local ok, e = pcall(emf.offer, w, 5002, {sequence = 'trade'})\n"
    "  emf.reply(tostring(ok) .. ' ' .. tostring(e))\n"
    "end)\n"
    "emf.command('bad', '', function(w)\n"
    "  local ok, e = pcall(emf.offer, w, 5000, {verb = 'Dance'})\n"
    "  local ok2, e2 = pcall(emf.offer, w, 5000, {nosuch = 1})\n"
    "  emf.reply(tostring(ok) .. ' ' .. tostring(e) .. ' | ' .. tostring(ok2) .. ' ' .. tostring(e2))\n"
    "end)\n";
}  // namespace

TEST_CASE("lua 1.14: emf.offer passes the verb, sequence, range and flags; an accept calls fn with the player and entity") {
    Rig r(L"luaoffer");
    with_offers(r.api);
    add_script(r.dir, "bm", kStable);
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("bm.go 5000");
    REQUIRE(r.fake.replies.size() == 1);
    CHECK(r.fake.replies[0] == "true");
    REQUIRE(r.fake.offers.size() == 1u);
    const Fake::Off& o = r.fake.offers[0];
    CHECK(o.entity == 5000u);
    CHECK(o.o.size == sizeof(EmfOffer));
    CHECK(o.o.verb == EMF_VERB_OPEN);
    CHECK(o.o.sequence == EMF_SEQUENCE_BUTTON);
    CHECK(o.o.range_m == 12);
    CHECK(o.o.flags == EMF_OFFER_LOW_PRIORITY);
    CHECK(o.text.empty());
    REQUIRE(o.cb);
    o.cb(kServer, 1, 5000, o.user);
    CHECK(logged("accept 1 5000 true"));

    r.run_command("bm.chest 5003");  // 1.42: the game's storage screen of the entity's own inventory
    REQUIRE(r.fake.offers.size() == 2u);
    CHECK(r.fake.offers[1].o.sequence == EMF_SEQUENCE_CHEST);
    CHECK(EMF_SEQUENCE_CHEST == 0x3E49C160u);
    CHECK(r.fake.replies.back().find("sequence is \"touch\", \"button\" or \"chest\"") != std::string::npos);  // any other: refused
    r.fake.offers.pop_back();

    r.run_command("bm.words 5001");
    REQUIRE(r.fake.offers.size() == 2u);
    CHECK(r.fake.offers[1].text == "Beasts");
    r.run_command("bm.spawn");
    CHECK(r.fake.replies.back() == "9000");
    CHECK(r.fake.offers.back().o.verb == EMF_VERB_OPEN);

    r.run_command("bm.bad");
    const std::string bad = r.fake.replies.back();
    CHECK(bad.find("false") == 0);
    CHECK(bad.find("Dance") != std::string::npos);
    CHECK(bad.find("nosuch") != std::string::npos);
}

TEST_CASE("lua 1.14: one script's offered entity is not another's; offer_remove takes only its own") {
    Rig r(L"luaoffer2");
    with_offers(r.api);
    add_script(r.dir, "aa", kStable);
    add_script(r.dir, "bb", kStable);
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("aa.go 5000");
    r.run_command("bb.go 5000");
    REQUIRE(r.fake.replies.size() == 2);
    CHECK(r.fake.replies[0] == "true");
    CHECK(r.fake.replies[1] == "false");  // aa's
    CHECK(r.fake.offers.size() == 1u);
    r.run_command("bb.off 5000");
    CHECK(r.fake.replies.back() == "false");
    CHECK(r.fake.offers_removed.empty());
    r.run_command("aa.off 5000");
    CHECK(r.fake.replies.back() == "true");
    CHECK(r.fake.offers_removed == std::vector<uint32_t>{5000});
    r.run_command("bb.go 5000");  // free again
    CHECK(r.fake.replies.back() == "true");
}

TEST_CASE("lua 1.14: a reload removes the script's offers on the next server step; their late accepts reach nobody") {
    Rig r(L"luaoffer3");
    with_offers(r.api);
    add_script(r.dir, "rl", kStable);
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("rl.go 5000");
    REQUIRE(r.fake.offers.size() == 1u);
    const Fake::Off o = r.fake.offers[0];
    Sleep(20);  // a newer modification time than the load saw
    write(r.dir + L"\\rl\\main.lua", "emf.log('reloaded')\n");
    host.check_reload_now();
    CHECK(r.fake.offers_removed.empty());  // not from the reload: the removal needs the server world
    o.cb(kServer, 1, 5000, o.user);         // an accept in between
    CHECK_FALSE(logged("accept 1 5000"));
    host.tick(kServer);
    CHECK(r.fake.offers_removed == std::vector<uint32_t>{5000});
}

TEST_CASE("lua 1.14: on a core without offers emf.offer answers false") {
    Rig r(L"luaoffer4");  // a v24 core
    add_script(r.dir, "old", kStable);
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("old.go 5000");
    REQUIRE(r.fake.replies.size() == 1);
    CHECK(r.fake.replies[0] == "false");
}

TEST_CASE("lua 1.14: only the touch and button sequences; a slot the core had dropped is freed by offer_remove") {
    Rig r(L"luaoffer5");
    with_offers(r.api);
    add_script(r.dir, "aa", kStable);
    add_script(r.dir, "bb", kStable);
    add_script(r.dir, "sq", "emf.command('go', '', function(w)\n"
                            "  local ok, e = pcall(emf.offer, w, 5000, {sequence = 0x79CB89E4})\n"
                            "  emf.reply(tostring(ok) .. ' ' .. tostring(e))\n"
                            "end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("sq.go");
    REQUIRE(r.fake.replies.size() == 1);
    CHECK(r.fake.replies[0].find("false") == 0);
    CHECK(r.fake.replies[0].find("touch") != std::string::npos);
    CHECK(r.fake.offers.empty());
    r.run_command("aa.go 5000");
    CHECK(r.fake.replies.back() == "true");
    r.fake.offer_remove_ok = false;  // the entity went: the core dropped the offer
    r.run_command("aa.off 5000");
    CHECK(r.fake.replies.back() == "false");
    r.run_command("bb.go 5000");  // the slot is gone: another script may offer there now
    CHECK(r.fake.replies.back() == "true");
}

TEST_CASE("lua 1.15: emf.offered is true while the script's own offer is in EMF's hands") {
    Rig r(L"luaoffered");
    with_offers(r.api);
    add_script(r.dir, "bm", std::string(kStable) +
        "emf.command('has', '', function(w, args) emf.reply(tostring(emf.offered(w, tonumber(args[1])))) end)\n");
    add_script(r.dir, "xx", "emf.command('has', '', function(w, args) emf.reply(tostring(emf.offered(w, tonumber(args[1])))) end)\n");
    Host host(&r.api, r.dir);
    host.load_all();
    r.run_command("bm.has 5000");
    CHECK(r.fake.replies.back() == "false");  // not offered
    r.run_command("bm.go 5000");
    r.run_command("bm.has 5000");
    CHECK(r.fake.replies.back() == "true");
    r.run_command("xx.has 5000");
    CHECK(r.fake.replies.back() == "false");  // another script's
    r.fake.offer_active = false;  // the core dropped it
    r.run_command("bm.has 5000");
    CHECK(r.fake.replies.back() == "false");
    r.fake.offer_active = true;
    r.run_command("bm.off 5000");
    r.run_command("bm.has 5000");
    CHECK(r.fake.replies.back() == "false");
}
