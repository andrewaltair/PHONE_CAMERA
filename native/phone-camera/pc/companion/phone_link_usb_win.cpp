// The USB backend on Windows: UsbDk switches phones into accessory mode; once WinUSB is bound to the
// accessory (winusb_bind.cpp, via the SYSTEM scheduled task), a second libusb context streams through it.

#include <windows.h>

#include <libusb.h>

#include "log.h"
#include "phone_link.h"
#include "winusb_bind.h"

namespace mycam {

bool PhoneLink::InitUsb() {
    // UsbDk lets libusb talk to phones while Windows' own MTP/ADB drivers are bound to them.
    // libusb silently falls back to WinUSB without it, which cannot open phones, so check explicitly.
    HMODULE usbdk = LoadLibraryExW(L"UsbDkHelper.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (usbdk) FreeLibrary(usbdk);
    // Switch to UsbDk after init: libusb_init_context applies options before it has probed for UsbDk,
    // so passing LIBUSB_OPTION_USE_USBDK there always fails with LIBUSB_ERROR_NOT_FOUND.
    bool ok = usbdk && libusb_init_context(&ctx_, nullptr, 0) == LIBUSB_SUCCESS;
    if (ok && libusb_set_option(ctx_, LIBUSB_OPTION_USE_USBDK) != LIBUSB_SUCCESS) {
        libusb_exit(ctx_);
        ctx_ = nullptr;
        ok = false;
    }
    Log("usb: UsbDk helper %s, libusb %s", usbdk ? "found" : "MISSING", ok ? "ready" : "FAILED");
    if (ok && libusb_init_context(&winusbCtx_, nullptr, 0) != LIBUSB_SUCCESS) winusbCtx_ = nullptr;
    return ok;
}

void PhoneLink::ExitUsb() {
    if (winusbCtx_) libusb_exit(winusbCtx_);
    winusbCtx_ = nullptr;
    if (ctx_) libusb_exit(ctx_);
    ctx_ = nullptr;
}

bool PhoneLink::UsbNeedsDriver() { return AccessoryNeedsDriver(); }

void PhoneLink::RunAccessory(libusb_device* accessory) {
    // Prefer WinUSB: UsbDk refuses to redirect a device that already has a working driver, and the
    // accessory gets WinUSB bound on first connection (see winusb_bind.cpp).
    libusb_device* viaWinUsb = winusbCtx_ ? FindAccessory(winusbCtx_) : nullptr;
    if (viaWinUsb) {
        RunSession(viaWinUsb, winusbCtx_);
        libusb_unref_device(viaWinUsb);
    } else {
        RunSession(accessory, ctx_);
    }
}

const char* PhoneLink::BackendName(libusb_context* ctx) const { return ctx == ctx_ ? "UsbDk" : "WinUSB"; }

} // namespace mycam
