#pragma once
// The pictures the MyCam camera shows when there is no live video ("Camera paused", "Waiting for the
// phone"), embedded as PNG resources (art: design/tools/make_status_frames.ps1) and decoded to NV12.

#include <stdint.h>

#include <vector>

namespace mycam {

struct Nv12Image {
    std::vector<uint8_t> data;
    uint32_t width = 0, height = 0;
    bool empty() const { return data.empty(); }
};

enum StatusImageId : int {
    kImagePaused = 201,
    kImageWaiting = 202,
};

// Decodes an embedded PNG resource (RCDATA) into NV12. Returns an empty image on failure.
// Requires COM to be initialised on the calling thread.
Nv12Image LoadStatusImage(int resourceId);

} // namespace mycam
