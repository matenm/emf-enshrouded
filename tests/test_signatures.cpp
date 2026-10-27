#include <doctest.h>
#include <cstring>
#include <set>
#include <string>
#include "common/signatures.h"
#include "common/sigscan.h"

using namespace emf;

namespace {
const SigDef kDef{Sig::SimStep, "Test", "48 8B ?? 05", {0x1001, 0x1003, 0x1005}, true};
}

TEST_CASE("resolve_one: a single match at the expected RVA is usable") {
    const uint8_t text[] = {0x90, 0x48, 0x8B, 0x11, 0x05, 0x90};
    // module at 0x0, .text at 0x1000 -> match at RVA 0x1001
    Resolved r = resolve_one(text, 0x1000, 0x0, kDef, 0);
    CHECK(r.count == 1);
    CHECK(r.addr == 0x1001);
    CHECK(r.rva_matches);
    CHECK(is_usable(r, true));
}

TEST_CASE("resolve_one: the expected RVA is the detected build's own") {
    const uint8_t text[] = {0x90, 0x90, 0x90, 0x48, 0x8B, 0x11, 0x05};  // match at RVA 0x1003
    CHECK(resolve_one(text, 0x1000, 0x0, kDef, 1).rva_matches);         // build 1 expects 0x1003
    CHECK_FALSE(resolve_one(text, 0x1000, 0x0, kDef, 0).rva_matches);   // build 0 expects 0x1001
    CHECK_FALSE(resolve_one(text, 0x1000, 0x0, kDef, -1).rva_matches);  // unknown build: no expected RVA
    CHECK(is_usable(resolve_one(text, 0x1000, 0x0, kDef, -1), false));  // ...but a unique match is still usable
}

TEST_CASE("known builds: client 1269579 and 1273043, and the dedicated server of 1273043, found by sha256") {
    auto builds = known_builds();
    REQUIRE(builds.size() == kKnownBuildCount);
    REQUIRE(kKnownBuildCount == 3);
    CHECK(builds[0].number == 1269579u);
    CHECK_FALSE(builds[0].server);
    CHECK(builds[1].number == 1273043u);
    CHECK_FALSE(builds[1].server);
    CHECK(builds[2].number == 1273043u);
    CHECK(builds[2].server);
    CHECK(find_known_build("82e5020a8736348cd0374f004b2f5e5ef456b5bd27aa5420df34c7dd241cfeeb") == 0);
    CHECK(find_known_build("d64966e23b1a458ee6f147bcbceac0b577fa7e39a212f371287d8cc119817882") == 1);
    CHECK(find_known_build("d7baa768eb00aa895d9f5a1cd8747ccf66858c7b542f50983092b2e3261588ee") == 2);
    CHECK(find_known_build("") == -1);
    CHECK(find_known_build("D64966E23B1A458EE6F147BCBCEAC0B577FA7E39A212F371287D8CC119817882") == 1);  // any case
    CHECK(find_known_build("0000000000000000000000000000000000000000000000000000000000000000") == -1);
    CHECK(known_build_index(1273043) == 1);  // the game (client) exe
    CHECK(known_build_index(1273043, true) == 2);
    CHECK(known_build_index(1269579, true) == -1);
    CHECK(known_build_index(1) == -1);
}

TEST_CASE("absent_on: an RVA of 0 marks a function the exe does not have (client-only code on the server)") {
    const SigDef* chat = find_signature("QueueChatMessage");
    REQUIRE(chat != nullptr);
    CHECK(absent_on(*chat, 2));
    CHECK_FALSE(absent_on(*chat, 1));
    CHECK_FALSE(absent_on(*chat, -1));  // unknown build: nothing is known to be absent
    CHECK(absent_on(*find_signature("ClientWorldSetup"), 2));
    CHECK_FALSE(absent_on(*find_signature("SimStep"), 2));
}

