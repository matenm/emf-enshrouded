#pragma once
// ';'-separated environment lists (VK_ADD_LAYER_PATH, VK_INSTANCE_LAYERS) the loader puts EMF's Vulkan layer into.
#include <cwctype>
#include <string>

namespace emf::loader {

// `list` with `item` as its first entry; unchanged when an entry equal to `item` (ignoring case) is already there.
inline std::wstring list_with_first(const std::wstring& list, const std::wstring& item) {
    const auto same = [&](size_t from, size_t to) {
        if (to - from != item.size()) return false;
        for (size_t i = 0; i < item.size(); ++i)
            if (std::towlower(list[from + i]) != std::towlower(item[i])) return false;
        return true;
    };
    for (size_t from = 0; from <= list.size();) {
        size_t to = list.find(L';', from);
        if (to == std::wstring::npos) to = list.size();
        if (same(from, to)) return list;
        from = to + 1;
    }
    return list.empty() ? item : item + L";" + list;
}

}  // namespace emf::loader
