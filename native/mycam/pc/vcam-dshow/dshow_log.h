#pragma once
// MyCamVCamDShow.dll loads into whatever app opens the camera (Discord, OBS, a browser...), not into
// MyCamCompanion.exe, so it can't use companion/log.cpp's single shared file. Writes its own, so a
// support request can attach it the same way as mycam.log.

namespace mycam {

// %LOCALAPPDATA%\MyCam\mycam-dshow.log. Opens, appends and closes per call (diagnostic-only volume:
// lifecycle events and a periodic delivery heartbeat, not per-frame) so concurrent host processes don't
// need a cross-process lock.
void DShowLog(const char* fmt, ...);

} // namespace mycam
