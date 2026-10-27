#include "pixels.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace emf::core::bridge {

namespace {

constexpr double kSdrWhiteNits = 203.0;  // ITU-R BT.2408: SDR white in an HDR signal

// BT.2020 to BT.709 primaries, linear light.
constexpr double kToBt709[3][3] = {{1.660491, -0.587641, -0.072850},
                                   {-0.124550, 1.132900, -0.008349},
                                   {-0.018151, -0.100579, 1.118730}};

uint8_t to_byte(double v01) {
    const double v = std::clamp(v01, 0.0, 1.0) * 255.0 + 0.5;
    return static_cast<uint8_t>(v);
}

// The sRGB encoding of [0, 1] in 4096 steps (the HDR paths call it per channel).
const std::array<uint8_t, 4097>& srgb_lut() {
    static const std::array<uint8_t, 4097> lut = [] {
        std::array<uint8_t, 4097> t{};
        for (size_t i = 0; i < t.size(); ++i) t[i] = to_byte(srgb_encode(static_cast<double>(i) / 4096.0));
        return t;
    }();
    return lut;
}

uint8_t encode_mapped(double relative) {
    const double v = tone_map(std::max(relative, 0.0));
    return srgb_lut()[static_cast<size_t>(std::clamp(v, 0.0, 1.0) * 4096.0 + 0.5)];
}

// Linear BT.709 light relative to SDR white, to sRGB bytes (B, G, R).
void write_hdr(double r, double g, double b, uint8_t* out) {
    out[0] = encode_mapped(b);
    out[1] = encode_mapped(g);
    out[2] = encode_mapped(r);
}

}  // namespace

size_t bytes_per_pixel(PixelFormat f) { return f == PixelFormat::RGBA16F ? 8 : 4; }

