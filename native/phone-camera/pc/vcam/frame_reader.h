#pragma once
#include <windows.h>
#include <stdint.h>
#include <vector>

namespace mycam {

// Reads the latest phone frame from shared memory and renders it into an NV12 buffer of any size,
// applying rotation/mirroring and letterboxing. Shows a dark "no signal" frame when nothing is live.
class FrameReader {
public:
    FrameReader();
    ~FrameReader();

    // Renders into dst (NV12, `pitch` bytes per row, UV plane starts at dst + pitch * height).
    void Render(uint8_t* dst, LONG pitch, UINT32 width, UINT32 height);

    // True if the companion has published a frame we have not rendered yet (cheap; no copy).
    bool HasNewFrame();

private:
    bool EnsureMapping();
    bool CopyLatest();

    HANDLE mapping_ = nullptr;
    void* view_ = nullptr;

    // Last good frame copied out of shared memory.
    std::vector<uint8_t> frame_;
    UINT32 frameWidth_ = 0;
    UINT32 frameHeight_ = 0;
    UINT32 rotation_ = 0;
    bool mirror_ = false;
    bool fill_ = false;
    uint64_t frameTick_ = 0;
    LONG lastSeq_ = -1;
};

} // namespace mycam
