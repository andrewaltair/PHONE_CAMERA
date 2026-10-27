#pragma once
// A tiny row/column layout helper for the custom-drawn windows (docs/redesign.md §8.1, W7). Coordinates are
// DIPs. Text sizes come from a measure callback (DirectWrite in the app, a fake in the tests), so the
// layout reflows when the text size changes instead of relying on hand-typed coordinates.
// Pure C++: no Windows dependencies, unit-tested in pc/tests.

#include <algorithm>
#include <vector>

namespace mycam::ui {

struct Size {
    float w = 0, h = 0;
};

struct Box {
    float l = 0, t = 0, r = 0, b = 0;
    float W() const { return r - l; }
    float H() const { return b - t; }
    bool Contains(float x, float y) const { return x >= l && x < r && y >= t && y < b; }
    bool Intersects(const Box& o) const { return l < o.r && o.l < r && t < o.b && o.t < b; }
    Box Inset(float dx, float dy) const { return {l + dx, t + dy, r - dx, b - dy}; }
    Box Offset(float dx, float dy) const { return {l + dx, t + dy, r + dx, b + dy}; }
};

inline Box MakeBox(float x, float y, float w, float h) { return {x, y, x + w, y + h}; }

// Vertical stack: hands out full-width rows from the top down.
class Column {
public:
    Column(float x, float y, float width) : x_(x), y_(y), w_(width) {}

    // A row of the given height across the whole column.
    Box Row(float h) {
        Box b = MakeBox(x_, y_, w_, h);
        y_ += h;
        return b;
    }
    // A row of the given size, aligned left (or centred / right) in the column.
    Box Item(Size s, int align = 0) {
        const float w = std::min(s.w, w_);
        const float x = align == 0 ? x_ : align == 1 ? x_ + (w_ - w) / 2 : x_ + w_ - w;
        Box b = MakeBox(x, y_, w, s.h);
        y_ += s.h;
        return b;
    }
    void Gap(float g) { y_ += g; }
    float Y() const { return y_; }
    float X() const { return x_; }
    float Width() const { return w_; }

private:
    float x_, y_, w_;
};

// Places items left to right and wraps to a new line when the next one doesn't fit. Every item on a line
// is centred vertically on that line. An item wider than `width` gets a line of its own and is clipped to
// the width. Returns one box per item; `bottom` (optional) receives the bottom of the last line.
inline std::vector<Box> Flow(const std::vector<Size>& items, float x, float y, float width, float hgap, float vgap,
                             float* bottom = nullptr) {
    std::vector<Box> out(items.size());
    size_t lineStart = 0;
    float cx = x, lineH = 0, cy = y;
    auto closeLine = [&](size_t end) {
        for (size_t k = lineStart; k < end; ++k) {
            const float dy = (lineH - out[k].H()) / 2;
            out[k] = out[k].Offset(0, dy);
        }
    };
    for (size_t i = 0; i < items.size(); ++i) {
        const float w = std::min(items[i].w, width);
        if (i > lineStart && cx + w > x + width + 0.01f) {
            closeLine(i);
            cy += lineH + vgap;
            cx = x;
            lineH = 0;
            lineStart = i;
        }
        out[i] = MakeBox(cx, cy, w, items[i].h);
        cx += w + hgap;
        lineH = std::max(lineH, items[i].h);
    }
    closeLine(items.size());
    if (bottom) *bottom = items.empty() ? y : cy + lineH;
    return out;
}

// Splits `width` into columns: fixed widths first, the rest shared by the flexible (0-width) ones.
inline std::vector<Box> Split(float x, float y, float width, float h, const std::vector<float>& widths, float gap) {
    float fixed = 0;
    int flex = 0;
    for (float w : widths) w > 0 ? fixed += w : ++flex;
    const float spare = std::max(0.f, width - fixed - gap * float(widths.empty() ? 0 : widths.size() - 1));
    std::vector<Box> out;
    float cx = x;
    for (float w : widths) {
        const float ww = w > 0 ? w : spare / float(std::max(flex, 1));
        out.push_back(MakeBox(cx, y, ww, h));
        cx += ww + gap;
    }
    return out;
}

} // namespace mycam::ui
