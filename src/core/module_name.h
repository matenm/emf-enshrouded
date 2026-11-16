#pragma once
#include <string>

namespace emf::core {

// The file name without its extension of the module (DLL/EXE) that contains `addr`, e.g. "my_mod", or "unknown":
// the name a mod's API calls go under (hook owners, marker owners). UTF-8.
std::string module_stem_of(const void* addr);

}  // namespace emf::core
