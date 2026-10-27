// Engine struct offsets read from instructions inside signature-located functions (the save state's job result and
// world slot moved by 0x10 between builds 1269579 and 1273043; reading the old offsets gave slot 0).
#include <doctest.h>

#include <cstring>
#include <initializer_list>
#include <set>
#include <string>
#include <vector>

#include "common/code_offsets.h"
#include "common/sigscan.h"

using namespace emf;

namespace {
constexpr uintptr_t kTextBase = 0x1000;
constexpr size_t kFn = 0x10;  // the save function's offset inside the fake .text

void put(std::vector<uint8_t>& text, size_t at, std::initializer_list<uint8_t> bytes) {
    std::memcpy(text.data() + at, bytes.begin(), bytes.size());
}
// A save2WriteWorld-like body: the job-result store at +0x5a and the slot read at +0x2c7 (as in build 1273043).
std::vector<uint8_t> save_function(uint32_t result, uint32_t slot, uint8_t slot_modrm = 0x86) {
    std::vector<uint8_t> text(0x400, 0xCC);
    std::vector<uint8_t> r = {0x48, 0x89, 0x81};  // mov [rcx+disp32], rax
    for (int i = 0; i < 4; ++i) r.push_back(uint8_t(result >> (8 * i)));
    std::memcpy(text.data() + kFn + 0x5a, r.data(), r.size());
    std::vector<uint8_t> s = {0x0F, 0xB7, slot_modrm};  // movzx eax, word ptr [rsi+disp32] (0x87: [rdi+disp32])
    for (int i = 0; i < 4; ++i) s.push_back(uint8_t(slot >> (8 * i)));
    s.insert(s.end(), {0x66, 0x89, 0x85});  // mov word ptr [rbp+..], ax
    std::memcpy(text.data() + kFn + 0x2c7, s.data(), s.size());
    return text;
}
std::array<Resolved, kSigCount> sigs_with_save_at(uintptr_t addr) {
    std::array<Resolved, kSigCount> sigs{};
    sigs[static_cast<size_t>(Sig::Save2WriteWorld)] = Resolved{addr, 1, true};
    return sigs;
}
const size_t kResult = static_cast<size_t>(CodeOffset::Save2Result);
const size_t kSlot = static_cast<size_t>(CodeOffset::Save2WorldSlot);
const size_t kLoadFlag = static_cast<size_t>(CodeOffset::InstWorldSlotFlag);
}  // namespace

TEST_CASE("code offsets: read from the instruction inside the located function, checked against the known build") {
    const auto text = save_function(0xB729C, 0xB7297);
    const auto sigs = sigs_with_save_at(kTextBase + kFn);
    const int b1273043 = known_build_index(1273043);
    auto r = resolve_code_offsets(text, kTextBase, sigs, b1273043);
    CHECK(r[kResult].count == 1);
    CHECK(r[kResult].value == 0xB729Cu);
    CHECK(r[kResult].value_matches);
    CHECK(is_usable(r[kResult], true));
    CHECK(r[kSlot].value == 0xB7297u);
    CHECK(is_usable(r[kSlot], true));

    // The same code on an exe whose hash says 1269579 (which recorded 0xB728C / 0xB7287): the hash pins the code, so
    // the recorded value is used and the disagreement is flagged.
    r = resolve_code_offsets(text, kTextBase, sigs, known_build_index(1269579));
    CHECK(r[kResult].value == 0xB728Cu);
    CHECK_FALSE(r[kResult].value_matches);
    CHECK(r[kResult].from_record);
    CHECK(is_usable(r[kResult], true));
    // An unknown build trusts a single match.
    r = resolve_code_offsets(text, kTextBase, sigs, -1);
    CHECK(is_usable(r[kResult], false));
    CHECK(is_usable(r[kSlot], false));
}

