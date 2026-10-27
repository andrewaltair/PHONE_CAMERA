# Web camera: the iPhone page and its messages

*Status: **agreed and built** (branch `aiyan-webtest`). It started as saif's proposal, answering Aiyan's
Test 2 results. Assignment: [AIYAN-IPHONE.md](AIYAN-IPHONE.md). Background: [iphone-web-plan.md](iphone-web-plan.md).*

An iPhone (or iPad, or any browser with WebCodecs) becomes MyCam's camera through a page that the PC
companion serves itself. Nothing is installed on the phone. The page:
- captures the camera (`getUserMedia`);
- encodes H.264 on the phone's hardware (WebCodecs `VideoEncoder`);
- sends MyCam protocol packets over a secure WebSocket.

On the PC, the page is a third transport next to USB and Android Wi-Fi, in the same single-threaded
`SessionLoop`.

## Test results that settled it (iPhone 13, iOS 27)

- **Certificate:** `wss://` works with a self-signed certificate after one tap-through. No certificate
  install is needed.
- **Encoder:** WebCodecs H.264 runs in hardware. Every size and rate tested is supported: 720p, 1080p
  and 4K, at 30 and 60 fps. Encoding takes about 15 ms per frame.
- **Sending:** 1080p30 ran at 30 fps (7.6 Mbps) and 1080p60 at 59 fps (11.3 Mbps). The round trip was
  about 5 ms and no frames were skipped.

## The server (PC companion, `pc/companion/web_server.cpp`)

HTTPS on **TCP 47900**, with TLS from Schannel. The installer adds an inbound firewall rule for
`MyCamCompanion.exe` on that port, **private networks only**.

| Request | What it does |
|---|---|
| `GET /` (also `/app.js`, `/protocol.js`, `/style.css`, `/icon.png`) | The page, from the exe's resources (`companion.rc` 301–305) |
| `POST /pair`, form `t=<code>&name=<device>` | Trades the QR code's one-time code for a device token. Returns `{"ok":true,"device":"<token>","name":"iPhone"}`, or 403 `{"error":"code"}`; 409 while an Android phone is pairing with that same code (PROTOCOL.md "QR pairing") |
| `POST /check`, form `d=<device token>` | Returns `{"ok":true,"busy":false,"name":"iPhone"}`, or 403 when the PC doesn't know (or forgot) the device |
| `GET /ws?d=<device token>` (WebSocket upgrade) | The camera link. Refused (403) without a valid device token, or when the `Origin` isn't this server. Refused with 503 while another phone uses MyCam |

**Responses** carry `Cache-Control: no-store` and `Referrer-Policy: no-referrer`. The page also gets a
strict Content Security Policy (only its own scripts and styles, and connections only to this PC).

### Certificate

- **Made once per user:** a self-signed RSA-2048 / SHA-256 certificate in `CurrentUser\My`.
- **Name:** "MyCam web camera".
- **Key:** not exportable.
- **Names inside it:** localhost and the PC's IPv4 addresses at the time it was made.
- **Lifetime:** 397 days, because iOS refuses TLS certificates that live longer. It is renewed 30 days
  before it ends, which means one more tap-through.

### Tokens

| | Size | Lives | Where |
|---|---|---|---|
| Pairing code (in the QR code) | 128 bits, base64url | Once, for 5 minutes. A new one appears by itself while the "Connect an iPhone" page is open, and none exists when it's closed | PC memory only |
| Device token | 256 bits, base64url | Until "Forget" | The page's `localStorage` (`mycam.device`). The PC keeps only its SHA-256, in `HKCU\Software\MyCam\WebDevices\<id>` (`TokenHash`, `Name`, `Added`) |

