#pragma once

#include "status_images.h"

namespace mycam {

constexpr uint32_t kLinuxWaitingFrameWidth = 1280;
constexpr uint32_t kLinuxWaitingFrameHeight = 720;

// Draws the waiting picture directly into packed NV12.  It intentionally
// does not read frame_waiting.png: that inherited asset contains the old
// product name.  The frame geometry stays 1280x720 so the existing marquee
// coordinates remain valid and scaling stays in the common camera path.
Nv12Image BuildPhoneCameraWaitingFrame();

} // namespace mycam