TEST_CASE("code offsets: a missing or doubled instruction, or an unusable function signature, gives nothing usable") {
    auto text = save_function(0xB729C, 0xB7297);
    std::memset(text.data() + kFn + 0x2c7, 0xCC, 10);  // slot read gone
    put(text, kFn + 0x70, {0x48, 0x89, 0x81, 0x00, 0x00, 0x00, 0x00});  // a second result store
    auto r = resolve_code_offsets(text, kTextBase, sigs_with_save_at(kTextBase + kFn), -1);
    CHECK(r[kSlot].count == 0);
    CHECK_FALSE(is_usable(r[kSlot], false));
    CHECK(r[kResult].count == 2);
    CHECK_FALSE(is_usable(r[kResult], false));

    const auto good = save_function(0xB729C, 0xB7297);
    std::array<Resolved, kSigCount> none{};  // Save2WriteWorld not found
    r = resolve_code_offsets(good, kTextBase, none, -1);
    CHECK(r[kResult].count == 0);
    CHECK(r[kSlot].count == 0);
    auto twice = sigs_with_save_at(kTextBase + kFn);
    twice[static_cast<size_t>(Sig::Save2WriteWorld)].count = 2;  // the signature itself is ambiguous
    CHECK(resolve_code_offsets(good, kTextBase, twice, -1)[kResult].count == 0);
}

TEST_CASE("code offsets: the search stays inside .text when the function sits near its end") {
    auto text = save_function(0xB729C, 0xB7297);
    // the first 4 bytes of the result store as the last bytes of .text: a match would need bytes past the end
    put(text, text.size() - 4, {0x48, 0x89, 0x81, 0x9C});
    const auto r = resolve_code_offsets(text, kTextBase, sigs_with_save_at(kTextBase + text.size() - 8), -1);
    CHECK(r[kResult].count == 0);
    // a function address outside .text
    CHECK(resolve_code_offsets(text, kTextBase, sigs_with_save_at(0x10), -1)[kResult].count == 0);
}

TEST_CASE("code offsets: the load hook's slot flag comes from loadSavegameStep's own code") {
    std::vector<uint8_t> text(0x400, 0xCC);
    // cmp byte ptr [rbx+0x1756], 0 ; mov rcx, qword ptr [rbx+0x1778]   (loadSavegameStep + 0x7c on 1273043)
    put(text, 0x20 + 0x7c, {0x80, 0xBB, 0x56, 0x17, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x8B, 0x78, 0x17, 0x00, 0x00});
    std::array<Resolved, kSigCount> sigs{};
    sigs[static_cast<size_t>(Sig::LoadSavegameStep)] = Resolved{kTextBase + 0x20, 1, true};
    const auto r = resolve_code_offsets(text, kTextBase, sigs, known_build_index(1273043));
    CHECK(r[kLoadFlag].count == 1);
    CHECK(r[kLoadFlag].value == 0x1756u);
    CHECK(is_usable(r[kLoadFlag], true));
    CHECK(r[kResult].count == 0);  // the save function is not located here
}

TEST_CASE("code offset table: one entry per CodeOffset, in order, unique names, a value on every known build") {
    auto all = all_code_offsets();
    REQUIRE(all.size() == kCodeOffsetCount);
    std::set<std::string> names;
    for (size_t i = 0; i < all.size(); ++i) {
        CHECK(static_cast<size_t>(all[i].id) == i);
        CHECK(names.insert(all[i].name).second);
        CHECK(parse_pattern(all[i].pattern).bytes.size() >= all[i].disp_at + 4);
        CHECK(all[i].window > 0);
        for (uint32_t v : all[i].value) CHECK(v != 0);
    }
    CHECK(all[kResult].value[0] == 0xB728Cu);  // build 1269579 (docs/engine_parts/re_saves.md)
    CHECK(all[kResult].value[1] == 0xB729Cu);  // build 1273043
    CHECK(all[kSlot].value[0] == 0xB7287u);
    CHECK(all[kSlot].value[1] == 0xB7297u);
    CHECK(all[kLoadFlag].value[0] == 0x1756u);  // GameServer instance: u8 "local world slot set", after the u16 slot
    CHECK(all[kLoadFlag].value[1] == 0x1756u);
    CHECK(all[kLoadFlag].function == Sig::LoadSavegameStep);
    const auto& period = all[static_cast<size_t>(CodeOffset::StorageTickPeriod)];
    const auto& tick = all[static_cast<size_t>(CodeOffset::StorageTick)];
    CHECK(period.function == Sig::SimStep);
    CHECK(period.value[0] == 0x4C0u);  // world + 0xE15DC0 (storage) + 0x4C0 = kTickPeriodNs on both builds
    CHECK(period.value[1] == 0x4C0u);
    CHECK(tick.value[0] == 0x4DCu);
    CHECK(tick.value[1] == 0x4DCu);
    // the dedicated server of 1273043: a smaller save state, the same instructions
    CHECK(all[kResult].value[2] == 0xE4u);
    CHECK(all[kSlot].value[2] == 0xDFu);
    CHECK(all[kLoadFlag].value[2] == 0x1756u);
    CHECK(period.value[2] == 0x4C0u);
    CHECK(tick.value[2] == 0x4DCu);
}

