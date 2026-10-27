#pragma once
// What the connected phone can do right now, shared by the settings window and the tray menu so both grey
// out the same choices (capability-driven, CLAUDE.md "Works on every phone"). Pure, unit-tested.

#include <algorithm>

#include "protocol.h"

namespace mycam::caps {

// `connected`: a phone is connected (link state Idle or later) and has sent its CameraInfo.
// Capabilities beyond that arrive once its camera has started (width > 0).
inline bool Known(bool connected, const proto::CameraInfo& c) { return connected && c.valid && c.width > 0; }

inline bool QualityEnabled(bool connected, const proto::CameraInfo& c, uint8_t quality) {
    if (!connected || !c.valid) return false;
    if (quality == proto::kQuality4K) return !Known(connected, c) || (c.flags & proto::kCamHas4K);
    return true;
}

// Frame rates: only what the phone reports working at the current quality (30 until it has reported).
inline bool FpsEnabled(bool connected, const proto::CameraInfo& c, int fps) {
    if (!connected || !c.valid) return false;
    if (fps <= 30) return true;
    if (!Known(connected, c)) return false;
    const uint8_t mask = proto::FpsMask(c, c.quality);
    return fps >= 120 ? (mask & proto::kFps120Bit) != 0 : (mask & proto::kFps60Bit) != 0;
}

// What the current quality will run at: the chosen rate, or the best working one below it.
inline int EffectiveFps(const proto::CameraInfo& c) {
    const uint8_t mask = c.width > 0 ? proto::FpsMask(c, c.quality) : proto::kFps30Bit;
    if (c.fps >= 120 && (mask & proto::kFps120Bit)) return 120;
    if (c.fps >= 60 && (mask & proto::kFps60Bit)) return 60;
    return 30;
}

inline bool ZoomOutEnabled(bool k, const proto::CameraInfo& c) { return k && c.zoomX100 > c.zoomMinX100; }
inline bool ZoomInEnabled(bool k, const proto::CameraInfo& c) { return k && c.zoomX100 < c.zoomMaxX100; }
inline bool ZoomResetEnabled(bool k, const proto::CameraInfo& c) {
    return k && c.zoomX100 != 100 && c.zoomMinX100 <= 100 && c.zoomMaxX100 >= 100;
}
inline bool EvDownEnabled(bool k, const proto::CameraInfo& c) { return k && c.evMax > c.evMin && c.ev > c.evMin; }
inline bool EvUpEnabled(bool k, const proto::CameraInfo& c) { return k && c.evMax > c.evMin && c.ev < c.evMax; }
inline bool FocusEnabled(bool k, const proto::CameraInfo& c) { return k && (c.flags & proto::kCamHasAutofocus); }
inline bool TorchEnabled(bool k, const proto::CameraInfo& c) { return k && (c.flags & proto::kCamTorchAvailable); }
// Something differs from the defaults (1x, 0 EV, auto focus, torch off).
inline bool AutoEnabled(bool k, const proto::CameraInfo& c) {
    return k && (c.zoomX100 != std::clamp<uint16_t>(100, c.zoomMinX100, c.zoomMaxX100) || c.ev != 0 ||
                 (c.flags & (proto::kCamTorchOn | proto::kCamFocusLocked)));
}

// "1080p" / "4K" for a picture height.
inline const wchar_t* QualityLabel(uint32_t height) {
    return height >= 2000 ? L"4K" : height >= 1000 ? L"1080p" : height >= 700 ? L"720p" : L"";
}

} // namespace mycam::caps
