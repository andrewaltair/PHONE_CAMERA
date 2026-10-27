# MyCam wire protocol (version 3)

The phone and the PC talk over the two bulk endpoints of an Android Open Accessory (AOA) connection.
All integers are **big-endian**. Implementations:

- Android: `app/src/main/java/io/github/whoissaaif/mycam/Protocol.kt`
- Windows: `pc/companion/protocol.h` and `pc/companion/packet_parser.cpp`

Byte-exact examples live in [`golden.txt`](golden.txt), and both test suites check against them.
**When you change the protocol, update both implementations and `golden.txt` together.**

## AOA identification

The PC sends these strings during the AOA handshake. The phone's `res/xml/accessory_filter.xml` must
match the manufacturer and model.

| Index | Field | Value |
|---|---|---|
| 0 | manufacturer | `MyCam` |
| 1 | model | `MyCam Webcam` |
| 2 | description | `Use this phone as a USB webcam` |
| 3 | version | `1` |
| 4 | URI | `https://github.com/whoissaaif/mycam`, shown when the app isn't installed |
| 5 | serial | `0001` |

## Phone → PC packets

20-byte header followed by `length` bytes of payload:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `0x4D43414D` (`MCAM`) |
| 4 | 1 | type |
| 5 | 1 | flags (bit 0: key frame) |
| 6 | 2 | reserved, 0 |
| 8 | 8 | presentation time, µs (frames only; otherwise 0) |
| 16 | 4 | payload length (max 8 MiB) |

| Type | Name | Payload |
|---|---|---|
| 0 | HELLO | u16 protocol version (reply to `CMD_HELLO`). 1 = original, 2 = adds pause, 3 = adds video quality and camera controls |
| 1 | CONFIG | u16 width, u16 height, u16 sensor orientation, u8 facing, then SPS/PPS in Annex-B (may be empty) |
| 2 | FRAME | one H.264 access unit, Annex-B. Key frames carry SPS/PPS on most phones. |
| 3 | ORIENT | u16 device rotation, degrees clockwise (0/90/180/270) |
| 4 | STATE | u8 state (0 idle, 1 streaming, 2 error, 3 paused [v2]), u8 facing |
| 5 | LOG | UTF-8 text; the PC writes it to `mycam.log` |
| 6 | CAMERA | [v3] 18 bytes: u8 quality (0 720p, 1 1080p, 2 4K), u8 fps (30/60/120), u16 zoom×100, u16 zoomMin×100, u16 zoomMax×100, i8 ev, i8 evMin, i8 evMax, u8 evStep×100, u8 flags, u16 width, u16 height, u8 actual fps. Flags: 0x01 torch available, 0x02 torch on, 0x04 focus locked, 0x08 60 fps available, 0x10 4K available, 0x20 autofocus, 0x40 120 fps available (1.3.2; older PCs ignore it). Since 1.3.2 three more bytes follow (21 total): one frame-rate mask per quality (720p, 1080p, 4K) with 0x01 = 30, 0x02 = 60, 0x04 = 120 fps, worked out by the phone for every camera facing the current way (normal and high-speed modes, and the encoder at that exact size). PCs offer only the rates in the mask for the selected quality; with an 18-byte payload they fall back to the 0x08/0x40 flags. Width 0 = capabilities not known yet (camera not started since connecting). Sent after HELLO and whenever a setting changes. |

Facing: 0 = back, 1 = front.

A receiver treats a header as valid only if the magic matches, the reserved bytes are 0, the type is
known and the length is within limits. Otherwise it skips one byte and looks again. This resyncs after
garbage, and stops a stray `MCAM` inside video data from being mistaken for a header. New packet types
therefore need a protocol version bump.

## PC → phone commands

Fixed 8 bytes:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `0x4D434D44` (`MCMD`) |
| 4 | 1 | command |
| 5 | 1 | argument |
| 6 | 2 | reserved, 0 |