TEST_CASE("report_signature: on an unknown server build, client-only code that is not found is absent, not unusable") {
    const SigDef& chat = *find_signature("QueueChatMessage");  // absent on the known server build
    const Resolved none{};
    auto r = report_signature(chat, none, -1, true);
    CHECK_FALSE(r.usable);
    CHECK(r.absent);
    CHECK(r.note == "signature QueueChatMessage not found (client-only code is absent from the server)");
    r = report_signature(chat, none, -1, false);  // the game exe of an unknown build: its chat box code is missing
    CHECK_FALSE(r.absent);
    CHECK(r.note == "signature QueueChatMessage unusable (matches=0)");
    r = report_signature(*find_signature("SimStep"), none, -1, true);  // server code missing: a real problem
    CHECK_FALSE(r.absent);
    CHECK(r.note == "signature SimStep unusable (matches=0)");
    const Resolved twice{0x1000, 2, false};  // matched twice: ambiguous, not absent
    r = report_signature(chat, twice, -1, true);
    CHECK_FALSE(r.absent);
    CHECK(r.note == "signature QueueChatMessage unusable (matches=2)");
    r = report_signature(chat, none, 2, true);  // the known server build: counted, no line of its own (as before)
    CHECK(r.absent);
    CHECK(r.note.empty());
    const Resolved once{0x1000, 1, false};
    r = report_signature(*find_signature("SimStep"), once, -1, true);
    CHECK(r.usable);
    CHECK_FALSE(r.absent);
    CHECK(r.note.empty());
}

TEST_CASE("resolve_one: a duplicated pattern is never usable") {
    const uint8_t text[] = {0x48, 0x8B, 0x00, 0x05, 0x48, 0x8B, 0x7F, 0x05};
    Resolved r = resolve_one(text, 0x1000, 0x0, kDef, 0);
    CHECK(r.count == 2);
    CHECK_FALSE(is_usable(r, true));
    CHECK_FALSE(is_usable(r, false));
}

TEST_CASE("resolve_one: a single match at a different RVA is refused on the known build only") {
    const uint8_t text[] = {0x90, 0x90, 0x90, 0x48, 0x8B, 0x11, 0x05};
    Resolved r = resolve_one(text, 0x1000, 0x0, kDef, 0);
    CHECK(r.count == 1);
    CHECK_FALSE(r.rva_matches);
    CHECK_FALSE(is_usable(r, true));   // exe hash matches a known build but code moved: something is wrong
    CHECK(is_usable(r, false));        // unknown build: trust the unique match
}

TEST_CASE("resolve_one: no match is not usable") {
    const uint8_t text[] = {0x90, 0x90};
    CHECK_FALSE(is_usable(resolve_one(text, 0x1000, 0x0, kDef, -1), false));
}

TEST_CASE("signature table: one entry per Sig, in order, unique names, valid patterns") {
    auto all = all_signatures();
    REQUIRE(all.size() == kSigCount);
    std::set<std::string> names;
    for (size_t i = 0; i < all.size(); ++i) {
        CHECK(static_cast<size_t>(all[i].id) == i);
        CHECK(names.insert(all[i].name).second);
        CHECK(parse_pattern(all[i].pattern).bytes.size() >= 12);
        // every signature has an RVA on every client build; on the server only the client-only ones are absent
        for (size_t b = 0; b < kKnownBuildCount; ++b) {
            CAPTURE(all[i].name);
            const bool client_only = all[i].id == Sig::ClientWorldSetup || all[i].id == Sig::QueueChatMessage ||
                                     (all[i].id >= Sig::UiFrame && all[i].id <= Sig::CharacterSelected);
            CHECK((all[i].rva[b] == 0) == (known_builds()[b].server && client_only));
        }
    }
    REQUIRE(find_signature("SimStep") != nullptr);
    CHECK(find_signature("SimStep")->id == Sig::SimStep);
    CHECK(find_signature("NoSuchThing") == nullptr);
}

TEST_CASE("required_signatures_ok: needs every required signature usable, ignores optional ones") {
    std::array<Resolved, kSigCount> res{};
    auto defs = all_signatures();
    for (size_t i = 0; i < kSigCount; ++i) res[i] = Resolved{0x1000 + i, 1, true};
    CHECK(required_signatures_ok(res, true));
    // an optional signature failing does not block startup
    size_t optional_index = 0;
    while (defs[optional_index].required) ++optional_index;
    res[optional_index].count = 0;
    CHECK(required_signatures_ok(res, true));
    // a required one failing does
    res[static_cast<size_t>(Sig::SimStep)].count = 2;
    CHECK_FALSE(required_signatures_ok(res, true));
}

