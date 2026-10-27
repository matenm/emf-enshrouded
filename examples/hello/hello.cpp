// The first mod of docs/modding/README.md (kept compiling by the repository build).
#include <windows.h>  // GetTickCount64

#include "emf/emf.hpp"
#include "emf/generated/components.hpp"

namespace gen = emf::gen::ecs;

extern "C" __declspec(dllexport) bool emf_mod_load(const EmfApi* api) {
    if (!emf::init(api)) return false;        // refuses (and says why in the log) if the core is too old

    // A console command: type "hello.where" into the in-game console (the key under Esc) or emf\commands.txt.
    emf::add_command("hello.where", ": where am I?", [](emf::World w, const std::vector<std::string>&) {
        auto* t = w.get<gen::CurrentTransform>(emf::local_player());
        if (!t) {
            EMF_REPLY("no player yet");
            return;
        }
        EMF_REPLY("you are at %.1f, %.1f, %.1f m", t->transform.position.x / 4294967296.0,
                  t->transform.position.y / 4294967296.0, t->transform.position.z / 4294967296.0);
    });

    // Something every second, on the host's server world.
    emf::on_tick([](emf::World w) {
        static uint64_t last = 0;
        if (!w.is_server() || GetTickCount64() - last < 1000) return;
        last = GetTickCount64();
        // ... read components, spawn, give items ...
    });

    EMF_LOG("hello", "loaded");
    return true;
}
