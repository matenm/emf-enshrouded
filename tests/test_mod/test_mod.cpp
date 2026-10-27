// A minimal EMF mod used by the core tests.
#include "emf/emf_api.h"

static uint32_t g_seen_version = 0;

extern "C" __declspec(dllexport) bool emf_mod_load(const EmfApi* api) {
    g_seen_version = api ? api->version : 0;
    return true;
}

extern "C" __declspec(dllexport) uint32_t emf_test_mod_seen_version() { return g_seen_version; }
