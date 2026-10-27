#pragma once

#include <string_view>

#include "status_images.h"

namespace mycam {

constexpr uint32_t kLinuxWaitingFrameWidth = 1280;
constexpr uint32_t kLinuxWaitingFrameHeight = 720;

// Text actually drawn into the waiting frame. Keeping these constants
// public lets the source test reject a missing bitmap glyph before CI ships
// a visually corrupted status frame.
inline constexpr std::string_view kWaitingBrandText = "PHONE_CAMERA";
inline constexpr std::string_view kWaitingHeadlineText = "WAITING FOR YOUR PHONE";
inline constexpr std::string_view kWaitingConnectText = "CONNECT YOUR PHONE";
inline constexpr std::string_view kWaitingAndroidText = "ANDROID USB OR WI-FI";
inline constexpr std::string_view kWaitingIphoneText = "IPHONE SCAN QR CODE";
inline constexpr std::string_view kWaitingProgressText = "WAITING FOR CONNECTION";

bool LinuxWaitingGlyphAvailable(char c);

// Draws the waiting picture directly into packed NV12.  It intentionally
// does not read frame_waiting.png: that inherited asset contains the old
// product name.  The frame geometry stays 1280x720 so the existing marquee
// coordinates remain valid and scaling stays in the common camera path.
Nv12Image BuildPhoneCameraWaitingFrame();

} // namespace mycam
