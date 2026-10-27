// usb_probe.exe: initialises libusb with the UsbDk backend (debug logging on) and lists devices.
#include <libusb.h>
#include <stdio.h>

int main() {
    libusb_init_option options[] = {{LIBUSB_OPTION_LOG_LEVEL, {LIBUSB_LOG_LEVEL_DEBUG}}};
    libusb_context* ctx = nullptr;
    int r = libusb_init_context(&ctx, options, 1);
    if (r == 0) r = libusb_set_option(ctx, LIBUSB_OPTION_USE_USBDK);
    printf("libusb_init_context: %d (%s)\n", r, libusb_error_name(r));
    if (r != 0) return 1;
    libusb_device** list;
    ssize_t n = libusb_get_device_list(ctx, &list);
    printf("devices: %zd\n", n);
    for (ssize_t i = 0; i < n; ++i) {
        libusb_device_descriptor d;
        libusb_get_device_descriptor(list[i], &d);
        printf("  %04x:%04x class %02x\n", d.idVendor, d.idProduct, d.bDeviceClass);
    }
    if (n >= 0) libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return 0;
}
