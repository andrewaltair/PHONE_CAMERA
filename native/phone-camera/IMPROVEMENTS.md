# MyCam: improvement backlog

Effort: **S** = small (hours), **M** = medium (a day or two), **L** = large (a week or more).
Status: ✅ done · 🟡 partly done · ⏳ to do. Day-to-day tracking lives in [TASKS.md](TASKS.md).

## Field test results

| Scenario | Result | Notes |
|---|---|---|
| Long video call | ✅ Uninterrupted | 2026-10-09 |
| Phone screen locked while streaming | ✅ Uninterrupted | 2026-10-09 |
| Unplug and replug | ✅ Reconnects instantly | 2026-10-09 |
| Opening the phone's own camera app | ✅ Stream stops | Correct behavior. **Do not change.** |
| Pause / resume (phone and PC) | ✅ Works | 1.1.0, user test 2026-10-09 |
| Windows locked while streaming | ✅ Pauses, resumes on unlock | Fixed in 1.1.0 (was a privacy issue); user test 2026-10-09 |
| Install, upgrade, uninstall (installer) | ✅ Works | Tested on this PC, 2026-10-09 |
| **Clean install** from the GitHub release (no MyCam, no UsbDk, no phone driver binding) | ✅ Works | 1.2.0, 2026-10-09: installer brought UsbDk, first-use phone switch + WinUSB prompt worked, streaming 1080p, 0 decode errors |
| Signed release APK (R8-shrunk, 1.5 MB) | ✅ Works | 1.2.0 on the phone, back and front camera |

The shipped binaries depend only on DLLs that are part of Windows (checked with `dumpbin`): no Visual C++
runtime, nothing from the build tools. Windows 11 **N** editions need the Media Feature Pack (video codecs).
A test on a *different* PC is still worth doing; Windows Sandbox can't run here because CPU virtualization
is off in this PC's firmware.

Still untested: pause from the phone's **notification**, pause **surviving unplug/replug**, PC sleep and
resume, killing or crashing the companion mid-stream, and two phones plugged in at once.

---

## 1. Privacy and control: ✅ done (1.1.0)

### 1.1 Pause button on the phone **and** the PC (S): ✅
**Pause / Resume** in four places: the phone app, the phone's notification, the PC tray menu and the PC
settings window. Pausing on either side pauses both: the phone camera turns fully off, and apps see an
Aero **"Camera paused"** picture instead of black or a frozen frame.
- The phone owns the pause state and remembers it across reconnects, so a cable glitch never turns a
  paused camera back on.
- Protocol v2: PC → phone `CMD_PAUSE` / `CMD_RESUME`; phone → PC `STATE_PAUSED`.
- When there's no phone video (not connected, starting, phone problem), the camera shows a
  "Waiting for the phone" picture instead of black.

### 1.2 Pause when Windows is locked or asleep (S): ✅
The phone camera turns off on lock or sleep and comes back on unlock. This is separate from the manual
pause: unlocking never undoes a pause you chose. As a guard, no live video reaches the camera while paused
or locked, even frames already in flight.

### 1.3 Aero design on the phone app (M): ✅
Glass header, Win7 command link for Pause / Resume, Aero buttons, blue "main instruction" status,
LIVE pill, Selawik font, new launcher icon (adaptive, legacy and themed).

---

## 2. Reliability

### 2.1 Recover from a force-kill or crash (S): 🟡
- ✅ A phone left streaming after the companion died is now told to stop as soon as a new companion
  connects.
- ✅ Streaming now goes over WinUSB, so a killed companion normally no longer leaves the phone stuck behind
  UsbDk.
- ⏳ The UsbDk fallback path (first connection, before the WinUSB driver is bound) can still leave the
  phone hidden until replugged. Detect it and reset the port, or tell the user to replug.
