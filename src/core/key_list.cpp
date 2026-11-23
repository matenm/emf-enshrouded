#include "key_list.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <map>

#include "emf/keys.hpp"
#include "manifest.h"

namespace emf::core {

size_t KeyGroup::on_uses() const {
    size_t n = 0;
    for (const auto& u : uses) n += u.on;
    return n;
}

std::vector<KeyGroup> group_keys(const std::vector<KeyUse>& uses, const std::function<uint32_t(unsigned)>& scan_to_vk) {
    std::vector<KeyGroup> out;
    std::map<std::string, size_t> by_id;  // "vk:<n>" or "text:<key>" (a key that names no virtual key) -> index
    for (const auto& u : uses) {
        if (u.key.empty()) continue;  // no key: nothing in use
        uint32_t vk = 0;
        std::string name;
        if (u.scan) {
            char* end = nullptr;
            const unsigned long sc = std::strtoul(u.key.c_str(), &end, 0);
            if (end && !*end && sc >= 1 && sc <= 0x7F && scan_to_vk) vk = scan_to_vk(static_cast<unsigned>(sc));
            name = scan_code_shown(u.key);
        } else {
            vk = emf::keys::parse_key(std::wstring(u.key.begin(), u.key.end()), &name);
            if (!vk) name = u.key;
        }
        const std::string id = vk ? "vk:" + std::to_string(vk) : "text:" + u.key;
        auto it = by_id.find(id);
        if (it == by_id.end()) {
            it = by_id.emplace(id, out.size()).first;
            out.push_back(KeyGroup{name, {}});
        } else if (u.scan) {
            out[it->second].name = name;  // the console key's name says where it is, whatever the layout
        }
        out[it->second].uses.push_back(u);
    }
    std::stable_sort(out.begin(), out.end(), [](const KeyGroup& a, const KeyGroup& b) {
        if (a.clash() != b.clash()) return a.clash();
        return a.name < b.name;
    });
    return out;
}

}  // namespace emf::core
