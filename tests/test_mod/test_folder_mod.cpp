// A test mod copied into folder mods by tests/test_mod_loading.cpp: its emf_mod_load logs "load <folder>" through the
// API's log function, so the test sees which copies loaded and in which order.
#include <windows.h>

#include <string>

#include "emf/emf_api.h"

extern "C" __declspec(dllexport) bool emf_mod_load(const EmfApi* api) {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&emf_mod_load), &self);
    char path[MAX_PATH] = {};
    GetModuleFileNameA(self, path, MAX_PATH);
    std::string p(path);
    p = p.substr(0, p.find_last_of("\\/"));
    const std::string folder = p.substr(p.find_last_of("\\/") + 1);
    if (api && api->log) api->log("test_folder_mod", "load %s", folder.c_str());
    return true;
}
