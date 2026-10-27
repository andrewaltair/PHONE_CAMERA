# iPhone over a cable: what works, what doesn't, and the way forward

*Part 1b of [AIYAN-IPHONE.md](AIYAN-IPHONE.md): a foundation, not the finished feature. Status 2026-10-11:
the PC side is ready for the hotspot route; **the tests on a real iPhone are still to do**. The findings
table below is left empty until they're run. Nothing in it is a guess.*

## Why there's no simple "plug in and stream"

- **No accessory mode:** Android phones have Android Open Accessory, which MyCam uses: the PC switches the
  phone into accessory mode and a plain USB pipe appears. iPhones have nothing like it for third parties.
- **usbmux needs an app:** Apple's USB channel is **usbmux**, which Windows reaches through Apple Mobile
  Device Support (from the Apple Devices app or iTunes). It tunnels TCP connections *to a port on the
  phone*, so something on the phone must be listening there. A web page can't listen on a port. That
  takes a native iOS app, and a native app needs a Mac with Xcode and the $99/year developer program
  (the reasons the web page exists).

Two routes remain:

| Route | Needs on the phone | Needs on the PC | Status |
|---|---|---|---|
| **A. Personal Hotspot over USB** | The MyCam web page (no app), Personal Hotspot turned on | Apple Devices app (its driver makes the network adapter) | PC side built, to be tested |
| **B. usbmux + native iOS app** | A MyCam iOS app listening on a TCP port | Apple Devices app; a usbmux transport in the companion | Reference code on branch `aiyan`, not merged |

## Route A: Personal Hotspot over USB

With Personal Hotspot on and the iPhone plugged in, Windows gets a network adapter called **"Apple Mobile
Device Ethernet"** over the cable. The iPhone is the router, usually `172.20.10.1`, and the PC gets an
address in `172.20.10.0/28`. If Safari on the iPhone can reach the PC at that address, the same web page
and the same server work over the cable, with no app.

### What the PC already does for it

- **Address choice:** `web_proto.cpp` (`ClassifyAdapter`) recognises "Apple Mobile Device" adapters as
  `IPhoneUsb`. The "Connect an iPhone" page lists that address as "… · iPhone over USB" in the network
  choice, so the QR code can point at it.
- **Status:** a session that arrives on that adapter is reported as **"USB hotspot"**: in the status
  ("Back camera · USB hotspot") and in the tray balloon ("iPhone connected over USB hotspot").
- **Listening:** the server listens on all addresses, so nothing else changes.
- **Firewall:** Windows usually treats the hotspot network as **Public**, and the firewall rule allows
  private networks only. Allowing it on that one network would need the user to set it to Private
  (Settings → Network → Apple Mobile Device Ethernet → Private network). Whether that is needed, and
  whether to handle it, is part of the test below.

### The test (to run with the iPhone 13)

1. Install the Apple Devices app on the PC (Microsoft Store) and plug the iPhone in. Tap "Trust This
   Computer".
2. On the iPhone: Settings → Personal Hotspot → Allow Others to Join. Turn Wi-Fi off on the PC, so the
   cable is the only link between them.
3. Check Windows: `ipconfig` shows an adapter "Apple Mobile Device Ethernet" with a `172.20.10.x`
   address. Note the network profile it gets (Public or Private).
4. In MyCam: Connect an iPhone → choose the "iPhone over USB" network → scan the code with the iPhone →
   Start.
5. Record the results below:
   - Did it connect? If not, did it work after setting the network to Private?
   - Delay: use the same method as in web-camera-format.md.
   - Throughput: the page's LOG lines and the companion's `stats:` lines.
   - 1080p30 and 1080p60.
   - 10 minutes steady?
6. Try without a SIM card (or with mobile data off), and on a carrier plan that blocks tethering: does the
   Personal Hotspot setting appear at all?

### Findings

| Question | Result |
|---|---|
| Does the hotspot adapter appear on the PC (Apple Devices app installed)? | *not tested yet* |
| Network profile Windows gives it (Public / Private) | *not tested yet* |
| Does Safari reach the PC at the hotspot address? Firewall change needed? | *not tested yet* |
| Delay over the cable, 1080p30 / 1080p60 | *not tested yet* |
| Throughput (Mbps) and smoothness over 10 minutes | *not tested yet* |
| No SIM / mobile data off: can Personal Hotspot be turned on? | *not tested yet* |
| Carrier that blocks tethering | *not tested yet* |

What is already known, independent of this test: Personal Hotspot depends on the carrier. Some plans hide
the setting, and on some iPhones it can't be turned on without a SIM or with mobile data off. If that is
the case on a user's phone, route A isn't available to them, and Wi-Fi (or route B) is what's left.

## Route B: usbmux and a native app (future)

Aiyan's earlier code on branch `aiyan` (`pc/companion/usbmux_transport.cpp`, `pc/tools/fake_usbmuxd.py`):
- talks to Apple's usbmuxd service;
- finds the iPhone;
- opens a TCP tunnel to a port on the phone.

It stays there as reference and **must not be merged as it is**. Its two-thread design can deadlock the
session loop (PROGRESS.md §14).

If an iOS app becomes possible (a Mac and the developer program), rebuild it the way the web transport
was built:
- a pump that reads the tunnel and calls `FeedBytes`;
- a send path for the 8-byte commands;
- everything on the worker thread, inside the one `SessionLoop` (see `phone_link_web.cpp` and
  `RunTcpSession`).

The app would speak the same MyCam protocol, as the page does.

## Recommendation

1. Ship Wi-Fi (the web page) as the iPhone path. It works without an app or extra software.
2. Run the route A test. If the hotspot adapter carries the page reliably, document it as "iPhone over
   USB" (Apple Devices app + Personal Hotspot). Decide whether the companion should help with the Public
   network profile, for example by explaining it in the window, or with a second firewall rule limited to
   the Apple adapter (that would need saif's agreement).
3. Keep route B for if and when a native iOS app is possible.
