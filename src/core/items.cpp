#include "items.h"

#include <windows.h>

#include <cstring>
#include <iterator>

#include "common/fnv.h"
#include "common/offsets.h"
#include "common/seh.h"

namespace emf::core {
namespace {

template <class T>
bool rd(const void* addr, T& out) {
    return seh_read(addr, &out, sizeof(T));
}

std::string read_debug_name(const uint8_t* info) {
    const uint8_t* field = info + off::kItemInfoDebugName;
    uint32_t rel = 0, len = 0;
    if (!rd(field, rel) || !rd(field + 4, len) || !rel || !len || len > off::kMaxItemNameLength) return {};
    std::string s(len, '\0');
    if (!seh_read(field + rel, s.data(), len)) return {};
    for (char c : s)
        if (c < 0x20 || c > 0x7e) return {};
    return s;
}

bool read_item(const uint8_t* info, ItemRecord& out) {
    if (!info || !rd(info + off::kItemInfoId, out.id) || !seh_read(info + off::kItemInfoGuid, out.guid, 16) ||
        !rd(info + off::kItemInfoMaxStack, out.max_stack))
        return false;
    out.name = read_debug_name(info);
    return true;
}

struct ItemMap {
    const uint8_t* bits = nullptr;
    uint64_t cap = 0;
    const uint8_t* keys = nullptr;
    const uint8_t* vals = nullptr;
    uint32_t count = 0;
};
bool read_map(const uint8_t* registry, ItemMap& m) {
    if (!registry) return false;
    const uint8_t* map = registry + off::kItemMap;
    if (!rd(map + off::kMapBits, m.bits) || !rd(map + off::kMapCapacity, m.cap) || !rd(map + off::kMapKeys, m.keys) ||
        !rd(map + off::kMapValues, m.vals) || !rd(map + off::kMapCount, m.count))
        return false;
    const bool pow2 = m.cap && (m.cap & (m.cap - 1)) == 0;
    return m.count && pow2 && m.cap <= off::kMapMaxCapacity && m.bits && m.keys && m.vals;
}
bool occupied(const ItemMap& m, uint64_t slot) {
    uint64_t word = 0;
    return rd(m.bits + (slot >> 6) * 8, word) && ((word >> (slot & 63)) & 1);
}

bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

}  // namespace

uint32_t item_id_from_guid(const uint8_t guid[16]) {
    bool zero = true;
    for (int i = 0; i < 16; ++i) zero &= guid[i] == 0;
    return zero ? 0 : fnv1a32(guid, 16);
}

std::vector<ItemRecord> list_items(const uint8_t* registry) {
    std::vector<ItemRecord> out;
    ItemMap m;
    if (!read_map(registry, m)) return out;
    for (uint64_t slot = 0; slot < m.cap; ++slot) {
        if (!occupied(m, slot)) continue;
        const uint8_t* info = nullptr;
        ItemRecord r;
        if (rd(m.vals + slot * 8, info) && read_item(info, r)) out.push_back(std::move(r));
    }
    return out;
}

uint8_t* item_data_ptr(const uint8_t* registry, uint32_t id) {
    ItemMap m;
    if (!id || !read_map(registry, m)) return nullptr;
    uint64_t slot = id & (m.cap - 1);
    for (uint64_t probes = 0; probes < m.cap && occupied(m, slot); ++probes) {
        uint32_t key = 0;
        if (!rd(m.keys + slot * 4, key)) return nullptr;
        if (key == id) {
            uint8_t* info = nullptr;
            return rd(m.vals + slot * 8, info) ? info : nullptr;
        }
        slot = (slot + 1 < m.cap) ? slot + 1 : 0;
    }
    return nullptr;
}

std::optional<ItemRecord> item_by_id(const uint8_t* registry, uint32_t id) {
    const uint8_t* info = item_data_ptr(registry, id);
    ItemRecord r;
    if (info && read_item(info, r)) return r;
    return std::nullopt;
}

namespace {
bool guarded_copy(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// What may be written. VirtualQuery is slow on the game's ~400 MB resource region (Windows walks it to compute
// RegionSize), so regions are remembered; a stale entry is caught by VirtualProtect's old value or a guarded copy.
enum class Access { Refuse, Write, Unlock };
struct Region {
    uintptr_t begin = 0, end = 0;
    Access access = Access::Refuse;
};
SRWLOCK g_write_lock = SRWLOCK_INIT;  // one EMF writer at a time (protect/restore pairs must not interleave)
Region g_regions[8];
size_t g_next_region = 0;

Access classify(const MEMORY_BASIC_INFORMATION& mbi) {
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD)) return Access::Refuse;
    switch (mbi.Protect & 0xff) {
        case PAGE_READWRITE:
        case PAGE_WRITECOPY: return Access::Write;
        case PAGE_READONLY: return mbi.Type == MEM_PRIVATE ? Access::Unlock : Access::Refuse;  // not EXE/DLL or views
        default: return Access::Refuse;  // code (PAGE_EXECUTE_*) or no access
    }
}

const Region& region_of(uintptr_t at) {
    for (const auto& r : g_regions)
        if (at >= r.begin && at < r.end) return r;
    MEMORY_BASIC_INFORMATION mbi{};
    Region r;
    if (VirtualQuery(reinterpret_cast<void*>(at), &mbi, sizeof(mbi))) {
        r.begin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        r.end = r.begin + mbi.RegionSize;
        r.access = classify(mbi);
    } else {
        r.begin = at;
        r.end = at + 1;
    }
    Region& slot = g_regions[g_next_region++ % std::size(g_regions)];
    slot = r;
    return slot;
}

void forget_regions() {
    for (auto& r : g_regions) r = Region{};
}
}  // namespace

