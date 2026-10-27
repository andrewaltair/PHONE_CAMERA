// phone-camera-companion on Linux (docs/LINUX-SUPPORT.md): a headless daemon. It runs the same PhoneLink as the
// Windows tray app (USB, Android Wi-Fi and the iPhone page, one session at a time) and writes the video into a
// v4l2loopback camera. Headless: status goes to the log (stderr) and to phone-camera-tray through
// the control socket (control_posix.h), and --pair shows the iPhone QR code in the terminal.
//
//   phone-camera-companion --daemon [--device /dev/video10] [--size 1280x720] [--pair] [--wifi]
//   phone-camera-companion --pair

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "../third_party/qrcodegen/qrcodegen.hpp"
#include "control_posix.h"
#include "linux_device.h"
#include "linux_identity.h"
#include "log.h"
#include "phone_link.h"
#include "platform.h"
#include "status_images.h"

using namespace mycam;

namespace {

PhoneLink* g_link = nullptr;
constexpr int kCameraConfigurationExit = 78; // EX_CONFIG: systemd must not restart a missing/unsafe camera.

void OnSignal(int) {
    if (g_link) g_link->Quit(); // An atomic store: safe in a signal handler.
}

const char* StateName(LinkState s) {
    switch (s) {
    case LinkState::NoDriver: return "no USB and no iPhone server: nothing can connect";
    case LinkState::Searching: return "waiting for a phone";
    case LinkState::Waiting: return "phone found, waiting for the PHONE_CAMERA app";
    case LinkState::Idle: return "connected, camera off";
    case LinkState::Streaming: return "streaming";
    case LinkState::PhoneError: return "the phone reported a camera problem";
    case LinkState::Paused: return "paused on the phone";
    }
    return "?";
}

// The pairing address as a QR code, in half-block characters (two rows per line) with a quiet zone. It goes
// to stdout for the person at the terminal, never to the log: it holds the one-time pairing code.
void PrintQr(const std::string& url) {
    const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(url.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
    const int border = 2, size = qr.getSize();
    auto dark = [&](int x, int y) { return x >= 0 && y >= 0 && x < size && y < size && qr.getModule(x, y); };
    printf("\nScan this with the iPhone's camera (same Wi-Fi), then tap Allow in Safari:\n\n");
    for (int y = -border; y < size + border; y += 2) {
        std::string line;
        for (int x = -border; x < size + border; ++x) {
            const bool top = dark(x, y), bottom = dark(x, y + 1);
            // Light modules are drawn, dark ones left blank: right for a dark terminal background.
            line += !top && !bottom ? "█" : !top ? "▀" : !bottom ? "▄" : " ";
        }
        printf("%s\n", line.c_str());
    }
    printf("\nThe code changes after each use and every few minutes; a new QR code is printed when it does.\n\n");
    fflush(stdout);
}

void Usage(const char* argv0) {
    fprintf(stderr,
            "usage: %s --pair\n"
            "         show a QR code to connect an iPhone (Safari, same Wi-Fi), from the running PHONE_CAMERA\n"
            "       %s --daemon [--device /dev/video10] [--size WIDTHxHEIGHT] [--pair] [--wifi]\n"
            "         run PHONE_CAMERA (the phone-camera service does this)\n"
            "  --device  an owner-approved V4L2 device; without it exactly one PHONE_CAMERA-labelled loopback is required\n"
            "  --size    the camera size apps see (default 1280x720)\n"
            "  --pair    with --daemon: show the iPhone QR code in this terminal from the start\n"
            "  --wifi    also look for Android phones on the Wi-Fi\n",
            argv0, argv0);
}

} // namespace

int main(int argc, char** argv) {
    std::string device;
    unsigned width = 1280, height = 720;
    bool daemon = false, pair = false, wifi = false, deviceExplicit = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--daemon") daemon = true;
        else if (a == "--pair") pair = true;
        else if (a == "--wifi") wifi = true;
        else if (a == "--device" && i + 1 < argc) {
            device = argv[++i];
            deviceExplicit = true;
        }
        else if (a == "--size" && i + 1 < argc) {
            if (sscanf(argv[++i], "%ux%u", &width, &height) != 2 || width % 2 || height % 2 || !width || !height) {
                fprintf(stderr, "--size: expected an even WIDTHxHEIGHT, like 1280x720\n");
                return 2;
            }
        } else {
            Usage(argv[0]);
            return 2;
        }
    }
    if (!PrepareLinuxIdentity()) {
        fprintf(stderr, "PHONE_CAMERA: HOME/XDG directories are unavailable; cannot create private state.\n");
        return 1;
    }
    if (!daemon) {
        if (pair) return RunPairClient(PrintQr);
        Usage(argv[0]);
        return 2;
    }

    LogInit();
    LinuxCameraSelection camera;
    std::string cameraWhy;
    if (!ResolveLinuxCamera(device, deviceExplicit, &camera, &cameraWhy)) {
        fprintf(stderr, "PHONE_CAMERA: camera preflight failed: %s\n", cameraWhy.c_str());
        return kCameraConfigurationExit;
    }
    device = camera.path;
    if (camera.explicitOverride && camera.card != "PHONE_CAMERA") {
        Log("camera: using explicitly assigned %s (card label is %s; it is not auto-selected)", device.c_str(),
            camera.card.empty() ? "unknown" : camera.card.c_str());
    }
    SetLinuxCameraPolicy(camera.explicitOverride, camera.card);
    FrameWriter::Configure(device, width, height);
    LinkState lastState = LinkState(-1);
    std::wstring lastUrl;
    ControlServer* control = nullptr;
    PhoneLink link(
        [&](const LinkStatus& s) {
            if (control) control->OnStatus(s);
            if (s.state != lastState) {
                lastState = s.state;
                const std::string phone = Utf8(s.phoneName);
                Log("status: %s%s%s", StateName(s.state), phone.empty() ? "" : " - ", phone.c_str());
            }
            if (pair && s.webUrl != lastUrl) {
                lastUrl = s.webUrl;
                if (!s.webUrl.empty()) PrintQr(Utf8(s.webUrl));
            }
        },
        nullptr); // No driver binding to ask for on Linux.
    g_link = &link;
    struct sigaction sa = {};
    sa.sa_handler = OnSignal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN); // A phone that drops its connection mid-send is a failed send, not a crash.

    // "Waiting for the phone" / "Camera paused" while there is no live video. They also make PHONE_CAMERA appear in
    // apps at all: with exclusive_caps, v4l2loopback offers a camera only once a frame has been written.
    link.SetStatusImages(LoadStatusImage(kImagePaused), LoadStatusImage(kImageWaiting));
    link.SetWireless(wifi);
    uint64_t mirror = 0, fill = 0; // The tray's picture options, kept across restarts.
    StoreGetU64("", "Mirror", &mirror);
    StoreGetU64("", "Fill", &fill);
    link.SetMirror(mirror != 0);
    link.SetFill(fill != 0);
    if (pair) link.StartWebPairing(0);
    ControlServer server(&link, mirror != 0, fill != 0);
    if (server.Start()) control = &server;
    Log("phone-camera-companion: camera %s at %ux%u%s", device.c_str(), width, height,
        wifi ? ", Android Wi-Fi on" : "");
    link.Run(); // Until SIGINT / SIGTERM.
    control = nullptr;
    server.Stop();
    g_link = nullptr;
    Log("phone-camera-companion: stopped");
    return 0;
}