double pq_to_linear(double e) {
    constexpr double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
    constexpr double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
    const double p = std::pow(std::clamp(e, 0.0, 1.0), 1.0 / m2);
    return std::pow(std::max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}

float half_to_float(uint16_t h) {
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FFu;
    uint32_t bits = 0;
    if (exp == 0) {
        if (mant) {  // subnormal: normalise
            exp = 127 - 15 + 1;
            while (!(mant & 0x400u)) {
                mant <<= 1;
                --exp;
            }
            mant &= 0x3FFu;
            bits = sign | (exp << 23) | (mant << 13);
        } else {
            bits = sign;
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);  // infinity, NaN
    } else {
        bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

double srgb_encode(double v) {
    v = std::clamp(v, 0.0, 1.0);
    return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
}

double tone_map(double x) {
    // Linear up to a knee, then a smooth roll-off towards white (the slope stays 1 at the knee): SDR white stays almost
    // white, highlights compress, nothing passes 1.
    constexpr double k = 0.8;
    if (!(x > k)) return std::max(x, 0.0);
    return k + (1.0 - k) * (1.0 - std::exp(-(x - k) / (1.0 - k)));
}

std::vector<uint8_t> to_srgb_bgr(const uint8_t* src, size_t row_pitch, uint32_t width, uint32_t height, PixelFormat f,
                                 Transfer t, bool* hdr) {
    std::vector<uint8_t> out(static_cast<size_t>(width) * height * 3);
    const bool is_hdr = (t == Transfer::Pq2020 && (f == PixelFormat::A2B10G10R10 || f == PixelFormat::A2R10G10B10)) ||
                        (t == Transfer::LinearScrgb && f == PixelFormat::RGBA16F);
    if (hdr) *hdr = is_hdr;
    std::array<double, 1024> pq{};  // 10-bit PQ code -> linear light relative to SDR white
    if (is_hdr && f != PixelFormat::RGBA16F)
        for (size_t i = 0; i < pq.size(); ++i) pq[i] = pq_to_linear(static_cast<double>(i) / 1023.0) * 10000.0 / kSdrWhiteNits;
    uint8_t* d = out.data();
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = src + static_cast<size_t>(y) * row_pitch;
        for (uint32_t x = 0; x < width; ++x, d += 3) {
            switch (f) {
                case PixelFormat::BGRA8: {
                    const uint8_t* s = row + x * 4;
                    d[0] = s[0];
                    d[1] = s[1];
                    d[2] = s[2];
                    break;
                }
                case PixelFormat::RGBA8: {
                    const uint8_t* s = row + x * 4;
                    d[0] = s[2];
                    d[1] = s[1];
                    d[2] = s[0];
                    break;
                }
                case PixelFormat::A2B10G10R10:
                case PixelFormat::A2R10G10B10: {
                    uint32_t w;
                    std::memcpy(&w, row + x * 4, 4);
                    const uint32_t lo = w & 0x3FF, mid = (w >> 10) & 0x3FF, hi = (w >> 20) & 0x3FF;
                    const uint32_t r = f == PixelFormat::A2B10G10R10 ? lo : hi;
                    const uint32_t b = f == PixelFormat::A2B10G10R10 ? hi : lo;
                    if (!is_hdr) {
                        d[0] = static_cast<uint8_t>((b * 255 + 511) / 1023);
                        d[1] = static_cast<uint8_t>((mid * 255 + 511) / 1023);
                        d[2] = static_cast<uint8_t>((r * 255 + 511) / 1023);
                    } else {
                        const double R = pq[r], G = pq[mid], B = pq[b];
                        write_hdr(kToBt709[0][0] * R + kToBt709[0][1] * G + kToBt709[0][2] * B,
                                  kToBt709[1][0] * R + kToBt709[1][1] * G + kToBt709[1][2] * B,
                                  kToBt709[2][0] * R + kToBt709[2][1] * G + kToBt709[2][2] * B, d);
                    }
                    break;
                }
                case PixelFormat::RGBA16F: {
                    uint16_t h[4];
                    std::memcpy(h, row + x * 8, 8);
                    const double r = half_to_float(h[0]), g = half_to_float(h[1]), b = half_to_float(h[2]);
                    if (is_hdr) {
                        write_hdr(r, g, b, d);  // scRGB: 1.0 = SDR white already
                    } else {                    // linear values on an SDR swapchain: encode them
                        d[0] = srgb_lut()[static_cast<size_t>(std::clamp(b, 0.0, 1.0) * 4096.0 + 0.5)];
                        d[1] = srgb_lut()[static_cast<size_t>(std::clamp(g, 0.0, 1.0) * 4096.0 + 0.5)];
                        d[2] = srgb_lut()[static_cast<size_t>(std::clamp(r, 0.0, 1.0) * 4096.0 + 0.5)];
                    }
                    break;
                }
            }
        }
    }
    return out;
}

std::vector<uint8_t> scale_down_bgr(const std::vector<uint8_t>& bgr, uint32_t width, uint32_t height,
                                    uint32_t max_width, uint32_t* out_w, uint32_t* out_h) {
    if (!max_width || width <= max_width || !height) {
        *out_w = width;
        *out_h = height;
        return bgr;
    }
    const uint32_t tw = max_width;
    const uint32_t th = (std::max)(1u, static_cast<uint32_t>((static_cast<uint64_t>(height) * tw + width / 2) / width));
    std::vector<uint8_t> out(static_cast<size_t>(tw) * th * 3);
    for (uint32_t y = 0; y < th; ++y) {
        const uint32_t y0 = static_cast<uint32_t>(static_cast<uint64_t>(y) * height / th);
        const uint32_t y1 = (std::max)(y0 + 1, static_cast<uint32_t>(static_cast<uint64_t>(y + 1) * height / th));
        for (uint32_t x = 0; x < tw; ++x) {
            const uint32_t x0 = static_cast<uint32_t>(static_cast<uint64_t>(x) * width / tw);
            const uint32_t x1 = (std::max)(x0 + 1, static_cast<uint32_t>(static_cast<uint64_t>(x + 1) * width / tw));
            uint64_t sum[3] = {0, 0, 0};
            for (uint32_t sy = y0; sy < y1; ++sy)
                for (uint32_t sx = x0; sx < x1; ++sx)
                    for (int c = 0; c < 3; ++c) sum[c] += bgr[(static_cast<size_t>(sy) * width + sx) * 3 + c];
            const uint64_t n = static_cast<uint64_t>(y1 - y0) * (x1 - x0);
            for (int c = 0; c < 3; ++c)
                out[(static_cast<size_t>(y) * tw + x) * 3 + c] = static_cast<uint8_t>((sum[c] + n / 2) / n);
        }
    }
    *out_w = tw;
    *out_h = th;
    return out;
}

}  // namespace emf::core::bridge
