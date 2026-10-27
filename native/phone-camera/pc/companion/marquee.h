#pragma once
// The XP-style green marquee bar animated on the "Waiting for the phone" picture (docs/redesign.md §6.1, §7.2).
// Pure code (no Windows calls) so the unit tests can check it.

#include <stdint.h>

#include "status_images.h"

namespace mycam {

// The empty progress track drawn into res/frame_waiting.png, in 1280x720 coordinates (scaled to the real
// image size). The source of truth is the "marquee x y w h" line in design/tools/frame_layout.txt, written
// by the frame generator: keep these four numbers in sync with it.
constexpr int kMarqueeTrackX = 490;
constexpr int kMarqueeTrackY = 447;
constexpr int kMarqueeTrackW = 300;
constexpr int kMarqueeTrackH = 18;
constexpr int kMarqueeRefWidth = 1280;
constexpr int kMarqueeRefHeight = 720;

// Animation timing: 3 chunks cross the track in kMarqueePassSec, then nothing for kMarqueePauseSec.
constexpr int kMarqueeChunks = 3;
constexpr double kMarqueePassSec = 2.0;
constexpr double kMarqueePauseSec = 0.4;

struct MarqueeGeometry {
    int x = 0, y = 0, w = 0, h = 0;     // Track, in image pixels (what DrawMarquee restores from the base).
    int ix = 0, iy = 0, iw = 0, ih = 0; // Track interior: chunks are clipped to it.
    int chunkW = 0, gap = 0;            // 8 px and 2 px at 1280x720.
    bool valid() const { return iw > 0 && ih > 0; }
};

// Track geometry for an image of this size (empty if the track would not fit).
MarqueeGeometry MarqueeGeometryFor(uint32_t width, uint32_t height);

// Left edge (image x, unclipped) of chunk `index` at time t, or false during the pause between passes.
// Chunks move left to right with chunk 0 leading; the group enters and leaves the interior fully.
bool MarqueeChunkLeft(const MarqueeGeometry& g, double tSeconds, int index, int* left);

// Restores the track area of `img` from `base` (same size), then draws the chunks for time t directly
// into NV12: Y for every chunk pixel, U/V only for 2x2 blocks that lie entirely inside a chunk.
// Touches nothing outside the track rectangle (rounded out to even coordinates for chroma).
void DrawMarquee(Nv12Image& img, const Nv12Image& base, double tSeconds);

} // namespace mycam