TEST_CASE("signature table has MarkWritten (component change tracking), optional") {
    const SigDef* d = find_signature("MarkWritten");
    REQUIRE(d != nullptr);
    CHECK(d->rva[0] == 0x9c5df0u);
    CHECK(d->rva[1] == 0x9c75e0u);
    CHECK(d->rva[2] == 0x667ec0u);  // dedicated server
    CHECK_FALSE(d->required);
}

TEST_CASE("signature table has EmitEvent (send an ECS event from a program job), optional") {
    const SigDef* d = find_signature("EmitEvent");
    REQUIRE(d != nullptr);
    CHECK(d->rva[0] == 0x955400u);
    CHECK(d->rva[1] == 0x956670u);
    CHECK_FALSE(d->required);
}

TEST_CASE("signature table has GetTemplateComponent (an entity's template data), optional") {
    const SigDef* d = find_signature("GetTemplateComponent");
    REQUIRE(d != nullptr);
    CHECK(d->rva[0] == 0x9a6f00u);
    CHECK(d->rva[1] == 0x9a82e0u);
    CHECK_FALSE(d->required);
}

TEST_CASE("signature table has QueueChatMessage (the chat box sending a typed line), optional") {
    const SigDef* d = find_signature("QueueChatMessage");
    REQUIRE(d != nullptr);
    CHECK(d->rva[0] == 0xd5ce70u);
    CHECK(d->rva[1] == 0xd5e8f0u);
    CHECK_FALSE(d->required);
}

TEST_CASE("signature table has the inventory transaction and readOther, optional, on both builds") {
    struct Want {
        const char* name;
        uint32_t rva_1269579, rva_1273043;
    } const want[] = {
        {"InventoryTxCtor", 0x21dd00u, 0x21dea0u},     {"InventoryTxInit", 0x411df0u, 0x412170u},
        {"InventoryAddItems", 0x40b170u, 0x40b4f0u},   {"InventoryCommit", 0x408b50u, 0x408ed0u},
        {"InventoryRollback", 0x410e60u, 0x4111e0u},   {"InventoryFreeArrays", 0x21de50u, 0x21dff0u},
        {"ReadOther", 0x9a5e20u, 0x9a7200u},
    };
    for (const auto& w : want) {
        CAPTURE(w.name);
        const SigDef* d = find_signature(w.name);
        REQUIRE(d != nullptr);
        CHECK(d->rva[0] == w.rva_1269579);
        CHECK(d->rva[1] == w.rva_1273043);
        CHECK_FALSE(d->required);
    }
    CHECK(kSigCount == 50);
}

TEST_CASE("signature table has requestServerSave (API v19 request_save), on every exe") {
    const SigDef* d = find_signature("RequestServerSave");
    REQUIRE(d != nullptr);
    CHECK(d->id == Sig::RequestServerSave);
    CHECK(d->rva[0] == 0xe438f0u);  // re_saves.md, build 1269579
    CHECK(d->rva[1] == 0xe463e0u);  // 1273043
    CHECK(d->rva[2] == 0x7afea0u);  // its dedicated server
    CHECK(d->call_at == -1);
    CHECK_FALSE(d->required);
}

TEST_CASE("signature table has the server's chat receive loop and chatLogAdd (players' chat commands), on every exe") {
    struct Want {
        const char* name;
        uint32_t rva_1269579, rva_1273043, rva_server;
    } const want[] = {
        {"ChatReceive", 0xe47c20u, 0xe4a710u, 0x7b45b0u},
        {"ChatLogAdd", 0xe546e0u, 0xe572a0u, 0x90b330u},
    };
    for (const auto& w : want) {
        CAPTURE(w.name);
        const SigDef* d = find_signature(w.name);
        REQUIRE(d != nullptr);
        CHECK(d->rva[0] == w.rva_1269579);
        CHECK(d->rva[1] == w.rva_1273043);
        CHECK(d->rva[2] == w.rva_server);
        CHECK_FALSE(d->required);
    }
}

