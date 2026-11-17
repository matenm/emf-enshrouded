#include <doctest.h>
#include <cstring>
#include <set>
#include <string>
#include <vector>
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

TEST_CASE("known builds: client 1269579, 1273043, 1283531, 1285601 and 1287369, and the dedicated servers of 1273043, 1283531, 1285601 and 1287369, by sha256") {
    auto builds = known_builds();
    REQUIRE(builds.size() == kKnownBuildCount);
    REQUIRE(kKnownBuildCount == 9);
    CHECK(builds[0].number == 1269579u);
    CHECK_FALSE(builds[0].server);
    CHECK(builds[1].number == 1273043u);
    CHECK_FALSE(builds[1].server);
    CHECK(builds[2].number == 1273043u);
    CHECK(builds[2].server);
    CHECK(builds[3].number == 1283531u);  // the update of 2026-11-02 (game git 2c08d7bbd8f4)
    CHECK_FALSE(builds[3].server);
    CHECK(builds[4].number == 1283531u);
    CHECK(builds[4].server);
    CHECK(builds[5].number == 1285601u);  // the hotfix of 2026-11-03
    CHECK_FALSE(builds[5].server);
    CHECK(builds[6].number == 1285601u);
    CHECK(builds[6].server);
    CHECK(builds[7].number == 1287369u);  // the update of 2026-11-04
    CHECK_FALSE(builds[7].server);
    CHECK(builds[8].number == 1287369u);
    CHECK(builds[8].server);
    CHECK(find_known_build("82e5020a8736348cd0374f004b2f5e5ef456b5bd27aa5420df34c7dd241cfeeb") == 0);
    CHECK(find_known_build("d64966e23b1a458ee6f147bcbceac0b577fa7e39a212f371287d8cc119817882") == 1);
    CHECK(find_known_build("d7baa768eb00aa895d9f5a1cd8747ccf66858c7b542f50983092b2e3261588ee") == 2);
    CHECK(find_known_build("0403981fd2e8ad18bd8c5698dff03c8d505b5a46677832c496db9c645513f2a6") == 3);
    CHECK(find_known_build("4429653ac0cfa154bc08e138853caa5d8557f82129b7488031f40345816aa6dc") == 4);
    CHECK(find_known_build("ce6fdba63e27744943b695921383a29133285d359e07671f6ca33a47e2147618") == 5);
    CHECK(find_known_build("c5c9fe50c6398d2adedd98923c2d04bf9d9b4af64c1447ac167cdf8f874ff84f") == 6);
    CHECK(find_known_build("5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77") == 7);
    CHECK(find_known_build("f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2") == 8);
    CHECK(find_known_build("") == -1);
    CHECK(find_known_build("D64966E23B1A458EE6F147BCBCEAC0B577FA7E39A212F371287D8CC119817882") == 1);  // any case
    CHECK(find_known_build("0000000000000000000000000000000000000000000000000000000000000000") == -1);
    CHECK(known_build_index(1273043) == 1);  // the game (client) exe
    CHECK(known_build_index(1273043, true) == 2);
    CHECK(known_build_index(1283531) == 3);
    CHECK(known_build_index(1283531, true) == 4);
    CHECK(known_build_index(1285601) == 5);
    CHECK(known_build_index(1285601, true) == 6);
    CHECK(known_build_index(1287369) == 7);
    CHECK(known_build_index(1287369, true) == 8);
    CHECK(known_build_index(1269579, true) == -1);
    CHECK(known_build_index(1) == -1);
}