TEST_CASE("code offsets: the server reads the world slot through rdi (the client through rsi)") {
    const auto text = save_function(0xE4, 0xDF, 0x87);
    const auto r = resolve_code_offsets(text, kTextBase, sigs_with_save_at(kTextBase + kFn), known_build_index(1273043, true));
    CHECK(r[kSlot].count == 1);
    CHECK(r[kSlot].value_matches);
    CHECK(r[kSlot].value == 0xDFu);
    CHECK(r[kResult].value_matches);
}

TEST_CASE("code offsets: a known build whose scan finds nothing (or whose function is not located) uses its records") {
    std::vector<uint8_t> blank(0x400, 0xCC);
    const int b1269579 = known_build_index(1269579);
    auto r = resolve_code_offsets(blank, kTextBase, sigs_with_save_at(kTextBase + kFn), b1269579);
    CHECK(r[kResult].count == 0);
    CHECK(r[kResult].from_record);
    CHECK(r[kResult].value == 0xB728Cu);
    CHECK(is_usable(r[kResult], true));
    CHECK(r[kSlot].value == 0xB7287u);
    std::array<Resolved, kSigCount> none{};  // the function signature itself is not usable
    r = resolve_code_offsets(blank, kTextBase, none, known_build_index(1273043));
    CHECK(r[kLoadFlag].value == 0x1756u);
    CHECK(is_usable(r[kLoadFlag], true));
    CHECK_FALSE(is_usable(r[kResult], false));  // but the same result read as an unknown build is not usable
}

TEST_CASE("code offsets: on an unknown build the save result and slot must sit close together") {
    const auto sigs = sigs_with_save_at(kTextBase + kFn);
    auto r = resolve_code_offsets(save_function(0xC0005, 0xC0000), kTextBase, sigs, -1);  // 5 apart: as on both builds
    CHECK(is_usable(r[kResult], false));
    CHECK(is_usable(r[kSlot], false));
    r = resolve_code_offsets(save_function(0xC0040, 0xC0000), kTextBase, sigs, -1);  // 0x40 apart: a stray match
    CHECK_FALSE(is_usable(r[kResult], false));
    CHECK_FALSE(is_usable(r[kSlot], false));
}

TEST_CASE("code offsets: the hook plan names what is missing") {
    auto p = plan_offset_hook("save2WriteWorld (on_save)", false, {{"Save2Result", true}, {"Save2WorldSlot", true}});
    CHECK_FALSE(p.hook);
    CHECK(p.reason.find("signature") != std::string::npos);
    p = plan_offset_hook("save2WriteWorld (on_save)", true, {{"Save2Result", true}, {"Save2WorldSlot", false}});
    CHECK_FALSE(p.hook);
    CHECK(p.reason.find("Save2WorldSlot") != std::string::npos);
    CHECK(p.reason.find("Save2Result") == std::string::npos);
    p = plan_offset_hook("save2WriteWorld (on_save)", true, {{"Save2Result", true}, {"Save2WorldSlot", true}});
    CHECK(p.hook);
    CHECK(p.reason.empty());
}

