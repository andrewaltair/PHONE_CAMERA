#pragma once
// Pixel conversion for the settings window's live preview (docs/redesign.md §8.2). Pure C++, unit-tested.

#include <stddef.h>
#include <stdint.h>

namespace mycam {

// Converts a tightly packed NV12 image of (2 * outWidth) x (2 * outHeight) into a 32-bit BGRA image of
// outWidth x outHeight, averaging each 2x2 block of luma (a cheap, smooth downscale; NV12 chroma is
// already one sample per 2x2 block). BT.601 limited range, alpha 255. `bgra` holds outWidth*outHeight*4.
void Nv12ToBgraHalf(const uint8_t* nv12, uint32_t outWidth, uint32_t outHeight, uint8_t* bgra);

} // namespace mycam
