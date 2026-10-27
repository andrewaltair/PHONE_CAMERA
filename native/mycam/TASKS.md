# MyCam: task tracker

Working list. The detail behind each item is in [IMPROVEMENTS.md](IMPROVEMENTS.md) (section numbers in
brackets). Move items between sections as they progress, and record releases and decisions at the bottom.

Status: `[ ]` to do · `[~]` in progress · `[x]` done

---

## Now

- [x] Clean install test: removed everything, installed 1.2.0 from the GitHub release, first-use flow and streaming work
- [ ] Test the remaining scenarios [2.3]
  - [ ] Pause / Resume from the phone's notification
  - [ ] Pause survives unplug and replug
  - [ ] PC sleep and resume while streaming
  - [ ] Companion crash or kill mid-stream, then restart
  - [x] A phone that has never been connected (first-use prompts): passed in the clean install test
  - [ ] Two phones plugged in at once
  - [ ] Phone low-battery / battery-saver mode

## Next

- [ ] Recover when the UsbDk fallback leaves the phone hidden [2.1]
- [~] No UAC prompt for new phones (SYSTEM scheduled task from the installer) [2.2]: built and installed (1.3.2), test with a never-connected phone

- [~] Dim the phone screen while streaming, with a heat warning [4.1]: built, test on the phone
- [x] Write `design/STYLE.md` [7.2]
- [~] Windows XP redesign (branch `xp-redesign`, docs/redesign.md) [7]: phone app, PC window + preview + pairing dialog, logo, hills pictures, waiting marquee built
  - [ ] Test on the phone over USB and Wi-Fi (pairing dialog, countdown, per-PC forget, first run, dim screen)
  - [ ] Test the PC window with a real phone; check high contrast and reduced motion by eye
  - [ ] Narrator pass on the PC window (UI Automation)
  - [~] "Scan for phones" on the PC (docs/redesign.md §8.6): one-shot 6 s Wi-Fi scan, results with Connect, tray item; checked with demos, `uia_dump` and a real phone; Narrator pass to do
  - [ ] Release APK test (R8) before publishing
  - [~] First-run guide shown only once (persist on Skip, "Got it" or first connection)
  - [~] "Scan for PCs" (phone) and "Scan for phones" (PC) buttons
- [ ] Designer mockup pass: sidebar nav, cards, dropdowns/toggles/sliders, splash, QR pairing ([redesign-v2.md](docs/redesign-v2.md)) [7]
- [~] Redesign fixes from the owner's first pass (2026-10-11), all built; test on the phone and the PC:
  - [x] Phone: a finished scan that found nothing no longer says "Devices found"
  - [x] Phone: tapping a PC in Find Devices asks it to connect, so a pairing starts from either screen
  - [x] PC: "Scan for Devices" goes to the Devices page
  - [x] PC: the paused camera picture is the supplied image exactly
  - [x] PC: Resolution and Frame rate open with no phone (entries still capability-gated) + a note saying why
  - [x] PC: page transitions run on the performance counter and the vertical blank, with cached text layouts
  - [x] PC: Minimise and Close are drawn again (they were in the model but no page drew them)
  - [x] PC: Devices has a Connected Device section above Available Devices
- [x] PC window opened as a transparent flash (first frame now drawn before ShowWindow)

- [~] iPhone support through a web page (Aiyan; branch `aiyan-webtest`, docs/AIYAN-IPHONE.md) [9]: built, iPhone tests left
  - [x] PC: HTTPS/WebSocket server (Schannel, port 47900), the page as a third transport, QR pairing and
        remembered iPhones, firewall rule in the installer
  - [x] Page: protocol, capability masks, back-pressure, wake lock + dim, pause/background, reconnect
  - [~] iPhone 13: install → Connect an iPhone → scan → tap through the warning → Start → video in the
        Camera app (works; dev install) and a browser call (to do), then through the real installer
  - [~] Reconnect without scanning; Forget makes it scan again (Forget confirmed, also from the Devices page)
  - [ ] Pause/resume from the PC and the page; lock → "paused" picture → unlock → resumes
  - [x] Rotation: portrait, landscape and upside down upright on both cameras (iPhone 13)
  - [ ] The PC's quality / frame-rate choices match the iPhone; 1080p60 works
  - [ ] Delay measured and written into docs/web-camera-format.md; 1080p30 steady for 10 minutes
  - [ ] Personal Hotspot over USB test (docs/iphone-wired.md)
  - [ ] saif: Android USB and Android Wi-Fi unchanged
  - [x] PC: Camera and Resolution dropdowns opened an invisible list (a closed dropdown laid out after the open one cleared its panel)

