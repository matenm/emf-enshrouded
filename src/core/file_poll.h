#pragma once
// The files EMF looks at about once a second, on the tick of the world that runs commands (core.cpp): emf\commands.txt
// (lines typed into it run as console commands) and every folder mod's mod.json (ManifestWatch: settings follow it while
// the game runs). Separate from core.cpp so the wiring is unit-tested.
#include <cstdint>
#include <functional>
#include <string>

namespace emf::core {

class CommandCenter;
class CommandFile;
class ManifestWatch;
class ModRegistry;
class SettingsStore;

struct FilePoll {
    CommandFile* commands_file = nullptr;
    CommandCenter* commands = nullptr;
    ManifestWatch* manifests = nullptr;
    std::wstring mods_dir;  // emf\mods ("" = no manifests looked at)
    const ModRegistry* mods = nullptr;
    SettingsStore* settings = nullptr;
    std::function<void(const std::string&)> reply;  // a problem with commands.txt, once until it changes
    std::function<void(const std::string&)> log;    // a line about a mod's mod.json
    uint64_t last = 0;                               // GetTickCount64 of the last look (0: none yet)
    std::string last_problem;

    // Looks once at least a second has passed since the last look (the first call always looks). True when it looked.
    bool tick(uint64_t now);
};

}  // namespace emf::core
