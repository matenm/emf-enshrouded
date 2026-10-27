// An EMF mod that registers a tick callback, then returns false from emf_mod_load.
// The core must keep it mapped: the callback it registered still points into this DLL.
#include "emf/emf_api.h"

static int g_ticks = 0;
static void on_tick(EmfWorld*, void*) { ++g_ticks; }

extern "C" __declspec(dllexport) bool emf_mod_load(const EmfApi* api) {
    if (api && api->on_tick) api->on_tick(on_tick, nullptr);
    return false;
}

extern "C" __declspec(dllexport) int emf_test_mod_reject_ticks() { return g_ticks; }
