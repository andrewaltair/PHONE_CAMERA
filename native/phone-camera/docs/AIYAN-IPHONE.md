# iPhone support: Aiyan's assignment

*From saif (project owner), 2026-10-11. Written for Aiyan and for Aiyan's Claude agent: read this file
first, then `CLAUDE.md`, then the documents it links. Branch: `aiyan-webtest`.*

## Summary

**iPhone support for MyCam is Aiyan's part of the project**, end to end:

1. **iPhone support:** wireless working completely, and a foundation for wired.
2. **The PC side, properly deployed:** the MyCam PC app serves the page itself, shows a **QR code**, and when
   an iPhone scans it, Safari opens the page and the iPhone's camera streams into MyCam.

saif reviews and merges. Everything else in MyCam (the Android app, USB, Android Wi-Fi, the design system,
releases) belongs to saif. Change those only where your work needs a shared interface, and say so in the
pull request.

---

## 1. What MyCam is (read this if you're new to the code)

MyCam turns a phone into a webcam for Windows 11. On the PC, apps (Zoom, Teams, OBS, browsers) see a
camera called **"MyCam"**.

- **Android app** (`app/`, Kotlin). Camera → hardware H.264 encoder → USB (Android Open Accessory) or Wi-Fi.
- **PC companion** (`pc/companion/`, C++17, Win32 tray app). Receives the stream, decodes H.264 with Media
  Foundation, and writes frames to shared memory. It has the settings window, in the XP "Luna" design.
- **Virtual camera** (`pc/vcam/`, a COM DLL). Windows loads it into its camera service. It reads frames from
  shared memory and gives them to apps.
- **The MyCam protocol** (`protocol/PROTOCOL.md`, v3). Phone→PC *packets* have a 20-byte `MCAM` header;
  PC→phone *commands* are 8 bytes and start with `MCMD`. `protocol/golden.txt` holds exact example bytes that
  every implementation is tested against.
- **The PC session loop:** one **single-threaded** `SessionLoop` in `pc/companion/phone_link.cpp` runs the
  protocol for whatever phone is connected (USB or Wi-Fi). A transport only provides a "pump" that feeds
  received bytes in, and a way to send 8-byte commands. Look at `RunTcpSession` / `ReceiveRecords` /
  `SendTcp` in `phone_link_wifi.cpp` for the pattern your transport should copy.

Release 1.4.0 has Android over USB and over Wi-Fi (paired and encrypted). The repo is **public** (MIT).

## 2. Why iPhone goes through a web page, not an app

A native iPhone app isn't possible for now: building iOS apps needs a Mac with Xcode, which we don't have, and
sharing them needs the Apple Developer Program at $99/year, which we won't pay.

So the iPhone uses **Safari**:
- it opens a page **served by the MyCam PC app over HTTPS**;
- it captures the camera (`getUserMedia`);
- it encodes H.264 on the iPhone's hardware (**WebCodecs** `VideoEncoder`);
- it sends MyCam protocol packets to the PC over a **secure WebSocket** (`wss://`).

The PC handles them like any phone's stream.

**Already proven by your tests** (iPhone 13, iOS 27):
- **Test 1:** WebCodecs H.264 is available.
- **Test 2:**
  - `wss://` works with a self-signed certificate after one tap-through, so no certificate install is needed;
  - ~5 ms round trip;
  - 1080p30 at 30 fps (7.6 Mbps);
  - 1080p60 at 59 fps (11.3 Mbps).
- **Test 3:** the page speaks the MyCam protocol to a fake PC (`pc/tools/webtest/webtest_server.py`).

Background and the agreed format:
- `docs/iphone-web-plan.md` explains why the web page, and its catches;
- `docs/web-camera-format.md` is **the message format, agreed: build against it**.

The web-page path also works on iPads, on Android phones without the app, and on laptops. That's a bonus;
iPhone is the target.

---

## 3. Your part 1: iPhone support

### 1a. Wireless: complete and production quality

Turn Test 3's page into the real one, served by the PC companion (see part 2), not by Python.

