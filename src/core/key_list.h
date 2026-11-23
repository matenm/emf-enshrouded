#pragma once
// Every key EMF and its mods use, by key (GitHub #55): what uses each one, and the keys two switched-on users share
// (then one press does both). Pure (the caller reads the settings); built into the core and EMF.exe.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace emf::core {

struct KeyUse {
    std::string owner;       // "emf" or a mod's id
    std::string owner_name;  // what the Mods tab calls it ("EMF", "Night Raids")
    std::string label;       // the key setting's label
    std::string key;         // its value as the settings store keeps it ("F3", "0xc0"; "0x29" for a scan code)
    bool scan = false;       // `key` is a scan code (EMF's console key), not a key's name
    bool on = true;          // its owner is switched on
};

struct KeyGroup {
    std::string name;           // the key's name ("F3", "the key under Esc (0x29)")
    std::vector<KeyUse> uses;   // in the order given
    size_t on_uses() const;     // the uses whose owner is switched on
    bool clash() const { return on_uses() >= 2; }
};

// The keys in `uses` grouped by the key they are (compared as the key settings read them: "f3" is F3), empty keys left
// out; the clashes first, then by name. `scan_to_vk` gives the virtual key a scan code is on this keyboard (Windows:
// MapVirtualKeyW(scan, MAPVK_VSC_TO_VK)), so the console key groups with a mod's key on the same physical key.
std::vector<KeyGroup> group_keys(const std::vector<KeyUse>& uses, const std::function<uint32_t(unsigned)>& scan_to_vk);

}  // namespace emf::core
