#pragma once
// Pure NV12 image operations used by the virtual camera. No Windows dependencies, so they can be
// unit-tested (pc/tests).

#include <stddef.h>
#include <stdint.h>

namespace mycam {

// Fills an NV12 image with black (Y=16, UV=128). The UV plane starts at dst + pitch * height.
void FillBlackNV12(uint8_t* dst, ptrdiff_t pitch, uint32_t width, uint32_t height);

// Draws a tightly packed NV12 source (srcWidth x srcHeight, both even) into the destination:
// rotates it clockwise by `rotation` (0/90/180/270), optionally mirrors it horizontally, then scales
// it and centres it. fill = false: fit inside (letterbox; pixels outside are left untouched, so call
// FillBlackNV12 first). fill = true: cover the whole output, cropping what overflows. Nearest-neighbour.
void DrawFittedNV12(const uint8_t* src, uint32_t srcWidth, uint32_t srcHeight, uint32_t rotation, bool mirror,
                    uint8_t* dst, ptrdiff_t pitch, uint32_t width, uint32_t height, bool fill = false);

// Converts a 32-bit BGRA image (width, height even) to tightly packed NV12, BT.601 limited range, the
// same colour space the phone's H.264 video decodes to. dst must hold width * height * 3 / 2 bytes.
void BgraToNV12(const uint8_t* bgra, ptrdiff_t stride, uint32_t width, uint32_t height, uint8_t* dst);

// Converts a tightly packed NV12 image (width, height even) to tightly packed I420 (YU12): the Y plane, then
// all U, then all V. For cameras that take planar YUV (Linux v4l2loopback). dst holds width * height * 3 / 2.
void Nv12ToI420(const uint8_t* nv12, uint32_t width, uint32_t height, uint8_t* dst);

} // namespace mycam
