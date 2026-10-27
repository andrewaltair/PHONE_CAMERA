# libusb (vendored, trimmed to the Windows backend)

Vendored from [libusb](https://libusb.info) (LGPL-2.1, see `COPYING`), nano version `LIBUSB_NANO` 11953 in
`libusb/version_nano.h`.

The upstream MyCam snapshot trimmed this copy to the Windows backend. PHONE_CAMERA builds this same
vendored subset as a separate shared DLL. The tree keeps only what `pc/CMakeLists.txt`'s `usb` target
actually compiles: `libusb/core.c`, `descriptor.c`, `hotplug.c`, `io.c`,
`strerror.c`, `sync.c`, the Windows backend under `libusb/os/` (`events_windows`, `threads_windows`,
`windows_common`, `windows_usbdk`, `windows_winusb`), their headers, and `msvc/config.h`.

Upstream also ships backends for Linux, macOS, the BSDs, Haiku and Emscripten/WebUSB, plus its own build
systems (autotools, Xcode, Android NDK, MSVC project files), examples and test suite — none of which this
build ever touches, so they were dropped to cut the vendored tree from 152 files down to this set. To pull
in a newer libusb release, re-vendor from a fresh checkout of the upstream repo and re-apply this same trim
(or just copy the files listed above).
