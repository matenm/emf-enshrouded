#include "key_list.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>

#include "emf/keys.hpp"
#include "manifest.h"

namespace emf::core {

uint32_t scan_to_vk(unsigned scan) {
    switch (scan) {  // the numpad's scan codes 0x47-0x53 (not extended)
        case 0x47: return VK_NUMPAD7;
        case 0x48: return VK_NUMPAD8;
        case 0x49: return VK_NUMPAD9;
        case 0x4A: return VK_SUBTRACT;
        case 0x4B: return VK_NUMPAD4;
        case 0x4C: return VK_NUMPAD5;
        case 0x4D: return VK_NUMPAD6;
        case 0x4E: return VK_ADD;
        case 0x4F: return VK_NUMPAD1;
        case 0x50: return VK_NUMPAD2;
        case 0x51: return VK_NUMPAD3;
        case 0x52: return VK_NUMPAD0;
        case 0x53: return VK_DECIMAL;
        default: return MapVirtualKeyW(scan, MAPVK_VSC_TO_VK);
    }
}

size_t KeyGroup::on_uses() const {
    size_t n = 0;
    for (const auto& u : uses) n += u.on;
    return n;
}

bool KeyGroup::clash() const {
    bool not_shared = false;
    for (const auto& u : uses) not_shared |= u.on && !u.shared;
    return on_uses() >= 2 && not_shared;
}

namespace {
// "F2" before "F10": runs of digits compare by value, the rest by letter in any case.
bool name_before(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const bool da = a[i] >= '0' && a[i] <= '9', db = b[j] >= '0' && b[j] <= '9';
        if (da && db) {
            size_t ei = i, ej = j;
            while (ei < a.size() && a[ei] >= '0' && a[ei] <= '9') ++ei;
            while (ej < b.size() && b[ej] >= '0' && b[ej] <= '9') ++ej;
            const std::string na = a.substr(i, ei - i), nb = b.substr(j, ej - j);
            const size_t za = na.find_first_not_of('0'), zb = nb.find_first_not_of('0');
            const std::string va = za == std::string::npos ? "" : na.substr(za);
            const std::string vb = zb == std::string::npos ? "" : nb.substr(zb);
            if (va.size() != vb.size()) return va.size() < vb.size();
            if (va != vb) return va < vb;
            i = ei;
            j = ej;
            continue;
        }
        const char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
        const char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[j])));
        if (ca != cb) return ca < cb;
        ++i;
        ++j;
    }
    return a.size() - i < b.size() - j;
}
}  // namespace

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
        return name_before(a.name, b.name);
    });
    return out;
}

}  // namespace emf::core
