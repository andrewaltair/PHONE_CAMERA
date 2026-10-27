#pragma once
// What the phone link reports to the UI (PhoneLink::Status()). Kept apart from phone_link.h so code that
// only describes the state (status_text.h) builds without the USB, decoder and camera headers.

#include <stdint.h>

#include <string>
#include <vector>

#include "discovery.h"
#include "protocol.h"
#include "web_proto.h"

namespace mycam {

enum class LinkState {
    NoDriver,    // UsbDk is not installed; we cannot talk to phones.
    Searching,   // No phone found.
    Waiting,     // Phone is in accessory mode but the MyCam app has not answered yet.
    Idle,        // Connected; camera off because no PC app is using the webcam.
    Streaming,   // Frames flowing.
    PhoneError,  // The phone reported a camera problem.
    Paused,      // The user paused the camera (on the phone or the PC). Camera is off.
};

struct LinkStatus {
    LinkState state = LinkState::Searching;
    int facing = 0;          // proto::Facing
    uint32_t width = 0, height = 0;
    bool lockPaused = false; // Windows is locked or asleep: MyCam keeps the phone camera off.
    proto::CameraInfo camera; // v3: phone video settings + what its camera supports (valid once received).
    bool wireless = false;        // The current phone is connected over Wi-Fi (else USB).
    bool wirelessSearch = false;  // "Find phones on Wi-Fi" is on.
    std::wstring phoneName;       // Wi-Fi phones: the name the phone announced.
    std::wstring pairingCode;     // Pairing a Wi-Fi phone: the 6-digit code both screens must show.
    bool pairingAllowed = false;  // ...and this PC's user allowed it (1.5): waiting for the phone's answer.
    // Bumped by every successful new pairing (QR code, 6-digit code or iPhone): the UI then says "Paired with
    // <pairedName>" and shows the camera. pairedWifi: an Android phone over Wi-Fi (not an iPhone page).
    uint32_t pairedCount = 0;
    std::wstring pairedName;
    bool pairedWifi = false;
    // "Scan for phones": a one-shot look for phones on the Wi-Fi, and what answered.
    bool scanning = false;                 // A scan window is running.
    uint32_t scansDone = 0;                // Scans finished since the companion started (0: never scanned).
    std::vector<NearbyPhone> nearbyPhones; // At most NearbyList::kMax, in the order they answered.
    // The iPhone web camera (a page in Safari; docs/web-camera-format.md).
    bool web = false;             // The current phone is a web page (wireless is set too).
    std::wstring webLink;         // How that page reaches the PC: "Wi-Fi", "USB hotspot" or "network".
    bool webAway = false;         // That page is paused and gone for now (iPhone locked, Safari in the
                                  // background): it resumes when opened again; Resume on the PC can't.
    bool webReady = false;        // The page's server is listening on port 47900.
    // "Connect an iPhone": the QR code's address while pairing. It holds the one-time code: never log it.
    std::wstring webUrl;
    uint64_t webCodeExpires = 0;  // GetTickCount64 time the code runs out (a new one follows by itself).
    std::vector<web::NetAddress> webAddresses; // Addresses the QR code can point at, best first.
    uint32_t webAddress = 0;      // The one it points at (IPv4, network order).
    struct WebDeviceRow { std::string id; std::wstring name; uint64_t addedUnix = 0; bool connected = false; };
    std::vector<WebDeviceRow> webDevices; // Pages that can reconnect without scanning, oldest first.
};

} // namespace mycam
