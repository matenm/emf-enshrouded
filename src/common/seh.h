#pragma once
#include <cstddef>

namespace emf {

// Copies n bytes from src to dst. Returns false (and copies nothing useful) if the read faults.
bool seh_read(const void* src, void* dst, size_t n);

}  // namespace emf
