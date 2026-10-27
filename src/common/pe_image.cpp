#include "pe_image.h"

#include <windows.h>

namespace emf {

std::vector<Section> sections(const void* module_base) {
    std::vector<Section> out;
    if (!module_base) return out;
    auto base = reinterpret_cast<uintptr_t>(module_base);
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return out;
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return out;
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        char name[9] = {};
        memcpy(name, sec->Name, 8);
        out.push_back({name, base + sec->VirtualAddress, sec->Misc.VirtualSize});
    }
    return out;
}

std::optional<Section> section(const void* module_base, std::string_view name) {
    for (auto& s : sections(module_base))
        if (s.name == name) return s;
    return std::nullopt;
}

uintptr_t map_image_file(const wchar_t* path) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0;
    // SEC_IMAGE_NO_EXECUTE: laid out like a loaded image, but never executable and no loader work
    HANDLE section = CreateFileMappingW(file, nullptr, PAGE_READONLY | SEC_IMAGE_NO_EXECUTE, 0, 0, nullptr);
    CloseHandle(file);
    if (!section) return 0;
    void* view = MapViewOfFile(section, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(section);  // the view keeps the mapping alive
    return reinterpret_cast<uintptr_t>(view);
}

}  // namespace emf
