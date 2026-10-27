#pragma once
#include <cstddef>
#include <string>

namespace emf {

// Lowercase hex SHA-256 (Windows CNG). Empty string on failure.
std::string sha256_hex(const void* data, size_t size);
std::string sha256_file(const std::wstring& path);

}  // namespace emf
