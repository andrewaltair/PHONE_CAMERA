// The USB backend on Linux: the distro's libusb talks to usbfs directly, for the AOA switch and for streaming.
// The only obstacle is permission on /dev/bus/usb, which the udev rule shipped with the package grants.

#include <libusb.h>

#include "log.h"
#include "phone_link.h"

namespace mycam {

bool PhoneLink::InitUsb() {
    const bool ok = libusb_init_context(&ctx_, nullptr, 0) == LIBUSB_SUCCESS;
    if (!ok) ctx_ = nullptr;
    Log("usb: libusb %s", ok ? "ready" : "FAILED");
    return ok;
}

void PhoneLink::ExitUsb() {
    if (ctx_) libusb_exit(ctx_);
    ctx_ = nullptr;
}

bool PhoneLink::UsbNeedsDriver() { return false; } // No driver binding on Linux.

void PhoneLink::RunAccessory(libusb_device* accessory) { RunSession(accessory, ctx_); }

const char* PhoneLink::BackendName(libusb_context*) const { return "usbfs"; }

} // namespace mycam
