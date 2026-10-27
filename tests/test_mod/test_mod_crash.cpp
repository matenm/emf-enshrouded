// A broken EMF mod: faults inside emf_mod_load. The core must survive and report it.
#include "emf/emf_api.h"

extern "C" __declspec(dllexport) bool emf_mod_load(const EmfApi*) {
    *static_cast<volatile int*>(nullptr) = 1;
    return true;
}
