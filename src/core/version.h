#pragma once
// EMF's version, from the repository's VERSION file (CMake passes it as EMF_VERSION).
#ifndef EMF_VERSION
#error "EMF_VERSION must be defined by the build (see CMakeLists.txt)"
#endif

namespace emf::core {
constexpr const char* version() { return EMF_VERSION; }
constexpr const char* version_string() { return "emf_core " EMF_VERSION; }
}  // namespace emf::core