TEST_CASE("signature table: build 1283531's RVAs, game and dedicated server, as emf_sigcheck found them") {
    struct Want {
        const char* name;
        uint32_t client, server;
    } const want[] = {
        {"SimStep", 0xa2c5a0u, 0x6c8f10u},          {"GetComponent", 0x9b2e00u, 0x64ed50u},
        {"GetEntityTransform", 0x9b3df0u, 0x64fd40u}, {"GetTemplateComponent", 0x9b3b90u, 0x64fae0u},
        {"SpawnWrapper", 0x984db0u, 0x620d00u},     {"WaitWorldJobs", 0xa2d710u, 0x6ca080u},
        {"IterNext", 0x9c26c0u, 0x65e610u},         {"CurrentEntityId", 0x9bf920u, 0x65b870u},
        {"AllocPendingHit", 0x1235e70u, 0x98bfb0u}, {"AttrAdd", 0x3ef490u, 0x190810u},
        {"PostSelectCharacter", 0xd726a0u, 0u},     {"PostSelectWorld", 0xd729d0u, 0u},
        {"PostSessionArea", 0xd727f0u, 0u},         {"PostStart", 0xd6fe60u, 0u},
        {"QueuePush", 0x95e350u, 0x5f9f60u},        {"SendServerInfo", 0xe65fa0u, 0x7c2610u},
    };
    const int client = known_build_index(1283531), server = known_build_index(1283531, true);
    REQUIRE(client >= 0);
    REQUIRE(server >= 0);
    for (const auto& w : want) {
        const std::string name = w.name;
        CAPTURE(name);
        const SigDef* d = find_signature(w.name);
        REQUIRE(d != nullptr);
        CHECK(d->rva[static_cast<size_t>(client)] == w.client);
        CHECK(d->rva[static_cast<size_t>(server)] == w.server);
    }
    // every signature was checked on both exes of 1283531 (none is kRvaUnchecked there), except the HUD editor's UI
    // signatures (checked on 1287369 alone) and the two of the chat transport (API v27), added when only the exes of
    // 1273043 (their RVAs from the notes), 1285601 (client) and 1287369 were at hand
    for (const auto& d : all_signatures()) {
        if (d.id >= Sig::UiScopeOpen && d.id <= Sig::UiPopTransform) continue;
        if (d.id == Sig::ChatClientSend || d.id == Sig::ClientOutboxFlush || d.id == Sig::PostJoinPending) continue;
        if (d.id == Sig::HandleAccountAction || d.id == Sig::SessionRequestKick) continue;  // 1287369 alone
        if (d.id == Sig::PlayerVitalBar || d.id == Sig::LocaPoolLookup || d.id == Sig::LocaSlotFind) continue;  // 1287369 alone
        if (d.id == Sig::ApplyHitToEntity) continue;  // 1287369 alone (both exes)
        if (d.id == Sig::PanelLayerOpen) continue;    // 1287369 alone (client)
        if (d.id == Sig::ApplyBuffCore) continue;     // 1287369 alone (both exes)
        if (d.id >= Sig::UiCameraBlock && d.id <= Sig::MapMarkerEntryLabel) continue;  // 1287369 alone (client)
        if (d.id == Sig::VoxelRay || d.id == Sig::ResetBoundsSet || d.id == Sig::FogSample) continue;  // 1287369 alone (both exes)
        if (d.id >= Sig::MapCanvasDraw && d.id <= Sig::UiPopClip) continue;  // 1287369 alone (client)
        if (d.id >= Sig::CreatePathQuery && d.id <= Sig::PathQueryDefaults) continue;  // 1287369 alone (both exes)
        if (d.id == Sig::WaterSurfacePlane) continue;  // 1287369 alone (both exes)
        if (d.id >= Sig::PathGridRegister && d.id <= Sig::PathGridCellList) continue;  // 1287369 alone (both exes)
        if (d.id >= Sig::BlockKernel && d.id <= Sig::BlockColumnRelease) continue;  // 1287369 alone (both exes)
        if (d.id == Sig::HudScreenRead) continue;  // 1287369 alone (client)
        if (d.id == Sig::TerrainConfigFind || d.id == Sig::VoxelBoxFillAir) continue;  // 1287369 alone (both exes)
        CHECK(d.rva[static_cast<size_t>(client)] != kRvaUnchecked);
        CHECK(d.rva[static_cast<size_t>(server)] != kRvaUnchecked);
    }
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
                                     all[i].id == Sig::ClientOutboxFlush || all[i].id == Sig::PostJoinPending ||
                                     (all[i].id >= Sig::UiFrame && all[i].id <= Sig::CharacterSelected) ||
                                     (all[i].id >= Sig::UiScopeOpen && all[i].id <= Sig::UiPopTransform) ||
                                     all[i].id == Sig::PlayerVitalBar || all[i].id == Sig::LocaPoolLookup ||
                                     all[i].id == Sig::LocaSlotFind || all[i].id == Sig::PanelLayerOpen ||
                                     (all[i].id >= Sig::UiCameraBlock && all[i].id <= Sig::MapMarkerEntryLabel) ||
                                     (all[i].id >= Sig::MapCanvasDraw && all[i].id <= Sig::UiPopClip) ||
                                     all[i].id == Sig::HudScreenRead;
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
    CHECK(kSigCount == 98);
}

TEST_CASE("signature table has XpRequired and MaxPlayerLevel (API v22 levels), optional, on both exes of 1283531") {
    const SigDef* xp = find_signature("XpRequired");
    REQUIRE(xp != nullptr);
    CHECK(xp->rva[3] == 0xe97620u);  // the client
    CHECK(xp->rva[4] == 0x93e880u);  // the dedicated server
    CHECK_FALSE(xp->required);
    const SigDef* cap = find_signature("MaxPlayerLevel");
    REQUIRE(cap != nullptr);
    CHECK(cap->rva[3] == 0xe99270u);
    CHECK(cap->rva[4] == 0x9404f0u);
    CHECK_FALSE(cap->required);
}

TEST_CASE("signature table has PostJoinPending (the bridge's join), optional, the game exe of 1287369 only") {
    const SigDef* d = find_signature("PostJoinPending");
    REQUIRE(d != nullptr);
    CHECK(d->id == Sig::PostJoinPending);
    CHECK_FALSE(d->required);
    CHECK(d->rva[7] == 0xd6fa30u);  // 1287369 client
    CHECK(d->rva[8] == 0u);         // its dedicated server has no menu
    CHECK_FALSE(rva_checked(*d, 1));
    CHECK_FALSE(rva_checked(*d, 3));
    CHECK_FALSE(rva_checked(*d, 5));
}

TEST_CASE("signature table has ChatClientSend and ClientOutboxFlush (API v27 mod messages), optional") {
    const SigDef* send = find_signature("ChatClientSend");
    REQUIRE(send != nullptr);
    CHECK(send->id == Sig::ChatClientSend);
    CHECK_FALSE(send->required);
    CHECK(send->rva[1] == 0xe99f70u);  // 1273043 client
    CHECK(send->rva[2] == 0x94e320u);  // its dedicated server (the unused copy)
    CHECK(send->rva[5] == 0xeb1140u);  // 1285601 client
    CHECK(send->rva[7] == 0xeb1220u);  // 1287369 client
    CHECK(send->rva[8] == 0x958570u);  // 1287369 dedicated server
    CHECK_FALSE(rva_checked(*send, 0));  // 1269579, 1283531, 1285601 server: not checked
    CHECK_FALSE(rva_checked(*send, 3));
    CHECK(rva_checked(*send, 7));
    const SigDef* flush = find_signature("ClientOutboxFlush");
    REQUIRE(flush != nullptr);
    CHECK(flush->id == Sig::ClientOutboxFlush);
    CHECK_FALSE(flush->required);
    CHECK(flush->rva[1] == 0x867710u);
    CHECK(flush->rva[5] == 0x871e10u);
    CHECK(flush->rva[7] == 0x871ef0u);
    for (const size_t server : {2u, 4u, 6u, 8u}) CHECK(absent_on(*flush, static_cast<int>(server)));  // client-only code
    CHECK_FALSE(absent_on(*flush, 7));
}

