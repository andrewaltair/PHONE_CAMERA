#pragma once
// Frame hand-off between PHONE_CAMERA.exe (user session) and MyCamVCam.dll, which Windows loads
// into the Camera Frame Server service. The section lives in the Global namespace so both sides can
// see it; the DLL creates it (services hold SeCreateGlobalPrivilege) and the companion opens it.

#include <windows.h>
#include <stdint.h>

#include "../companion/phone_state.h" // PhoneState, the header's phoneState field.

namespace mycam {

constexpr wchar_t kSharedFrameName[] = L"Global\\PHONE_CAMERAFrame";
// Fallback when kSharedFrameName can't be created or opened (see frame_reader.cpp / frame_writer.cpp):
// no "Global\" prefix, so it lives in the caller's own session and needs no special privilege.
constexpr wchar_t kSharedFrameNameLocal[] = L"PHONE_CAMERAFrame";
constexpr uint32_t kSharedMagic = 0x4D434652; // 'MCFR'
constexpr uint32_t kSharedVersion = 1;
constexpr uint32_t kHeaderBytes = 4096;
constexpr uint32_t kMaxWidth = 3840;
constexpr uint32_t kMaxHeight = 2160;
constexpr uint32_t kMaxFrameBytes = kMaxWidth * kMaxHeight * 3 / 2;
constexpr uint32_t kSharedBytes = kHeaderBytes + kMaxFrameBytes;

// Allow SYSTEM, LOCAL SERVICE, administrators and interactive users to read/write.
constexpr wchar_t kSharedSddl[] = L"D:(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;BA)(A;;GRGW;;;IU)";

struct SharedHeader {
    uint32_t magic;
    uint32_t version;
    // Seqlock: odd while the companion is writing a frame, even when the frame is stable.
    volatile LONG seq;
    uint32_t width;     // NV12 frame, tightly packed (stride == width).
    uint32_t height;
    uint32_t rotation;  // Clockwise degrees (0/90/180/270) the consumer must rotate the frame by.
    uint32_t mirror;    // Non-zero: flip horizontally after rotating.
    uint32_t phoneState;
    volatile uint64_t frameTick;     // GetTickCount64() when the frame was written (companion).
    volatile uint64_t consumerTick;  // GetTickCount64() of the last frame request (virtual camera).
    uint32_t fill;      // Non-zero: crop to fill the output instead of letterboxing (appended in 1.3).
};
static_assert(sizeof(SharedHeader) <= kHeaderBytes, "header too large");

inline uint8_t* FrameData(void* base) { return static_cast<uint8_t*>(base) + kHeaderBytes; }

} // namespace mycam
