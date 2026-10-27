#include "code_offsets.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "offsets.h"
#include "pe_image.h"
#include "sigscan.h"

namespace emf {
namespace {
// Values: {1269579, 1273043, 1273043 server}. In save2WriteWorld (docs/engine_parts/re_saves.md):
//   Save2Result: its first store `mov [rcx+disp32], rax` (function start + 0x5a on 1273043) writes the job result.
//   Save2WorldSlot: `movzx eax, word ptr [rsi+disp32]; mov word ptr [rbp+..], ax` copies the world slot id for the
//   save (function start + 0x2c7 on 1273043; the server uses rdi, so the register byte is a wildcard). The server's
//   save state is smaller: result +0xE4, slot +0xDF (the same 5 bytes apart).
// In SimStep (the sim step, its argument is the entity storage): `mov rax, [rsi+disp32]; test rdx, rdx` reads the tick
//   period (+0x71d on 1273043) and `lea rbx, [rsi+disp32]; mov r15, [rsp+..]` takes the tick's address (+0x6f5).
// In loadSavegameStep (re_saves.md "Load"): `cmp byte ptr [rbx+disp32], 0; mov rcx, [rbx+..]` (function start + 0x7c
//   on 1273043) tests the instance's "local world slot set" flag; the u16 slot id is the field right before it.
constexpr CodeOffsetDef kCodeOffsets[] = {
    {CodeOffset::Save2Result, "Save2Result", Sig::Save2WriteWorld, "48 89 81 ?? ?? ?? ??", 3, 0x100,
     {0xB728Cu, 0xB729Cu, 0xE4u}},
    {CodeOffset::Save2WorldSlot, "Save2WorldSlot", Sig::Save2WriteWorld, "0F B7 ?? ?? ?? ?? ?? 66 89 85", 3, 0x1000,
     {0xB7287u, 0xB7297u, 0xDFu}},
    {CodeOffset::InstWorldSlotFlag, "InstWorldSlotFlag", Sig::LoadSavegameStep, "80 BB ?? ?? ?? ?? 00 48 8B 8B", 2,
     0x1000, {0x1756u, 0x1756u, 0x1756u}},
    {CodeOffset::StorageTickPeriod, "StorageTickPeriod", Sig::SimStep, "48 8B 86 ?? ?? ?? ?? 48 85 D2", 3, 0x1000,
     {0x4C0u, 0x4C0u, 0x4C0u}},
    {CodeOffset::StorageTick, "StorageTick", Sig::SimStep, "48 8D 9E ?? ?? ?? ?? 4C 8B BC 24", 3, 0x1000,
     {0x4DCu, 0x4DCu, 0x4DCu}},
};
static_assert(off::kTickPeriodNs - off::kStorage == 0x4C0 && off::kTick - off::kStorage == 0x4DC,
              "the SimStep code offsets confirm offsets.h");
static_assert(std::size(kCodeOffsets) == kCodeOffsetCount, "one entry per CodeOffset");

ResolvedOffset resolve_one(std::span<const uint8_t> text, uintptr_t text_base, uintptr_t function,
                           const CodeOffsetDef& def, int build) {
    ResolvedOffset r;
    if (function < text_base || function - text_base >= text.size()) return r;
    const size_t start = function - text_base;
    const size_t len = std::min(def.window, text.size() - start);
    const ScanResult s = scan(text.subspan(start, len), function, parse_pattern(def.pattern));
    r.count = s.count;
    if (s.count == 0) return r;
    // The whole pattern matched inside the window, so its displacement bytes are inside `text` too.
    std::memcpy(&r.value, text.data() + (s.first - text_base) + def.disp_at, sizeof(r.value));
    r.scanned = r.value;
    r.value_matches =
        build >= 0 && build < static_cast<int>(kKnownBuildCount) && r.value == def.value[static_cast<size_t>(build)];
    return r;
}
}  // namespace

std::span<const CodeOffsetDef> all_code_offsets() { return kCodeOffsets; }

std::array<ResolvedOffset, kCodeOffsetCount> resolve_code_offsets(std::span<const uint8_t> text, uintptr_t text_base,
                                                                  const std::array<Resolved, kSigCount>& sigs,
                                                                  int build) {
    std::array<ResolvedOffset, kCodeOffsetCount> out{};
    const bool known = build >= 0 && build < static_cast<int>(kKnownBuildCount);
    for (size_t i = 0; i < kCodeOffsetCount; ++i) {
        const auto& def = kCodeOffsets[i];
        const auto& fn = sigs[static_cast<size_t>(def.function)];
        if (is_usable(fn, build >= 0)) out[i] = resolve_one(text, text_base, fn.addr, def, build);
        if (known && !(out[i].count == 1 && out[i].value_matches)) {
            out[i].value = def.value[static_cast<size_t>(build)];
            out[i].from_record = true;
        }
    }
    if (!known) {  // a stray single match of the short result pattern would put the two far apart
        auto& result = out[static_cast<size_t>(CodeOffset::Save2Result)];
        auto& slot = out[static_cast<size_t>(CodeOffset::Save2WorldSlot)];
        if (result.count == 1 && slot.count == 1) {
            const uint32_t gap = result.value > slot.value ? result.value - slot.value : slot.value - result.value;
            result.pair_ok = slot.pair_ok = gap <= kMaxSaveResultSlotGap;
        }
    }
    return out;
}

std::array<ResolvedOffset, kCodeOffsetCount> resolve_code_offsets(const void* module_base,
                                                                  const std::array<Resolved, kSigCount>& sigs,
                                                                  int build) {
    auto text = section(module_base, ".text");
    if (!text) return {};
    return resolve_code_offsets(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text->start), text->size),
                                text->start, sigs, build);
}

