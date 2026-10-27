#pragma once
// The bridge's screenshots: where they go (only <emf>\screens\<time stamp>.png, never a path from a client), turning a
// copied swapchain image into a PNG (Windows' own WIC encoder, loaded on demand), and writing it as a new file.
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace emf::core::bridge {

// "2026-10-26_20-15-00-123.png"; n > 1 adds "_n" (another picture in the same millisecond).
std::wstring screenshot_name(const SYSTEMTIME& t, unsigned n);
// True if `path` names a .png file directly inside <emf_dir>\screens: no other folder, no "..", no ':' stream or
// drive, no '/'. The folder is compared ignoring case.
bool screenshot_path_ok(const std::wstring& emf_dir, const std::wstring& path);

enum class PixelOrder { BGRA, RGBA };
// Packs `height` rows of `width` 4-byte pixels (`row_pitch` bytes apart, in `order`) into tight 24-bit BGR rows, the
// alpha left out (a swapchain's alpha is not what the screen shows).
std::vector<uint8_t> pack_bgr(const uint8_t* src, size_t row_pitch, uint32_t width, uint32_t height, PixelOrder order);
// Encodes tight 24-bit BGR as PNG with WIC (ole32 is loaded on first use). Empty and *why on a failure.
std::vector<uint8_t> encode_png_bgr(const uint8_t* bgr, uint32_t width, uint32_t height, std::string* why);
// Writes `bytes` as the new file `path` (screenshot_path_ok must hold): creates <emf_dir>\screens if needed, refuses a
// screens folder that is a link (junction or symbolic link), never overwrites. False and *why on a failure.
bool write_screenshot(const std::wstring& emf_dir, const std::wstring& path, const std::vector<uint8_t>& bytes,
                      std::string* why);

}  // namespace emf::core::bridge