- **Protocol** (exactly as in `docs/web-camera-format.md`):
  - one MyCam packet per binary WebSocket message;
  - big-endian;
  - HELLO (v3), STATE, ORIENT, CONFIG (with SPS/PPS), FRAME (keyframe flag, µs timestamps), LOG;
  - handle the commands HELLO, START, STOP, KEYFRAME, SET_FACING, PAUSE/RESUME, SET_QUALITY, SET_FPS. Zoom,
    exposure, torch and focus only where Safari supports them; otherwise ignore them.
  - **Check your packet bytes against `protocol/golden.txt`** (for example with a small Node or browser
    test).
- **Only offer what the device can do.** This is a hard project rule, see `CLAUDE.md` "Works on every
  phone". Read `track.getCapabilities()` and probe `VideoEncoder.isConfigSupported()`, then send a CAMERA
  packet whose per-quality frame-rate masks list only combinations that actually work. The PC's settings UI
  grey out everything else.
- **Rotation:** test portrait and landscape. Decide whether Safari's frames arrive upright (send rotation 0)
  or need ORIENT, and write the result into `docs/web-camera-format.md`.
- **Back-pressure:** if `ws.bufferedAmount` keeps growing, skip frames until the next keyframe and lower the
  bitrate, then raise it again later. Never queue seconds of video.
- **The phone must stay awake** while streaming (Screen Wake Lock), with a dim/black mode like the Android
  app's.
- **Background and lock:** iOS stops the camera when Safari is backgrounded or the phone locks. Report
  STATE(paused) when you can. When the page comes back, reconnect and resume on its own; the PC shows its
  "paused" picture meanwhile.
- **Reconnect:** a dropped connection retries by itself. A page that was paired before reconnects without
  scanning again (see the tokens in part 2).
- **Look and language:** follow the XP Luna design (`design/STYLE.md`, `redesign.md`): task-pane look, big
  Start button, the camera switch, quality. Plain English, short sentences. The Android app shows a camera
  preview on by default (`PhonePreview.kt`); match that on the web page unless you have a reason not to.
- **Quality bar:** at least 1080p30 steady on the iPhone 13, 1080p60 where the device allows, and the full
  delay (camera to app on the PC) measured and written down. Aim for about 250 ms or less.

### 1b. Wired: a foundation, not the finished feature

Wired iPhone support needs a native app (Apple's usbmux tunnels TCP to a port *on the phone*, so something
on the phone must listen), and a native app is blocked by the Mac / $99 problem. So for wired, build
the **foundation** and stop there:

- **Investigate "Personal Hotspot over USB" first.** Plugged into a PC with Apple's drivers (from the Apple
  Devices app or iTunes) and with Personal Hotspot on, an iPhone gives Windows a **network adapter over the
  cable**. The same web page and PC server might then work over USB with no app. Find out:
  - does it work on your iPhone and carrier;
  - does Safari reach the PC at the hotspot address;
  - what the delay and throughput are over the cable;
  - what happens with no SIM or a carrier that blocks hotspot.

  Write the findings up.
- If it works, make the PC server listen on that adapter too, and show the right QR code / address for it.
- **Your earlier usbmux code** (branch `aiyan`) stays as reference for a future native app. Don't merge it:
  its two-thread design can deadlock the session loop (the review details are in PROGRESS.md section 14).
- Deliverable: a short `docs/iphone-wired.md` saying what works, what doesn't, and the recommended path.

## 4. Your part 2: PC side deployment

The finished feature is part of the normal MyCam installer and PC app. **No Python, no separate tools for
users.**

- **HTTPS + WebSocket server inside the companion** (C++):
  - port **47900**;
  - use **Schannel** (built into Windows) for TLS, so no new big dependency;
  - serve the page from resources embedded in the exe (`companion.rc`), not loose files;
  - one WebSocket endpoint, `/ws`.
- **Certificate:** generate a self-signed certificate once per install, keep it in the current user's
  certificate store or protected with DPAPI, and reuse it, so the iPhone only has to tap through the warning
  the first time.
- **Session loop:** a web phone is **a third transport in the single-threaded `SessionLoop`**, exactly
  like `RunTcpSession`: a pump that reads WebSocket frames and hands their payloads to `FeedBytes`, plus a
  send path for 8-byte commands. **No extra threads touching session state.** The rule that the cable wins
  over Wi-Fi also applies: a plugged-in Android phone takes over.
