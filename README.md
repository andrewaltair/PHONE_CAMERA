# PHONE_CAMERA

Turn an Android phone or iPhone into a Windows webcam. The native PHONE_CAMERA
companion receives the phone's video and exposes **PHONE_CAMERA** as a system
camera for video calls, browsers and recording apps. OBS is not required.

The Android companion, iPhone browser page, PC controls and virtual-camera
backends are one product, based on [MyCam](https://github.com/whoissaaif/mycam)
by whoissaaif. The upstream MIT license and third-party notices are retained.

## How it connects

| Phone | Connection | Phone-side software |
| --- | --- | --- |
| Android | USB or paired local Wi-Fi | PHONE_CAMERA Android app |
| iPhone / iPad | Local Wi-Fi; USB Personal Hotspot is an IP connection | Safari page served by the PC |

The Windows companion decodes the stream and hands frames directly to a
Media Foundation virtual camera on Windows 11, or a DirectShow camera on
Windows 10 1809 and later. Camera selection, resolution, frame rate, zoom,
exposure, torch, focus, pause and pairing depend on the phone and browser.
The source supports modes up to 4K and 120 fps where the device supports them;
this is not a promise that every device supports those modes.

## Windows setup from source

The Windows installer and virtual-camera registration require administrator
approval. Android USB additionally uses the signed UsbDk driver; it is not
needed for Wi-Fi or the iPhone browser path. The source setup requires Visual
Studio 2022 C++ Build Tools, a Windows SDK and CMake.

```powershell
# Build and test without installing or changing system registration.
powershell -ExecutionPolicy Bypass -File setup.ps1 -BuildOnly

# Build and install PHONE_CAMERA, including support for Android USB.
powershell -ExecutionPolicy Bypass -File setup.ps1

# Build and install for iPhone / Wi-Fi without installing UsbDk.
powershell -ExecutionPolicy Bypass -File setup.ps1 -SkipUsbDk
```

Open PHONE_CAMERA from the Windows tray, choose the phone connection and then
select **PHONE_CAMERA** as the camera in your calling or recording app. On an
iPhone, scan the PC's QR code in Safari. The native page uses local HTTPS and
device pairing; first-time certificate trust depends on the phone and has not
been verified on every device.

A graphical installer can also be built from
`native/phone-camera/installer/build.ps1` with Inno Setup 6. See
[build and installation details](docs/MYCAM_NATIVE.md). The repository does not
claim an installer has been published as a release.

## Android companion

The Android app source is in `native/phone-camera/app`. It uses the new
application ID `ge.andrewaltair.phonecamera`, so it installs separately from
the original MyCam application. Building requires JDK 17, the Android SDK and
the Gradle wrapper:

```powershell
cd native/phone-camera
.\gradlew.bat testDebugUnitTest lintDebug assembleDebug
```

A debug APK is for testing and is not a production-signed release.

## Audio and the previous browser mode

The native PHONE_CAMERA virtual camera is **video-only**; it does not create a
Windows virtual microphone. The previous WebRTC browser mode remains in the
repository for its original camera-and-microphone browser stream. It is
separate from the native camera and is not used by the primary no-OBS path:

```powershell
powershell -ExecutionPolicy Bypass -File legacy-webrtc-setup.ps1
```

The legacy WebRTC signaling room has no pairing PIN, so use it only on a
trusted network. Its viewer and former OBS source share one receiving slot.

## Validation

```powershell
npm ci --ignore-scripts
npm test
npm run test:phone
powershell -ExecutionPolicy Bypass -File scripts/build-windows.ps1
```

The native tests and class-factory checks can validate builds without
registering a camera or installing a driver. They do not replace a real
Android/iPhone-to-Windows test, nor do they measure end-to-end latency or
image quality. Linux code inherited from MyCam remains a preview and is not
part of the Windows setup above.

## Licenses and attribution

- PHONE_CAMERA: [MIT](LICENSE).
- MyCam-derived source: [MIT](native/phone-camera/LICENSE).
- libusb, qrcodegen, fonts and UsbDk retain their own notices; see
  [third-party and native build notes](docs/MYCAM_NATIVE.md).