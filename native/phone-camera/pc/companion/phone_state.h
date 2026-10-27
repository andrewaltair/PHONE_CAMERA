#pragma once
// What the phone is doing, as the camera shows it (the Windows camera reads it from the shared frame header,
// common/shared_frame.h). Kept apart so code that only sets it builds without Windows headers.

#include <stdint.h>

namespace mycam {

enum PhoneState : uint32_t {
    kPhoneNone = 0,       // No phone connected.
    kPhoneWaiting = 1,    // Phone in accessory mode, waiting for the MyCam app to respond.
    kPhoneIdle = 2,       // MyCam app connected, camera off.
    kPhoneStreaming = 3,  // Frames are flowing.
};

} // namespace mycam
