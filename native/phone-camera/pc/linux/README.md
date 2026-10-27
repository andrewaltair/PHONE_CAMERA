# PHONE_CAMERA on Linux (preview)

PHONE_CAMERA turns your phone into a webcam for Zoom, OBS, Discord and browsers. On Linux it runs in the
background and shows up as a camera called **PHONE_CAMERA**. This daemon/tray path is based on the
MIT-licensed MyCam source; its protocol, pairing and crypto remain compatible with the phone app.

**Status:** a preview. The iPhone (over Wi-Fi) is tested on Ubuntu 24.04. Android phones (USB and Wi-Fi)
are built in but not tested on Linux yet. A tray icon shows the status and has the everyday controls; there
is no settings window yet.

## Install

Download `phone-camera_<version>_amd64.deb`, then:

```
sudo apt install ./phone-camera_<version>_amd64.deb
```

The package does not enable or start a service, install a tray autostart entry, or launch the tray. After the
owner has provided an approved camera, start the daemon and optional tray explicitly:

```
systemctl --user start phone-camera
phone-camera-tray &
```

The install also:

- installs the daemon, tray and USB udev access;
- reloads udev rules only; reconnect the intended phone to apply the rule;
- does **not** install or load a global `v4l2loopback` configuration. That module may be shared with OBS or
  another virtual-camera product, so PHONE_CAMERA never overwrites, unloads or stops it.

Before starting the service, the owner must provide exactly one loopback output labelled `PHONE_CAMERA`.
For a private machine, an owner may configure `v4l2loopback` with `card_label=PHONE_CAMERA` and a chosen
`video_nr`; the package does not do this. If the module is shared, leave its existing options alone and start
the daemon with an explicitly owner-approved `--device /dev/videoN` path.

If the install says **v4l2loopback is missing**, install it and reboot:

```
sudo apt install v4l2loopback-dkms
```

## Connect an iPhone (or iPad)

The iPhone and the PC must be on the same network.

1. Click the PHONE_CAMERA tray icon → **Connect an iPhone…**. A QR code appears. (In a terminal:
   `phone-camera-companion --pair` shows the same code.)
2. Point the iPhone's camera at it and open the link.
3. Safari warns that the connection isn't private. That is expected (PHONE_CAMERA makes its own certificate):
   tap **Show Details**, then **visit this website**.
4. Allow the camera. The window says "Paired with ..." and you can close it.

Next time, just open the page again on the iPhone: no QR code needed. Tip: in Safari, tap Share →
**Add to Home Screen**, so clearing Safari's history doesn't make the iPhone forget the PC.

## Use it

Pick **PHONE_CAMERA** as the camera in your app. While no phone is connected it shows "Waiting for the phone".
The phone's camera only runs while an app is showing PHONE_CAMERA, and turns off about 4 seconds after the last
one closes it.

**The tray icon** (top right) shows how PHONE_CAMERA is doing: grey with no phone, blue when ready, green while
streaming, amber while paused, red on an error. Its menu has the same controls as on Windows: pause and
resume, back or front camera, quality, mirror, fill the frame, connect an iPhone, reconnect, start at login,
and quit. **Quit PHONE_CAMERA** stops the camera too; it starts again at the next login, or from the PHONE_CAMERA entry
in your apps.

**OBS:** right-click the PHONE_CAMERA source → **Properties**, scroll down, and untick **Use Buffering**. With
buffering on, the picture runs noticeably behind.

**Port ownership:** the PHONE_CAMERA Linux preview keeps the upstream iPhone page on TCP port `47900` so
existing phone clients continue to work. The original MyCam daemon and PHONE_CAMERA cannot both listen on
that same port on one host at the same time; stop one before starting the other. A second process retries
the listener and does not replace the first process.

## If something doesn't work

| Problem | Fix |
|---|---|
| OBS crashes when you add a camera | Ubuntu 24.04's own OBS (30.0.2) crashes on any virtual camera. Use the OBS team's build: `sudo add-apt-repository ppa:obsproject/obs-studio && sudo apt install obs-studio`. |
| PHONE_CAMERA isn't in the app's camera list | Check `systemctl --user status phone-camera`, then `v4l2-ctl --list-devices`. Automatic selection requires one output labelled "PHONE_CAMERA"; otherwise pass an owner-approved `--device /dev/videoN`. |
| The service exits before pairing | This is intentional when camera preflight cannot find a safe node (exit status `78`); systemd does not restart that configuration failure. Provide the approved node, then run `systemctl --user start phone-camera`. |
| A shared v4l2loopback module has no PHONE_CAMERA label | Do not unload or overwrite its configuration. Run `phone-camera-companion --device /dev/videoN` only after the device owner explicitly assigns that node to PHONE_CAMERA. |
| The iPhone can't open the page | A firewall may block port 47900: `sudo ufw allow 47900/tcp`. Also check both are on the same network. |
| `phone-camera-companion --pair` says PHONE_CAMERA isn't running | `systemctl --user start phone-camera` |
| No tray icon | GNOME needs its AppIndicator extension, which Ubuntu turns on by default. On other GNOME desktops, install "AppIndicator and KStatusNotifierItem Support". Then run `phone-camera-tray &`. |
| Anything else | The log: `journalctl --user -u phone-camera -f`. It never contains pairing codes. |

## Uninstall

```
sudo apt remove phone-camera
```

## For developers

Building from source, the architecture and the tests: [docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md).
The daemon can also run by hand: `phone-camera-companion --daemon [--device /dev/videoN] [--size WxH] [--pair] [--wifi]`
(stop the service first: `systemctl --user stop phone-camera`). When reinstalling a `.deb` of the same version,
use `sudo apt install --reinstall`, or apt keeps the old one.