TEST_CASE("signature table has sendServerInfo (API v21 world settings: the game's resync), on both exes of 1273043") {
    const SigDef* d = find_signature("SendServerInfo");
    REQUIRE(d != nullptr);
    CHECK(d->id == Sig::SendServerInfo);
    CHECK(d->rva[0] == kRvaUnchecked);  // 1269579: not checked
    CHECK_FALSE(rva_checked(*d, 0));    // so world settings never set the resync flag there
    CHECK(rva_checked(*d, 1));
    CHECK(rva_checked(*d, 2));
    CHECK_FALSE(rva_checked(*d, -1));   // an unknown build
    CHECK(d->rva[1] == 0xe4f110u);      // GameServer_sendServerInfo (docs/modding/world_settings.md)
    CHECK(d->rva[2] == 0x7b9060u);      // its dedicated server
    CHECK_FALSE(d->required);
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
    // build 1283531 moved the task group with the rest of the world's block (+0x1C00): mov rcx, [rcx+0xE17960]
    const uint8_t moved[] = {0x48, 0x8B, 0x89, 0x60, 0x79, 0xE1, 0x00, 0xE9, 0x00, 0x00, 0x00, 0x00};
    CHECK(resolve_one(moved, 0x1000, 0x0, *d, -1).count == 1);
    // a field outside the world's 0xE1xxxx block (the singleton array): not the world's task group
    const uint8_t other_disp[] = {0x48, 0x8B, 0x89, 0xD0, 0x62, 0x74, 0x00, 0xE9, 0x00, 0x00, 0x00, 0x00};
    CHECK(resolve_one(other_disp, 0x1000, 0x0, *d, -1).count == 0);
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

namespace {
std::vector<uint8_t> hex_bytes(const char* text) {
    std::vector<uint8_t> out;
    for (const char* p = text; *p;) {
        if (*p == ' ') {
            ++p;
            continue;
        }
        out.push_back(static_cast<uint8_t>(std::stoul(std::string(p, 2), nullptr, 16)));
        p += 2;
    }
    return out;
}
size_t matches_in(const char* sig, const std::vector<uint8_t>& code) {
    std::vector<uint8_t> text(4, 0xCC);  // a little padding before and after
    text.insert(text.end(), code.begin(), code.end());
    text.insert(text.end(), 4, 0xCC);
    return resolve_one(text, 0x1000, 0x0, *find_signature(sig), -1).count;
}
}  // namespace

TEST_CASE("the signatures with a world or UI offset in their bytes match the code before and after the update of 2026-11-02") {
    // Each pair: the function's first bytes on build 1273043 and on build 1283531, where the world's block moved by
    // +0x1C00 (every ECS program entry 8 bytes longer), the iterator's entity list and count by +8, the UI's pregame
    // queue by +0x20, and allocPendingHit got a bigger frame. The offsets are wildcards; the code around them is not.
    struct Pair {
        const char* sig;
        const char* before;
        const char* after;
    } const pairs[] = {
        {"GetComponent", "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8D BA C0 5D E1 00 44 89",
         "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8D BA C0 79 E1 00 44 89"},
        {"GetEntityTransform", "48 89 5C 24 08 57 48 83 EC 20 48 8D B9 C0 5D E1 00 89 54 24 38 48 8D 8F",
         "48 89 5C 24 08 57 48 83 EC 20 48 8D B9 C0 79 E1 00 89 54 24 38 48 8D 8F"},
        {"GetTemplateComponent",
         "48 89 5C 24 08 57 48 83 EC 20 48 8D B9 C0 5D E1 00 89 54 24 38 48 8B 0F 48 8D 54 24 38 48 83 C1 30 41 8B D8 E8",
         "48 89 5C 24 08 57 48 83 EC 20 48 8D B9 C0 79 E1 00 89 54 24 38 48 8B 0F 48 8D 54 24 38 48 83 C1 30 41 8B D8 E8"},
        {"SpawnWrapper", "48 83 EC 58 0F 10 02 8B 15 11 22 33 44 33 C0 41 0F 10 08 48 81 C1 C0 5D E1 00",
         "48 83 EC 58 0F 10 02 8B 15 11 22 33 44 33 C0 41 0F 10 08 48 81 C1 C0 79 E1 00"},
        {"WaitWorldJobs", "48 8B 89 60 5D E1 00 E9 11 22 33 44", "48 8B 89 60 79 E1 00 E9 11 22 33 44"},
        {"IterNext", "40 57 48 83 EC 20 4C 8B 01 48 8B FA 8B 41 08 4C 8B C9 49 3B 80 30 08 00 00",
         "40 57 48 83 EC 20 4C 8B 01 48 8B FA 8B 41 08 4C 8B C9 49 3B 80 38 08 00 00"},
        {"CurrentEntityId",
         "8B 41 08 4C 8B C2 85 C0 75 06 89 02 48 8B C2 C3 8D 50 FF 48 8B 01 48 03 D2 48 8B 88 28 08 00 00 8B 04 D1 41",
         "8B 41 08 4C 8B C2 85 C0 75 06 89 02 48 8B C2 C3 8D 50 FF 48 8B 01 48 03 D2 48 8B 88 30 08 00 00 8B 04 D1 41"},
        {"AllocPendingHit", "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 0F B6 FA 48 81 C1 10 01 00 00",
         "48 89 5C 24 08 57 48 83 EC 40 48 8B D9 0F B6 FA 48 81 C1 10 01 00 00"},
        {"PostStart",
         "48 89 5C 24 10 57 48 83 EC 20 48 8B 81 D0 00 00 00 48 8B 98 C0 3C 03 00 4C 8D 43 18 8B 43 10 49 3B 00 75 13 33 FF "
         "48 85 FF 0F 95 C0 48 8B 5C 24 38 48 83 C4 20 5F C3 8B 53 40 8B C2 83 E0 0F 74 05 2B D0 83 C2 10 48 8B 7B 30 8D 42 "
         "01 8B CA 48 03 F9 C6 07 28",
         "48 89 5C 24 10 57 48 83 EC 20 48 8B 81 D0 00 00 00 48 8B 98 E0 3C 03 00 4C 8D 43 18 8B 43 10 49 3B 00 75 13 33 FF "
         "48 85 FF 0F 95 C0 48 8B 5C 24 38 48 83 C4 20 5F C3 8B 53 40 8B C2 83 E0 0F 74 05 2B D0 83 C2 10 48 8B 7B 30 8D 42 "
         "01 8B CA 48 03 F9 C6 07 28"},
        {"PostSessionArea",
         "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 81 D0 00 00 00 0F B6 F2 48 8B 98 C0 3C 03 00 8B 43 10 48 3B 43 "
         "18 74 7F 44 8B 43 40 41 8B C0 83 E0 0F 74 07 44 2B C0 41 83 C0 10 48 8B 7B 30 41 8D 40 02 41 8B C8 48 03 F9 66 C7 "
         "07 27 00",
         "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 81 D0 00 00 00 0F B6 F2 48 8B 98 E0 3C 03 00 8B 43 10 48 3B 43 "
         "18 74 7F 44 8B 43 40 41 8B C0 83 E0 0F 74 07 44 2B C0 41 83 C0 10 48 8B 7B 30 41 8D 40 02 41 8B C8 48 03 F9 66 C7 "
         "07 27 00"},
        {"PostSelectWorld",
         "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 81 D0 00 00 00 0F B7 DA 48 8B B8 C0 3C 03 00 4C 8D 47 18 8B 47 "
         "10 49 3B 00 74 7D 8B 57 40 8B C2 83 E0 0F 74 05 2B D0 83 C2 10 48 8B 77 30 8D 42 03 8B CA 48 03 F1 66 C7 06 16 00",
         "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 81 D0 00 00 00 0F B7 DA 48 8B B8 E0 3C 03 00 4C 8D 47 18 8B 47 "
         "10 49 3B 00 74 7D 8B 57 40 8B C2 83 E0 0F 74 05 2B D0 83 C2 10 48 8B 77 30 8D 42 03 8B CA 48 03 F1 66 C7 06 16 00"},
        {"PostSelectCharacter",
         "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 48 8B 81 D0 00 00 00 41 0F B7 D8 8B FA 48 8B B0 C0 "
         "3C 03 00 4C 8D 46 18 8B 46 10 49 3B 00 74 78 8B 56 40 8B C2 83 E0 0F 74 05 2B D0 83 C2 10 4C 8B 76 30 33 C0 8B CA "
         "4C 03 F1 49 89 06 41 89 46 08 8D 42 0C 41 C6 06 11",
         "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 48 8B 81 D0 00 00 00 41 0F B7 D8 8B FA 48 8B B0 E0 "
         "3C 03 00 4C 8D 46 18 8B 46 10 49 3B 00 74 78 8B 56 40 8B C2 83 E0 0F 74 05 2B D0 83 C2 10 4C 8B 76 30 33 C0 8B CA "
         "4C 03 F1 49 89 06 41 89 46 08 8D 42 0C 41 C6 06 11"},
    };
    for (const auto& p : pairs) {
        const std::string name = p.sig;
        CAPTURE(name);
        REQUIRE(find_signature(p.sig) != nullptr);
        CHECK(matches_in(p.sig, hex_bytes(p.before)) == 1);
        CHECK(matches_in(p.sig, hex_bytes(p.after)) == 1);
    }
    // the code around the offsets still counts: GetComponent with another instruction in place of its lea
    CHECK(matches_in("GetComponent",
                     hex_bytes("48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B BA C0 79 E1 00 44 89")) == 0);
}

TEST_CASE("rebased: a function another DLL hooked is found in the exe's file copy and called in the live module") {
    // the exe's bytes as shipped (pristine, mapped at 0x10000) and the live module (at 0x7000000) where another DLL
    // put a jump over the function's first bytes
    std::vector<uint8_t> pristine(64, 0xCC);
    const uint8_t start[] = {0x48, 0x8B, 0x11, 0x05};
    std::memcpy(pristine.data() + 3, start, sizeof start);  // RVA 0x1003, build 1's
    std::vector<uint8_t> live = pristine;
    const uint8_t jump[] = {0xE9, 0x10, 0x20, 0x30, 0x40};
    std::memcpy(live.data() + 3, jump, sizeof jump);
    CHECK(resolve_one(live, 0x7001000, 0x7000000, kDef, 1).count == 0);  // matched in the live code: lost
    std::array<Resolved, kSigCount> found{};
    found[0] = resolve_one(pristine, 0x11000, 0x10000, kDef, 1);
    REQUIRE(is_usable(found[0], true));
    const auto at = rebased(found, 0x10000, 0x7000000);
    CHECK(at[0].addr == 0x7001003);  // the same RVA in the live module
    CHECK(at[0].count == 1);
    CHECK(at[0].rva_matches);
    CHECK(at[1].addr == 0);  // nothing found stays nothing
    CHECK(at[1].count == 0);
}

TEST_CASE("signature table has the UI scope open and close and the layer's transform push and pop (client only, checked on 1287369)") {
    struct Want {
        const char* name;
        uint32_t rva;
    } const want[] = {{"UiScopeOpen", 0xa3cb10u}, {"UiScopeClose", 0xa40080u}, {"UiPushTransform", 0x11c4aa0u}, {"UiPopTransform", 0x11c2850u}};
    for (const auto& w : want) {
        CAPTURE(w.name);
        const SigDef* d = find_signature(w.name);
        REQUIRE(d != nullptr);
        CHECK(d->rva[7] == w.rva);
        CHECK(rva_checked(*d, 7));
        CHECK(d->rva[8] == 0u);
        CHECK(absent_on(*d, 8));
        CHECK(d->rva[5] == kRvaUnchecked);
        CHECK_FALSE(rva_checked(*d, 5));
        CHECK_FALSE(d->required);
        // the pattern's own bytes, each ?? as 00, match exactly once in a buffer that holds them once
        const Pattern p = parse_pattern(d->pattern);
        std::vector<uint8_t> buf(64, 0xCC);
        buf.insert(buf.end(), p.bytes.begin(), p.bytes.end());
        buf.insert(buf.end(), 64, 0xCC);
        const ScanResult r = scan(buf, 0x1000, p);
        CHECK(r.count == 1);
        CHECK(r.first == 0x1000 + 64);
    }
    // the offsets the editor uses are inside the patterns
    auto has = [](const char* name, const char* bytes) {
        const std::string pat = find_signature(name)->pattern;
        return pat.find(bytes) != std::string::npos;
    };
    CHECK(has("UiScopeClose", "80 79 20 00"));
    CHECK(has("UiPushTransform", "83 79 08 04"));
    CHECK(has("UiPushTransform", "E0 6C 00 00"));
    CHECK(has("UiPopTransform", "10 42 00 00"));
    CHECK(has("UiPopTransform", "08 42 00 00"));
    CHECK(has("UiPopTransform", "E0 21 00 00"));
    CHECK(has("UiPopTransform", "2C 52 03 00"));
    CHECK(has("UiPopTransform", "D2 52 03 00"));
}

TEST_CASE("signature table has HandleAccountAction and SessionRequestKick (refusing a join), optional, both exes of 1287369") {
    const SigDef* h = find_signature("HandleAccountAction");
    REQUIRE(h != nullptr);
    CHECK(h->id == Sig::HandleAccountAction);
    CHECK_FALSE(h->required);
    CHECK(h->call_at == -1);
    CHECK(h->rva[7] == 0xe4b3b0u);  // 1287369 client (a game that hosts)
    CHECK(h->rva[8] == 0x7a6e50u);  // its dedicated server
    CHECK_FALSE(rva_checked(*h, 5));
    const SigDef* k = find_signature("SessionRequestKick");
    REQUIRE(k != nullptr);
    CHECK(k->id == Sig::SessionRequestKick);
    CHECK_FALSE(k->required);
    CHECK(k->call_at == 0x18);  // the handler's Kick call site; the function is the call's target
    CHECK(k->rva[7] == 0xa6edc0u);
    CHECK(k->rva[8] == 0xa02c90u);
    CHECK_FALSE(rva_checked(*k, 5));
}

TEST_CASE("signature table has PlayerVitalBar (the HUD parts): a call site, client only, 1287369's function at 0xc7fc40") {
    const SigDef* d = find_signature("PlayerVitalBar");
    REQUIRE(d != nullptr);
    CHECK(d->id == Sig::PlayerVitalBar);
    CHECK_FALSE(d->required);
    CHECK(d->call_at == 0x10);  // the health bar's call; the pattern holds the three data and style offsets
    CHECK(d->rva[7] == 0xc7fc40u);
    CHECK(d->rva[8] == 0u);  // the dedicated server has no UI
    CHECK(absent_on(*d, 8));
    CHECK(std::string(d->pattern).find("4C 8D 86 F8 DB 00 00 48 8D 57 18") == 0);
    CHECK(std::string(d->pattern).find("48 8D 97 F0 00 00 00") != std::string::npos);
    CHECK(std::string(d->pattern).find("48 8D 97 C8 01 00 00") != std::string::npos);
    CHECK(kSigCount == 98);
}

TEST_CASE("signature table has LocaPoolLookup and LocaSlotFind (item names): function starts, client only, 1287369") {
    const SigDef* l = find_signature("LocaPoolLookup");
    REQUIRE(l != nullptr);
    CHECK(l->id == Sig::LocaPoolLookup);
    CHECK_FALSE(l->required);
    CHECK(l->call_at == -1);
    CHECK(l->rva[7] == 0x12169B0u);
    CHECK(l->rva[8] == 0u);  // the dedicated server has no text
    CHECK(std::string(l->pattern).find("39 BE D8 00 00 00") != std::string::npos);  // the active language at +0xD8
    const SigDef* f = find_signature("LocaSlotFind");
    REQUIRE(f != nullptr);
    CHECK(f->id == Sig::LocaSlotFind);
    CHECK_FALSE(f->required);
    CHECK(f->rva[7] == 0x1216280u);
    CHECK(f->rva[8] == 0u);
    CHECK(std::string(f->pattern).find("48 8B 42 20") != std::string::npos);  // the slot's table at +0x20
}

TEST_CASE("signature table has ApplyHitToEntity (hits): a call site, both exes of 1287369") {
    const SigDef* d = find_signature("ApplyHitToEntity");
    REQUIRE(d != nullptr);
    CHECK(d->id == Sig::ApplyHitToEntity);
    CHECK_FALSE(d->required);
    CHECK(d->call_at == 0x1B);  // impact_hit_conclusion's entity call; the pattern holds its stack layout
    CHECK(d->rva[7] == 0x3efd10u);
    CHECK(d->rva[8] == 0x190fb0u);  // the dedicated server applies hits too
    for (size_t b = 0; b < 7; ++b) CHECK(d->rva[b] == kRvaUnchecked);
    CHECK(std::string(d->pattern).find("4C 8D 8D 40 02 00 00") == 0);
    REQUIRE(d->call_target_pattern != nullptr);
    CHECK(std::string(d->call_target_pattern).find("4C 89 44 24 18 48 89 54 24 10") == 0);
    CHECK(kSigCount == 98);
}

TEST_CASE("signature table has PanelLayerOpen (the panel colour): a call site, client only, 1287369's function at 0xa3c8b0") {
    const SigDef* d = find_signature("PanelLayerOpen");
    REQUIRE(d != nullptr);
    CHECK(d->id == Sig::PanelLayerOpen);
    CHECK_FALSE(d->required);
    CHECK(d->call_at == 0x1d);  // the panel's uiLayerOpen call: the bundle at +0xc8, enable 1, frameColor at +0xdbf8
    CHECK(d->rva[7] == 0xa3c8b0u);
    CHECK(d->rva[8] == 0u);  // the dedicated server has no UI
    CHECK(absent_on(*d, 8));
    CHECK(std::string(d->pattern).find("4C 8B 88 C8 00 00 00") == 0);
    CHECK(std::string(d->pattern).find("4D 8B 89 F8 DB 00 00 E8") != std::string::npos);
    REQUIRE(d->call_target_pattern != nullptr);
    CHECK(std::string(d->call_target_pattern).find("4C 8B DC 53 48 81 EC A0 00 00 00") == 0);
    CHECK(kSigCount == 98);
}

TEST_CASE("signature table has the markers' four guards (v33): client only, 1287369's places, displacements masked") {
    struct Want {
        const char* name;
        Sig id;
        uint32_t rva;
        const char* starts;
    };
    const Want want[] = {
        {"UiCameraBlock", Sig::UiCameraBlock, 0xcf239fu, "49 81 C0 ?? ?? ?? ?? 89 45 80"},
        {"MapMarkerSliceClear", Sig::MapMarkerSliceClear, 0x30447fu, "E8 ?? ?? ?? ?? 48 8B 44 24 28 33 C9 48 89 88 ?? ?? ?? ??"},
        {"MapMarkerEntryLayout", Sig::MapMarkerEntryLayout, 0x316534u, "89 46 08 0F 10 07 0F 11 46 10"},
        {"MapMarkerEntryLabel", Sig::MapMarkerEntryLabel, 0x31684du, "0F 11 4E 50 F2 0F 11 46 60"},
    };
    for (const Want& w : want) {
        CAPTURE(w.name);
        const SigDef* d = find_signature(w.name);
        REQUIRE(d != nullptr);
        CHECK(d->id == w.id);
        CHECK_FALSE(d->required);
        CHECK(d->call_at == -1);
        CHECK(d->rva[7] == w.rva);
        CHECK(d->rva[8] == 0u);  // the dedicated server has no UI
        CHECK(std::string(d->pattern).find(w.starts) == 0);
        for (size_t b = 0; b < 7; ++b) CHECK(d->rva[b] == (known_builds()[b].server ? 0u : kRvaUnchecked));
    }
    CHECK(kSigCount == 98);
}

TEST_CASE("signature table has the ground search's two (v34): the voxel ray and the reset bounds' setter, both exes") {
    struct Want {
        const char* name;
        Sig id;
        uint32_t game, server;
    };
    const Want want[] = {{"VoxelRay", Sig::VoxelRay, 0xa0f770u, 0x6ac7e0u},
                         {"ResetBoundsSet", Sig::ResetBoundsSet, 0xeb37f0u, 0x95b460u}};
    for (const Want& w : want) {
        CAPTURE(w.name);
        const SigDef* d = find_signature(w.name);
        REQUIRE(d != nullptr);
        CHECK(d->id == w.id);
        CHECK_FALSE(d->required);
        CHECK(d->rva[7] == w.game);
        CHECK(d->rva[8] == w.server);
        for (size_t b = 0; b < 7; ++b) CHECK(d->rva[b] == kRvaUnchecked);
    }
    CHECK(kSigCount == 98);
}

TEST_CASE("signature table has the fog sampler (the shroud at a map teleport's landing), both exes") {
    const SigDef* d = find_signature("FogSample");
    REQUIRE(d != nullptr);
    CHECK(d->id == Sig::FogSample);
    CHECK_FALSE(d->required);
    CHECK(d->rva[7] == 0x1222180u);
    CHECK(d->rva[8] == 0xa484b0u);
    for (size_t b = 0; b < 7; ++b) CHECK(d->rva[b] == kRvaUnchecked);
    CHECK(kSigCount == 98);
}

TEST_CASE("signature table has the water surface query (a map teleport lands on the water, not the lake bed), both exes") {
    const SigDef* d = find_signature("WaterSurfacePlane");
    REQUIRE(d != nullptr);
    CHECK(d->id == Sig::WaterSurfacePlane);
    CHECK_FALSE(d->required);
    CHECK(d->rva[7] == 0x1121680u);
    CHECK(d->rva[8] == 0x8a70f0u);
    for (size_t b = 0; b < 7; ++b) CHECK(d->rva[b] == kRvaUnchecked);
}

TEST_CASE("signature table has the minimap's three (the game's map canvas, the UI clip push and pop), client only") {
    struct Want {
        const char* name;
        Sig id;
        uint32_t game;
        int call_at;
    };
    const Want want[] = {{"MapCanvasDraw", Sig::MapCanvasDraw, 0xd7c690u, -1},
                         {"UiPushClip", Sig::UiPushClip, 0x11c3a30u, -1},
                         {"UiPopClip", Sig::UiPopClip, 0x11c2610u, 0x15}};
    for (const Want& w : want) {
        CAPTURE(w.name);
        const SigDef* d = find_signature(w.name);
        REQUIRE(d != nullptr);
        CHECK(d->id == w.id);
        CHECK_FALSE(d->required);
        CHECK(d->rva[7] == w.game);
        CHECK(d->rva[8] == 0u);  // the dedicated server draws nothing: absent there (its sigcheck said FAIL before)
        CHECK(absent_on(*d, 8));
        CHECK(d->call_at == w.call_at);
    }
    CHECK(kSigCount == 98);
}

TEST_CASE("signature table has the four path query calls (API v37 path_find), both exes of 1287369") {
    struct Want {
        const char* name;
        Sig id;
        uint32_t game, server;
    };
    const Want want[] = {{"CreatePathQuery", Sig::CreatePathQuery, 0x95fa00u, 0x5fb550u},
                         {"DestroyPathQuery", Sig::DestroyPathQuery, 0x960310u, 0x5fbe60u},
                         {"GetPathQueryResult", Sig::GetPathQueryResult, 0x9b7530u, 0x6533c0u},
                         {"PathQueryDefaults", Sig::PathQueryDefaults, 0x982890u, 0x61e720u}};
    for (const Want& w : want) {
        CAPTURE(w.name);
        const SigDef* d = find_signature(w.name);
        REQUIRE(d != nullptr);
        CHECK(d->id == w.id);
        CHECK_FALSE(d->required);
        CHECK(d->rva[7] == w.game);
        CHECK(d->rva[8] == w.server);
        for (size_t b = 0; b < 7; ++b) CHECK(d->rva[b] == kRvaUnchecked);
    }
    // destroy is kind 8, not the kind-7 twin (ResetPathQueryResults keeps the record)
    CHECK(std::string(find_signature("DestroyPathQuery")->pattern).find("41 B0 08") != std::string::npos);
    CHECK(kSigCount == 98);
}

TEST_CASE("signature table has the path grid's four guards (path.door): door table, metres, nodes, cell list; both exes of 1287369") {
    struct Want {
        const char* name;
        Sig id;
        uint32_t game, server;
        const char* holds[4];  // bytes the pattern must keep: the offsets path.door reads
    };
    const Want want[] = {
        // the door count (+0xabe1c0, 0x1000 at most), the 0x38 stride, the table (+0xa861c0), the cells (+0x130),
        // a door's box flattened to its lowest node layer, the cell list (+0xb8, next +8), a cell's state (+0x2c)
        {"PathGridRegister", Sig::PathGridRegister, 0x9e2b80u, 0x67eb20u,
         {"0F B7 81 C0 E1 AB 00 41 B8 00 10 00 00", "48 6B D8 38", "48 81 C3 C0 61 A8 00", "4C 8B B1 30 01 00 00"}},
        // metres -> (cell, node): the origin (+0x120), nodes per cell (+0xd1, +0xd2), nodes per metre (+0xf0, +0xf8),
        // the cell shifts (+0x108, +0x10c)
        {"PathGridBoxToCells", Sig::PathGridBoxToCells, 0x975160u, 0x610ff0u,
         {"F2 41 0F 10 88 20 01 00 00", "41 0F B6 BF D1 00 00 00", "F2 41 0F 10 8F F0 00 00 00",
          "45 0F B6 9F D2 00 00 00"}},
        // a node's two bytes: the cell's block (+0x40), its nodes (+0x28), the node strides (+0x12c, +0x130, +0x134)
        {"PathGridNodePos", Sig::PathGridNodePos, 0x968ff0u, 0x604c00u,
         {"48 8B 49 40", "4C 8D 59 28", "0F B7 96 2C 01 00 00", "0F B7 86 34 01 00 00"}},
        // the cell list walk inside PG_RegisterInteraction (+0x212): the list (+0xb8), a cell's coords (+0x20, +0x28),
        // its state (+0x2c), the next cell (+8)
        {"PathGridCellList", Sig::PathGridCellList, 0x9e2d92u, 0x67ed32u,
         {"49 8B 96 B8 00 00 00", "F2 0F 10 42 20 8B 4A 28", "0F B6 4A 2C", "48 8B 52 08 48 85 D2"}},
    };
    for (const Want& w : want) {
        CAPTURE(w.name);
        const SigDef* d = find_signature(w.name);
        REQUIRE(d != nullptr);
        CHECK(d->id == w.id);
        CHECK_FALSE(d->required);
        CHECK(d->call_at == -1);
        CHECK(d->rva[7] == w.game);
        CHECK(d->rva[8] == w.server);
        for (size_t b = 0; b < 7; ++b) CHECK(d->rva[b] == kRvaUnchecked);
        for (const char* h : w.holds) CHECK(std::string(d->pattern).find(h) != std::string::npos);
    }
    // review M6 (0.6.11): short patterns without the function's alignment fillers, so a small change elsewhere in it
    // does not turn path.door off; the doors' flattening to one layer stays pinned
    const std::string reg = find_signature("PathGridRegister")->pattern;
    CHECK(reg.find("40 80 FF 02 74 ?? 40 80 FF 01 75 ??") != std::string::npos);
    CHECK(reg.size() < 600u);  // about 190 bytes (it was about 600)
    for (const char* name : {"PathGridRegister", "PathGridCellList"}) {
        const std::string p = find_signature(name)->pattern;
        CHECK(p.find("0F 1F 40 00") == std::string::npos);
        CHECK(p.find("0F 1F 84 00") == std::string::npos);
        CHECK(p.find("66 90") == std::string::npos);
    }
    CHECK(kSigCount == 98);
}

TEST_CASE("signature table has the four for breaking blocks (API v44): kernel, region read, config lookup, damage get; both exes of 1287369") {
    struct Want {
        const char* name;
        Sig id;
        uint32_t game, server;
        const char* holds;  // bytes the pattern must keep
    };
    const Want want[] = {
        // the building-mining kernel's start (its 0x3A50-byte frame)
        {"BlockKernel", Sig::BlockKernel, 0x4966e0u, 0x22dc40u, "48 8D AC 24 B0 C6"},
        // the voxel region read: the slice's data pointer
        {"VoxelRegionRead", Sig::VoxelRegionRead, 0x10b20e0u, 0x836910u, "48 8B 71 08"},
        // the building config lookup: the resource at registry + 0x38, the blob array at +8 (its count at +0xC); the
        // full 52 bytes (its first 18 have a twin)
        {"BuildingConfigFind", Sig::BuildingConfigFind, 0xe8c9b0u, 0x933af0u, "48 8B 41 38 48 85 C0 74 3A 44 8B 50 0C"},
        // the damage store's get: the cell read from the second argument
        {"VoxelDamageGet", Sig::VoxelDamageGet, 0x4a9cf0u, 0x2426c0u, "F2 0F 10 02 48 8B FA 44 8B 4A"},
    };
    for (const Want& w : want) {
        CAPTURE(w.name);
        const SigDef* d = find_signature(w.name);
        REQUIRE(d != nullptr);
        CHECK(d->id == w.id);
        CHECK_FALSE(d->required);
        CHECK(d->call_at == -1);
        CHECK(d->rva[7] == w.game);
        CHECK(d->rva[8] == w.server);
        for (size_t b = 0; b < 7; ++b) CHECK(d->rva[b] == kRvaUnchecked);
        CHECK(std::string(d->pattern).find(w.holds) != std::string::npos);
    }
    CHECK(std::string(find_signature("BuildingConfigFind")->pattern).size() >= 52 * 3 - 1);
    CHECK(kSigCount == 98);
}

TEST_CASE("signature table has the four that read the shipped world's voxels (ruin protection): column acquire, brick seek, brick decode, column release; both exes of 1287369") {
    struct Want {
        const char* name;
        Sig id;
        uint32_t game, server;
    };
    const Want want[] = {
        {"BlockColumnAcquire", Sig::BlockColumnAcquire, 0x10ab270u, 0x82f840u},
        {"BlockBrickSeek", Sig::BlockBrickSeek, 0x10c6170u, 0x84adc0u},
        {"BlockBrickDecode", Sig::BlockBrickDecode, 0x10cc7d0u, 0x8514b0u},
        {"BlockColumnRelease", Sig::BlockColumnRelease, 0x10cc750u, 0x851430u},
    };
    for (const Want& w : want) {
        CAPTURE(w.name);
        const SigDef* d = find_signature(w.name);
        REQUIRE(d != nullptr);
        CHECK(d->id == w.id);
        CHECK_FALSE(d->required);
        CHECK(d->rva[7] == w.game);
        CHECK(d->rva[8] == w.server);
    }
}

TEST_CASE("signature table has HudScreenRead (the chat key gate): the HUD screen byte's read in the chat window, client of 1287369 only") {
    const SigDef* d = find_signature("HudScreenRead");
    REQUIRE(d != nullptr);
    CHECK_FALSE(d->required);
    CHECK(d->call_at == -1);
    CHECK(d->rva[7] == 0xcdc3dcu);
    CHECK(d->rva[8] == 0u);  // the dedicated server has no HUD
    CHECK(absent_on(*d, 8));
    for (size_t b : {0, 1, 3, 5}) CHECK(d->rva[b] == kRvaUnchecked);  // older clients: not at hand
    for (size_t b : {2, 4, 6}) CHECK(d->rva[b] == 0u);              // older servers: no HUD either
    // movzx r15d, byte [rdi + disp32] (the u32 at +4 is the offset); cmp r15b, imm8 (+11: TextChat)
    CHECK(std::string(d->pattern).rfind("44 0F B6 BF ?? ?? ?? ?? 41 80 FF ??", 0) == 0);
}