TEST_CASE("signature table has WriteOther (another entity's component, marked for the players and the save)") {
    const SigDef* d = find_signature("WriteOther");
    REQUIRE(d != nullptr);
    CHECK(d->rva[1] == 0x9a6fe0u);  // 1273043
    CHECK(d->rva[2] == 0x647860u);  // its dedicated server
    CHECK(d->rva[1] - find_signature("ReadOther")->rva[1] == static_cast<uint32_t>(-0x220));  // right before readOther
    CHECK_FALSE(d->required);
}

// ---- attributes, damage, the main menu (docs/engine_parts/re_attributes_menu.md) ----

TEST_CASE("resolve_one with call_at: the function is the target of the call at match + call_at (a body with a twin)") {
    // text at 0x1000: 90 | AA BB E8 <rel32> | 90 ...; the call at 0x1003 targets 0x1003 + 5 + 0x20 = 0x1028
    uint8_t text[0x40] = {};
    const uint8_t site[] = {0x90, 0xAA, 0xBB, 0xE8, 0x20, 0x00, 0x00, 0x00, 0x90};
    std::memcpy(text, site, sizeof site);
    SigDef def{Sig::SimStep, "Call", "AA BB E8 ?? ?? ?? ??", {0x1028, 0x1028, 0}, false, 2};
    Resolved r = resolve_one(text, 0x1000, 0x0, def, 0);
    CHECK(r.count == 1);
    CHECK(r.addr == 0x1028);  // the callee, not the match
    CHECK(r.rva_matches);
    CHECK(is_usable(r, true));
    def.rva = {0x1001, 0x1001, 0};  // the call site's own RVA is not the function's
    CHECK_FALSE(resolve_one(text, 0x1000, 0x0, def, 0).rva_matches);
    // no E8 at call_at: not a call, never usable
    SigDef bad{Sig::SimStep, "NotACall", "AA BB E8 ?? ?? ?? ??", {0x1028, 0x1028, 0}, false, 1};
    Resolved b = resolve_one(text, 0x1000, 0x0, bad, -1);
    CHECK(b.addr == 0);
    CHECK_FALSE(is_usable(b, false));
}

TEST_CASE("kRvaUnchecked: a known build whose exe was not at hand accepts a single match anywhere, not two") {
    const uint8_t text[] = {0x90, 0x90, 0x90, 0x48, 0x8B, 0x11, 0x05};
    const SigDef def{Sig::SimStep, "Old", "48 8B ?? 05", {kRvaUnchecked, 0x1003, 0}, false};
    CHECK(resolve_one(text, 0x1000, 0x0, def, 0).rva_matches);  // 1269579: not checked
    CHECK(is_usable(resolve_one(text, 0x1000, 0x0, def, 0), true));
    CHECK_FALSE(absent_on(def, 0));
    const uint8_t twice[] = {0x48, 0x8B, 0x00, 0x05, 0x48, 0x8B, 0x7F, 0x05};
    CHECK_FALSE(is_usable(resolve_one(twice, 0x1000, 0x0, def, 0), true));
}

TEST_CASE("signature table has attrFind, attrAdd (through its unique call site) and allocPendingHit, on every exe") {
    struct Want {
        const char* name;
        uint32_t rva_1273043, rva_server;
        int call_at;
    } const want[] = {
        {"AttrFind", 0x121e150u, 0x986f90u, -1},
        {"AttrAdd", 0x3e8720u, 0x18b120u, 0x11},  // the call in applyHitToEntity's Health block; the body has a twin
        {"AllocPendingHit", 0x1218bd0u, 0x9819c0u, -1},
    };
    for (const auto& w : want) {
        CAPTURE(w.name);
        const SigDef* d = find_signature(w.name);
        REQUIRE(d != nullptr);
        CHECK(d->rva[0] == kRvaUnchecked);  // the 1269579 exe was not at hand
        CHECK(d->rva[1] == w.rva_1273043);
        CHECK(d->rva[2] == w.rva_server);
        CHECK(d->call_at == w.call_at);
        CHECK_FALSE(d->required);
    }
}

