// Developer tool: runs the iPhone page's server (WebServer) on its own and plays the PC side, so the page,
// TLS and the WebSocket can be tested without the companion. Built with -DMYCAM_BUILD_TOOLS=ON.
//
//   web_probe [seconds]     prints the pairing address, then for each page that connects: HELLO, START,
//                           a KEYFRAME after 3 s, and STOP after `seconds` (default 10). Every packet is
//                           checked and summarised. The address holds a one-time code: it is printed here
//                           on purpose (this is a test tool); the companion never logs it.

#include <winsock2.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "../companion/log.h"
#include "../companion/packet_parser.h"
#include "../companion/protocol.h"
#include "../companion/web_server.h"

namespace mycam {
void LogInit() {}
void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    printf("  log: ");
    vprintf(fmt, args);
    printf("\n");
    va_end(args);
    fflush(stdout);
}
} // namespace mycam

using namespace mycam;

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? atoi(argv[1]) : 10;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    WebServer web;
    if (!web.Start()) return 1;
    web.StartPairing(0);
    uint32_t generation = ~0u;

    PacketParser parser;
    bool session = false, hello = false;
    uint64_t sessionStart = 0, lastHello = 0;
    int frames = 0, keys = 0, problems = 0;
    int64_t lastPts = -1;
    bool keySent = false, stopSent = false;
    auto command = [&](proto::Command c, uint8_t arg = 0) {
        uint8_t b[proto::kCommandSize];
        proto::MakeCommand(b, c, arg);
        printf("PC -> page: command %d arg %d\n", int(c), int(arg));
        web.SendToPhone(b, sizeof(b));
    };
    web.SetPhoneHandler([&](const uint8_t* data, size_t size) {
        parser.Feed(data, size, [&](const Packet& p) {
            switch (p.type) {
            case proto::kHello:
                hello = true;
                printf("page: HELLO version %d\n", p.length >= 2 ? int(proto::ReadU16(p.payload)) : -1);
                command(proto::kCmdStart);
                break;
            case proto::kConfig:
                printf("page: CONFIG %ux%u sensor %u facing %u, %u bytes of SPS/PPS\n", proto::ReadU16(p.payload),
                       proto::ReadU16(p.payload + 2), proto::ReadU16(p.payload + 4), p.payload[6], p.length - 7);
                break;
            case proto::kFrame:
                ++frames;
                if (p.flags & proto::kFlagKeyFrame) ++keys;
                if (frames == 1 && !(p.flags & proto::kFlagKeyFrame)) { ++problems; printf("PROBLEM: first frame isn't a key frame\n"); }
                if (p.ptsUs <= lastPts) { ++problems; printf("PROBLEM: timestamp went back\n"); }
                lastPts = p.ptsUs;
                if ((p.flags & proto::kFlagKeyFrame) || frames % 30 == 1)
                    printf("page: FRAME %s %u bytes pts %lld (%d so far)\n", (p.flags & 1) ? "KEY" : "delta", p.length,
                           static_cast<long long>(p.ptsUs), frames);
                break;
            case proto::kOrient: printf("page: ORIENT %u\n", proto::ReadU16(p.payload)); break;
            case proto::kState: printf("page: STATE %u facing %u\n", p.payload[0], p.length > 1 ? p.payload[1] : 0); break;
            case proto::kLog: printf("page: LOG %.*s\n", int(p.length), reinterpret_cast<const char*>(p.payload)); break;
            case proto::kCamera: {
                proto::CameraInfo c;
                if (proto::ParseCameraInfo(p.payload, p.length, &c))
                    printf("page: CAMERA q%u %u fps, %ux%u actual %u, flags 0x%02x, masks %02x %02x %02x\n", c.quality,
                           c.fps, c.width, c.height, c.actualFps, c.flags, c.fpsModes[0], c.fpsModes[1], c.fpsModes[2]);
                break;
            }
            }
            fflush(stdout);
        });
    });

    for (;;) {
        web.Poll(50);
        if (web.Generation() != generation) {
            generation = web.Generation();
            const std::string url = web.PairingUrl();
            if (!url.empty()) printf("pairing address: %s\n", url.c_str());
            fflush(stdout);
        }
        const uint64_t now = GetTickCount64();
        if (!session && web.PhonePending()) {
            std::string id, name, link;
            web.TakePhone(&id, &name, &link);
            printf("session: %s over %s\n", name.c_str(), link.c_str());
            session = true;
            hello = keySent = stopSent = false;
            frames = keys = problems = 0;
            lastPts = -1;
            parser.Reset();
            sessionStart = now;
            lastHello = 0;
        }
        if (session && !web.PhoneConnected()) {
            printf("session ended: %d frames (%d key), %d problem(s)\n", frames, keys, problems);
            fflush(stdout);
            session = false;
            continue;
        }
        if (!session) continue;
        if (!hello && now - lastHello > 2000) {
            command(proto::kCmdHello);
            lastHello = now;
        }
        if (hello && !keySent && now - sessionStart > 3000) {
            command(proto::kCmdKeyFrame);
            keySent = true;
        }
        if (hello && !stopSent && now - sessionStart > uint64_t(seconds) * 1000) {
            command(proto::kCmdStop);
            stopSent = true;
        }
    }
}
