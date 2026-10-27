#include "preview_convert.h"

namespace mycam {

namespace {
inline uint8_t Clamp8(int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }
} // namespace

void Nv12ToBgraHalf(const uint8_t* nv12, uint32_t outWidth, uint32_t outHeight, uint8_t* bgra) {
    const size_t srcW = size_t(outWidth) * 2, srcH = size_t(outHeight) * 2;
    const uint8_t* uvPlane = nv12 + srcW * srcH;
    for (uint32_t y = 0; y < outHeight; ++y) {
        const uint8_t* row0 = nv12 + (size_t(y) * 2) * srcW;
        const uint8_t* row1 = row0 + srcW;
        const uint8_t* uv = uvPlane + size_t(y) * srcW; // Chroma row y covers luma rows 2y and 2y+1.
        uint8_t* out = bgra + size_t(y) * outWidth * 4;
        for (uint32_t x = 0; x < outWidth; ++x) {
            const int luma = (row0[2 * x] + row0[2 * x + 1] + row1[2 * x] + row1[2 * x + 1] + 2) / 4;
            const int c = 298 * (luma - 16);
            const int d = uv[2 * x] - 128, e = uv[2 * x + 1] - 128;
            out[4 * x + 0] = Clamp8((c + 516 * d + 128) >> 8);
            out[4 * x + 1] = Clamp8((c - 100 * d - 208 * e + 128) >> 8);
            out[4 * x + 2] = Clamp8((c + 409 * e + 128) >> 8);
            out[4 * x + 3] = 255;
        }
    }
}

} // namespace mycam
