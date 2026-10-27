// A test mod that imports a helper DLL from its own folder: it only loads when the loader searches the mod's folder.
#include "emf/emf_api.h"

extern "C" __declspec(dllimport) int emf_test_helper_value();

extern "C" __declspec(dllexport) bool emf_mod_load(const EmfApi* api) {
    if (api && api->log) api->log("test_needs_helper", "load needs_helper %d", emf_test_helper_value());
    return true;
}
