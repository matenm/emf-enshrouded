#pragma once
// Helpers for the sample mod's spawn test (F9/F11 spawn, F10 destroy).
#include <cstdint>
#include <string_view>

#include "emf/emf.hpp"

namespace sample {

// Text GUID -> 16 bytes (see emf::parse_guid).
inline bool parse_guid(std::string_view s, uint8_t* out, bool ms_layout) { return emf::parse_guid(s, out, ms_layout); }

// guid_layout from the ini (what GetPrivateProfileStringW gives): true for the Windows ("ms") layout. mod.json makes it a
// choice, which the core stores as its index (0 = ms, 1 = raw); "raw"/"text" from an older ini still read as raw.
inline bool guid_layout_ms(std::wstring_view v) {
    if (v == L"1") return false;
    return v.empty() || !(v[0] == L'r' || v[0] == L'R' || v[0] == L't' || v[0] == L'T');
}

// The key that triggers each spawn slot (slot 0 = spawn_guid, slot 1 = spawn_guid2).
inline const char* spawn_key(int slot) { return slot == 0 ? "F9" : "F11"; }

}  // namespace sample
