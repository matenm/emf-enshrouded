#pragma once
// The sample mod's item list export (emf/items.csv): the game's items with their ids, GUIDs and stack sizes.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "emf/emf.hpp"

namespace sample {

inline std::string csv_field(const std::string& s) {
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string q = "\"";
    for (char c : s) {
        if (c == '"') q += '"';
        q += c;
    }
    return q + "\"";
}

// "name,item_id,guid,max_stack" lines sorted by name (GUIDs in the text form parse_guid reads).
inline std::string items_csv(std::vector<EmfItemInfo> items) {
    std::sort(items.begin(), items.end(),
              [](const EmfItemInfo& a, const EmfItemInfo& b) { return std::string(a.name) < std::string(b.name); });
    std::string out = "name,item_id,guid,max_stack\n";
    char id[16];
    for (const auto& i : items) {
        std::snprintf(id, sizeof(id), "0x%08x", i.item_id);
        out += csv_field(i.name) + "," + id + "," + emf::guid_to_text(i.guid) + "," + std::to_string(i.max_stack) + "\n";
    }
    return out;
}

}  // namespace sample