- **First connection:** the QR code points at `https://<pc-ip>:47900/?t=<code>&pc=<PC id>`
  (`pc` since 1.5, for the Android app's own pairing, PROTOCOL.md "QR pairing"; the page ignores it). The page first
  removes both from the address bar (`history.replaceState`, so they don't stay in Safari's history). Then
  it calls `POST /pair`, keeps the device token, and connects.
- **Android phone, scanned with Google Lens or the camera app (1.5):** the page opens in Chrome. It
  doesn't use the code yet. It offers **Open in the MyCam app**, which is an `intent:` link carrying the
  same `t` and `pc`, with the GitHub releases page as the fallback when the app isn't installed. It also
  offers **Use this page instead**, which runs the normal web pairing above.
- **Later connections:** opening `https://<pc-ip>:47900/` again (or the Home Screen icon) and tapping
  Start reconnects without scanning.
- **Never logged:** tokens never go to the log. The settings window shows the address without the code.

### Pairing and the network

- **Choosing the address:** the PC picks the address for the QR code. Wi-Fi comes first, then Ethernet,
  then an iPhone's USB hotspot (Apple Mobile Device Ethernet), then anything else. VPNs and tunnels go last.
- **Left out:** virtual switches (Hyper-V, WSL, VMware, VirtualBox, Docker) and link-local addresses.
- **Several networks:** with more than one address, the user picks the one the iPhone is on.

## Messages on `/ws`

**Binary messages only**, each carrying exactly **one** complete MyCam packet (page → PC) or command
(PC → page). The format is the normal MyCam protocol, big-endian: see `protocol/PROTOCOL.md`, "Phone → PC
packets", "PC → phone commands" and "Flow". There is no extra encryption, because `wss://` already
encrypts everything.

### Page → PC packets

Every packet starts with the 20-byte header:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `MCAM` |
| 4 | 1 | type |
| 5 | 1 | flags (bit 0 = key frame) |
| 6 | 2 | reserved, 0 |
| 8 | 8 | timestamp in µs |
| 16 | 4 | payload length |

| Type | Name | What the page sends |
|---|---|---|
| 0 | HELLO | Version **3**, in reply to the PC's HELLO |
| 1 | CONFIG | Width and height of the encoded pixels, the turn they need (see Rotation), facing, then SPS/PPS taken from the key frame. Sent before the first frame and before the key frame after any change (size, camera, bitrate) |
| 2 | FRAME | One access unit, Annex-B, with flag bit 0 on key frames. Timestamps come from the page's monotonic clock and only ever go up |
| 3 | ORIENT | How far the phone is turned clockwise, 0/90/270 (see Rotation); 0 for browsers that label their frames |
| 4 | STATE | Idle / streaming / error / paused, and facing |
| 5 | LOG | The browser's user agent, the camera's limits and modes, bitrate changes, errors. Lines from before HELLO are kept and sent after it |
| 6 | CAMERA | The 21-byte form (see below) |

**CAMERA, as the page fills it in:**
- **Frame-rate masks:** one per quality. A quality gets the 30 fps bit when the camera reaches that size
  *and* `VideoEncoder.isConfigSupported()` accepts it (30 stands for "this size works", the lowest rate
  the protocol has). The 60 / 120 bits need the camera's `frameRate.max` as well.
- **Small cameras:** a camera below 720p is still offered as 720p at 30.
- **Measured rate:** if the camera then delivers less than 75 % of the asked rate, that rate's bit is
  cleared and CAMERA is sent again.
- **Zoom and torch:** from `track.getCapabilities()` (the iPhone 13 has both).
- **Brightness:** EV is 0/0/0, because Safari can't set exposure. The PC greys it out.
- **Autofocus flag:** not set, because Safari doesn't expose focus control.
- **Quality:** the one that really runs.
- **Fps:** the one the PC asked for.

### PC → page commands

8 bytes each: `MCMD`, the command, an argument, and 2 reserved bytes set to 0.

| Cmd | The page |
|---|---|
| HELLO | Replies HELLO, STATE, ORIENT, CAMERA (and any early LOG lines) |
| START | Starts the camera and encoder: CONFIG, STATE(streaming), FRAMEs. If already streaming: new CONFIG and a key frame. If paused: STATE(paused) |
| STOP | Stops the camera (releases the track), sends STATE(idle) |
| KEYFRAME | New CONFIG and a key frame |
| SET_FACING | Switches between `environment` and `user`, restarts the camera if it runs |
| PAUSE / RESUME | Camera off / back on (if the PC still wants video). The page stores the pause, so it survives reconnects |
| SET_QUALITY / SET_FPS | Stored; the camera restarts with the best working mode at or below them |
| SET_ZOOM / SET_TORCH | `applyConstraints({advanced: [{zoom}]})` / `[{torch}]` where the camera has them |
| SET_EXPOSURE / SET_FOCUS | Ignored (Safari can't do them; the PC greys them out) |

### Close codes (PC → page)

| Code | Meaning | The page |
|---|---|---|
| 1000 | Normal close | Reconnects |
| 4001 | Another phone is using MyCam | Shows "Your PC is busy", retries every 3 s |
| 4002 | The same device opened a newer connection | Stops (says it's open in another tab) |
| 4003 | The PC forgot this device | Deletes its token and asks for the QR code again |

The PC sends a WebSocket ping every 5 s and drops a link that has been silent for 15 s.

## Behaviour details

- **Encoder:**
  - Baseline profile, at the level for the size and rate: `avc1.42001f`/`420020` for 720p,
    `420028`/`42002a` for 1080p, `420033`/`420034` for 4K.
  - `latencyMode: "realtime"`, `hardwareAcceleration: "prefer-hardware"`, `avc: {format: "annexb"}`.
  - Starting bitrate: 4 / 8 / 20 Mbps for 720p / 1080p / 4K, ×1.5 at 60 fps and above.
  - A key frame every 2 s.
- **Rotation** (from the iPhone 13 tests, iOS 27):
  - **What Safari gives:** the camera's own landscape pixels, 1920×1080, whichever way the phone is held,
    and its frames carry no rotation label (`VideoFrame.rotation` is undefined).
  - **What the page sends:** like an Android camera, CONFIG carries the sensor's turn, **90° for both the
    back and the front camera** (270 for the front turned it upside down in every position). ORIENT carries
    how far the phone is turned clockwise from upright.
  - **Where ORIENT comes from:** the screen angle, `(360 − screen.orientation.angle) % 360`, so portrait 0,
    top to the left 270, top to the right 90. Safari never turns its screen for an upside-down iPhone, so
    that position (180) comes from the motion sensors: the page asks for "Motion & Orientation Access" when
    Start is tapped. Without that permission, upside-down isn't followed.
  - **What the PC does:** it turns the picture by (sensor + ORIENT) for the back camera and
    (sensor − ORIENT) for the front, exactly as for Android.
  - **Turning the phone:** this only sends ORIENT; the encoder isn't restarted.
  - **Results:** back camera upright in portrait and both landscapes. The front-camera fix and upside-down
    portrait are still to confirm.
  - **Other browsers:** frames that carry a rotation label are turned by that label (ORIENT 0). Laptop
    webcams are left as they are.
- **Back-pressure:**
  - If `ws.bufferedAmount` goes over about a quarter of a second of video (at least 256 KB), the page
    skips frames and makes the next one a key frame.
  - At most every 2 s, it also lowers the bitrate by 30 % (never below 1 Mbps).
  - After 5 s without skipping, and with the buffer nearly empty, it raises the bitrate by 15 % at a time,
    up to the starting rate.
  - A busy encoder (`encodeQueueSize > 2`) only skips that frame.
  - Bitrate changes go to the PC log.
- **Screen:** the page holds a Screen Wake Lock (taken again whenever it is visible) and has a black
  "Dim the screen" mode. There is no camera preview, as on Android: the `<video>` the frames are read from
  is 2 px and transparent.
- **Background and lock:**
  - When the page is hidden, it sends STATE(paused) while the socket still works, then stops the camera.
  - The PC shows the "Camera paused" picture.
  - If iOS then drops the connection, the PC keeps that **paused session for 5 minutes**, still showing
    the paused picture.
  - When the page is visible again, it reconnects (with the same device token), the PC takes it straight
    back into the session, and it starts again if an app still wants video.
- **Reconnect:** a dropped link retries by itself (1, 2, 4… up to 10 s) while the page is visible. A
  reloaded page takes over its own session directly.
- **Busy:** pages that arrive while another phone (USB or Android Wi-Fi) has the session are told so and
  wait. A plugged-in Android phone always takes over, as with Android over Wi-Fi.

## Example: the first seconds

    page -> PC     POST /pair  t=<code>&name=iPhone             -> {"device": "<token>"}
    page -> PC     POST /check d=<token>                        -> {"ok": true, "busy": false}
    page -> PC     GET /ws?d=<token>  (upgrade)                 -> 101
    PC   -> page   MCMD 01 00 0000                              HELLO
    page -> PC     MCAM 00 ... 0003                             HELLO, version 3
    page -> PC     MCAM 04 ... 00 00                            STATE idle, back camera
    page -> PC     MCAM 03 ... 0000                             ORIENT 0
    page -> PC     MCAM 06 ... (21 bytes)                       CAMERA
    PC   -> page   MCMD 02 00 0000                              START (an app opened the camera)
    page -> PC     MCAM 01 ... 0780 0438 005A 00 <SPS/PPS>      CONFIG 1920x1080 turned 90° (phone upright)
    page -> PC     MCAM 04 ... 01 00                            STATE streaming
    page -> PC     MCAM 02 01 ... <key frame>                   FRAME, key frame
    page -> PC     MCAM 02 00 ... <frame>                       FRAME ...

## How it is tested

- **`ctest`:**
  - `mycam_tests`: WebSocket frame parsing (masking, the three length forms, fragments, control frames,
    limits), the upgrade handshake (RFC 6455's example), HTTP requests, tokens (one use, expiry, hashes)
    and address ranking.
  - `web_golden`: runs `pc/tools/webtest/golden_check.js`, which checks the page's `protocol.js` against
    `protocol/golden.txt`. It needs Node.js and is skipped without it.
- **End to end without a phone** (`-DMYCAM_BUILD_TOOLS=ON`): `pc/tools/web_probe.exe` runs the server
  alone and plays the PC.
  - `node pc/tools/webtest/fake_page.js "<address>"` checks the routes, pairing and refusals, then streams
    fake frames.
  - `node pc/tools/webtest/browser_check.js "<address>"` runs the real page in headless Chrome with a fake
    camera (real WebCodecs encoding).
- **On the iPhone:** see the checklist in [AIYAN-IPHONE.md](AIYAN-IPHONE.md) §5 and the results table in
  TASKS.md.

## Delay (camera to app)

**Not measured yet: this needs the iPhone.** The measured parts so far are encode (about 15 ms) and the
Wi-Fi round trip (about 5 ms). The PC's decode and copy took 3–12 ms on Android streams.

Suggested method:
1. Show a millisecond clock on the PC screen.
2. Point the iPhone at it.
3. Open the Windows Camera app next to it.
4. Photograph both: the difference is the full delay.
5. Repeat at 1080p30 and 1080p60.
6. Write the results here.

Target: about 250 ms or less.
