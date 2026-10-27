// The screenshot's pixel conversions: 8-bit, 10-bit (both packings), HDR10 (PQ, BT.2020, tone-mapped) and scRGB half
// floats to 8-bit sRGB BGR, and scaling down.
#include <doctest.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "core/bridge/pixels.h"

using namespace emf::core::bridge;

namespace {
uint32_t pack_a2b10g10r10(uint32_t r, uint32_t g, uint32_t b) { return (3u << 30) | (b << 20) | (g << 10) | r; }
uint32_t pack_a2r10g10b10(uint32_t r, uint32_t g, uint32_t b) { return (3u << 30) | (r << 20) | (g << 10) | b; }
std::vector<uint8_t> words(std::initializer_list<uint32_t> w) {
    std::vector<uint8_t> out(w.size() * 4);
    std::memcpy(out.data(), w.begin(), out.size());
    return out;
}
uint16_t float_to_half(float f) {  // enough for the test values (normal numbers, no rounding cases)
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int exp = static_cast<int>((x >> 23) & 0xFF) - 127 + 15;
    if (exp <= 0) return static_cast<uint16_t>(sign);
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) | ((x >> 13) & 0x3FF));
}
}  // namespace

TEST_CASE("pixels: 8-bit BGRA and RGBA are taken as they are") {
    const uint8_t bgra[] = {10, 20, 30, 0, 40, 50, 60, 0};
    CHECK(to_srgb_bgr(bgra, 8, 2, 1, PixelFormat::BGRA8, Transfer::Srgb) == std::vector<uint8_t>{10, 20, 30, 40, 50, 60});
    CHECK(to_srgb_bgr(bgra, 8, 2, 1, PixelFormat::RGBA8, Transfer::Srgb) == std::vector<uint8_t>{30, 20, 10, 60, 50, 40});
    CHECK(bytes_per_pixel(PixelFormat::RGBA16F) == 8);
    CHECK(bytes_per_pixel(PixelFormat::A2B10G10R10) == 4);
}

TEST_CASE("pixels: 10-bit A2B10G10R10 (format 64) and A2R10G10B10 (58) become 8-bit, each in its own bit order") {
    bool hdr = true;
    auto px = words({pack_a2b10g10r10(1023, 512, 0)});  // R full, G half, B none
    CHECK(to_srgb_bgr(px.data(), 4, 1, 1, PixelFormat::A2B10G10R10, Transfer::Srgb, &hdr) ==
          std::vector<uint8_t>{0, 128, 255});
    CHECK_FALSE(hdr);
    px = words({pack_a2r10g10b10(1023, 512, 0)});
    CHECK(to_srgb_bgr(px.data(), 4, 1, 1, PixelFormat::A2R10G10B10, Transfer::Srgb) == std::vector<uint8_t>{0, 128, 255});
}

TEST_CASE("pixels: the PQ curve, the sRGB encoding and the tone map") {
    CHECK(pq_to_linear(0.0) == doctest::Approx(0.0));
    CHECK(pq_to_linear(1.0) == doctest::Approx(1.0));               // 10000 nits
    CHECK(pq_to_linear(0.5807) == doctest::Approx(0.0203).epsilon(0.02));  // about 203 nits (SDR white in HDR10)
    CHECK(srgb_encode(0.0) == doctest::Approx(0.0));
    CHECK(srgb_encode(1.0) == doctest::Approx(1.0));
    CHECK(srgb_encode(0.214) == doctest::Approx(0.5).epsilon(0.01));
    CHECK(tone_map(0.0) == doctest::Approx(0.0));
    CHECK(tone_map(1.0) > 0.6);    // SDR white stays bright
    CHECK(tone_map(1.0) <= 1.0);
    CHECK(tone_map(100.0) == doctest::Approx(1.0));  // highlights roll off to white, never past it
    CHECK(tone_map(4.0) <= 1.0);
    CHECK(half_to_float(0x3C00) == 1.0f);
    CHECK(half_to_float(0x3800) == 0.5f);
    CHECK(half_to_float(0xC000) == -2.0f);
    CHECK(half_to_float(0x0000) == 0.0f);
}

TEST_CASE("pixels: an HDR10 frame is decoded, tone-mapped and reported as HDR") {
    // PQ code for about 203 nits (SDR white) in all three channels, and black
    const uint32_t white = static_cast<uint32_t>(std::lround(0.5807 * 1023));
    auto px = words({pack_a2b10g10r10(white, white, white), pack_a2b10g10r10(0, 0, 0)});
    bool hdr = false;
    const auto bgr = to_srgb_bgr(px.data(), 8, 2, 1, PixelFormat::A2B10G10R10, Transfer::Pq2020, &hdr);
    CHECK(hdr);
    REQUIRE(bgr.size() == 6);
    CHECK(bgr[0] == bgr[1]);  // grey stays grey through the BT.2020 to BT.709 matrix
    CHECK(bgr[1] == bgr[2]);
    CHECK(bgr[0] > 200);      // SDR white is bright
    CHECK(bgr[3] == 0);       // black is black
}

TEST_CASE("pixels: an scRGB half-float frame is tone-mapped; negative values clip to black") {
    std::vector<uint8_t> px(16);
    const uint16_t one = float_to_half(1.0f), neg = float_to_half(-0.5f), quarter = float_to_half(0.25f);
    const uint16_t a[8] = {one, one, one, one, neg, quarter, neg, one};
    std::memcpy(px.data(), a, 16);
    bool hdr = false;
    const auto bgr = to_srgb_bgr(px.data(), 16, 2, 1, PixelFormat::RGBA16F, Transfer::LinearScrgb, &hdr);
    CHECK(hdr);
    REQUIRE(bgr.size() == 6);
    CHECK(bgr[0] > 200);
    CHECK(bgr[3] == 0);  // B of pixel 2: negative
    CHECK(bgr[4] > 0);   // G of pixel 2: 0.25
    CHECK(bgr[5] == 0);  // R of pixel 2: negative
}

TEST_CASE("pixels: scaling down averages blocks and keeps the aspect ratio") {
    // 4x2: left half white, right half black
    std::vector<uint8_t> bgr;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 4; ++x)
            for (int c = 0; c < 3; ++c) bgr.push_back(x < 2 ? 255 : 0);
    uint32_t w = 0, h = 0;
    const auto small = scale_down_bgr(bgr, 4, 2, 2, &w, &h);
    CHECK(w == 2);
    CHECK(h == 1);
    REQUIRE(small.size() == 6);
    CHECK(small[0] == 255);
    CHECK(small[3] == 0);
    const auto same = scale_down_bgr(bgr, 4, 2, 4000, &w, &h);
    CHECK(w == 4);
    CHECK(h == 2);
    CHECK(same == bgr);
}
