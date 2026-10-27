# What changed from the original PHONE_CAMERA

PHONE_CAMERA now uses a single native video path derived from
[MyCam](https://github.com/whoissaaif/mycam). The Android application and
iPhone browser page send video to the same Windows companion, which publishes
it directly as the `PHONE_CAMERA` system camera.

| Capability | Original browser mode | Unified native video mode |
| --- | --- | --- |
| Windows camera | OBS Browser Source and OBS Virtual Camera | Media Foundation on Windows 11; DirectShow on Windows 10 1809+ |
| Android | No dedicated PHONE_CAMERA Android app | Android companion with USB accessory and paired Wi-Fi paths |
| iPhone | Safari/WebRTC over local Wi-Fi or USB Personal Hotspot | Safari/WebCodecs with device pairing over a local IP link |
| Video settings | Browser requests, depending on capabilities | Device-reported camera settings and supported modes, including high-resolution/high-frame-rate options where available |
| Connection protection | Legacy signaling has no PIN | Existing MyCam-derived pairing and device authorization |
| Microphone | Browser stream can include the phone microphone | Video only; no Windows virtual microphone |
| Linux | Manual browser/Node setup | Inherited v4l2loopback daemon is preview code |

These are source-level capabilities, not a measured performance comparison.
The native camera components can be compiled and activated in a local test
without registration. A real phone, system registration, driver, calling app,
latency, image quality and long-running connection must be tested separately.

The legacy browser code remains available and is not used by the primary
no-OBS native video path. See [architecture and setup](MYCAM_NATIVE.md).