- 🟡 USB debugging on (#9, built, needs a phone test): the accessory then enumerates as a composite device
  (18d1:2d01, accessory + adb), and the driver binder skipped every `&MI_` interface, so the accessory
  never got WinUSB and never connected. It now binds interface 0 (`MI_00`, always the accessory) of 2d01,
  2d04 and 2d05, and leaves adb, audio and the audio-only products (2d02, 2d03) alone
  (`IsAccessoryInstance`, unit-tested).

### 2.2 Fewer UAC prompts on first use (S): 🟡 built in 1.3.2, needs a new-phone test
A new phone used to prompt twice: once at install, then again for WinUSB binding.
- ✅ The installer registers the scheduled task "MyCam phone driver" (runs as SYSTEM, only
  `MyCamCompanion.exe --bind-driver`). Signed-in users may start it but not change it (verified: a non-admin
  start works; a non-admin re-register gets "Access is denied"). The uninstaller deletes it.
- ✅ The companion starts the task instead of asking for UAC, and only trusts it if it still points at its
  own exe. If the task is missing, it falls back to the old one-time UAC prompt.
- ⏳ End-to-end check: a phone that has never been connected should go straight to "Ready" with no prompt.
  The test phone is already bound. Use another phone, or remove the accessory device in Device Manager
  (View > Show hidden devices) and replug.

### 2.3 Test the remaining scenarios (S–M): ⏳
See "Still untested" above, plus phone low-battery mode and Android killing the service under memory
pressure. Fix whatever breaks.

---

## 3. Picture quality and camera control: ✅ done (1.3.1), except phone-side latency

### 3.1 Crop to fill (S): ✅ (1.3)
A portrait phone gives a narrow image with black bars. Add a "Crop to fill" option that center-crops to
16:9 (`DrawFittedNV12` in `pc/vcam/frame_transform.cpp`), with a toggle in the settings window.

### 3.2 Resolution and frame-rate options (M): ✅ (1.3); 30 / 60 / 120 fps via high-speed capture in 1.3.2 (needs a test)
Correction: the test phone's back camera *does* offer a constrained high-speed mode (1080p and 720p at 120 and
240 fps); only the normal modes stop at 30. 1.3.2 opens a high-speed session at 120 fps and the encoder keeps
every other frame (`KEY_MAX_FPS_TO_ENCODER`, Android 10+), giving 1080p60. Short exposure (≤ 8 ms) makes it
noisier in dim light. If the phone refuses the session, the stream falls back to 30 fps. The front camera has
no high-speed mode, so it offers only 30 fps.
1.3.2 also offers **120 fps** (the high-speed capture passed straight through, 30 Mbps) as a third choice on
both apps; each choice is enabled only if the camera and encoder can do it (CAMERA flag 0x40), and the
virtual camera offers 1080p120 and 720p120 to apps. Noise is the same at 60 and 120 (same exposure limit).
Availability is worked out per quality (CAMERA frame-rate masks, 21-byte payload), by the same
`modeFor()` check that starts the camera, so a phone that only does 4K30 never offers 60/120 at 4K.
Before the camera has reported anything, the phone's Resolution list now leaves 4K off too (#11).
720p / 1080p / 4K, and 60 fps where the phone supports it. Add a protocol command to request a mode, and
advertise the matching media types from the virtual camera.