- **QR code and pairing:**
  - **Where:** a "Connect an iPhone" task in the settings window (XP style, see
    `settings_window.cpp` / `ui_model.h` / `pairing_dialog.cpp`) shows a QR code for
    `https://<pc-ip>:47900/?t=<token>`.
  - **Generator:** use a small, MIT-licensed generator, vendored in `pc/third_party/` with its licence, for
    example Nayuki's `qrcodegen`.
  - **The token:** random (at least 128 bits), single use, and short-lived (a few minutes). The PC refuses
    the WebSocket upgrade without a valid token.
  - **After the first connection,** the PC gives the page a long-lived device token (kept in the page's
    storage), so the same iPhone reconnects later without scanning again. The PC settings list these devices
    with a "Forget" option, like "Forget Wi-Fi phones".
  - **Several network adapters:** pick the address on the network the phone is likely on (Wi-Fi/Ethernet
    rather than VPNs), and let the user switch if there are several.
- **Firewall:** the installer (`installer/mycam.iss`, Inno Setup) adds an **inbound** rule for
  `MyCamCompanion.exe`, TCP 47900, **private networks only**; the uninstaller removes it.
- **Status:** the tray and settings say e.g. "iPhone connected over Wi-Fi", and the companion log
  (`%LOCALAPPDATA%\MyCam\mycam.log`) records connects, refusals and the page's LOG messages. **Never log
  tokens.**
- **Tests** (in `pc/tests/tests.cpp`, run by `ctest`): WebSocket frame parsing (masking, lengths, control
  frames), the upgrade handshake (`Sec-WebSocket-Accept`), and the token check. Unit-test everything that
  doesn't need a real iPhone.

## 5. Definition of done

- [ ] Install MyCam with the normal installer → Settings → "Connect an iPhone" → scan the QR code with an
      iPhone → tap through the certificate warning once → allow the camera → Start → the iPhone's video shows
      in the Windows Camera app and in a browser video call.
- [ ] The same iPhone reconnects later without scanning again; "Forget" makes it scan again.
- [ ] Pause and resume from the PC and from the page; locking the iPhone shows the "paused" picture, and
      unlocking and returning to Safari resumes.
- [ ] The PC's quality and frame-rate choices show only what the iPhone can do.
- [ ] No firewall prompt for users (the installer adds the rule).
- [ ] Delay measured and written down; 1080p30 steady for 10 minutes without growing delay.
- [ ] The Android USB and Android Wi-Fi paths still work exactly as before (saif tests these at review).
- [ ] `docs/web-camera-format.md` updated with the final details; `docs/iphone-wired.md` written.
- [ ] TASKS.md / IMPROVEMENTS.md updated (an iPhone section).

## 6. Rules for working in this repo (for you and your agent)

Read **`CLAUDE.md`**: it's the project's rule book. The most important parts:

- **The repo is public.** Read every change before committing it. **Never commit** `inspo/`, `screenshots/`,
  `release/`, keys or certificates (no `*.pfx`, `*.pem`, `*.jks`), or anything with personal data.
- **Commit and push** your work to `aiyan-webtest` as you go. **No tags, no releases.** saif cuts releases
  after merging.
- **Don't change the protocol on your own.** If the format needs to change, agree it with saif first, then
  update `protocol/PROTOCOL.md`, `protocol/golden.txt`, `Protocol.kt` and `pc/companion/protocol.h`
  together.
- **Keep the session loop single-threaded.**
- **Works on every device:** offer only what the connected device reports, never tuned to one phone.
- **Design:** the XP Luna look (`design/STYLE.md`, `redesign.md`).
- **Windows tooling:** PowerShell 5 mangles UTF-8; use the editor tools or `sed` for files with non-ASCII
  text. Build commands are in CLAUDE.md "Build and test".
- When it's ready, merge `main` into `aiyan-webtest`, make sure the PC build and tests pass, and open a pull
  request to `main` describing what you tested.

Questions about scope or the protocol: ask saif before building.
