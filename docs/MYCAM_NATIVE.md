# MyCam native upstream companion

This directory documents the source-only upstream snapshot imported at
`native/mycam/`. It is an optional native companion reference, kept separate
from PHONE_CAMERA's existing WebRTC mode. The import does not replace, alter,
enable, install, or automatically select the WebRTC path, and it does not
claim that this repository can build or run MyCam as integrated product code.

## Provenance and scope

- Upstream project: [whoissaaif/mycam](https://github.com/whoissaaif/mycam)
- Upstream commit: [`a0499eab7375a99db6a6ca5ee3b8faa7f2d35f6a`](https://github.com/whoissaaif/mycam/commit/a0499eab7375a99db6a6ca5ee3b8faa7f2d35f6a)
- Import method: `git archive` of that commit, extracted below `native/mycam/`.
- Snapshot inventory: 329 tracked upstream files; no upstream `.git` directory,
  untracked files, working-tree data, credentials, signing keys, or build
  outputs were imported. The tracked file-content total at the source commit
  is 36,767,630 bytes (about 35.1 MiB, excluding archive metadata).

The upstream source keeps its own project name (`MyCam`), package layout,
build files, notices, and licenses. This subtree is not a binary distribution
and no APK, EXE, installer, driver, or generated release artifact is included.

The imported source contains:

- `app/`: Android Kotlin/Compose application. It captures the phone camera and
  sends MyCam protocol packets over Android Open Accessory USB or paired Wi-Fi.
- `pc/`: C++ PC companion and virtual-camera implementations. Windows 11 uses
  the Media Foundation Frame Server virtual-camera API; Windows 10 (build
  17763+) uses the DirectShow fallback. The same source also has a Linux
  preview daemon/tray path using v4l2loopback.
- `pc/companion/web/`: the iPhone/iPad Safari page embedded by the PC/Linux
  companion. iPhone uses WebCodecs H.264 over the local network; there is no
  separate iPhone app in this upstream snapshot.
- `protocol/`, `docs/`, `pc/tests/`, and Android tests: shared protocol,
  architecture notes, golden vectors, and test sources.

## Platform differences and known limitations

- **Android:** requires Android Open Accessory support (the upstream app has
  `minSdk = 24`). USB accessory mode can take over a connected phone while the
  companion is running; Wi-Fi requires one-time pairing. Android support is
  implemented in the source, but is not wired into PHONE_CAMERA here.
- **Windows:** Windows 11 uses `MFCreateVirtualCamera`; Windows 10 uses a
  DirectShow source filter. The companion and virtual-camera pieces are native
  Windows code and require the Windows SDK/toolchain. The upstream installer
  is not run or included by this import.
- **Linux:** marked **preview** by upstream. It uses a distro-provided
  v4l2loopback device and dynamically linked FFmpeg/libusb/OpenSSL. Upstream
  documents iPhone-over-Wi-Fi testing on Ubuntu 24.04; Android USB/Wi-Fi is
  built but was not tested on Linux in that snapshot. The tray is optional and
  there is no settings window in the preview.
- **iPhone/iPad:** requires a current browser with WebCodecs H.264 (upstream
  documents Safari), both devices on the same network, and the page in the
  foreground. Safari may stop the camera when backgrounded or the device is
  locked. Wired iPhone support is a plan, not a shipped feature.
- **General:** the upstream architecture supports one phone at a time; USB
  takes priority over Wi-Fi/iPhone. Pairing uses a self-signed certificate for
  the companion's local HTTPS page. These are upstream behavior/limitations,
  not claims about PHONE_CAMERA integration.

## Upstream build and test commands

Run these from the imported upstream root (`native/mycam/`) with the required
toolchain and dependency files available. They are recorded as upstream
commands; they do not modify or activate PHONE_CAMERA's WebRTC mode.

### Android

Requirements: Android Studio or a compatible JDK/Android Gradle environment,
the Android SDK required by the Gradle configuration (compile SDK 37), and the
SDK/platform dependencies resolved by Gradle.

```text
gradlew.bat assembleDebug
gradlew.bat testDebugUnitTest
```

The upstream release signing configuration is optional for debug builds. A
release keystore is external and is intentionally not part of this import.

### Windows PC companion

Requirements: Visual Studio 2022 Build Tools, Windows 11 SDK, CMake, and the
Windows libraries selected by the upstream CMake files.

```text
cmake -S pc -B pc/build -A x64
cmake --build pc/build --config Release
ctest --test-dir pc/build -C Release
```

Add `-DMYCAM_BUILD_TOOLS=ON` at configure time for upstream developer tools.
The upstream installer command requires Inno Setup 6 and is not an import or
installation step:

```text
powershell -ExecutionPolicy Bypass -File installer\build.ps1 -Version X.Y.Z
```

### Linux preview

The upstream Ubuntu 24.04 example installs build tools and development
packages, then configures the Linux CMake split:

```text
sudo apt install build-essential cmake pkg-config libusb-1.0-0-dev libssl-dev libavcodec-dev libavutil-dev libswscale-dev
cmake -S pc -B pc/build-linux -DCMAKE_BUILD_TYPE=Release
cmake --build pc/build-linux -j
cd pc/build-linux && cpack
ctest --test-dir pc/build-linux
```

Linux also needs a suitable v4l2loopback installation for an actual camera
device; the upstream package recommends `v4l2loopback-dkms`. FFmpeg is linked
dynamically from the distribution rather than vendored. GTK 3 and Ayatana
AppIndicator are optional for the tray target; without them the daemon still
builds without `mycam-tray`.

### iPhone page and protocol checks

There is no separate iPhone build: `pc/companion/web/` is embedded into the
companion executable. Rebuild the companion after changing those files. The
`web_golden` test needs Node.js and is skipped by CTest when Node.js is absent.
The upstream developer-only `web_probe`, `fake_page.js`, and
`browser_check.js` checks require the tools build and (for the browser check) a
headless Chrome environment.

## Licensing and attribution

The upstream MyCam application and companion source are provided under the
MIT License in [`native/mycam/LICENSE`](../native/mycam/LICENSE). Keep that
notice with copies or substantial portions of the upstream source. This
upstream snapshot has its own name and licensing terms; importing it does not
relicense PHONE_CAMERA or imply that PHONE_CAMERA distributes a MyCam binary.

The imported tree retains the third-party notices required by upstream,
including:

- [`pc/third_party/libusb/COPYING`](../native/mycam/pc/third_party/libusb/COPYING):
  LGPL-2.1 for the vendored Windows libusb subset.
- [`pc/third_party/qrcodegen/LICENSE`](../native/mycam/pc/third_party/qrcodegen/LICENSE):
  Project Nayuki MIT license and attribution.
- [`app/src/main/assets/licenses/DejaVu-LICENSE.txt`](../native/mycam/app/src/main/assets/licenses/DejaVu-LICENSE.txt):
  DejaVu/Bitstream Vera/Arev font notices.

The upstream README also identifies FFmpeg as a distribution-provided
LGPL/GPL dependency on Linux and UsbDk as an Apache-2.0 dependency downloaded
by the upstream Windows installer. Neither dependency is silently converted
to a PHONE_CAMERA dependency by this source import; check the applicable
upstream and dependency licenses before redistribution.

## Verification record for this import

The source was read from the clean upstream checkout at the exact commit above
and extracted with `git archive`; the original upstream project was not edited.
Verification should compare the imported path against that commit with:

```text
git -C ..\mycam-reference archive --format=tar a0499eab7375a99db6a6ca5ee3b8faa7f2d35f6a
git -C ..\mycam-reference ls-tree -r --name-only a0499eab7375a99db6a6ca5ee3b8faa7f2d35f6a
```

On 27 October 2026, the imported Windows PC source was configured with Visual
Studio 2022 Build Tools and built in Release mode. The companion and both
virtual-camera DLLs compiled; the two CTest entries (`mycam_tests` and
`web_golden`) passed. These are build and protocol/unit checks, not a device
test. The driver and application were not installed or registered. Android,
Linux, a real phone-to-PC connection, and integration with the existing WebRTC
mode were not tested by this import.
