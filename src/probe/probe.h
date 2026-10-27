#pragma once
#include <array>
#include <cstdint>

#include "common/signatures.h"

namespace emf::probe {

// Installs the probe hooks. Called once from the init thread after signatures resolved.
void start(const std::array<Resolved, kSigCount>& sigs, bool known_build);
// Disables hooks (only when the DLL is unloaded with FreeLibrary, not at process exit).
void stop();

}  // namespace emf::probe