bool is_usable(const ResolvedOffset& r, bool known_build) {
    if (known_build) return r.value != 0;
    return r.count == 1 && r.pair_ok;
}

bool world_tick_offsets_ok(const std::array<ResolvedOffset, kCodeOffsetCount>& r, bool known_build) {
    if (known_build) return true;
    const auto& period = r[static_cast<size_t>(CodeOffset::StorageTickPeriod)];
    const auto& tick = r[static_cast<size_t>(CodeOffset::StorageTick)];
    return is_usable(period, false) && is_usable(tick, false) && period.value == off::kTickPeriodNs - off::kStorage &&
           tick.value == off::kTick - off::kStorage;
}

bool offset_check_ok(const std::array<ResolvedOffset, kCodeOffsetCount>& r, size_t i, bool known_build) {
    if (i >= r.size()) return false;
    if (known_build) return r[i].count == 1 && r[i].value_matches;
    const bool tick = i == static_cast<size_t>(CodeOffset::StorageTickPeriod) || i == static_cast<size_t>(CodeOffset::StorageTick);
    return is_usable(r[i], false) && (!tick || world_tick_offsets_ok(r, false));
}

std::vector<std::string> unconfirmed_offset_notes(const std::array<ResolvedOffset, kCodeOffsetCount>& r) {
    std::vector<std::string> out;
    const auto defs = all_code_offsets();
    for (size_t i = 0; i < r.size(); ++i) {
        if (!r[i].from_record) continue;
        const char* name = i < defs.size() ? defs[i].name : "?";
        char line[200];
        snprintf(line, sizeof(line), "code offset %s +0x%x: the scan did not confirm it (%zu match(es), 0x%x); using the "
                 "recorded value", name, r[i].value, r[i].count, r[i].scanned);
        out.push_back(line);
    }
    return out;
}

HookPlan plan_offset_hook(const char* hook_name, bool function_usable,
                          std::initializer_list<std::pair<const char*, bool>> offsets) {
    HookPlan p;
    if (!function_usable) {
        p.reason = std::string(hook_name) + " NOT hooked: its signature is not usable on this game build";
        return p;
    }
    std::string missing;
    for (const auto& [name, ok] : offsets)
        if (!ok) missing += (missing.empty() ? "" : ", ") + std::string(name);
    if (!missing.empty()) {
        p.reason = std::string(hook_name) + " NOT hooked: not found in its code: " + missing;
        return p;
    }
    p.hook = true;
    return p;
}

}  // namespace emf
