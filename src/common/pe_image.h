#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace emf {

struct Section {
    std::string name;
    uintptr_t start = 0;
    size_t size = 0;
};

// Sections of a PE image mapped at `module_base` (an HMODULE works).
std::vector<Section> sections(const void* module_base);
std::optional<Section> section(const void* module_base, std::string_view name);
// Maps an exe/DLL file as an image (sections at their RVAs) read-only, apart from any copy already loaded, and keeps
// it mapped; 0 on failure. (LoadLibraryEx(AS_DATAFILE) would hand back the loaded module itself, hooks included.)
uintptr_t map_image_file(const wchar_t* path);

}  // namespace emf