namespace {
// A SimStep-like body: `mov rax, [rsi+period]; test rdx, rdx` and `lea rbx, [rsi+tick]; mov r15, [rsp+..]`.
std::vector<uint8_t> sim_step(uint32_t period, uint32_t tick) {
    std::vector<uint8_t> text(0x900, 0xCC);
    std::vector<uint8_t> p = {0x48, 0x8B, 0x86};
    for (int i = 0; i < 4; ++i) p.push_back(uint8_t(period >> (8 * i)));
    p.insert(p.end(), {0x48, 0x85, 0xD2});
    std::memcpy(text.data() + 0x10 + 0x71d, p.data(), p.size());
    std::vector<uint8_t> k = {0x48, 0x8D, 0x9E};
    for (int i = 0; i < 4; ++i) k.push_back(uint8_t(tick >> (8 * i)));
    k.insert(k.end(), {0x4C, 0x8B, 0xBC, 0x24});
    std::memcpy(text.data() + 0x10 + 0x6f5, k.data(), k.size());
    return text;
}
std::array<Resolved, kSigCount> sigs_with_sim_step_at(uintptr_t addr) {
    std::array<Resolved, kSigCount> sigs{};
    sigs[static_cast<size_t>(Sig::SimStep)] = Resolved{addr, 1, true};
    return sigs;
}
}  // namespace

TEST_CASE("code offsets: the world's tick fields are confirmed from SimStep's code on an unknown build") {
    const auto sigs = sigs_with_sim_step_at(kTextBase + 0x10);
    auto r = resolve_code_offsets(sim_step(0x4C0, 0x4DC), kTextBase, sigs, -1);
    CHECK(r[static_cast<size_t>(CodeOffset::StorageTickPeriod)].value == 0x4C0u);
    CHECK(r[static_cast<size_t>(CodeOffset::StorageTick)].value == 0x4DCu);
    CHECK(world_tick_offsets_ok(r, false));
    r = resolve_code_offsets(sim_step(0x4C8, 0x4DC), kTextBase, sigs, -1);  // the period moved: not the compiled layout
    CHECK_FALSE(world_tick_offsets_ok(r, false));
    std::array<Resolved, kSigCount> none{};
    r = resolve_code_offsets(sim_step(0x4C0, 0x4DC), kTextBase, none, -1);  // SimStep not located
    CHECK_FALSE(world_tick_offsets_ok(r, false));
    CHECK(world_tick_offsets_ok(r, true));  // a known build: the compiled layout is its own
}

TEST_CASE("code offsets: the notes for unconfirmed offsets name each offset, all five included") {
    std::array<ResolvedOffset, kCodeOffsetCount> r{};
    r[static_cast<size_t>(CodeOffset::Save2Result)] = {0xb729c, 0xb7300, 1, false, true};
    r[static_cast<size_t>(CodeOffset::StorageTick)] = {0x4dc, 0, 0, false, true};
    const auto notes = unconfirmed_offset_notes(r);
    REQUIRE(notes.size() == 2);
    CHECK(notes[0].find("Save2Result +0xb729c") != std::string::npos);
    CHECK(notes[0].find("1 match(es), 0xb7300") != std::string::npos);
    CHECK(notes[1].find("StorageTick +0x4dc") != std::string::npos);
    CHECK(unconfirmed_offset_notes({}).empty());
}

TEST_CASE("code offsets: sigcheck's row check: a known build needs the scan to confirm, an unknown build the compiled tick layout") {
    const auto sigs = sigs_with_sim_step_at(kTextBase + 0x10);
    auto r = resolve_code_offsets(sim_step(0x4C8, 0x4DC), kTextBase, sigs, -1);  // the period moved
    const size_t period = static_cast<size_t>(CodeOffset::StorageTickPeriod);
    const size_t tick = static_cast<size_t>(CodeOffset::StorageTick);
    CHECK(is_usable(r[period], false));             // it matched once...
    CHECK_FALSE(offset_check_ok(r, period, false));  // ...but not where EMF's layout expects it
    CHECK_FALSE(offset_check_ok(r, tick, false));
    r = resolve_code_offsets(sim_step(0x4C0, 0x4DC), kTextBase, sigs, -1);
    CHECK(offset_check_ok(r, period, false));
    CHECK(offset_check_ok(r, tick, false));
}
