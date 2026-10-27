#include "status_text.h"

#include "protocol.h"

namespace mycam {

StatusView DescribeStatus(const LinkStatus& s, bool cameraRegistered, const std::wstring& backend) {
    if (!cameraRegistered) {
        return {L"Camera not registered", L"Run the MyCam installer again.", kIconError,
                L"Camera not registered", L"Run the MyCam installer again."};
    }
    // Tagged onto the ready/streaming facts line only (not the headline or tray tooltip, which stay the
    // same as on Windows 11): visible in the window for a support request, without changing the common case.
    const std::wstring tag = backend == L"DirectShow" ? L" (DirectShow)" : L"";
    const std::wstring camera = s.facing == proto::kFacingFront ? L"Front camera" : L"Back camera";
    // A web page (an iPhone in Safari) says how it reaches the PC: Wi-Fi, or the iPhone's USB hotspot.
    const std::wstring link = s.web && !s.webLink.empty() ? s.webLink : s.wireless ? L"Wi-Fi" : L"USB";
    if (s.lockPaused && s.state != LinkState::NoDriver) {
        return {L"Paused", L"Windows is locked. MyCam turns the camera back on when you unlock.", kIconPaused,
                L"Windows is locked", L"MyCam turns the camera back on when you unlock."};
    }
    switch (s.state) {
    case LinkState::NoDriver:
        return {L"USB driver missing", L"Run the MyCam installer again to set up the USB driver.", kIconError,
                L"USB can't be used", L"Run the MyCam installer again to set up the USB driver."};
    case LinkState::Searching:
        if (s.wirelessSearch) {
            return {L"No phone connected",
                    L"Plug in your phone, or turn on “Use over Wi-Fi” in MyCam on the phone (same Wi-Fi).", kIconDisconnected,
                    L"Looking on USB and Wi-Fi",
                    L"Plug in your phone, or turn on “Use over Wi-Fi” in MyCam on the phone (same Wi-Fi)."};
        }
        return {L"No phone connected", L"Plug in your Android phone with a USB cable.", kIconDisconnected,
                L"Looking for a phone on USB", L"Plug in your Android phone with a USB cable."};
    case LinkState::Waiting:
        if (s.wireless) {
            const std::wstring phone = s.phoneName.empty() ? std::wstring(L"the phone") : s.phoneName;
            if (!s.pairingCode.empty()) {
                return {L"Pairing with " + phone + L"…",
                        L"Code " + s.pairingCode + L". If the phone shows the same code, allow it here and on the phone.",
                        kIconDisconnected, L"Code " + s.pairingCode,
                        L"Allow it here and on the phone if both show the same code. Paired phones connect without asking.",
                        StatusProgress::Pairing};
            }
            return {L"Phone found on Wi-Fi", L"Connecting to " + phone + L"…", kIconDisconnected,
                    L"Connecting to " + phone + L"…", L"Keep MyCam open on the phone.", StatusProgress::Working};
        }
        return {L"Phone found", L"Open MyCam on the phone, and tap OK if it asks.", kIconDisconnected,
                L"Connecting over USB…", L"Open MyCam on the phone, and tap OK if it asks.", StatusProgress::Working};
    case LinkState::Idle:
        return {L"Ready", camera + (s.wireless ? L" · " + link : L"") + L". Choose “MyCam” as the camera in any app.", kIconReady,
                camera + L" · " + link + tag, L"Choose “MyCam” as the camera in any app. The camera turns on only while an app uses it."};
    case LinkState::Streaming: {
        std::wstring detail = camera;
        if (s.width) detail += L" · " + std::to_wstring(s.width) + L" × " + std::to_wstring(s.height);
        if (s.wireless) detail += L" · " + link;
        std::wstring facts = camera;
        if (s.width) facts += L" · " + std::to_wstring(s.width) + L" × " + std::to_wstring(s.height);
        facts += L" · " + link + tag;
        return {L"Streaming", detail, kIconStreaming, facts, L"An app is using the camera. Pause it any time; apps then see a “Camera paused” picture."};
    }
    case LinkState::PhoneError:
        return {L"Camera problem", L"Check the phone. Another app may be using its camera.", kIconError,
                camera + L" · " + link, L"Check the phone. Another app may be using its camera."};
    case LinkState::Paused:
        if (s.web && s.webAway) {
            const std::wstring phone = s.phoneName.empty() ? std::wstring(L"the iPhone") : s.phoneName;
            return {L"Paused", L"Open MyCam again on " + phone + L" to resume. Apps see a “Camera paused” picture.",
                    kIconPaused, camera + L" · " + link,
                    L"The MyCam page on " + phone + L" is locked or in the background. Open it again to resume."};
        }
        return {L"Paused", L"The camera is off. Apps see a “Camera paused” picture. Resume here or on the phone.", kIconPaused,
                camera + L" · " + link, L"The camera is off. Apps see a “Camera paused” picture. Resume here or on the phone."};
    }
    return {L"", L"", kIconDisconnected};
}

} // namespace mycam
