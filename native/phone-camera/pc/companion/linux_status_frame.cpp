#include "linux_status_frame.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "marquee.h"

namespace mycam {

namespace {

struct Yuv {
    uint8_t y;
    uint8_t u;
    uint8_t v;
};

Yuv Rgb(int r, int g, int b) {
    const int y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
    const int u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
    const int v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
    return {uint8_t(std::clamp(y, 0, 255)), uint8_t(std::clamp(u, 0, 255)), uint8_t(std::clamp(v, 0, 255))};
}

void Fill(Nv12Image& image, Yuv color) {
    const size_t ySize = size_t(image.width) * image.height;
    std::fill(image.data.begin(), image.data.begin() + ySize, color.y);
    for (size_t i = ySize; i < image.data.size(); i += 2) {
        image.data[i] = color.u;
        image.data[i + 1] = color.v;
    }
}

// Rectangles in the waiting frame use even dimensions wherever possible. Y
// is still exact; the chroma rectangle is rounded to complete 2x2 samples,
// which is the only representation NV12 permits.
void Rect(Nv12Image& image, int x, int y, int w, int h, Yuv color) {
    const int width = int(image.width), height = int(image.height);
    const int x0 = std::clamp(x, 0, width), y0 = std::clamp(y, 0, height);
    const int x1 = std::clamp(x + w, 0, width), y1 = std::clamp(y + h, 0, height);
    if (x0 >= x1 || y0 >= y1) return;
    uint8_t* luma = image.data.data();
    for (int row = y0; row < y1; ++row)
        std::fill(luma + size_t(row) * image.width + x0, luma + size_t(row) * image.width + x1, color.y);

    const int cx0 = x0 & ~1, cy0 = y0 & ~1;
    const int cx1 = std::min(width, (x1 + 1) & ~1), cy1 = std::min(height, (y1 + 1) & ~1);
    uint8_t* chroma = luma + size_t(image.width) * image.height;
    for (int row = cy0 / 2; row < cy1 / 2; ++row) {
        for (int col = cx0; col < cx1; col += 2) {
            chroma[size_t(row) * image.width + col] = color.u;
            chroma[size_t(row) * image.width + col + 1] = color.v;
        }
    }
}

struct Glyph {
    uint8_t rows[7];
};

Glyph GlyphFor(char c) {
    switch (c) {
    case 'B': return {{0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}};
    case 'A': return {{0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}};
    case 'C': return {{0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}};
    case 'D': return {{0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}};
    case 'E': return {{0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}};
    case 'F': return {{0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}};
    case 'G': return {{0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}};
    case 'H': return {{0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}};
    case 'I': return {{0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F}};
    case 'M': return {{0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}};
    case 'N': return {{0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}};
    case 'O': return {{0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}};
    case 'P': return {{0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}};
    case 'Q': return {{0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}};
    case 'R': return {{0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}};
    case 'T': return {{0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}};
    case 'U': return {{0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}};
    case 'W': return {{0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}};
    case 'Y': return {{0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}};
    case '-': return {{0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}};
    case 'S': return {{0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}};
    case '_': return {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F}};
    default: return {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
    }
}

bool GlyphAvailable(char c) {
    switch (c) {
    case 'A': case 'B': case 'C': case 'D': case 'E': case 'F': case 'G': case 'H': case 'I':
    case 'M': case 'N': case 'O': case 'P': case 'Q': case 'R': case 'S': case 'T': case 'U':
    case 'W': case 'Y': case '-': case '_': case ' ': return true;
    default: return false;
    }
}

void Text(Nv12Image& image, std::string_view text, int x, int y, int scale, Yuv color) {
    for (const char c : text) {
        const Glyph glyph = GlyphFor(c);
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if ((glyph.rows[row] & (1u << (4 - col))) == 0) continue;
                Rect(image, x + col * scale, y + row * scale, scale, scale, color);
            }
        }
        x += (c == ' ' ? 3 : 6) * scale;
    }
}

} // namespace

bool LinuxWaitingGlyphAvailable(char c) { return GlyphAvailable(c); }

Nv12Image BuildPhoneCameraWaitingFrame() {
    Nv12Image image;
    image.width = kLinuxWaitingFrameWidth;
    image.height = kLinuxWaitingFrameHeight;
    image.data.resize(size_t(image.width) * image.height * 3 / 2);

    const Yuv background = Rgb(24, 49, 80);
    const Yuv panel = Rgb(43, 77, 116);
    const Yuv panelInner = Rgb(31, 61, 96);
    const Yuv edge = Rgb(103, 153, 202);
    const Yuv white = Rgb(245, 250, 255);
    const Yuv muted = Rgb(190, 216, 238);
    const Yuv track = Rgb(248, 250, 252);
    Fill(image, background);

    Rect(image, 120, 72, 1040, 576, panel);
    Rect(image, 170, 106, 940, 146, Rgb(57, 95, 139));
    Rect(image, 172, 108, 936, 142, edge);
    // A simple camera mark, kept deliberately neutral and independent from
    // the inherited MyCam artwork.
    Rect(image, 212, 143, 54, 54, white);
    Rect(image, 260, 155, 32, 30, white);
    Rect(image, 220, 151, 38, 38, Rgb(57, 95, 139));
    Text(image, kWaitingBrandText, 330, 121, 6, white);
    Text(image, kWaitingHeadlineText, 418, 184, 4, muted);

    Rect(image, 260, 286, 760, 334, Rgb(89, 133, 177));
    Rect(image, 262, 288, 756, 330, panelInner);
    Text(image, kWaitingConnectText, 418, 326, 4, white);
    Text(image, kWaitingAndroidText, 434, 376, 3, muted);
    Text(image, kWaitingIphoneText, 438, 414, 3, muted);

    // This is the locked marquee track: x=490, y=447, w=300, h=18 in the
    // 1280x720 reference frame. DrawMarquee restores and animates this exact
    // area later; the pause phase leaves the base track untouched.
    Rect(image, kMarqueeTrackX, kMarqueeTrackY, kMarqueeTrackW, kMarqueeTrackH, track);
    Rect(image, kMarqueeTrackX, kMarqueeTrackY, kMarqueeTrackW, 2, Rgb(172, 184, 198));
    Rect(image, kMarqueeTrackX, kMarqueeTrackY + kMarqueeTrackH - 2, kMarqueeTrackW, 2, Rgb(172, 184, 198));
    Rect(image, kMarqueeTrackX, kMarqueeTrackY, 2, kMarqueeTrackH, Rgb(172, 184, 198));
    Rect(image, kMarqueeTrackX + kMarqueeTrackW - 2, kMarqueeTrackY, 2, kMarqueeTrackH, Rgb(172, 184, 198));
    Text(image, kWaitingProgressText, 430, 507, 3, muted);
    return image;
}

} // namespace mycam