- [~] QR pairing for Android: scan the PC's QR code (the same one iPhones use), pair and connect; PC asks
      Allow on 6-digit pairing too [12]: works on Public networks (no inbound connection), "Paired" on both screens;
      tested on the CMF Phone 1 (2026-10-13); second phone and iPhone left
- [x] Security review of QR pairing [12.1]: the camera gate (the phone asks once per PC before any video),
      phone names cleaned before they reach a window, refusals treated as hints, the probe sweep using the
      real subnet, and an unanswered Allow no longer banning the phone. Both test suites pass; the gate
      verified on the CMF Phone 1 (2026-10-14): fresh QR pairing holds the camera off until Allow, then
      streams normally. A multi-line phone name still wants a run on the device

- [~] Windows 10 support before launch: DirectShow camera, installer picks per Windows version
      [10, docs/WINDOWS10-SUPPORT.md]: shipped in 1.5.0; confirmed working on a friend's real Windows 10 PC —
      Discord shows live video, pause/resume, zoom/EV, camera switch, 0 decode errors over a ~7 min session.
      Zoom, OBS, a Chromium browser, and an upgrade install from a Windows-11-targeted previous version still untried
- [~] Linux support before launch: headless CLI daemon first (USB → v4l2loopback), Wi-Fi/iPhone/GUI
      phased in after [13, docs/LINUX-SUPPORT.md]. Test machine: Ubuntu 24.04, kernel 7.0 — v4l2loopback
      works (a test pattern shows in a browser). Build seam landed: `mycam_tests` + `web_golden` pass on
      Linux and in CI (OpenSSL in place of CNG). Video path works: FFmpeg decoder + v4l2loopback writer,
      checked with `v4l2_play` (an H.264 file → /dev/video10, rotation/mirror/letterbox). TLS on OpenSSL.
      `mycam-companion --daemon` builds and serves the iPhone page (platform.h / net.h seam; same
      PhoneLink as Windows). iPhone → Linux works end to end (2026-10-18, iPhone over Wi-Fi: paired by QR,
      66 frames, 0 decode errors, pause/resume from the phone); verified in Discord (Electron/Chromium) on
      the real machine: picture shows, upright when the phone rotates, normal colours. Also verified in OBS
      (PPA build; Ubuntu 24.04's packaged OBS 30.0.2 crashes on any v4l2loopback device, `free(): invalid
      pointer`, even an ffmpeg test pattern: not MyCam). The slight delay went away after unticking "Use Buffering" in
      the OBS V4L2 source (tell Linux users that). The phone camera now runs only while an app has MyCam
      open (inotify + /proc scan), and the camera shows the Windows waiting/paused pictures. `.deb` via
      `cpack` (CI artifact): daemon, udev rule generated from kAndroidVendors[], v4l2loopback options +
      load at boot, systemd user service. Reader detection verified on hardware (STOP 4 s after the
      last app closes MyCam). `mycam-companion --pair` shows the iPhone QR code from the running service
      (control socket in $XDG_RUNTIME_DIR). All on main (2026-10-20); 1.6.0 ships it as a preview.
      GUI phase 4a done (2026-10-20): `mycam-tray`, an AppIndicator tray with the Windows tray's icons and
      menu, over the control socket; verified on Ubuntu 24.04 with the iPhone (status, pause, mirror, front
      camera, Connect an iPhone window). Next: 4b, the settings window on a shared XP drawing kit
      (Direct2D on Windows, Cairo/Pango on Linux; needs saif's review and a Windows test). Android (USB +
      Wi-Fi) on Linux: last, once an Android phone is at hand

## Backlog

- [~] Lower phone-side latency (capture→encoded 150–230 ms on the test phone) [3.4]: camera/encoder tweaks and a camera-vs-encoder split in the log built; measure on the phone
- [~] 30 / 60 / 120 fps choice; 60 and 120 on the back camera through high-speed capture [3.2]: built, test on the phone
- [~] Screen-reader support (UI Automation) for the settings window and pairing dialog [5.2]: built (`uia_provider.cpp`, checked with `pc/tools/uia_dump`); try with Narrator
- [ ] Code-sign the installer and companion [5.3] — see [docs/AIYAN-WEBSITE.md](docs/AIYAN-WEBSITE.md)
- [~] USB debugging on: the accessory shows up as accessory+adb (composite), so WinUSB is now bound to its accessory interface (MI_00) too [2.1, #9]: built and unit-tested; test with a phone that has USB debugging on
- [ ] Test the release APK (not just debug) on a phone before every release: R8 only runs on release builds
- [ ] Uninstaller: remove UsbDk completely (service + UsbDk.sys stay behind after msiexec) [5.3]
- [ ] Test a clean install on a different PC (Sandbox needs CPU virtualization enabled in firmware) [2.3]
- [x] Decide public vs. private repo before sharing builds [8]: public since 1.4.0
- [ ] iPhone native app + usbmux transport on the PC (only if a Mac and the developer program become available) [9.2]
## Done

- [x] The phone's Resolution list no longer offers 4K before the camera has reported it [3.2, #11]
- [x] Before the camera opens, the CAMERA packet keeps zoom and EV inside their placeholder ranges [3.3, #16]
- [x] A failed decoder init retries and logs at most every 2 s, not on every frame [#15]
- [x] 4K: H.264 decodes on the GPU (D3D11/DXVA), CPU as fallback; 4K30 tested with a phone [3.4, #8]
- [x] Pairing keys and the phone id are kept out of cloud backups and phone-to-phone transfers (own prefs file, migrated on upgrade) [12]
- [x] Adaptive-bitrate Int overflow fix at 4K (`WebcamService.kt`), shipped in 1.3.2
- [x] Wireless option: Wi-Fi transport with discovery, one-time pairing and encryption; cable stays the default [11], shipped in 1.4.0

- [x] Assets complete: Play Store 512 px icon + 1024×500 feature graphic, dimmed streaming screen [7.3]

- [x] Picture quality and camera control: crop to fill, 720p/1080p/4K, 60 fps where supported, zoom/brightness/focus/torch + Auto, adaptive streaming [3] (1.3.1)

- [x] Package name `io.github.whoissaaif.mycam`, release key created, signed release [4.2] (1.2.0)
- [x] Ad brief for the motion designer (shared doc)
- [x] Wired phone-as-webcam: Android Open Accessory + Windows 11 virtual camera (1.0)
- [x] Fixes from first real-phone testing: MediaTek config, WinUSB streaming, reliable reads, logging (1.0)
- [x] Project hygiene: git, unit tests, protocol spec and golden vectors [6]
- [x] Design language decided: Windows XP, Luna Blue [7.1] (changed from the initial Windows 7 Aero choice; see Decisions below)
- [x] PC app: tray status icons, Aero settings window, Inno Setup installer [5.1–5.3]
- [x] Pause on phone and PC, "Camera paused" picture [1.1] (1.1.0)
- [x] Auto-pause while Windows is locked or asleep [1.2] (1.1.0)
- [x] Aero phone app and launcher icon [1.3] (1.1.0)
- [x] Published to GitHub, email hidden, release v1.1.1 [8]
- [x] The CPU wake lock is released when a link ends, so a phone left in wireless mode can sleep again [11]

---

## Releases

| Version | Date | What changed |
|---|---|---|
| **1.5.0** | 2026-10-18 | **Windows 10 support** [10]: a second COM DLL registers as a DirectShow video capture source (the OBS Virtual Camera approach) alongside the existing `MFCreateVirtualCamera` Frame Server backend; companion picks whichever matches the running Windows build. Installer registers the matching DLL and drops `MinVersion` to Windows 10 1809. Fixed an over-strict `QueryAccept` that rejected every DirectShow pin connection, and added a session-local shared-memory fallback for machines without a privileged service. Confirmed on a real Windows 10 PC: live video, pause/resume, zoom/EV, camera switch, 0 decode errors. Zoom, OBS, a Chromium browser, and an upgrade install from a Windows-11-targeted version are still untried. |
| **1.4.3** | 2026-10-14 | **Security review of QR pairing** (IMPROVEMENTS.md 12.1): the camera gate (the phone asks once per PC before any video, naming it; "Don't allow" forgets the PC), phone names cleaned before they reach a window or the registry, a `MYCAM-1` refusal treated as a hint instead of an answer, the probe sweep using the adapter's real subnet, the one-time code claimed for the duration of a handshake, constant-time FINISHED comparison, and an unanswered Allow no longer banning the phone. Verified on a real device: fresh QR pairing holds the camera off until Allow, then streams normally. |
| **1.4.2** | 2026-10-13 | **QR pairing for Android** (branch `qr-pairing`): scan the PC's QR code (the same one iPhones use) and it pairs and connects, with no code to compare and no "set the network to Private" step. "Paired with …" on both screens, then the camera. The 6-digit fallback now needs Allow on the PC too (closes the rogue-phone hole). The phone app stops when swiped away from recent apps. PC Home: "Generate QR". |
| 1.4.1 | 2026-10-11 | **iPhone support** through a page in Safari served by the PC (QR pairing, remembered iPhones, rotation from the motion sensors, firewall rule on private networks). Camera and Resolution dropdowns draw their list again. PC only: the phone app stays 1.4.0. |
| 1.4.0 | 2026-10-11 | **Stable.** Wi-Fi mode (discovery, one-time pairing with a code, encrypted sessions) and the v2 shell on both apps. A phone can now start the pairing itself from Find Devices. PC: Devices page split into Connected / Available, Scan goes there, Resolution and Frame rate open with no phone, vsync-paced transitions, Minimise and Close drawn again, the owner's "Camera paused" picture. Pairing surfaces moved off the old beige/navy. |
| 1.4.0-beta4 | 2026-10-11 | Windows XP (Luna) redesign of both apps, new camera logo, hills artwork, PC live preview, scan buttons, screen-reader support, Wi-Fi pairing dialog. |
| 1.3.3 | 2026-10-10 | Hotfix: the 1.3.2 phone app crashed on launch (R8 packageScope); clearer frame-rate hint. |
| 1.3.2 | 2026-10-10 | 30/60/120 fps per quality (high-speed capture), no-UAC phone setup, dim screen + heat warning, lower phone latency, 4K bitrate fixes. |
| 1.3.1 | 2026-10-09 | Quality & 60 fps, camera controls + Auto, crop to fill, adaptive streaming, lower PC latency (protocol v3). |
| 1.2.0 | 2026-10-09 | Package `io.github.whoissaaif.mycam` (new app on phones), signed with the release key, 1.5 MB APK. |
| 1.1.1 | 2026-10-09 | The phone's "get the app" link points to the GitHub repo. First GitHub release (exe + apk). |
| 1.1.0 | 2026-10-09 | Pause on phone and PC, auto-pause on Windows lock, status pictures, Aero phone app. |
| 1.0.0 | 2026-10-09 | First installer: tray status icons, Aero settings window, Inno Setup. |

Release steps: bump the versions (`app/build.gradle.kts`, `installer/mycam.iss`, `installer/build.ps1`),
run `installer\build.ps1` and `gradlew assembleDebug`, commit, tag `vX.Y.Z`, push, then
`gh release create vX.Y.Z <exe> <apk>`.

## Decisions

| Date | Decision |
|---|---|
| 2026-10-09 | Transport: Android Open Accessory (no Developer Options) + Windows 11 virtual camera |
| 2026-10-09 | UsbDk only switches the phone into accessory mode; streaming uses Windows' WinUSB driver |
| 2026-10-09 | Design language: Windows 7 Aero (option A) |
| 2026-10-10 | Design language changed to **Windows XP, Luna Blue only**, no dark mode; DejaVu Sans on the phone; new "Snap" camera logo (not an eye); preview on the PC only; original hills art (the photo in `inspo/` is a reference only) (docs/redesign.md) |
| 2026-10-09 | The phone owns the pause state; Windows-lock pause is PC-only and never undoes a user pause |
| 2026-10-09 | License: MIT. Repo private on `whoissaaif`. `inspo/` (Microsoft artwork) never committed |
| 2026-10-09 | Commits use the GitHub no-reply email |
| 2026-10-11 | A phone may ask a PC it can see to connect ("MYCAM+1" on the discovery socket), so a Wi-Fi pairing can be started from either screen (PROTOCOL.md "The phone asks for a session") |
| 2026-10-11 | The paused camera picture is `design/assets/source/camera-paused-reference.png` itself, cropped to 16:9 — not a recreation of it, and with no wordmark over it |
| 2026-10-11 | Repo is **public** again (it was private from 1.1.1). Everything pushed is world-readable, so the never-commit list is a hard rule. Commits are pushed as a matter of course; tagging and releasing still need an explicit ask. |
| 2026-10-11 | iPhone support goes through a web page served by the companion (WebCodecs + `wss://`), not a native app. One MyCam packet per WebSocket message; no protocol change (docs/web-camera-format.md) |
| 2026-10-11 | The QR code's one-time code is traded for a device token with `POST /pair` (HTTPS), not inside the WebSocket, so the "binary messages only" format stays as agreed. The PC stores only the token's SHA-256 |
| 2026-10-11 | The iPhone page's server listens whenever the companion runs (so paired iPhones reconnect without scanning); `/ws` needs a valid device token; the firewall rule is private networks only |
| 2026-10-12 | Android pairs over Wi-Fi by scanning the PC's QR code, the same code iPhones use (`t` + certificate fingerprint `f`), through `/app/pair` and `/app/connect` on the PC's HTTPS server. ZXing scanner bundled (no Play services). The 6-digit pairing stays as the fallback and now needs Allow on the PC too. Paired phones keep reconnecting through discovery (IMPROVEMENTS.md 12) |
| 2026-10-13 | **Scanning a QR code is consent to pair, not consent to be watched.** A code can be printed or forwarded, so the phone keeps a per-PC camera flag: a PC that hasn't been allowed gets a session with the camera off, and the phone asks once, naming it. A 6-digit pairing sets the flag as the user taps Allow (no extra tap); a QR pairing doesn't. Keys stored before the flag count as not allowed, which also stops a PC paired earlier from streaming again silently (PROTOCOL.md "Letting a PC use the camera", IMPROVEMENTS.md 12.1) |
| 2026-10-13 | Android QR pairing needs **no inbound connection to the PC**: the PC probes while the QR code shows, the phone answers with `MYCAM#1` (a proof of the code), and the PC connects out with the code mixed into the handshake keys. So no "set the network to Private" step (only iPhones still need it). Replaces the unreleased HTTPS `/app/pair` + pinning. After any pairing both screens say "Paired with …" and show the camera; a Wi-Fi pairing turns on Find phones on Wi-Fi (PROTOCOL.md "QR pairing", IMPROVEMENTS.md 12) |
| 2026-10-18 | Hosting: GitHub Pages (`gh-pages` branch), as recommended in docs/AIYAN-WEBSITE.md. Domain: `mycam.dev`, bought on Cloudflare. `CNAME` file pushed to `gh-pages`; DNS is 4 apex `A` records to GitHub's Pages IPs + a `www` `CNAME` to `whoissaaif.github.io`, all set to **DNS only** (not proxied) so GitHub can verify the domain and issue its own TLS cert. Site is live over HTTP at mycam.dev; HTTPS enforcement pending GitHub's cert issuance |