namespace {
bool write_once(uintptr_t begin, uintptr_t end, const void* src) {
    constexpr uintptr_t kPage = 0x1000;
    bool ok = true;
    for (uintptr_t at = begin & ~(kPage - 1); ok && at < end; at += kPage)  // check every page first: all or nothing
        ok = region_of(at).access != Access::Refuse;
    for (uintptr_t at = begin; ok && at < end;) {
        const uintptr_t page_end = (at & ~(kPage - 1)) + kPage;
        const uintptr_t stop = page_end < end ? page_end : end;
        void* p = reinterpret_cast<void*>(at);
        const void* from = static_cast<const uint8_t*>(src) + (at - begin);  // begin: the first byte to write
        if (region_of(at).access == Access::Write) {
            ok = guarded_copy(p, from, stop - at);
        } else {  // read-only private data: writable just for the copy, then back to what it was
            DWORD old = 0, tmp = 0;
            ok = VirtualProtect(p, stop - at, PAGE_READWRITE, &old) != 0;
            if (ok && (old & 0xff) != PAGE_READONLY) {  // it changed since we looked: put it back, refuse
                VirtualProtect(p, stop - at, old, &tmp);
                ok = false;
            } else if (ok) {
                ok = guarded_copy(p, from, stop - at);
                VirtualProtect(p, stop - at, old, &tmp);
            }
        }
        at = stop;
    }
    return ok;
}
}  // namespace

bool write_game_memory(void* dst, const void* src, size_t n) {
    if (!dst || !src || !n) return false;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(dst), end = begin + n;
    if (end < begin) return false;
    AcquireSRWLockExclusive(&g_write_lock);
    bool ok = write_once(begin, end, src);
    if (!ok) {  // a remembered region may be stale (memory freed and reused, protection changed): look again once
        forget_regions();
        ok = write_once(begin, end, src);
        if (!ok) forget_regions();
    }
    ReleaseSRWLockExclusive(&g_write_lock);
    return ok;
}

bool set_max_stack(const uint8_t* registry, uint32_t id, uint16_t max) {
    if (!max) return false;
    uint8_t* info = item_data_ptr(registry, id);
    return info && write_game_memory(info + off::kItemInfoMaxStack, &max, sizeof(max));
}

std::optional<ItemRecord> item_by_name(const uint8_t* registry, std::string_view name) {
    if (name.empty()) return std::nullopt;
    std::optional<ItemRecord> loose;
    for (auto& r : list_items(registry)) {
        if (r.name == name) return r;
        if (!loose && iequal(r.name, name)) loose = r;
    }
    return loose;
}

}  // namespace emf::core
