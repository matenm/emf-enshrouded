#pragma once
// Helpers shared by src/saves/*.cpp (implemented in backup.cpp). Not part of the saves API.
#include <optional>
#include <string>
#include <string_view>

#include "saves/backup.h"

namespace emf::core::json {
struct Value;
}

namespace emf::saves::detail {

std::wstring wide(std::string_view s);
std::string utf8(const std::wstring& w);
// A JSON string literal; bytes that are not UTF-8 become U+FFFD.
std::string quote(const std::string& s);
// One slot info as a JSON object on one line, after `indent`.
std::string slot_info_json(const SlotInfo& i, const char* indent);
// A slot info from a JSON object; nullopt for one EMF does not accept (unknown storage or type, no slot).
std::optional<SlotInfo> slot_info_from(const core::json::Value& v);
// The file's bytes (at most `max`), read with every share flag; false when it cannot be read or is bigger.
bool read_small(const std::wstring& path, std::string& out, size_t max = 8u << 20);
// Writes the file whole and flushes it; false on any failure.
bool write_whole(const std::wstring& path, const std::string& bytes);

}  // namespace emf::saves::detail