TEST_CASE("signature table has the main menu's UI frame, state handler, command posters and the character check (client only)") {
    struct Want {
        const char* name;
        uint32_t rva_1273043;
    } const want[] = {
        {"UiFrame", 0xd8cd40u},          {"PregameState", 0x868310u},    {"PostSelectCharacter", 0xd5e310u},
        {"PostSelectWorld", 0xd5e610u},  {"PostSessionArea", 0xd5e450u}, {"PostStart", 0xd5bc70u},
        {"CharacterSelected", 0xdd45b0u},
    };
    for (const auto& w : want) {
        CAPTURE(w.name);
        const SigDef* d = find_signature(w.name);
        REQUIRE(d != nullptr);
        CHECK(d->rva[0] == kRvaUnchecked);
        CHECK(d->rva[1] == w.rva_1273043);
        CHECK(d->rva[2] == 0u);  // the dedicated server has no menu
        CHECK(absent_on(*d, 2));
        CHECK_FALSE(d->required);
    }
}

TEST_CASE("signature table has WaitWorldJobs (the game's wait for a world's own jobs), optional, on every exe") {
    const SigDef* d = find_signature("WaitWorldJobs");
    REQUIRE(d != nullptr);
    CHECK(d->id == Sig::WaitWorldJobs);
    CHECK(d->rva[0] == 0xa1f440u);  // 1269579: inferred (SimStep + 0x1160, as on 1273043)
    CHECK(d->rva[1] == 0xa20f70u);  // 1273043, called once, from GameServer update (+0xe49b52)
    CHECK(d->rva[2] == 0x6c2120u);  // its dedicated server, called once (+0x7b39c7)
    CHECK(d->rva[1] - find_signature("SimStep")->rva[1] == 0x1160u);
    CHECK(d->rva[0] - find_signature("SimStep")->rva[0] == 0x1160u);
    CHECK_FALSE(d->required);  // without it EMF runs as before and says so
    // mov rcx, [rcx+0xE15D60] (the world's task group); jmp TaskGroupWait: matched once, at that RVA
    const uint8_t text[] = {0xCC, 0x48, 0x8B, 0x89, 0x60, 0x5D, 0xE1, 0x00, 0xE9, 0xAB, 0x4E, 0xEC, 0xFF, 0xCC};
    const Resolved r = resolve_one(text, 0xa20f6f, 0x0, *d, 1);
    CHECK(r.count == 1);
    CHECK(r.addr == 0xa20f70);
    CHECK(is_usable(r, true));
    const uint8_t other_disp[] = {0x48, 0x8B, 0x89, 0x68, 0x5D, 0xE1, 0x00, 0xE9, 0x00, 0x00, 0x00, 0x00};
    CHECK(resolve_one(other_disp, 0x1000, 0x0, *d, -1).count == 0);  // another field: not the world's task group
}

TEST_CASE("resolve_one with call_at: a target outside .text, or not matching the function's own pattern, is not usable") {
    // text at 0x1000: AA BB E8 -> 0x1010, where the body starts with C3 C3 (pattern "C3 C3")
    uint8_t text[0x20] = {};
    const uint8_t site[] = {0xAA, 0xBB, 0xE8, 0x09, 0x00, 0x00, 0x00};  // 0x1002 + 5 + 0x09 = 0x1010
    std::memcpy(text, site, sizeof site);
    text[0x10] = 0xC3;
    text[0x11] = 0xC3;
    SigDef def{Sig::SimStep, "Call", "AA BB E8 ?? ?? ?? ??", {0x1010, 0x1010, 0}, false, 2, "C3 C3"};
    CHECK(is_usable(resolve_one(text, 0x1000, 0x0, def, 0), true));
    text[0x11] = 0x90;  // another function there: the call does not reach the body the RE named
    CHECK_FALSE(is_usable(resolve_one(text, 0x1000, 0x0, def, 0), true));
    CHECK_FALSE(is_usable(resolve_one(text, 0x1000, 0x0, def, -1), false));
    text[0x11] = 0xC3;
    const uint8_t far_site[] = {0xAA, 0xBB, 0xE8, 0x00, 0x10, 0x00, 0x00};  // far outside the section
    std::memcpy(text, far_site, sizeof far_site);
    CHECK_FALSE(is_usable(resolve_one(text, 0x1000, 0x0, def, -1), false));
    CHECK(find_signature("AttrAdd")->call_target_pattern != nullptr);  // attrAdd's body (both twins match it)
}