### 3.3 Camera controls (M): ✅ (1.3), plus an Auto reset button (1.3.1). Lens choice via zoom ratio only where the phone exposes it (the test phone: 1–10×, no ultrawide).
Zoom, tap-to-focus, exposure, torch, and lens choice (ultrawide / telephoto). Phone UI first; exposing them
to PC apps through `IKsControl` is a stretch goal.
Before the camera has opened, the CAMERA packet reports zoom 1.00 and EV 0 inside its placeholder ranges, not
a saved value outside them (#16).

### 3.4 Lower latency (M): 🟡 PC side done (1.3–1.3.1): delivery on arrival, one less copy, adaptive bitrate when the USB link is the limit. Measured: PC decode+copy 3–12 ms; **phone capture→encoded 150–230 ms** (MediaTek encoder buffers ~3 frames). Next: try encoder low-latency vendor keys, fewer camera buffers, I-frame interval.
Decode on the GPU (D3D11 / DXVA) instead of the CPU (built, see below), cut extra frame copies, and add a
latency measurement (timestamp in the frame header → log).

Phone-side round (built, needs a test on the phone):
- The log splits the delay: `latency: ... (camera X ms, encoder ~Y ms)` (sensor → capture result vs.
  sensor → encoded frame), so we know which half to attack.
- Camera: electronic stabilisation off (it holds frames back to look ahead), fast noise reduction and
  edge modes, where offered.
- Encoder: no B-frames; on Android 12+ every vendor parameter with "low-latency" in its name is switched
  on, and the full list is logged (`encoder vendor parameters: ...`) for further tuning.
- PC: 16 queued USB reads (256 KB) instead of 4, so a 4K frame no longer stalls the phone's write while
  the previous frame decodes.

GPU decode (#8, done, 4K30 tested with a phone): the Media Foundation decoder gets a Direct3D 11
device and decodes through DXVA, falling back to the CPU when the decoder or GPU can't (logged as
`decoder: init ... on the GPU/CPU`). If a key frame fails on the GPU, the CPU takes over for the rest of the
run. Measured on an RTX 3050 with a synthetic 4K30 stream at 38 Mbps: decode+copy **36.3 ms on the CPU →
13.7 ms on the GPU**, output bit-identical. The CPU was over the 33 ms a 30 fps frame has; the GPU
leaves room. Still open from #8: the phone's bitrate control can't tell a busy PC from a full link, and
decoding still runs on the USB thread.

Decoder init failures (#15, fixed): a failed `decoder_.Init` used to be retried and logged on every incoming
frame (30-120 log lines a second); it now waits 2 s between attempts.

---

## 4. Phone app

### 4.1 Dim the screen while streaming (S): 🟡 built, needs a test on the phone
- After 30 s untouched while streaming (or the new "Dim the screen" command link) the app shows a nearly
  black screen at 2 % brightness; it drifts every minute against OLED burn-in. Any touch wakes it (that
  touch does nothing else).
- Heat: the service listens to `PowerManager` thermal status (Android 10+), warns on both screens from
  MODERATE up, and copies changes to the PC log.
Original plan: a dark aurora "streaming" screen keeps the phone cooler, saves battery and is more private,
with a warning when the phone gets hot.

### 4.2 Branding and release signing (S): ✅ (1.2.0)
- ✅ Real app icon (Aero webcam, adaptive + legacy + themed).
- ✅ Package name `io.github.whoissaaif.mycam` (was `com.example.mycam`). Android treats it as a new app:
  uninstall the old one once.
- ✅ Release signing: `tools\create-release-key.ps1` makes the key in `%USERPROFILE%\.mycam\` and a git-ignored
  `keystore.properties`; `gradlew assembleRelease` then signs. **Back up the key and its password.**

---

## 5. PC app: ✅ done (1.0–1.1), except code signing

### 5.1 Tray icon that shows status (S): ✅
Aero webcam with a status badge: disconnected (grayscale), ready, streaming (green), paused (amber),
error (red). 16–256 px, DPI-aware.

### 5.2 Settings window (M): ✅
Aero window (Direct2D): glass title bar, live status with a LIVE pill, Pause / Resume, back/front camera,
mirror, start with Windows, reconnect, open log folder. Keyboard: Tab / arrows, Space / Enter, Esc.
- ⏳ Screen readers: a UI Automation provider (`uia_provider.cpp`, docs/redesign.md §8.4) gives one fragment per
  element of `ui_model.h` (Invoke, Toggle, Selection / SelectionItem per radio set, ExpandCollapse, read-only
  RangeValue) and makes the status headline and sentence live regions. Checked with `pc/tools/uia_dump`
  (`-DMYCAM_BUILD_TOOLS=ON`); still to try with Narrator itself.

### 5.3 Real installer (M): ✅, code signing pending
Inno Setup wizard with Aero artwork (`installer/`, built by `installer/build.ps1`). It installs UsbDk if
needed, registers the camera, offers "Start with Windows", and lists MyCam in Settings → Apps. Upgrades
close the running companion cleanly; the uninstaller asks before removing UsbDk.
- ⏳ **Code signing.** Needs a code-signing certificate; `mycam.iss` is ready for it (`/DSIGN`).
- ⏳ **Remove UsbDk completely.** UsbDk's own uninstaller (`msiexec /x`) unhooks it from USB but leaves its
  service and `UsbDk.sys` behind, even after a restart (inert, but not clean). When the user chooses
  "Also remove UsbDk", the uninstaller should also run `sc delete UsbDk` and delete
  `%WINDIR%\System32\drivers\UsbDk.sys` (found in the clean-install test, 2026-10-09).
- Licensing: libusb is LGPL-2.1 and statically linked, so anyone given the binaries must also be able to
  get the source. The GitHub repo is private: if you share builds, share the source too (or make the repo
  public).

---

## 6. Project hygiene: ✅ done
- ✅ Git, with dev tools kept out of shipped builds (`-DMYCAM_BUILD_TOOLS=ON`).
- ✅ Unit tests: C++ (`pc/tests`: packet parser, frame transform, BGRA→NV12) and Android (`ProtocolTest`).
- ✅ Protocol spec in `protocol/PROTOCOL.md` (v2); both sides tested against `protocol/golden.txt`.

---

## 7. Design language and assets: 🟡 Windows XP redesign built (branch `xp-redesign`), testing left

### 7.1 Decision: ✅ Windows XP (Luna Blue), replacing Windows 7 Aero (2026-10-10)
The owner moved the look from Win7 Aero (the 2026-10-09 decision) to **Windows XP, Luna Blue only**:
- no dark mode;
- a new friendly camera logo, because the old webcam read as an eye and felt like surveillance;
- a camera preview on the PC only;
- a sunny "hills and sky" pause picture. The photo in the local `inspo/` folder is a reference only; the
  scene is painted by script.

The full spec, findings and roadmap are in [redesign.md](docs/redesign.md).

### 7.2 Style guide (S): ✅ [design/STYLE.md](design/STYLE.md)
The tokens live in `app/.../ui/theme/Color.kt`, `pc/companion/xp_draw.cpp` and `design/tools/luna_draw.ps1`.
Type: DejaVu Sans on the phone (shipped, free licence). Tahoma and Trebuchet MS on the PC (installed with
Windows, not shipped).

### 7.3 Assets: ✅ redrawn for XP
All of it is drawn from scratch by scripts in `design/tools/` (shared library `luna_draw.ps1`).

| Asset | Status |
|---|---|
| "Snap" logo: Windows `.ico`, Android adaptive / legacy / round / themed, notification icon | ✅ |
| Status family (tray, window, phone): ready, streaming (green ▶), paused (amber ‖), error (red ×), disconnected (grey) | ✅ |
| Camera pictures: "Camera paused" (hills + frosted glass card), "Waiting for the phone" (morning tint + animated green marquee drawn by the companion) | ✅ |
| Installer art, Play Store icon and feature graphic | ✅ |
| Phone first-run and Dim-screen hills (drawn in Compose) | ✅ |

### 7.4 What the redesign changed (S–L): 🟡 built, testing left
- **Phone:**
  - Now / Settings tabs and XP task groups, with the hero Pause.
  - A pairing dialog with a blurred backdrop and a countdown bar, and per-PC Forget.
  - First-run cards, a landscape two-pane layout, per-state frame-rate reasons, and the night-hills Dim screen.
  - Accessibility semantics.
- **PC:**
  - A 720 × 500 task-pane window that fits small screens, with a live preview (frosted LIVE / mode chips).
  - A pairing dialog with Cancel and a countdown, a green marquee while connecting, keyboard groups and
    access keys, and tray check marks plus a Quality submenu.
  - A UI Automation provider.
- **Left to do:** real-phone tests of every flow (USB and Wi-Fi), a Narrator pass, and checking high contrast
  and reduced motion by eye.

### Notes and risks
- **Tray menu:** uses the native Windows 11 menu, which is accessible.
- **Accessibility:** every text pair meets WCAG AA. Amber and red are used only in badges and icons.
- **Licensing:** no Microsoft artwork, wallpapers or fonts are shipped.

---

## 8. Publishing and releases: 🟡

- ✅ GitHub repo (private): https://github.com/whoissaaif/mycam (MIT license).
- ✅ Commit history uses a GitHub no-reply email; `inspo/` and raw screenshots are excluded.
- ✅ Releases **v1.1.1** and **v1.2.0** (installer + APK; 1.2.0 is signed with the release key).
- ✅ Release checklist in [TASKS.md](TASKS.md#releases).
- ⏳ Decide public vs. private before sharing (download links and the phone's "get the app" link only
  work for you while the repo is private).

---

## 9. iPhone support (L): 🟡 web camera built (branch `aiyan-webtest`), iPhone tests left

Aiyan's part of the project ([docs/AIYAN-IPHONE.md](docs/AIYAN-IPHONE.md)).

**Approach: a web page, not an app.** A native iOS app needs a Mac with Xcode and the $99/year developer
program, so the iPhone uses Safari instead ([docs/iphone-web-plan.md](docs/iphone-web-plan.md)). The PC
companion serves a page over HTTPS. The page:
- captures the camera;
- encodes H.264 on the phone's hardware (WebCodecs);
- sends normal MyCam packets over a secure WebSocket, one per message
  ([docs/web-camera-format.md](docs/web-camera-format.md)).

On the PC, the page is a third transport in the one single-threaded `SessionLoop`, next to USB and Android
Wi-Fi. Everything after the transport carries over unchanged: decoding, the virtual camera, pause and
lock-pause, the status pictures, the controls. It also works on iPads, on Android phones without the
app, and in desktop browsers.

### 9.1 Wireless (the web page): 🟡 built, iPhone tests left
- [x] PC: HTTPS + WebSocket server in the companion (`web_server.cpp`):
  - Schannel TLS with a self-signed certificate made once per user (`tls_server.cpp`);
  - the page embedded in the exe;
  - port 47900.
- [x] PC: the page as a transport in `SessionLoop` (`phone_link_web.cpp`), with no new threads.
  - A plugged-in phone takes over.
  - Other pages wait while a phone is connected.
  - A paused page that iOS disconnects keeps its session for 5 minutes.
- [x] Pairing: QR code → one-time code (128 bits, 5 minutes) → `POST /pair` → device token (256 bits;
  the PC stores only its hash). `/ws` refuses everything else.
- [x] Settings: "Connect an iPhone" page with the QR code (Nayuki's qrcodegen, MIT), a choice of network,
  and remembered iPhones with Forget. An iPhone card on Home.
- [x] Installer: inbound firewall rule, TCP 47900, private networks only; removed on uninstall.
- [x] Page (XP Luna look):
  - Start, pause/resume, back/front camera, dim screen, wake lock;
  - CAMERA frame-rate masks from the camera *and* the encoder;
  - back-pressure with bitrate steps;
  - STATE(paused) when iOS takes the camera, and reconnects by itself.
- [x] Tests:
  - WebSocket/HTTP/handshake/tokens in `mycam_tests`;
  - `protocol.js` against golden.txt (`web_golden`, needs Node.js);
  - end to end with `web_probe`, `fake_page.js`, and `browser_check.js` (the real page in headless Chrome
    with a fake camera).
- [ ] On the iPhone 13: the definition of done in AIYAN-IPHONE.md §5. This covers rotation, lock and
  resume, 1080p30 for 10 minutes, 1080p60, the measured delay, and Forget.

### 9.2 Wired: 🟡 foundation built, test left ([docs/iphone-wired.md](docs/iphone-wired.md))
- [x] The PC recognises an iPhone's USB hotspot adapter, offers it in the network choice and reports
  "USB hotspot".
- [ ] Test Personal Hotspot over USB (Apple Devices app), and fill in the findings table.
- [ ] Native app + usbmux: only if a Mac and the developer program become available. The old code on
  branch `aiyan` is reference only (two threads, can deadlock).

**Differences from Android (to tell users)**

| | Android | iPhone (web page) |
|---|---|---|
| Starting | The app opens itself when plugged in | Open the MyCam page (or its Home Screen icon) and tap Start |
| Phone locked or Safari in the background | Keeps streaming | Pauses: apps see "Camera paused"; it resumes when the page is back in front |
| Brightness and focus controls | Yes | No (Safari can't set them); zoom and torch where the camera has them |
| PC setup | MyCam installer | MyCam installer; tap through Safari's certificate warning once |

Continuity Camera (Apple's built-in iPhone webcam) only works with Macs, so it doesn't help on Windows.

---

## 10. Windows 10 support (M): 🟡 shipped in 1.5.0; confirmed working in Discord on a friend's real Windows 10 PC; Zoom/OBS/browser and an upgrade install still untried

Today the installer requires Windows 11, because the PC shows "MyCam" through `MFCreateVirtualCamera`,
which only exists on Windows 11 (build 22000+). Everything else (USB, WinUSB binding, decoding, tray,
settings window, installer) already works on Windows 10. Pre-launch item; full plan, work items and
test history are in [docs/WINDOWS10-SUPPORT.md](docs/WINDOWS10-SUPPORT.md).

**Plan, in short:** a second small COM DLL registered as a DirectShow video capture source (the approach
OBS Virtual Camera uses), reading the same `Global\MyCamFrame` shared memory `pc/vcam/` does today. The
installer picks which one to register by Windows version; `MinVersion` drops to Windows 10 1809. No
kernel driver, no driver signing.

---

## 11. Wireless option (L): 🟡 phases 1 and 2 built on branch `wireless` (1.4.0-beta3); a few tests left before merging

MyCam is wired-only by design (plug and play, no setup, no lag spikes, phone charges while streaming).
A wireless mode would be an **optional** second way to connect, with the cable staying the default.

**How it would work**
- Phone and PC on the same Wi-Fi (or the PC on the phone's hotspot). The phone app gets a "Wireless" switch
  that starts the same streaming service over a TCP connection instead of the USB accessory.
- **Discovery:** the PC companion finds phones on the local network (mDNS / DNS-SD, e.g.
  `_mycam._tcp`), so nobody types IP addresses.
- **Pairing, once per phone:** the PC shows a 6-digit code (or a QR code) and the phone confirms it.
  Unpaired devices are refused. A webcam on the network must not be open to anyone on the same Wi-Fi.
- **Encryption:** TLS with the key from pairing, so the video can't be watched on the network.
- **Same protocol** (`protocol/PROTOCOL.md`) on top of TCP. The PC side becomes a second transport next to
  USB. Aiyan's `ITransport` refactor (branch `aiyan`) is the natural base once its threading issues are fixed.
- **Adaptive bitrate** already exists and matters more here: Wi-Fi throughput and delay vary a lot.

**Differences from wired (to explain to users)**

| | Wired (USB) | Wireless (Wi-Fi) |
|---|---|---|
| Setup | Plug in | Same network + one-time pairing code |
| Delay | Lowest | Higher, with occasional spikes on busy Wi-Fi |
| Quality | Up to 4K / 120 fps | 1080p30 recommended; 4K and 120 fps only on strong 5 GHz Wi-Fi |
| Battery | Phone charges | Phone drains: warn in the app, keep the dim screen (4.1) |
| Starting | App opens itself when plugged in | Open MyCam on the phone and turn on Wireless |
| Windows | Nothing extra | Nothing extra: the PC only connects out, so there is no firewall prompt |

**Phase 1 (tested 2026-10-10, CMF Phone 1 / Android 16):** found in ~1 s, 1080p30 live through the virtual camera, phone-side delay ~106 ms, PC decode ~8 ms. The "Waiting for the phone" picture now mentions Wi-Fi.

Design: UDP discovery + TCP on the phone ("Use over Wi-Fi" switch) and PC
("Find phones on Wi-Fi"); the phone asks "Allow <PC>?" for every new connection (in the app and as a
notification); the cable always wins; Wi-Fi lock and a small send buffer on the phone keep delay low; the PC
side shares one single-threaded session loop with USB (no new threads). No pairing or encryption yet.

**Phase 2 (built and tested 2026-10-10):** one-time pairing with a 6-digit code compared on both screens (with a commitment, as in Bluetooth numeric comparison), then P-256 ECDH + HKDF + AES-256-GCM for every session; paired PCs connect without asking. Details in PROTOCOL.md "Wireless security". Tested live: pairing (codes matched), encrypted 1080p stream (0 decode errors), paired reconnect in 40 ms with no prompt, PC forgot the phone -> paired again with a new code, "Don't allow" -> no retry for 2 minutes. Interop proven with JDK-generated vectors in both test suites. Still to test before merging: cable takeover, 60/120 fps and 4K over Wi-Fi, a busy network.

**Work items**
- [ ] Phone: TCP server/client mode in the streaming service, mDNS advertising, pairing screen, battery warning
- [ ] PC: network transport, discovery list in the settings window, pairing UI, firewall rule in the installer
- [ ] Security: pairing, TLS, and refusing unpaired devices; review before release
- [ ] Bitrate/latency tuning for Wi-Fi; default to 1080p30 when wireless
- [ ] Testing: 2.4 vs 5 GHz, hotspot, Wi-Fi drop and reconnect, switching between cable and Wi-Fi

## 12. QR pairing for Android (M): 🟡 built and tested on the CMF Phone 1 (2026-10-13); iPhone and a second phone left

Pairing over Wi-Fi used to mean turning on Wi-Fi mode, finding the PC, waiting for a 6-digit code,
comparing it on two screens and tapping Allow. The PC already shows a QR code for iPhones, so Android
phones scan **the same code**: scan once, and the phone is paired and connected. The wire format is in
PROTOCOL.md "QR pairing". This section covers the design and the work.

**Decisions (owner):**
- **2026-10-12:**
  - **One QR code for both kinds of phone:** Safari for iPhones, MyCam's own scanner for Android.
  - **Scanner:** ZXing `core`, bundled (about 0.5 MB), on our own Camera2 code. It doesn't need Google
    Play services, so it works on every phone.
  - **The 6-digit pairing stays as the fallback** (Find Devices). It now needs Allow on the PC as well as on
    the phone. Before, a device on the network posing as a phone could approve its own pairing in
    milliseconds and then send video into the MyCam camera.
  - **Paired phones reconnect through discovery by themselves.**
- **2026-10-13:**
  - **No confirmation code and no Allow after a scan:** the scan is the consent, and it goes straight to
    connected. **Revised 2026-10-13 (same day, after a security review):** still true for *pairing* — there
    is no code to compare and the session comes up on its own — but not for the camera. A QR code is a
    bearer token that can be printed, forwarded or shown on a web page, so the phone now asks once, after
    connecting, whether that PC may use the camera, and keeps it off until it is told (§12.1).
  - **Nothing connects into the PC, so there's no "set the network to Private" step.** The first version
    paired over HTTPS to the PC's port 47900, which Windows blocks on Public networks. Now the PC only
    sends: it probes while the QR code shows, the phone answers with an invite proving it holds the code,
    and the PC connects out to the phone with the code mixed into the handshake's keys. iPhones still need
    the PC to accept connections (Safari connects to it), so they keep the Private requirement.
  - **Confirmation on both screens after any pairing** (QR code, 6-digit code, iPhone): "Paired with …",
    then the Camera page.

**Why it is safe without comparing codes:**
- The one-time code (128 bits) only exists on the PC's screen and in the scanning phone.
- It goes into the pairing key: `K = HKDF(z | code, …)`. Someone in between who never saw the screen can't
  produce a valid FINISHED.
- The invite's proof only lets the PC decide whether to connect. The handshake is the real check.
- From then on it is the ordinary paired handshake, with no new cryptography in the session itself.

**What that argument doesn't cover**, found on review the same day: it holds against anything *on the
network*, and it rests entirely on the first line — that the code only ever exists on a screen the user
trusts. Nothing in the protocol can tell a code off the user's own PC from one on a poster, in an email or
on a web page. An attacker who authors a code knows its `t`, so if they also hold the address in it on a
network the phone can reach, the handshake completes and that machine is paired — and, because the key and
wireless mode both persist, it could stream again later with nothing shown at all. The crypto is not the
gap; the missing step was anyone agreeing to be watched. Hence §12.1.

**Phone flow:**
1. On the Connect page, **Scan QR Code** opens a full-screen scanner (portrait while it shows). It asks for
   camera permission if needed, and it's disabled while a stream is live (the camera is in use).
2. A MyCam code (`t` + `pc`) gives a short vibration and closes the scanner. The Connect page shows
   "Looking for the PC…", then "Connecting to <PC>…".
3. Wireless mode turns on. When the PC's probe arrives, the phone sends the invite, and resends it every
   2 s. The PC connects in QR mode; the phone stores `K`.
4. It's connected: the app moves to the **Camera** tab and shows **"Paired with <PC>"** with OK.
5. Errors, in plain words, with **Scan again** and **Close**:
   - No probe within 20 s: "Can't find the PC on this network…".
   - `MYCAM-1 code`: "That code was already used or ran out…".
   - `MYCAM-1 busy`: "The PC is busy with another phone…".
   - Heard but never connected: "Found <PC>, but it didn't connect…".
   - Not a MyCam code: say nothing and keep scanning.
   - A code without `pc` (scanner screen): "Update MyCam on the PC".
   - Scanning a PC this phone is already connected to over Wi-Fi does nothing.

**PC flow:**
- On Home, the QR card is called **"Generate QR"** ("Show QR code"). The page is **"Connect a phone"**,
  with steps for both kinds of phone.
- While the code shows, the PC probes every 2 s (broadcast plus unicast to its /24) and answers invites.
  The code is used up after a pairing, and a new one appears.
- After any new pairing: the window goes to the **Camera** page and a **"Paired with <phone>"** pop-up
  appears (OK, or it closes by itself after 8 s). A Wi-Fi pairing also turns **Find phones on Wi-Fi** on, so
  the phone reconnects by itself next time.
- 6-digit pairing: **Allow** and **Don't allow** in the dialog and on the window's pairing page ("Don't
  allow" has the keyboard focus first). The page's Cancel button had never been wired up; it is now.

**Work items:**
- [x] PC: the QR URL carries `pc`; probes while the QR code shows; `MYCAM#1` invites (`ParseDiscoveryQrRequest`,
      `wifi::QrInviteProof`) with `MYCAM-1` refusals; QR mode in `Handshake` (`wifi::QrPairKey`); the code is
      used up afterwards.
- [x] PC: Allow / Don't allow on code pairing; a phone refused here isn't auto-connected again until restart.
- [x] PC UI: "Generate QR", "Connect a phone", the "Paired" pop-up (`PairingDialog::ShowPaired`), the Camera
      page after pairing (`SettingsWindow::ShowCamera`), Find phones on Wi-Fi turned on.
- [x] Phone: the scanner (`ui/QrScanner.kt`), the invite flow (`WebcamService.startQrPair` / `qrTick`), QR mode
      in `WifiHandshake`, `PairedDialog` and the Camera tab after pairing.
- [x] Golden vectors `qr.*` (JDK), checked by `QrPairingTest` and `mycam_tests`.
- [x] Removed the first version's HTTPS routes (`/app/pair`, `/app/connect`), the certificate pinning and
      the `f` field. They were never released.
- [ ] Page (`app.js`): on Android, offer "Open in the MyCam app" (an `intent:` link plus a `mycam://` intent
      filter) or "Use this page instead".
- [~] Tests on devices. **Passed 2026-10-13** (CMF Phone 1, release APK, PC on Ethernet set to **Public**):
      - Scan to encrypted session in 16–93 ms on the PC.
      - "Paired" on both screens; the Camera page and tab.
      - Find phones on Wi-Fi turned back on by the pairing.
      - The 6-digit fallback with Allow / Don't allow on the PC (earlier build).
      - Swiping MyCam away stops everything.

      **Still to test:**
      - An iPhone scanning the same code.
      - A second Android phone.
      - An expired or used code (`MYCAM-1 code`).
      - A network that drops broadcasts.
      - **The camera gate** (§12.1): Allow, Don't allow, and the first connection of a PC paired before it.
      - **A long or multi-line phone name** in the PC's pairing dialog (should stay on its line).

### 12.1 Security review of QR pairing (M): ✅ done (2026-10-13)

A review of the 1.4.2 branch. The crypto held up: no way in from the network, no code on the wire, nothing
logged. What it found was one design gap and a set of smaller things.

**The gap — nobody ever agreed to be watched.** See the revised decision above. Fixed by a per-PC camera
flag on the phone (PROTOCOL.md "Letting a PC use the camera"): a PC that hasn't been allowed gets a session
with the camera off, and the phone asks once, naming it. A 6-digit pairing sets the flag as the user taps
Allow, so that flow gains no extra tap. A QR pairing doesn't, so it gains one. Keys stored before the flag
existed read as "not allowed", which is what closes the silent-reconnect case for phones already paired.

**Also fixed:**
- Names a phone sends are cleaned before they reach a window or the registry (`web::CleanName`): valid UTF-8,
  no control characters, no line or paragraph separators, no bidi overrides, 40 bytes. Both the SERVER_HELLO
  name and the discovery name went through unchecked; line breaks in one would have drawn over the 6-digit
  code the user is told to compare. Layouts that carry such text are also clipped to their box now.
- A `MYCAM-1` refusal is a hint, not an answer (it is an unauthenticated datagram): the phone shows it,
  keeps inviting, and only reports it when the attempt runs out. A PC with an attempt in flight stays quiet
  instead of saying "busy", so a replayed invite can't make the real phone give up.
- The one-time code is claimed for the duration of a QR handshake; `POST /pair` answers 409 meanwhile.
- `wifi::SameMac` compares the phone's FINISHED in constant time (it was `operator!=` on a vector).
- Nobody answering the PC's Allow is no longer treated as a refusal: it was permanently blacklisting the
  phone until restart, and logging "refused". It now backs off like any "not now", and this PC's user gets
  their own 60 s from when the code appeared rather than whatever the phone left of a shared 75 s.
- The unicast probe sweep uses the adapter's real prefix instead of assuming a /24, and says so in the log
  when the subnet is too wide to sweep. On a /16 it was asking 253 of 65 000 addresses — the fallback failed
  on exactly the networks it exists for.
- The broadcast probe no longer goes out twice while the QR code shows.
- A QR invite's name gets the same 64 bytes as any other message (the proof was eating 33 of them).
- `QrPairing.parse` takes IPv4 literals only, and `unhex` plain ASCII hex only (`Character.digit` accepted
  other Unicode digit forms).
- A connect that a phone asked for over the network is no longer logged as "asked for on the PC".
- Turning "Find phones on Wi-Fi" on after a pairing is written to the log, since it also starts announcing
  this PC's name every 2 s.

**Known and left alone, with reasons:**
- **SERVER_HELLO goes out before the PC is authenticated**, so anything that connects to the phone's port
  47800 while wireless mode is on learns the phone's stable 16-byte id and its name. Fixing it means
  reordering the handshake, which is a protocol break for a tracking-grade leak; the gain didn't justify
  destabilising the connect path. Pre-existing, not from QR pairing.
- **A refused phone is remembered by IP address** (`netPhones_`), because discovery carries no device id, so
  a DHCP change can move that refusal onto a different phone or let the refused one back in. Only an
  explicit "Don't allow" is permanent now, which limits the blast radius.
- **An on-path attacker can still delay a QR pairing** by replaying invites. They could drop the packets
  instead, so authenticating the invite (which would break 1.4.2 phones) buys nothing.

**Fixed after 1.5.0: pairing keys left the phone in backups.** `allowBackup` was on with empty rules, so
every pairing key, "camera allowed" flag and the phone id went into Google's cloud backup and into a
phone-to-phone transfer with the rest of `mycam.xml`. They now live in their own `mycam_pairing.xml`
(`PairedPcs.PREFS`), which `backup_rules.xml` and `data_extraction_rules.xml` exclude. Settings still back
up. An upgraded phone moves the old entries over the first time it opens the store (`PairedPcs.migrate`). A
restored phone pairs again. Backups made before the fix still hold the old keys: Forget the PC on the PC
side to make one useless.

---

## 13. Linux support (L): ⏳

Pre-launch item alongside §10. Unlike Windows 10, there's no existing Linux code to extend — the PC
companion is a Win32 app end to end (UsbDk/WinUSB, Media Foundation decode, a COM virtual camera,
Schannel TLS, Win32 GUI). Full plan, phased work items and the open design question are in
[docs/LINUX-SUPPORT.md](docs/LINUX-SUPPORT.md).

**Plan, in short:** a phased port starting with a headless CLI daemon (USB only, writing decoded frames
into a `v4l2loopback` device) before any GUI work — that alone covers Zoom/OBS/browsers on Linux. Wi-Fi
and the iPhone web server follow in later phases; the tray/settings/pairing-dialog GUI is deliberately
last. **Decided (Aiyan, 2026-10-18): the Linux GUI keeps the same Windows XP Luna Blue look as Windows**
(one design language, per CLAUDE.md), reusing the portable UI model (`ui_model.h`, `ui_layout.h`,
`status_text.h`, `marquee.h`); to confirm with saif.

**Needs before starting:** a Linux test machine or VM, and `v4l2loopback` available on it.

**Progress:** a Linux test machine (Ubuntu 24.04) has v4l2loopback working. `pc/linux.cmake` builds the
portable companion code and the unit tests on Linux, with `wifi_crypto_openssl.cpp` standing in for
Windows CNG (the handshake on top moved to `wifi_crypto_common.cpp`, shared by both); CI runs them on
Ubuntu. The video path is in: `decoder_ffmpeg.cpp` (the `H264Decoder` interface on libavcodec, linked
dynamically) and `frame_writer_v4l2.cpp` (the `FrameWriter` interface into v4l2loopback, I420 at a fixed
size, fitted with `frame_transform`); `tools/v4l2_play.cpp` drives it from an H.264 file. TLS for the
iPhone page: `tls_server_openssl.cpp` (same `tls::Session` interface; certificate in `~/.config/mycam/`),
tested against an OpenSSL client (`tls_tests`). Correction to the plan: `web_server.cpp` and
`phone_link_web.cpp` were *not* portable (Winsock, registry, exe resources, adapter list), and the session
loop lives in `PhoneLink` with the USB code. So the OS now sits behind one seam: `platform.h` (time, text,
adapters, a settings store, secret protection, embedded files) and `net.h` (sockets), implemented by
`platform_win.cpp` (the same registry keys and DPAPI as before) and `platform_posix.cpp` (`~/.config/mycam/`);
the USB backend split is `phone_link_usb_win.cpp` (UsbDk + WinUSB) / `phone_link_usb_posix.cpp` (libusb).
The shared `PhoneLink`, `SessionLoop` and `WebServer` build on both. `mycam-companion --daemon` runs on
Linux: the iPhone page is served (QR code in the terminal with `--pair`), USB and Android Wi-Fi are built
but not yet tried on a phone.

**GUI, phase 4a (2026-10-20):** `mycam-tray` (`tray_linux.cpp`), a separate program in the desktop session,
so the camera service stays headless. It shows the Windows tray icons (the same `.ico` files, embedded) and
the Windows tray menu through AppIndicator, which Ubuntu's dock shows out of the box. The daemon does the
menu logic and sends one status line on every change (`control_proto.h`); menu clicks go back as commands
on the same control socket, which now serves each client on its own thread (a tray and `--pair` at once).
Mirror/Fill are saved in the store like on Windows. The "Connect an iPhone" window is plain GTK until 4b.
**Phase 4b, next:** the settings window, sharing the Windows page code through a cross-platform XP drawing
kit (Direct2D on Windows, Cairo/Pango on Linux) — decided 2026-10-20 over a separate Linux-only copy.

---

## Suggested order

1. **2.3** Test the untested scenarios (cheap, and it finds the real bugs)
2. **2.1 / 2.2** Remaining reliability and first-use polish
3. **3.1** Crop to fill, and **4.1** dim the screen while streaming
4. **7.2** Write `design/STYLE.md`
5. Everything else as needed (3.2–3.4, 5.2 accessibility, 5.3 code signing)
