# PHONE_CAMERA and MyCam: what is different

This comparison uses PHONE_CAMERA's original WebRTC mode and the MyCam source
snapshot from [upstream commit `a0499ea`](https://github.com/whoissaaif/mycam/tree/a0499eab7375a99db6a6ca5ee3b8faa7f2d35f6a).
The upstream snapshot is included under [`native/mycam`](../native/mycam) as a
separate application; it is not a feature toggle inside the WebRTC app.

| Capability | PHONE_CAMERA WebRTC mode | Imported MyCam source |
| --- | --- | --- |
| Phone capture | Safari camera and microphone via WebRTC | Android application; separate Safari/WebCodecs path for iPhone |
| USB | iPhone Personal Hotspot provides an IP link | Android Open Accessory USB; iPhone cable support is not implemented upstream |
| PC camera device | OBS Browser Source plus OBS Virtual Camera | Native Windows virtual-camera implementations; Linux preview uses v4l2loopback |
| Quality and frame rate | Requests 480p–1080p and 30/60 fps; actual settings depend on the browser and phone | Source supports up to 4K and 120 fps where the particular phone and encoder support them |
| Rate adaptation | WebRTC congestion control and a manual bitrate ceiling | Upstream includes bitrate/backpressure handling for its own transport |
| Device pairing | No join PIN; use a trusted LAN | Pairing and device authorization in the native application |
| Privacy controls | Start and Stop release the browser camera and microphone | Upstream has its own pause and device-consent flows |
| Camera controls | Browser-exposed lens, quality request, zoom and torch | Device-reported controls and mode combinations, with platform-dependent availability |
| Simultaneous viewers | One receiving slot; PC viewer and OBS replace one another | Upstream supports one phone connection at a time |

The WebRTC mode also reports capture settings and received resolution/FPS
where supported, and reconnects signaling within a bounded retry window. Its
HUD is not an end-to-end latency measurement. A source comparison or a
synthetic test does not establish which path is faster or sharper on a real
phone, PC, network, or video-call application.

See [native build, provenance and license notes](MYCAM_NATIVE.md) and the
[upstream architecture](../native/mycam/docs/ARCHITECTURE.md).
