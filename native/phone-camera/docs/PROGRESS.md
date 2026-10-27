# MyCam: progress log

How the project got from an idea to v1.3.3 and a working encrypted Wi-Fi beta, what was learned on the way,
and where it stands now. [TASKS.md](../TASKS.md) is the live to-do list. [IMPROVEMENTS.md](../IMPROVEMENTS.md)
holds the detail behind each item, and open problems are tracked as
[GitHub Issues](https://github.com/whoissaaif/mycam/issues).

---

## Where things stand (2026-10-10)

- **Latest release: v1.4.0**: https://github.com/whoissaaif/mycam/releases/tag/v1.4.0
  (`MyCam-1.4.0.apk` + `MyCam-Setup-1.4.0.exe`). Stable: Wi-Fi mode leaves beta and both apps ship the v2
  shell. Released from `xp-redesign` on 2026-10-11, tested live on the CMF Phone 1 over Wi-Fi.
- **Branches:**
  - `xp-redesign` = **1.4.0**, released and stable.
  - `main` = 1.3.3 (the previous stable); `wireless` = 1.4.0-beta3, now superseded by `xp-redesign`.
  - `aiyan` = a contributor's iPhone transport work, reviewed but not merged (see section 14).
- **Rollback points:** the tags `v1.4.0`, `v1.3.3`, `v1.3.2`, and `pre-wireless` (main before any Wi-Fi work).
- **Test devices now:** PC and phone both run 1.4.0. The phone is a CMF Phone 1 (A015, MediaTek, Android 16).
- **Working:**
  - **Streaming:** wired plug-and-play, front/back camera, and Wi-Fi (beta, paired + encrypted).
  - **Quality:** 720p/1080p/4K; 30/60/120 fps where each quality supports it.
  - **Camera controls:** zoom, brightness, focus lock, torch, and an Auto reset.
  - **Picture:** crop to fill; adaptive bitrate.
  - **Privacy:** pause from the phone or the PC; auto-pause while Windows is locked or asleep.
  - **Phone app:** dim screen while streaming; heat warning.
  - **Install:** no admin prompt for new phones after install; Aero design on both sides; installer; clean
    install verified.
- **Open issues:**
  - 4K decoding runs on the CPU, inside the USB/network read loop ([issue #8](https://github.com/whoissaaif/mycam/issues/8)).
  - USB debugging stops MyCam from connecting over USB.
  - Three Wi-Fi tests remain before merging (section 15).

---

## Timeline

### 1. Idea and architecture
The goal was an Android app that makes the phone a **wired-only, plug-and-play** webcam with front and back
cameras.
- Transport: **Android Open Accessory**. It needs no Developer Options and no ADB, and the phone shows one
  permission prompt.
- PC side: a **native Windows 11 virtual camera** (`MFCreateVirtualCamera` + a custom media source), so every
  app sees "MyCam" without a third-party driver.
- Pipeline: Camera2 → MediaCodec H.264 → USB → Media Foundation decoder → shared memory → virtual camera.

### 2. First real-phone testing (1.0 fixes)
| Symptom | Cause | Fix |
|---|---|---|
| "USB driver missing, run install.ps1" | UsbDk not installed / libusb couldn't use it | Init the libusb context first, then set the UsbDk option |
| "Plug in your phone" stayed grey; no camera prompt | The accessory-mode device had no driver, which broke UsbDk enumeration | Elevated `--bind-driver` installs inbox WinUSB; stream through a WinUSB libusb context |
| Phone camera in use (green dot) but black picture | The MediaTek encoder never sends `CODEC_CONFIG`; timed-out reads dropped data | Config from `onOutputFormatChanged`; queued async reads; queued keyframe requests |
| Phone kept streaming after the companion was killed | No session ownership | STOP when the phone streams without being asked |

User testing confirmed the stream survives calls and the phone screen locking, and reconnects instantly after
a replug. Opening the phone's own camera app stops the stream, and the user confirmed that's the correct
behaviour.

### 3. Planning and hygiene
- Wrote **IMPROVEMENTS.md** (later trimmed: no microphone, no live preview, no mirrored front camera by default).
- Project hygiene: git, unit tests on both sides (116 PC checks), protocol spec and golden vectors.
- **Design language: Windows 7 Aero (option A)**, taken from the user's `inspo/` folder (never committed).

### 4. PC app (1.0.0)
Aero tray status icons, a custom Direct2D settings window, an Inno Setup installer (bundles UsbDk and
offers to remove it on uninstall) and a per-monitor DPI manifest.

### 5. Privacy and control (1.1.0)
- Pause from the phone **and** from the PC. The phone owns the pause state, which persists. The virtual
  camera shows a "Camera paused" picture.
- Auto-pause while Windows is locked or asleep (PC-only, and it never undoes a user pause).
- Aero redesign of the phone app: Selawik font, glass header, command links and a new launcher icon.

### 6. GitHub (1.1.1)
Published to `whoissaaif/mycam` under MIT, then private from 1.1.1 until 1.4.0, and **public** again since. Commits use the no-reply email, and
`inspo/` was kept out of history. The first release had both the exe and the apk. The README shows the demo
GIF; the screenshots were removed.

### 7. Branding and signing (1.2.0)
- Package renamed to `io.github.whoissaaif.mycam`, so it installs as a new app on phones.
- Release keystore at `%USERPROFILE%\.mycam\` (the user entered the password), with signed release builds.
  The APK is 1.5 MB.
- An ad brief for a motion-designer friend was written as a shared doc.
- iPhone support was recorded as future section 9 (needs a Mac, an iPhone, a developer account and usbmux).

### 8. Clean install test
Everything was removed and the PC rebooted, then 1.2.0 was installed from the GitHub release. The first-use
flow and streaming worked. The only runtime dependencies are the bundled UsbDk and Windows' own DLLs. Windows
Sandbox couldn't be used because CPU virtualization is off in the firmware.

### 9. Picture quality and camera control (1.3.0 → 1.3.1, protocol v3)
- 720p / 1080p / 4K and 30 / 60 fps, with options the phone can't do disabled. The virtual camera also
  offers 4K30, 1080p60 and 720p60.
- Zoom, brightness (EV), focus once-and-lock, and torch, applied live from the phone or the PC.
- **Auto** button (user request) resets all camera controls in one click.
- Crop to fill instead of letterboxing.
- **Adaptive streaming:** a slow USB write lowers the bitrate by 25% and skips to the next keyframe. The
  bitrate climbs back by 15% every 5 s while writes stay fast. Added after 4K showed large latency.
- PC latency: frames are delivered to apps as they arrive, with one less full-frame copy.
- **60 fps investigation:** the user saw "1080p 60" in the phone's own camera app. A camera report added to
  the log shows both cameras max out at 30 fps at 1080p in Camera2's normal modes. This was first misread as
  "no 60 fps at all". On 2026-10-10 the same report showed the back camera has a high-speed mode (1080p at
  120/240 fps), and 1.3.2 uses it for 1080p60 (capture at 120, encode every other frame).

### 10. Documentation (2026-10-10)
- Wrote `CLAUDE.md` (architecture, build commands, rules, pitfalls) and this `PROGRESS.md`.

### 11. 1.3.2: four improvements, then a run of user-driven fixes
- **Play Store assets (7.3):** 512 px icon and 1024×500 feature graphic, drawn by `design/tools/make_store_art.ps1`.
- **Lower phone latency (3.4):**
  - The log now splits the delay into camera and encoder, and shows exposure and ISO.
  - Stabilisation is off, with fast noise/edge modes and no B-frames.
  - MediaTek's own low-latency encoder switch (`wfd.low-latency-enabled`) is found at runtime and turned on.
  - The PC queues 16 USB reads (256 KB) instead of 4.
  - Result: phone-side delay down from 150–230 ms to about **106–130 ms**.
- **No UAC prompt for new phones (2.2):** the installer registers a SYSTEM scheduled task ("MyCam phone
  driver") that only binds WinUSB. Users may start it but not change it; both were verified. The user
  approved this design.
- **Dim screen + heat warning (4.1):**
  - After 30 s untouched, the app shows a near-black screen at 2% brightness that drifts to avoid OLED
    burn-in. Any touch wakes it.
  - A thermal warning shows on both screens and in the PC log.
- **60 fps for real:** the back camera has a constrained high-speed mode. MyCam captures at 120 and the
  encoder keeps every other frame (`KEY_MAX_FPS_TO_ENCODER`).
  - It was noisy (8 ms exposure, plus the user had +4 EV). Full noise reduction and no sharpening help;
    the limit itself is physics.
- **30 / 60 / 120 fps choice:** the user asked why 120 was reduced to 60, so 120 is now passed through
  (about 30 Mbps). The virtual camera also offers 1080p120 and 720p120.
- **Works on every phone (user rule):** the user saw 60/120 selectable at 4K on a phone that only does 4K30.
  - Fix: one function, `CameraStreamer.modeFor(camera, quality, fps)`, decides what works. It checks normal
    modes, high-speed modes, and the encoder at that exact size.
  - The phone sends a frame-rate mask per quality: the CAMERA payload grew to 21 bytes, and older PCs ignore
    the extra.
  - Both UIs only offer what works and show the rate you'll actually get. This is saved as a standing rule in
    memory and in CLAUDE.md.
- **Duplicate frames fixed:** the encoder's repeat-frame timer equalled one frame interval, so late frames
  were duplicated. 30 fps streams went out at about 42 fps.
- **Adaptive-bitrate fixes:**
  - The bitrate maths overflowed `Int` at 4K. It now uses Long arithmetic.
  - The ceiling could drop below the floor and pin quality at 2 Mbps; [issue #8](https://github.com/whoissaaif/mycam/issues/8) caught this. The ceiling now
    expires after 30 s and never goes below 4 Mbps.
- **PC settings window:** 30/60/120 buttons, a gap between the quality and frame-rate rows, the "Now: …" line
  on its own row, and a Win7-style minimize button.
- Released as v1.3.2.

### 12. The 1.3.2 crash and the 1.3.3 hotfix
- The user reported "MyCam keeps stopping". With adb the crash was found: `IllegalAccessError` at start-up in
  `ProcessLifecycleInitializer`.
- **Cause:** R8's `packageScope` for androidx/kotlin made it merge a class across packages. Only release builds
  are optimized, so debug testing never saw it.
- **Fix:** removed `packageScope`. The fixed APK was installed and launched on the phone through adb, and the
  user confirmed it works.
- Released v1.3.3. The v1.3.2 page now carries a warning, and its APK was removed.
- New rule in TASKS.md: test the *release* APK on a phone before every release.

### 13. Planning: Windows 10 and wireless
- **IMPROVEMENTS 10, Windows 10:** a DirectShow camera for Windows 10, reusing the shared memory. It works in
  Zoom, browsers, OBS and Discord, but not the built-in Windows Camera app. A kernel driver was rejected (EV
  certificate and attestation).
- **IMPROVEMENTS 11, wireless:** an optional Wi-Fi mode. The cable stays the default.
- Rollback tag `pre-wireless`, then the work moved to branch `wireless`.

### 14. Contributor review: Aiyan's `aiyan` branch (iPhone)
- It moves the AOA code behind an `ITransport` interface and adds a usbmuxd transport. The usbmux protocol
  code is good, with tests.
- **Not merged.** The new threading can deadlock Android streaming: the session lock is held during a
  synchronous USB send while the USB thread waits for that same lock. There's also no iOS app yet, so the
  iPhone path can't be tested.
- The user decided to leave it on his branch for now.

### 15. Wireless (branch `wireless`, 1.4.0-beta1 → beta3)
- **Phase 1, transport:**
  - The phone has a "Use over Wi-Fi" switch: it answers UDP discovery on port 47801 and accepts TCP on 47800.
  - The PC has "Find phones on Wi-Fi": it broadcasts on every IPv4 network and connects out, so there's no
    firewall prompt.
  - One single-threaded session loop serves both USB and Wi-Fi, which avoids the deadlock in section 14.
  - The cable always wins; there's a Wi-Fi lock and a small send buffer on the phone.
  - Lint caught a missing `ACCESS_NETWORK_STATE` permission, which would have crashed.
  - First live test: found in about 1 s, 1080p30 through the virtual camera, about 106 ms phone-side.
- **Phase 2, security:**
  - Pairing is Bluetooth-style numeric comparison: the phone commits to its nonce first, both screens show a
    6-digit code, and the user taps Allow on the phone.
  - Every session uses P-256 ECDH + HKDF-SHA256, mixed with the pairing key, then AES-256-GCM records with
    counter nonces.
  - Paired PCs connect without asking.
  - Keys are stored in private app storage on the phone and DPAPI in HKCU on the PC. Both sides have a
    "Forget" option.
  - Interop is proven with test vectors generated by the JDK and checked by both suites: 4 Kotlin tests and
    146 PC checks, including CNG's little-endian ECDH secret.
  - Tested live:
    - pairing with matching codes;
    - an encrypted 1080p stream with 0 errors;
    - paired reconnect in 40 ms with no prompt;
    - the PC forgot the phone, and it paired again with a new code;
    - "Don't allow" stopped the PC asking (it waits 2 minutes before retrying).
- The waiting picture now says "USB cable, or over Wi-Fi". PROTOCOL.md specifies the whole handshake.
- **Left before merging:** cable takeover during Wi-Fi, 60/120 fps and 4K over Wi-Fi, a busy network.

---

## Lessons learned

- **USB on Windows:** UsbDk is good for the AOA switch but can't take over a driver-bound device. WinUSB
  through SetupAPI works and is Microsoft-signed.
- **MediaTek encoders** don't always follow the MediaCodec contract (no `CODEC_CONFIG`; frames are held back,
  which is the likely cause of the phone-side latency).
- **Measure before optimising:** latency logging on both clocks showed the PC isn't the bottleneck.
- **Tooling on Windows:** PowerShell 5 corrupts UTF-8 and Perl escapes misbehave. Use the Edit tool or sed.
  `git filter-branch` can delete untracked working-tree files (inspo was restored from `refs/original`).
- **GitHub licence detection** needs the plain MIT text, with nothing added.
- **Installer upgrades** must call `--quit` on the new exe, not the installed one.
- **Integer overflow** in bitrate maths at 4K: use Long for anything multiplied by a percentage.
- **Release builds differ from debug builds.** R8 broke 1.3.2 only in release, so test the release APK on a
  real phone before publishing.
- **Read the log before concluding.** "No 60 fps" was wrong: the camera report already listed high-speed
  modes. Capabilities depend on quality, so check every combination and never assume.
- **Make it generic.** Offer exactly what each phone reports, and discover vendor tweaks at runtime with a
  fallback.
- **Crypto across platforms:** pin the wire format first, then generate vectors with an independent
  implementation (the JDK) and test both sides against them. CNG's raw ECDH secret is little-endian.
- **Single-threaded protocol loops** avoid lock-ordering deadlocks between USB callbacks and command sends.
- **adb and MyCam can't share the USB phone.** The adb server blocks UsbDk, and USB debugging makes the
  accessory a composite device. Use wireless adb when testing Wi-Fi.
- **One companion at a time:** after `--quit`, wait for the process to exit before starting a new one.

## Next up (see TASKS.md)

1. Finish the Wi-Fi tests (cable takeover, 60/120 fps and 4K over Wi-Fi, a busy network), then merge
   `wireless` into `main` and release 1.4.0.
2. Decode on a separate thread, and on the GPU for 4K ([issue #8](https://github.com/whoissaaif/mycam/issues/8)).
3. MyCam with USB debugging on: bind WinUSB to the accessory's composite interface.
4. Remaining test scenarios: notification pause, pause across a replug, PC sleep, companion crash, two
   phones, battery saver.
5. Windows 10 support (IMPROVEMENTS 10), once a Windows 10 PC is available.
6. iPhone (IMPROVEMENTS 9 / Aiyan's branch), once his threading is fixed and an iOS app exists.
7. Code signing, `design/STYLE.md`, full UsbDk removal on uninstall. The repo is public again as of 1.4.0.