| Cmd | Name | Meaning |
|---|---|---|
| 1 | HELLO | Phone replies with HELLO, STATE and ORIENT |
| 2 | START | Turn the camera on and stream (resends CONFIG and a key frame if already streaming) |
| 3 | STOP | Turn the camera off |
| 4 | KEYFRAME | Resend CONFIG and request a key frame |
| 5 | SET_FACING | Switch camera; argument = facing |
| 6 | PAUSE | [v2] Pause: camera off until RESUME. The phone stores this, so it survives reconnects. |
| 7 | RESUME | [v2] Resume; the camera turns back on if the PC still wants video |
| 8 | SET_QUALITY | [v3] arg 0 720p, 1 1080p, 2 4K (restarts the camera if running) |
| 9 | SET_FPS | [v3] arg 30, 60 or 120 (120 since 1.3.2; restarts the camera if running). The phone falls back 120 → 60 → 30 to what the camera can do and reports the result as CAMERA actual fps. |
| 10 | SET_ZOOM | [v3] arg zoom×10, e.g. 6 = 0.6× ultrawide, 20 = 2× (applied live) |
| 11 | SET_EXPOSURE | [v3] arg signed int8 EV steps (applied live) |
| 12 | SET_TORCH | [v3] arg 0 off, 1 on (applied live; resets to off on reconnect) |
| 13 | SET_FOCUS | [v3] arg 0 continuous auto focus, 1 focus once and lock (applied live) |

Commands are validated the same way: magic, a known command number, and zero reserved bytes.

## Flow

1. PC opens the accessory and sends `HELLO` (repeated every 2 s until answered).
2. The phone answers `HELLO`, `STATE`, `ORIENT`.
3. When a PC app opens the MyCam camera, the PC sends `START`. The phone sends `CONFIG`, `STATE(streaming)`,
   then `FRAME`s.
4. About 4 s after the last PC app closes the camera, the PC sends `STOP`.

## Pause (v2)

The phone owns the pause state. It can be paused from the phone UI or by the PC (`PAUSE`), and resumed
either way (`RESUME`). While paused it reports `STATE(paused)`, keeps its camera off, and answers `START` with
`STATE(paused)` instead of video. The PC shows a "Camera paused" picture on the virtual camera.

The PC's automatic pause while Windows is locked is PC-only: the PC simply stops asking for video
(`STOP`) and never sends `START` until unlock. It does not change the phone's pause state.

## Web transport (iPhone page)

The same packets and commands also run over a secure WebSocket, one packet or command per binary message,
between the PC companion and a page it serves to iPhones. The packets are unchanged. Pairing (a QR code
and device tokens over HTTPS) sits outside this protocol. See `docs/web-camera-format.md`.

## Wireless transport (1.4, beta)

The same packets and commands can run over TCP instead of the USB accessory. The phone is the server and
the PC always connects, so Windows never needs an inbound firewall rule.

**Discovery (UDP, port 47801).** While "Find phones on Wi-Fi" is on, the PC broadcasts every 2 s, to the
directed broadcast address of each of its IPv4 networks and to 255.255.255.255:

    PC -> broadcast:   MYCAM?1 <pc name>            (ASCII/UTF-8, no terminator)
    phone -> PC:       MYCAM!1 <tcp port> <phone name>

