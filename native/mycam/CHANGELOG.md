# Changelog

Android app and PC companion share one version (SemVer: PATCH = fixes, MINOR = features, MAJOR = breaking
or protocol changes). Releases with their installers: https://github.com/whoissaaif/mycam/releases

## 1.5.1 — fixes

- **Windows 11: no more black camera.** A companion that started before any app opened MyCam (at login,
  say) fell back to the Windows 10 hand-off and never switched, so apps showed black and the phone camera
  never started. It now switches to the Windows 11 camera as soon as it appears. (#43, #44; came in 1.5.0)
- **USB phones with USB debugging on** connect again: WinUSB is now bound to the accessory interface of
  the phone's composite device. (#42)
- **4K keeps up:** H.264 is decoded on the GPU (DXVA) when the PC supports it. (#41)
- **Android: pairing keys stay out of backups and phone-to-phone transfers**, so a restored or copied phone
  doesn't carry this PC's pairing. Existing keys move out of the backed-up settings once. (#35)

No protocol changes.

## 1.5.0

Windows 10 support: a DirectShow camera next to the Windows 11 Frame Server one; the installer registers
whichever the PC supports.
