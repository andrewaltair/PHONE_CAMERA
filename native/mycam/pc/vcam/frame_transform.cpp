#include "frame_transform.h"

#include <string.h>

#include <algorithm>
#include <vector>

namespace mycam {

namespace {

// Maps pixel (u, v) of the rotated image back to (sx, sy) in the original w x h image.
inline void Inverse(uint32_t rot, int u, int v, int w, int h, int* sx, int* sy) {
    switch (rot) {
    case 90:  *sx = v;         *sy = h - 1 - u; break;
    case 180: *sx = w - 1 - u; *sy = h - 1 - v; break;
    case 270: *sx = w - 1 - v; *sy = u;         break;
    default:  *sx = u;         *sy = v;         break;
    }
}

} // namespace

void FillBlackNV12(uint8_t* dst, ptrdiff_t pitch, uint32_t width, uint32_t height) {
    for (uint32_t y = 0; y < height; ++y) memset(dst + y * pitch, 16, width);
    uint8_t* uv = dst + pitch * height;
    for (uint32_t y = 0; y < height / 2; ++y) memset(uv + y * pitch, 128, width);
}

void DrawFittedNV12(const uint8_t* src, uint32_t srcWidth, uint32_t srcHeight, uint32_t rotation, bool mirror,
                    uint8_t* dst, ptrdiff_t pitch, uint32_t width, uint32_t height, bool fill) {
    if (rotation != 90 && rotation != 180 && rotation != 270) rotation = 0;
    const int w = int(srcWidth), h = int(srcHeight);
    const bool swap = rotation == 90 || rotation == 270;
    const int ew = swap ? h : w, eh = swap ? w : h; // Size after rotation.
    const int W = int(width), H = int(height);

    // Fit (letterbox) or fill (cover, cropping the overflow) the rotated image, centred, keeping even
    // offsets and sizes for NV12. In fill mode dw/dh exceed the output and x0/y0 go negative.
    double scale = fill ? std::max(double(W) / ew, double(H) / eh) : std::min(double(W) / ew, double(H) / eh);
    int dw = std::max(2, int(ew * scale) & ~1), dh = std::max(2, int(eh * scale) & ~1);
    int x0 = ((W - dw) / 2) & ~1, y0 = ((H - dh) / 2) & ~1;
    // The part of the scaled image that lands inside the output.
    const int xa = std::max(0, x0), xb = std::min(W, x0 + dw), ya = std::max(0, y0), yb = std::min(H, y0 + dh);
    if (xa >= xb || ya >= yb) return;

    // Luma: precompute rotated-image coordinates for each visible output column/row.
    std::vector<int> xmap(xb - xa), ymap(yb - ya);
    for (int x = xa; x < xb; ++x) {
        int u = std::min(ew - 1, int((x - x0) * double(ew) / dw));
        xmap[x - xa] = mirror ? ew - 1 - u : u;
    }
    for (int y = ya; y < yb; ++y) ymap[y - ya] = std::min(eh - 1, int((y - y0) * double(eh) / dh));

    for (int y = ya; y < yb; ++y) {
        uint8_t* row = dst + y * pitch;
        int v = ymap[y - ya];
        for (int x = xa; x < xb; ++x) {
            int sx, sy;
            Inverse(rotation, xmap[x - xa], v, w, h, &sx, &sy);
            row[x] = src[sy * w + sx];
        }
    }

    // Chroma at half resolution, in UV-sample units of the source (w/2 x h/2).
    const uint8_t* srcUV = src + size_t(w) * h;
    const int cw = w / 2, ch = h / 2, cew = ew / 2, ceh = eh / 2, cdw = dw / 2, cdh = dh / 2;
    const int cx0 = x0 / 2, cy0 = y0 / 2;
    uint8_t* dstUV = dst + pitch * height;
    for (int y = ya / 2; y < yb / 2; ++y) {
        uint8_t* row = dstUV + y * pitch;
        int v = std::min(ceh - 1, int((y - cy0) * double(ceh) / cdh));
        for (int x = xa / 2; x < xb / 2; ++x) {
            int u = std::min(cew - 1, int((x - cx0) * double(cew) / cdw));
            if (mirror) u = cew - 1 - u;
            int sx, sy;
            Inverse(rotation, u, v, cw, ch, &sx, &sy);
            const uint8_t* s = srcUV + sy * w + sx * 2;
            row[x * 2] = s[0];
            row[x * 2 + 1] = s[1];
        }
    }
}

void BgraToNV12(const uint8_t* bgra, ptrdiff_t stride, uint32_t width, uint32_t height, uint8_t* dst) {
    uint8_t* y = dst;
    uint8_t* uv = dst + size_t(width) * height;
    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t* p = bgra + row * stride;
        for (uint32_t x = 0; x < width; ++x, p += 4) {
            y[row * width + x] = uint8_t(((66 * p[2] + 129 * p[1] + 25 * p[0] + 128) >> 8) + 16);
        }
    }
    // Chroma from the average of each 2x2 block.
    for (uint32_t row = 0; row < height / 2; ++row) {
        const uint8_t* a = bgra + (2 * row) * stride;
        const uint8_t* b = a + stride;
        for (uint32_t x = 0; x < width / 2; ++x) {
            const uint8_t* p[4] = {a + 8 * x, a + 8 * x + 4, b + 8 * x, b + 8 * x + 4};
            int r = 0, g = 0, bl = 0;
            for (const uint8_t* q : p) { r += q[2]; g += q[1]; bl += q[0]; }
            r = (r + 2) / 4; g = (g + 2) / 4; bl = (bl + 2) / 4;
            uv[row * width + 2 * x] = uint8_t(((-38 * r - 74 * g + 112 * bl + 128) >> 8) + 128);
            uv[row * width + 2 * x + 1] = uint8_t(((112 * r - 94 * g - 18 * bl + 128) >> 8) + 128);
        }
    }
}

void Nv12ToI420(const uint8_t* nv12, uint32_t width, uint32_t height, uint8_t* dst) {
    const size_t luma = size_t(width) * height, chroma = luma / 4;
    memcpy(dst, nv12, luma);
    const uint8_t* uv = nv12 + luma;
    uint8_t* u = dst + luma;
    uint8_t* v = u + chroma;
    for (size_t i = 0; i < chroma; ++i) { u[i] = uv[2 * i]; v[i] = uv[2 * i + 1]; }
}

} // namespace mycam
