#pragma once
// The order mods load in: every dependency before the mods that need it, otherwise by name (ignoring case). Mods
// whose dependencies are missing, the wrong version, rejected themselves or form a cycle, mods needing a newer EMF
// API, and a second mod with an id already taken are rejected with a reason. An optional dependency that is not
// installed (or not loaded) does not stop a mod; when it is installed its version must still fit, and it loads first.
// Pure logic (unit-tested); EMF's core and EMF.exe both use it, so EMF.exe's "Will not load" says what the core does.
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "manifest.h"

namespace emf::core {

struct ModCandidate {
    std::string id;
    Version version;
    std::vector<Dependency> depends;
    uint32_t emf = 0;       // lowest API version it needs
    std::string sort_name;  // folder or file name: the order among independent mods
};

struct OrderResult {
    std::vector<size_t> order;                             // indexes into the candidates, in load order
    std::vector<std::pair<size_t, std::string>> rejected;  // index, why
    std::vector<std::pair<size_t, std::string>> notes;     // index, something worth a log line (the mod still loads)
};

OrderResult order_mods(const std::vector<ModCandidate>& mods, uint32_t api_version);

// A script mod's dependencies with its script host added ("" = none): required, also when the mod listed it as optional.
std::vector<Dependency> with_script_host(std::vector<Dependency> deps, const std::string& host);

// A dependency as players see it: "lua ^1.3", "helper (optional)", "lua".
std::string dependency_text(const Dependency& d);

// What became of a mod earlier in the load order, for the mods after it.
enum class ModFate { Runs, Off, NotLoaded };
// Why a mod cannot run once the mods before it have loaded or not: a required dependency (or `host`, a script mod's
// script host) that is switched off or did not load, or an optional one that runs in a version outside its range (an
// optional one that is off or missing never stops it). `fate(id)` gives a mod's fate, nullopt for a mod not installed
// or not decided; `version_of(id)` its version (nullopt: unknown, not checked). "" when nothing stops it.
std::string unmet_dependency(const std::vector<Dependency>& deps, const std::string& host,
                             const std::function<std::optional<ModFate>(const std::string&)>& fate,
                             const std::function<std::optional<Version>(const std::string&)>& version_of);

}  // namespace emf::core
