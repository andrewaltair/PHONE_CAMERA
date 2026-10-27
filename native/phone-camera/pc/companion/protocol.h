#pragma once
// Wire protocol shared with the Android app (app/src/main/java/com/example/mycam/Protocol.kt).
#include <stdint.h>

namespace mycam::proto {

constexpr uint32_t kPacketMagic = 0x4D43414D; // 'MCAM'
constexpr size_t kHeaderSize = 20;            // u32 magic, u8 type, u8 flags, u16 reserved, i64 ptsUs, u32 length
constexpr uint32_t kMaxPayload = 8 * 1024 * 1024;

enum PacketType : uint8_t {
    kHello = 0,   // u16 protocol version
    kConfig = 1,  // u16 width, u16 height, u16 sensorOrientation, u8 facing, then SPS/PPS (Annex-B)
    kFrame = 2,   // H.264 Annex-B access unit
    kOrient = 3,  // u16 device rotation (degrees)
    kState = 4,   // u8 state, [u8 facing]
    kLog = 5,     // UTF-8 diagnostic text from the phone
    kCamera = 6,  // v3: camera settings + capabilities (CameraInfo, 18 bytes)
};
constexpr uint8_t kMaxPacketType = kCamera;

constexpr uint8_t kFlagKeyFrame = 0x01;

enum PhoneStreamState : uint8_t { kStateIdle = 0, kStateStreaming = 1, kStateError = 2, kStatePaused = 3 /* v2 */ };
enum Facing : uint8_t { kFacingBack = 0, kFacingFront = 1 };
enum Quality : uint8_t { kQuality720p = 0, kQuality1080p = 1, kQuality4K = 2 };

enum CameraFlags : uint8_t {
    kCamTorchAvailable = 0x01, kCamTorchOn = 0x02, kCamFocusLocked = 0x04,
    kCamHas60Fps = 0x08, kCamHas4K = 0x10, kCamHasAutofocus = 0x20, kCamHas120Fps = 0x40 /* 1.3.2 */
};

// TYPE_CAMERA payload (v3).
struct CameraInfo {
    bool valid = false;
    uint8_t quality = kQuality1080p, fps = 30;
    uint16_t zoomX100 = 100, zoomMinX100 = 100, zoomMaxX100 = 100;
    int8_t ev = 0, evMin = 0, evMax = 0;
    uint8_t evStepX100 = 0, flags = 0;
    uint16_t width = 0, height = 0;
    uint8_t actualFps = 0;
    // 1.3.2: which frame rates work at each quality (kFps* bits), so only working choices are offered.
    bool hasFpsModes = false;
    uint8_t fpsModes[3] = {};
};

enum FpsBits : uint8_t { kFps30Bit = 0x01, kFps60Bit = 0x02, kFps120Bit = 0x04 };

inline bool ParseCameraInfo(const uint8_t* p, uint32_t length, CameraInfo* out) {
    if (length < 18) return false;
    out->quality = p[0]; out->fps = p[1];
    out->zoomX100 = uint16_t(p[2] << 8 | p[3]); out->zoomMinX100 = uint16_t(p[4] << 8 | p[5]);
    out->zoomMaxX100 = uint16_t(p[6] << 8 | p[7]);
    out->ev = int8_t(p[8]); out->evMin = int8_t(p[9]); out->evMax = int8_t(p[10]); out->evStepX100 = p[11];
    out->flags = p[12]; out->width = uint16_t(p[13] << 8 | p[14]); out->height = uint16_t(p[15] << 8 | p[16]);
    out->actualFps = p[17];
    out->hasFpsModes = length >= 21;
    for (int q = 0; q < 3; ++q) out->fpsModes[q] = out->hasFpsModes ? p[18 + q] : 0;
    out->valid = true;
    return true;
}

// kFps* bits that work at `quality`; phones before 1.3.2 only send the overall 60/120 flags.
inline uint8_t FpsMask(const CameraInfo& c, uint8_t quality) {
    if (c.hasFpsModes && quality < 3) return c.fpsModes[quality];
    return uint8_t(kFps30Bit | ((c.flags & kCamHas60Fps) ? kFps60Bit : 0) | ((c.flags & kCamHas120Fps) ? kFps120Bit : 0));
}

constexpr uint32_t kCommandMagic = 0x4D434D44; // 'MCMD'
constexpr size_t kCommandSize = 8;

enum Command : uint8_t {
    kCmdHello = 1,
    kCmdStart = 2,
    kCmdStop = 3,
    kCmdKeyFrame = 4,
    kCmdSetFacing = 5,
    kCmdPause = 6,  // v2
    kCmdResume = 7, // v2
    kCmdSetQuality = 8,  // v3: arg Quality
    kCmdSetFps = 9,      // v3: arg 30 or 60 (120 since 1.3.2)
    kCmdSetZoom = 10,    // v3: arg zoom x10
    kCmdSetExposure = 11, // v3: arg int8 EV steps
    kCmdSetTorch = 12,   // v3: arg 0/1
    kCmdSetFocus = 13,   // v3: arg 0 continuous, 1 focus once and lock
};

inline uint16_t ReadU16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline uint32_t ReadU32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline int64_t ReadI64(const uint8_t* p) { return int64_t(uint64_t(ReadU32(p)) << 32 | ReadU32(p + 4)); }

inline void MakeCommand(uint8_t out[kCommandSize], Command cmd, uint8_t arg = 0) {
    out[0] = 0x4D; out[1] = 0x43; out[2] = 0x4D; out[3] = 0x44;
    out[4] = cmd; out[5] = arg; out[6] = 0; out[7] = 0;
}

// Accessory identification sent during the AOA handshake. Must match res/xml/accessory_filter.xml.
constexpr const char* kAccessoryManufacturer = "MyCam";
constexpr const char* kAccessoryModel = "MyCam Webcam";
constexpr const char* kAccessoryDescription = "Use this phone as a USB webcam";
constexpr const char* kAccessoryVersion = "1";
constexpr const char* kAccessoryUri = "https://github.com/whoissaaif/mycam";
constexpr const char* kAccessorySerial = "0001";

} // namespace mycam::proto
