# iPhone support without an app: a plan to discuss

*For Aiyan, from saif. Status: discussion only. Nothing is built yet.*

## Background

MyCam now works over **Wi-Fi** between Android and Windows (branch `wireless`, 1.4.0-beta3):
- the PC finds the phone on the local network;
- the two pair once with a 6-digit code;
- every session is encrypted (P-256 ECDH, HKDF-SHA256, AES-256-GCM);
- the H.264 video goes into the Windows virtual camera.

The full spec is in `protocol/PROTOCOL.md` ("Wireless transport" / "Wireless security").

We want iPhones too. **A native iPhone app isn't an option for now:**
- iOS apps can only be built and installed from a Mac with Xcode, and we don't have one.
- Sharing the app beyond our own phones needs the Apple Developer Program ($99/year).
- Without it, a self-installed app expires every 7 days.

So the idea is: **no app at all. The iPhone opens a web page served by the MyCam PC companion.**

## How it would work

1. **QR code:** the PC settings window shows a QR code. It holds the PC's local address and a one-time secret,
   for example `https://192.168.10.5:47900/?t=…`.
2. **Scan:** you scan it with the iPhone's Camera app, and Safari opens the MyCam page. The one-time secret is
   the pairing: only a phone that scanned *this* PC's screen gets in.
3. **Camera:** Safari asks "Allow camera?", then the page shows a Start button, front/back, and quality.
4. **Encoding:** the page encodes H.264 on the iPhone's hardware with **WebCodecs** (`VideoEncoder`, Safari on
   iOS 16.4+), in real-time latency mode.
5. **Transport:** the encoded frames go to the PC over a secure WebSocket (`wss://`).
6. **Decoding:** the PC decodes them with the decoder it already has, and the virtual camera, preview, pause
   and status pictures all keep working.

All new code lives on the **PC side**, plus one web page in HTML and JavaScript:
- an HTTPS + WebSocket server inside the companion;
- a self-made TLS certificate;
- QR pairing;
- a Windows Firewall rule, added by the installer;
- the page itself, in the app's design language.

All of it can be built and tested on Windows. Only the final check needs an iPhone.

Bonus: the same page works on **any** device with a modern browser: iPad, Android phones without our app,
or a spare laptop.

## The catches

**1. Safari needs HTTPS for the camera.** `http://192.168.x.x` is blocked; only secure pages may use
`getUserMedia`. The PC makes its own certificate, but no public authority signed it, so Safari warns
"This connection is not private" the first time. There are two ways around that:
- **Tap through the warning once.** It's simple, but it looks alarming, and it's **unverified** whether
  Safari then allows the camera on that page. This is the biggest unknown.
- **Install MyCam's certificate on the iPhone once** (a profile download, then turn it on in Settings,
  about 6 taps). It definitely works, and there are no warnings after that.

**2. The PC must accept incoming connections.** Today's Wi-Fi design has the PC only connecting out. A web
server needs a firewall rule, which the installer can add since it already runs as admin.

**3. The page only works in the foreground.** Locking the iPhone or switching apps stops the camera, and the
PC then shows "paused". A native app would have exactly the same limit. The page can keep the screen awake
(Screen Wake Lock).

**4. It has fewer camera controls than our Android app:**

| Control | Web page on iPhone |
|---|---|
| Front/back camera, 720p / 1080p / 4K, 30 / 60 fps | Yes |
| Zoom | Probably (newer iOS) |
| Brightness, focus lock, torch | Limited or no |

**5. Delay:** an estimated ~150–250 ms with the WebCodecs hardware encoder, a bit more than the Android
app's ~110 ms. That's fine for calls. Older iOS versions without WebCodecs would need a slower fallback
(MediaRecorder chunks, ~0.5–1 s).

## Native app vs. web page

| | Native iPhone app | Web page |
|---|---|---|
| Cost | Mac + $99/year | Free |
| Install | App Store / TestFlight | Scan a QR code |
| First-time setup | Pairing code | QR code + certificate step |
| Works while locked | No | No |
| Camera controls | All | Basic |
| Delay | ~100 ms | ~150–250 ms |
| Also works on iPad / Android / laptops | No | Yes |

## Step 1: a quick test before any real work

A tiny test page served from the PC (about an hour of work), opened on a real iPhone, to answer:
1. Does Safari allow the camera on a self-signed HTTPS page after tapping through the warning? Or is
   installing the certificate needed?
2. Is WebCodecs `VideoEncoder` with H.264 available on that iPhone, and at what resolutions and frame rates?
3. A rough idea of encode delay and frame rate at 1080p30 and 1080p60.

If the answers are good, we build it properly. If not, we fall back to installing the certificate, or
rethink.

## How this relates to your `aiyan` branch

Your branch connects an iPhone over **USB** through Apple's usbmux service. The usbmux protocol code is good
and well tested. It needs an iPhone app listening on the phone, though, and that brings back the Mac and
Apple-account problem. The web page doesn't need usbmux.

Two things from the review of that branch still matter if it's picked up later:
- **The threading can deadlock Android streaming.** The session lock is held during a synchronous USB send,
  while the USB thread waits for the same lock. The `wireless` branch avoids this with one single-threaded
  session loop for USB and Wi-Fi; please build on that.
- **It needs a rebase** on the current code (`wireless` / `main`).

## Open questions

- Which iPhone and iOS version can we test on? (Settings → General → About)
- For the certificate: tap through a warning once, or install the certificate once?
- Who builds what? A suggested split: the PC server/certificate/QR work and the web page could go to
  different people, since they meet only at the WebSocket message format, which we'd write down first.
