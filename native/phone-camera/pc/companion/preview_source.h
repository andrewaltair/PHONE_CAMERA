#pragma once
// The settings window's preview tap (docs/redesign.md §8.2). It reads the frame the companion last handed to
// the virtual camera (shared memory Global\PHONE_CAMERAFrame, pc/common/shared_frame.h) and renders it the way
// the virtual camera does: rotated, mirrored and fitted or filled into 16:9. So the preview shows exactly
// what apps receive (live video, or the Paused / animated Waiting picture), and the session loop needs no
// changes. Runs on the UI thread, only while the window is visible.
//
// The section only exists while an app has the PHONE_CAMERA camera open; when it doesn't, Poll() reports no
// frame and the window draws the status picture itself.

#include <windows.h>
#include <stdint.h>

#include <vector>

#include "status_images.h"

namespace mycam {

class PreviewSource {
public:
    ~PreviewSource() { Close(); }

    enum class Result { NoFrame, Unchanged, NewFrame };

    // Renders the latest fresh frame into `bgra` (width x height, both even, BGRA). NoFrame: no app is
    // using the camera or the last frame is older than 1.5 s (what the virtual camera treats as stale).
    Result Poll(uint32_t width, uint32_t height, std::vector<uint8_t>& bgra);
    // Size of the last frame read (before fitting), e.g. for the "1080p" chip.
    uint32_t FrameWidth() const { return frameW_; }
    uint32_t FrameHeight() const { return frameH_; }
    bool Mirrored() const { return mirror_; }
    void Close();

    // Renders an NV12 picture (the status images) the same way, letterboxed into 16:9.
    static void Render(const Nv12Image& image, uint32_t width, uint32_t height, std::vector<uint8_t>& bgra);

private:
    bool Open();

    HANDLE mapping_ = nullptr;
    const void* view_ = nullptr;
    uint64_t lastOpenAttempt_ = 0;
    LONG lastSeq_ = -1;
    uint32_t lastW_ = 0, lastH_ = 0;
    uint32_t frameW_ = 0, frameH_ = 0;
    bool mirror_ = false;
    std::vector<uint8_t> nv12_; // 2x the output size, box-filtered down in the conversion.
};

} // namespace mycam
