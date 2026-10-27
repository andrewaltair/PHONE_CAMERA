# PHONE_CAMERA native architecture and attribution

The main PHONE_CAMERA application is a branded, modified integration of the
[MyCam](https://github.com/whoissaaif/mycam) source by whoissaaif, originally
imported from commit `a0499eab7375a99db6a6ca5ee3b8faa7f2d35f6a`. It is no
longer an untouched upstream snapshot.

## Video path

Android USB, encrypted Android Wi-Fi and the paired iPhone Safari/WebCodecs
page feed the same native Windows companion. Its H.264 decoder publishes NV12
frames to a shared-memory channel consumed by the Windows virtual camera:

- Windows 11: Media Foundation virtual camera.
- Windows 10 1809+: DirectShow source filter.

The camera appears as `PHONE_CAMERA`. OBS is not part of this path. The
application ID, virtual-camera CLSIDs, Windows storage, process and installer
identities are distinct from the original MyCam installation; the phone wire
protocol and accessory identity remain compatible.

The native route is video-only. It does not supply a virtual microphone. The
old PHONE_CAMERA WebRTC browser stream is kept as a separate legacy mode for
its prior audio capability; the two transports do not share a phone session.

## Build and install

From the repository root:

```powershell
# Build and run native tests without system changes.
powershell -ExecutionPolicy Bypass -File setup.ps1 -BuildOnly

# Build and install the Windows camera and Android USB support.
powershell -ExecutionPolicy Bypass -File setup.ps1

# For iPhone or Wi-Fi only, omit the Android USB driver.
powershell -ExecutionPolicy Bypass -File setup.ps1 -SkipUsbDk
```

Installation requires administrator rights. It registers this product's COM
camera server, adds its private-network iPhone firewall rule and may install
UsbDk and its own phone-driver task. Windows startup is optional via
`-StartWithWindows` or the application's settings. It does not stop Windows Frame Server or
terminate a separate MyCam application.

Android builds require JDK 17, Android SDK platform 37 and Gradle dependencies:

```powershell
cd native/phone-camera
.\gradlew.bat testDebugUnitTest lintDebug assembleDebug
```

The Android application ID is `ge.andrewaltair.phonecamera`, so Android
treats it as a new application rather than an update of MyCam. The Gradle
debug APK is not a production-signed release.

A graphical Windows setup can be built with Inno Setup 6 using
`native/phone-camera/installer/build.ps1`. This compiles the application,
verifies the downloaded UsbDk package hash and builds an installer; building
does not run the installer. Android USB is an optional installer task.

The Linux daemon and v4l2loopback support remain preview code. The package does
not change global v4l2loopback options; a suitable PHONE_CAMERA-labelled output
device must be configured separately. See the
[Linux setup notes](../native/phone-camera/pc/linux/README.md). The Windows
setup does not install or validate that Linux path.

## Verification boundaries

Windows compile, CTest and process-local COM class-factory tests can run
without camera registration or a driver. They cannot establish that a phone,
a Windows camera consumer, USB, Wi-Fi, iPhone certificate trust, or actual
frame delivery works. Android build results are separate from installation and
real-device testing. Actual latency, FPS and image quality require measurements
on the target phone, PC and network.

## Licenses

- [PHONE_CAMERA MIT license](../LICENSE).
- [MyCam MIT license and original copyright](../native/phone-camera/LICENSE).
- [libusb LGPL-2.1](../native/phone-camera/pc/third_party/libusb/COPYING).
- [qrcodegen MIT license](../native/phone-camera/pc/third_party/qrcodegen/LICENSE).
- [DejaVu font notices](../native/phone-camera/app/src/main/assets/licenses/DejaVu-LICENSE.txt).
- UsbDk is distributed separately under its
  [Apache-2.0 license](https://github.com/daynix/UsbDk/blob/v1.00-22/LICENSE).

The Windows companion links the vendored libusb as a separate
`PHONE_CAMERA_libusb.dll`. Binary redistribution still needs the applicable
LGPL notices and source availability; retaining license text alone is not a
complete redistribution plan. Linux uses distribution-provided FFmpeg and
other libraries with their own applicable licenses.
