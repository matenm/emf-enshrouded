#pragma once
#include <windows.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "emf/emf_api.h"

namespace emf::core {

class ModRegistry;
class SettingsStore;
struct ModRecord;

struct LoadedMod {
    std::wstring path;
    HMODULE module = nullptr;
    std::string id;
};

struct LoadOptions {
    SettingsStore* settings = nullptr;  // manifest settings are declared here; the "enabled" switch is read here
    ModRegistry* registry = nullptr;    // filled with every mod found and what became of it
    // Filled with every mod DLL that stays mapped: the loaded ones and those whose emf_mod_load returned false or
    // faulted (their callbacks may still run), e.g. for hiding keys from mods while the console is open.
    std::vector<HMODULE>* kept_loaded = nullptr;
    uint32_t api_version = EMF_API_VERSION;
};

// Loads the mods in `dir`: loose *.dll files (id = file name) and folder mods (<dir>\<folder>\mod.json naming a DLL in
// that folder, or a script for a script host). Dependencies load first (see mod_order.h). A mod whose "enabled" key
// is 0 (or whose manifest says "enabled": false and whose ini has no value) is not loaded. The manifest settings of
// every mod that can load, switched on or off, are declared before the first emf_mod_load. A script mod ("main") needs
// its script host mod (lua for a .lua file): the host loads first, and without it (missing, switched off, failed) the
// script mod and the mods that need it do not run. emf_mod_load runs under an SEH guard; a DLL without it is unloaded;
// mods that return false or fault stay mapped (they may have registered callbacks) but are not returned.
std::vector<LoadedMod> load_mods(const std::wstring& dir, const EmfApi* api,
                                 const std::function<void(const std::string&)>& report,
                                 const LoadOptions& options = {});

// Settings follow mod.json while the game runs: when a folder mod's mod.json changes, its settings are declared again,
// so a new setting shows in the Mods tab (and reads through the API) within a second, and a changed label, range,
// list, group or enabledWhen applies. A setting taken out of mod.json stays until the next start; values are kept when
// they still fit. Mods the loader rejected before declaring anything are left alone.
class ManifestWatch {
public:
    // Looks at every folder mod's mod.json (call about once a second; the first look only notes the files). Returns a
    // log line for each mod whose settings were declared again, or whose mod.json no longer reads.
    std::vector<std::string> poll(const std::wstring& dir, const std::vector<ModRecord>& mods, SettingsStore& store);

private:
    std::mutex m_;  // the server and client worlds' threads may both look at a session's start
    std::vector<std::pair<std::wstring, uint64_t>> seen_;  // mod.json -> its write time and size when last read
};

}  // namespace emf::core