The phone answers (unicast, to the sender's address and port) only while "Use over Wi-Fi" is on. It
remembers the PC name per IP address to show it when that PC connects. A phone that hasn't answered for 6 s
is forgotten.

"Scan for phones" on the PC sends the same probe (no new messages): one right away, then every 2 s for 6 s,
also while "Find phones on Wi-Fi" is off. It only lists the answers; with the search off, the PC connects
only when the user picks a phone.

**The phone asks for a session (1.4).** A phone that can see a PC in its own list may ask that PC to
connect, so a pairing can be started from either screen:

    phone -> PC:       MYCAM+1 <tcp port> <phone name>

It goes unicast to the address and port the probe came from, which is the PC's discovery socket, so it only
reaches a PC that is probing at that moment — exactly the PCs the phone lists. The PC treats it like
"Connect" in its own list: it adds the phone to that list, clears any retry back-off for it, and connects
and runs the handshake below, whether or not "Find phones on Wi-Fi" is on. A PC that already has a session
ignores it. The phone resends it every 2 s for 10 s, because one datagram may be lost; the PC acts on the
first and ignores the rest while it is connecting.

**Session (TCP, port 47800).** The PC connects and runs the handshake below. After it, both directions
carry the usual packets and commands (the same bytes as over USB) inside encrypted records, and the session
continues exactly as over USB (`HELLO` every 2 s until answered, and so on).

One PC at a time. A USB connection always wins: the phone closes a Wi-Fi session when a cable link opens,
and the PC ends a Wi-Fi session when a phone is plugged in so the cable can take over.

**Retries.** The phone refused, or nobody answered within 60 s: the PC waits 2 minutes before trying that
phone again. A working session dropped: 2 s. The phone couldn't be reached: 10 s.

## Wireless security (1.4)

Every Wi-Fi session is authenticated and encrypted. A new PC must be **paired** once, in one of two ways:

- **QR code (1.5, the main way):** the phone scans the code the PC shows. See "QR pairing" below.
- **Code comparison (the fallback):** both screens show the same 6-digit code, and the user allows it on
  the phone and, since 1.5, on the PC too.

A paired PC connects without asking.

Primitives: P-256 ECDH (public keys as `04 | X | Y`, the shared secret is the X coordinate, big-endian),
SHA-256, HMAC-SHA256, HKDF-SHA256 (RFC 5869), AES-256-GCM with a 16-byte tag. `protocol/golden.txt` has
vectors for all of them (`wifi.*`, generated with the JDK), checked by both test suites.

**Handshake messages** (before encryption): `u16 length (type + body) | u8 type | body`, big-endian.

| Type | Name | Direction | Body |
|---|---|---|---|
| 1 | CLIENT_HELLO | PC → phone | `"MCHS"`, u8 version (1), u8 flags (bit 0: force pairing, bit 1: QR pairing), PC id (16), ephemeral public key (65), Npc (16), u8 name length, PC name (UTF-8) |
| 2 | SERVER_HELLO | phone → PC | phone id (16), ephemeral public key (65), Nph (16), u8 mode (0 paired, 1 pairing, 2 QR pairing), [pairing: commitment (32)], u8 name length, phone name |
| 3 | NONCE_A | PC → phone | Na (16), pairing only |
| 4 | NONCE_B | phone → PC | Nb (16), pairing only |
| 5 | PC_FINISHED | PC → phone | HMAC (32) |
| 6 | PHONE_FINISHED | phone → PC | HMAC (32) |
| 7 | REJECT | either | u8 reason (1 refused or timed out, 2 proof failed, 3 busy) |

Ids are random 16-byte values each side makes once. `z` = ECDH of the two ephemeral keys. The transcript `H`
is SHA-256 of messages 1 to 4 exactly as sent (length, type and body).

**Paired** (the phone knows a pairing key `K` for this PC id, and the PC didn't set "force pairing"):
SERVER_HELLO has mode 0, then both sides derive the session keys and exchange FINISHED. If the PC doesn't
have `K` for that phone any more, it sends REJECT and reconnects with "force pairing". If the phone's proof
check fails (it re-paired or forgot this PC), the PC forgets its key and pairs again.

**Pairing** (numeric comparison, as in Bluetooth): the phone picks Nb and sends the commitment
`SHA-256("MyCam commit v1" | Nb | phonePub | pcPub)` in SERVER_HELLO, before it sees Na. The PC sends Na, the
phone reveals Nb, and the PC checks the commitment. Both sides show
`code = (first 4 bytes of SHA-256(pcPub | phonePub | Na | Nb), as u32) mod 1,000,000` (6 digits). The
commitment means someone in between can't choose keys that make the codes match. The PC sends PC_FINISHED,
the phone checks it and asks the user. Only after "Allow" does the phone store `K` and send PHONE_FINISHED.
The PC then stores `K` too: the phone in its private app storage, the PC protected with DPAPI under
`HKCU\Software\MyCam\PairedPhones`.

**The PC asks too (1.5).** The PC shows the code with **Allow** and **Don't allow**, and stores `K` and
starts the session only after its user allows *and* PHONE_FINISHED has arrived. Without this, a device on
the network posing as a phone could approve its own pairing in milliseconds and then send video into the
MyCam camera. There are no new messages:

- The PC sends PC_FINISHED as before, so both screens show the code at the same time.
- It waits up to the same 75 s for both answers.
- If its user refuses or doesn't answer, it sends REJECT (1) and closes the connection.
- Auto-connect doesn't connect to a refused phone again until the companion restarts. The user can still
  pick that phone with Connect, or the phone can ask for a session (`MYCAM+1`).

If the phone's user allowed first, the phone keeps a key the PC never stored. The existing recovery
handles it: the next time they connect, the PC has no key, sends REJECT (2) and pairs again with "force
pairing".

    K (pairing) = HKDF(ikm = z, salt = Na | Nb, info = "MyCam pair v1", 32)
    session     = HKDF(ikm = z | K, salt = Npc | Nph, info = "MyCam session v1", 96)
                = PC-to-phone key (32) | phone-to-PC key (32) | finished key kFin (32)
    PC_FINISHED    = HMAC(kFin, "PC" | H)
    PHONE_FINISHED = HMAC(kFin, "PH" | H)

Each session uses fresh ephemeral keys, so a leaked pairing key doesn't reveal earlier sessions' video.

**Records** (after both FINISHED messages): `u32 BE ciphertext length | AES-256-GCM ciphertext | tag`.
The nonce is `u32 direction (0 PC to phone, 1 phone to PC) | u64 counter`, big-endian. The counter starts at 0
in each direction and goes up by one per record. A record that fails its tag check (tampered, replayed,
reordered or the wrong key) ends the session. The phone sends one record per packet; the maximum record is 16 MiB.

**Forgetting:** "Forget paired PCs" on the phone and "Forget Wi-Fi phones" on the PC delete the keys; the
next connection pairs again with a new code.


## QR pairing (1.5)

The phone pairs by scanning the QR code on the PC's "Connect a phone" page. **iPhones and Android phones
scan the same QR code.** An iPhone opens it in Safari (`docs/web-camera-format.md`). The MyCam Android app
reads it with its own scanner and pairs natively. So neither screen shows a code to compare, and the phone
goes straight from the scan to a connected session.

Scanning is not treated as permission to watch, though. A QR code can be printed or forwarded, so it is
weaker evidence than a code compared on two screens: once the session is up, the phone asks whether that PC
may use the camera and keeps it off until it is told. See "Letting a PC use the camera".

**Nothing connects into the PC.** As with every Wi-Fi session, the PC only sends: UDP probes, and then a
TCP connection out to the phone. So Android pairing works on any network, Windows "Public" ones included,
with no firewall rule. (The iPhone page is the exception: Safari has to connect to the PC.)

**The QR code.**

    https://<pc-ipv4>:47900/?t=<code>&pc=<PC id>

- `t` is the existing one-time code: 16 random bytes in base64url (22 characters), used once, valid for
  5 minutes.
- `pc` is the PC id (32 hex digits), as in CLIENT_HELLO.

The app treats a scanned text as a MyCam code only if it has the `https` scheme, port 47900, path `/`, and
both `t` and `pc`. A code with `t` but no `pc` comes from an older companion: the app asks the user to
update MyCam on the PC.

**Steps.**

1. **The PC probes.** While the QR code is showing, the PC sends the usual `MYCAM?1 <pc name>` probe every
   2 s: by broadcast, and also unicast to every address of the subnet the QR code's address is on. The
   unicast copy gets through on networks that drop broadcasts. The subnet is the one the adapter reports,
   not an assumed /24; wider than a /24 there are too many addresses to ask one by one, so only the
   broadcast goes out and the log says so. Phones answer as they always do.
2. **The phone asks.** After the scan, the phone turns wireless mode on and waits up to 20 s for a probe
   from the QR code's address. To that probe's source address and port, it sends:

       phone -> PC:   MYCAM#1 <tcp port> <proof> <phone name>
       proof = hex of the first 16 bytes of HMAC-SHA256(code, "MyCam qr invite v1" | tcp port, u16 BE)

   It resends this every 2 s until the PC connects or it gives up.
3. **The PC checks.** If `proof` matches the current code, the PC connects to `<source address>:<tcp port>`
   and runs the handshake with CLIENT_HELLO flag bit 1 (QR pairing). Resends from that phone are ignored
   meanwhile. Otherwise it answers on the same socket:

       PC -> phone:   MYCAM-1 code     (no such code: used, expired, or a different PC's)
       PC -> phone:   MYCAM-1 busy     (another phone has a session)

   Nothing in a refusal proves it came from that PC, and a source address is easy to forge on a local
   network, so the phone treats it as a hint: it shows the reason, keeps inviting, and only reports it when
   the attempt runs out. A forged datagram therefore costs a few seconds instead of ending the pairing with
   a message telling the user to go and make a new code. The phone also ignores any refusal that doesn't
   closely follow an invite of its own.

   A PC that already has a connect attempt in flight says nothing at all rather than "busy": that attempt is
   a moment, not a state, and it may be a replay of this phone's own invite from somewhere else, so the
   phone's next resend gets in as soon as it finishes.

4. **The handshake.** A phone that holds a scanned code for that PC id answers SERVER_HELLO with mode 2. A
   phone without one sends REJECT (2). Both sides then derive:

       K (pairing) = HKDF(ikm = z | code, salt = Npc | Nph, info = "MyCam qr pair v1", 32)
       session     = as in paired mode, from z and K

   The FINISHED messages follow as usual. The code is in K, so someone in between (who never saw the
   screen) can't produce a valid FINISHED. The code itself is 128 bits, so guessing it is hopeless.

5. **Done.** Both sides store `K` (replacing any older key), the PC uses up the code, and the QR code shows
   a new one. From the moment the PC accepts an invite until the handshake ends, that code is claimed:
   `POST /pair` answers **409** meanwhile, so one displayed code can't also become an iPhone's device token.
   A handshake that doesn't pair releases it again.

Golden vectors: `qr.*` (`qr.pair_key`, `qr.invite_proof`), generated with the JDK, checked by both test
suites.

**After pairing,** the phone keeps wireless mode on, and the PC turns "Find phones on Wi-Fi" on (the user
just paired a Wi-Fi phone) and writes that to its log, since it also means announcing its name on the
network every 2 s from then on. So they reconnect by themselves next time, as any paired phone does —
subject to the camera flag in "Letting a PC use the camera".

**Limits.**
- The phone and the PC must be able to reach each other directly, so a guest Wi-Fi with client isolation
  won't work.
- The scanner can't run while the phone is streaming, because the stream is using the camera.


## Letting a PC use the camera (1.5)

Pairing settles that the two sides share a key. It does not settle that the person holding the phone wanted
*that* PC to be able to watch through it, and a QR code is a bearer token: it can be printed, forwarded or
shown on a web page, so scanning one is weaker evidence than comparing a code on two screens.

So the phone keeps one flag per PC, beside the pairing key: **may this PC use the camera?**

- **Set when the user says so**, and nowhere else. A 6-digit pairing sets it as the user taps Allow, because
  that tap already is the answer. A QR pairing does not set it.
- **A PC without it still gets a session.** The handshake runs, the link comes up and the phone reports
  `STATE` = paused. `START` is answered with `STATE` (paused) and the camera stays off, exactly as a user
  pause behaves, so the PC shows its "Camera paused" picture and nothing else changes.
- **The phone asks once**, naming the PC: *"Paired with <PC>. Let this PC use the camera?"* **Allow** stores
  the flag and starts the camera if the PC is already asking for video. **Don't allow** forgets the PC
  (key and all) and drops the link, so a code scanned by mistake leaves nothing behind. Back and the close
  button mean "don't allow": a stray tap must not half-allow a PC.
- **A key stored before this flag existed** reads as "not allowed", so the first connection after an update
  asks. That is deliberate: those keys were created without anyone being asked.
- **Over the cable there is no flag.** Plugging the phone in is the consent, and USB links are never gated.

There are no new messages: a blocked camera is an ordinary paused camera on the wire.
