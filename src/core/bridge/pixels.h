#pragma once
// The screenshot's pixels: a copied swapchain image in its own format turned into 8-bit sRGB BGR rows for the PNG,
// and scaled down when asked. Pure functions (unit-tested); vk_capture.cpp maps Vulkan formats and colour spaces here.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace emf::core::bridge {

enum class PixelFormat {
    BGRA8,        // B8G8R8A8 UNORM / SRGB
    RGBA8,        // R8G8B8A8 UNORM / SRGB
    A2B10G10R10,  // 10-bit, R in the low bits
    A2R10G10B10,  // 10-bit, B in the low bits
    RGBA16F,      // 16-bit float (scRGB when its colour space says so)
};

enum class Transfer {
    Srgb,         // the values are what an SDR screen shows (sRGB-encoded, or UNORM shown as is)
    Pq2020,       // HDR10: ST 2084 (PQ) over BT.2020 primaries
    LinearScrgb,  // scRGB: linear BT.709, 1.0 = SDR white
};

size_t bytes_per_pixel(PixelFormat f);
// Converts `height` rows of `width` pixels (`row_pitch` bytes apart) to tight 24-bit BGR, sRGB-encoded. HDR input
// (PQ, scRGB) is decoded to linear light, mapped to BT.709 and tone-mapped (SDR white stays white, highlights roll off)
// before the sRGB encoding. *hdr (optional) tells whether it was HDR.
std::vector<uint8_t> to_srgb_bgr(const uint8_t* src, size_t row_pitch, uint32_t width, uint32_t height, PixelFormat f,
                                 Transfer t, bool* hdr = nullptr);
// Averages a BGR picture down to at most max_width wide (the height in proportion). Returns it unchanged when it is
// not wider; *out_w / *out_h get the new size.
std::vector<uint8_t> scale_down_bgr(const std::vector<uint8_t>& bgr, uint32_t width, uint32_t height,
                                    uint32_t max_width, uint32_t* out_w, uint32_t* out_h);

// Helpers (tested): the PQ curve to linear light (1.0 = 10000 nits); a half float to float; the sRGB encoding of a
// linear value in [0, 1]; the tone map from relative linear light (1.0 = SDR white) to [0, 1].
double pq_to_linear(double encoded);
float half_to_float(uint16_t h);
double srgb_encode(double linear);
double tone_map(double relative);

}  // namespace emf::core::bridge
