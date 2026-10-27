#include "marquee.h"

#include <math.h>
#include <string.h>

#include <algorithm>

namespace mycam {

namespace {

int Scale(int v, uint32_t actual, int ref) { return int(lround(double(v) * actual / ref)); }

struct Rgb { double r, g, b; };

// Progress chunk gradient (docs/redesign.md §3): #E2F8E2 -> #6FD86F (45 %) -> #2DB52D (55 %) -> #5ACD5A.
Rgb ChunkColor(double f) {
    struct Stop { double at; Rgb c; };
    static const Stop stops[] = {
        {0.00, {0xE2, 0xF8, 0xE2}},
        {0.45, {0x6F, 0xD8, 0x6F}},
        {0.55, {0x2D, 0xB5, 0x2D}},
        {1.00, {0x5A, 0xCD, 0x5A}},
    };
    f = std::clamp(f, 0.0, 1.0);
    for (size_t i = 1; i < sizeof(stops) / sizeof(stops[0]); ++i) {
        if (f <= stops[i].at) {
            const Stop& a = stops[i - 1];
            const Stop& b = stops[i];
            const double k = (f - a.at) / (b.at - a.at);
            return {a.c.r + (b.c.r - a.c.r) * k, a.c.g + (b.c.g - a.c.g) * k, a.c.b + (b.c.b - a.c.b) * k};
        }
    }
    return stops[3].c;
}

// Colour of row r of the interior (top = light highlight).
Rgb RowColor(const MarqueeGeometry& g, int r) { return ChunkColor(g.ih > 1 ? double(r - g.iy) / (g.ih - 1) : 0.5); }

// Same BT.601 limited-range maths as BgraToNV12, so chunks match the rest of the picture.
uint8_t ToY(int r, int g, int b) { return uint8_t(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16); }
uint8_t ToU(int r, int g, int b) { return uint8_t(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128); }
uint8_t ToV(int r, int g, int b) { return uint8_t(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128); }

} // namespace

MarqueeGeometry MarqueeGeometryFor(uint32_t width, uint32_t height) {
    MarqueeGeometry g;
    if (!width || !height) return g;
    g.x = std::clamp(Scale(kMarqueeTrackX, width, kMarqueeRefWidth), 0, int(width));
    g.y = std::clamp(Scale(kMarqueeTrackY, height, kMarqueeRefHeight), 0, int(height));
    g.w = std::min(Scale(kMarqueeTrackW, width, kMarqueeRefWidth), int(width) - g.x);
    g.h = std::min(Scale(kMarqueeTrackH, height, kMarqueeRefHeight), int(height) - g.y);
    // XP draws the chunks inside a 1 px border plus 1 px of white padding.
    const int insetX = std::max(1, Scale(2, width, kMarqueeRefWidth));
    const int insetY = std::max(1, Scale(2, height, kMarqueeRefHeight));
    g.ix = g.x + insetX;
    g.iy = g.y + insetY;
    g.iw = g.w - 2 * insetX;
    g.ih = g.h - 2 * insetY;
    g.chunkW = std::max(1, Scale(8, width, kMarqueeRefWidth));
    g.gap = std::max(1, Scale(2, width, kMarqueeRefWidth));
    if (!g.valid()) return MarqueeGeometry{};
    return g;
}

bool MarqueeChunkLeft(const MarqueeGeometry& g, double tSeconds, int index, int* left) {
    if (!g.valid() || index < 0 || index >= kMarqueeChunks) return false;
    const double period = kMarqueePassSec + kMarqueePauseSec;
    double phase = fmod(tSeconds, period);
    if (phase < 0) phase += period;
    if (phase >= kMarqueePassSec) return false;
    const int group = kMarqueeChunks * g.chunkW + (kMarqueeChunks - 1) * g.gap;
    // The group's left edge travels linearly from fully left of the interior to fully right of it.
    const double travel = double(g.iw + group);
    const int groupLeft = g.ix - group + int(floor(phase / kMarqueePassSec * travel));
    *left = groupLeft + (kMarqueeChunks - 1 - index) * (g.chunkW + g.gap);
    return true;
}

void DrawMarquee(Nv12Image& img, const Nv12Image& base, double tSeconds) {
    if (img.empty() || img.width != base.width || img.height != base.height || img.data.size() != base.data.size())
        return;
    const MarqueeGeometry g = MarqueeGeometryFor(img.width, img.height);
    if (!g.valid()) return;
    const size_t W = img.width, H = img.height;
    uint8_t* y = img.data.data();
    uint8_t* uv = y + W * H;
    const uint8_t* by = base.data.data();
    const uint8_t* buv = by + W * H;

    // Restore the track, rounded out to even coordinates so whole chroma blocks come back.
    const int x0 = g.x & ~1, y0 = g.y & ~1;
    const int x1 = std::min(int(W), (g.x + g.w + 1) & ~1), y1 = std::min(int(H), (g.y + g.h + 1) & ~1);
    for (int r = y0; r < y1; ++r) memcpy(y + r * W + x0, by + r * W + x0, size_t(x1 - x0));
    for (int r = y0 / 2; r < y1 / 2; ++r) memcpy(uv + r * W + x0, buv + r * W + x0, size_t(x1 - x0));

    for (int i = 0; i < kMarqueeChunks; ++i) {
        int left = 0;
        if (!MarqueeChunkLeft(g, tSeconds, i, &left)) return; // Pause: the empty track stays.
        const int cx0 = std::max(left, g.ix), cx1 = std::min(left + g.chunkW, g.ix + g.iw);
        if (cx0 >= cx1) continue;
        const int cy0 = g.iy, cy1 = g.iy + g.ih;
        for (int r = cy0; r < cy1; ++r) {
            const Rgb c = RowColor(g, r);
            memset(y + r * W + cx0, ToY(int(c.r + 0.5), int(c.g + 0.5), int(c.b + 0.5)), size_t(cx1 - cx0));
        }
        // Chroma only for 2x2 blocks that lie entirely inside the chunk.
        const int bx0 = (cx0 + 1) / 2, bx1 = cx1 / 2, by0 = (cy0 + 1) / 2, by1 = cy1 / 2;
        for (int br = by0; br < by1; ++br) {
            const Rgb a = RowColor(g, 2 * br), b = RowColor(g, 2 * br + 1);
            const int r = int((a.r + b.r) / 2 + 0.5), gr = int((a.g + b.g) / 2 + 0.5), bl = int((a.b + b.b) / 2 + 0.5);
            const uint8_t u = ToU(r, gr, bl), v = ToV(r, gr, bl);
            uint8_t* row = uv + br * W;
            for (int bx = bx0; bx < bx1; ++bx) {
                row[2 * bx] = u;
                row[2 * bx + 1] = v;
            }
        }
    }
}

} // namespace mycam
