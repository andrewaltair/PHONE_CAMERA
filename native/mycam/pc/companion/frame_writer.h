#pragma once
#include <windows.h>
#include <stdint.h>

namespace mycam {

// Publishes decoded frames to the virtual camera via the Global\MyCamFrame section. The section is
// created by the virtual camera (inside Frame Server), so it only exists while some app uses the camera.
class FrameWriter {
public:
    ~FrameWriter();

    // True if an app has requested a frame from the virtual camera within the last `withinMs`.
    bool ConsumerActive(uint64_t withinMs);

    void SetPhoneState(uint32_t state);
    void Write(const uint8_t* nv12, uint32_t width, uint32_t height, uint32_t rotation, bool mirror, bool fill = false);
    // Same, from separate Y and UV planes with a row pitch (decoder output, no intermediate copy).
    void WritePlanes(const uint8_t* y, const uint8_t* uv, ptrdiff_t pitch, uint32_t width, uint32_t height,
                     uint32_t rotation, bool mirror, bool fill);

private:
    bool EnsureMapping();
    void Close();

    HANDLE mapping_ = nullptr;
    void* view_ = nullptr;
    uint64_t lastAttempt_ = 0;
    bool onLocal_ = false; // view_ is the session-local fallback, not Global\MyCamFrame (see EnsureMapping).
};

} // namespace mycam
