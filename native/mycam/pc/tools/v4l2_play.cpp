// Developer tool (Linux, not shipped): plays a raw H.264 Annex-B file through the companion's decoder and
// v4l2loopback writer, the same path phone video takes, so the camera can be checked without a phone.
//
//   ffmpeg -f lavfi -i testsrc=size=1280x720:rate=30 -t 10 -c:v libx264 -bf 0 test.h264
//   v4l2_play test.h264 [/dev/video10] [1280x720] [rotation 0/90/180/270] [mirror]
//
// Loops until Ctrl+C.

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "../companion/decoder_ffmpeg.h"
#include "../companion/frame_writer_v4l2.h"

using namespace mycam;

namespace {

// Splits the file into access units, as the phone sends them (one per MyCam video packet).
std::vector<std::vector<uint8_t>> AccessUnits(const std::vector<uint8_t>& file) {
    std::vector<std::vector<uint8_t>> units;
    AVCodecParserContext* parser = av_parser_init(AV_CODEC_ID_H264);
    AVCodecContext* ctx = avcodec_alloc_context3(nullptr);
    if (!parser || !ctx) return units;
    const uint8_t* p = file.data();
    int left = int(file.size());
    while (true) {
        uint8_t* out = nullptr;
        int outSize = 0;
        const int used = av_parser_parse2(parser, ctx, &out, &outSize, p, left, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        p += used;
        left -= used;
        if (outSize > 0) units.emplace_back(out, out + outSize);
        if (left == 0 && outSize == 0) break; // The final call (left == 0) flushes the last unit.
    }
    av_parser_close(parser);
    avcodec_free_context(&ctx);
    return units;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s file.h264 [/dev/video10] [WIDTHxHEIGHT] [rotation] [mirror]\n", argv[0]);
        return 2;
    }
    const std::string device = argc > 2 ? argv[2] : "/dev/video10";
    unsigned width = 1280, height = 720;
    if (argc > 3 && sscanf(argv[3], "%ux%u", &width, &height) != 2) { fprintf(stderr, "bad size: %s\n", argv[3]); return 2; }
    const uint32_t rotation = argc > 4 ? uint32_t(atoi(argv[4])) : 0;
    const bool mirror = argc > 5 && strcmp(argv[5], "mirror") == 0;

    std::ifstream in(argv[1], std::ios::binary);
    const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto units = AccessUnits(file);
    if (units.empty()) { fprintf(stderr, "%s: no H.264 access units\n", argv[1]); return 1; }

    FrameWriter writer;
    std::string why;
    if (!writer.Open(device, width, height, &why)) { fprintf(stderr, "%s\n", why.c_str()); return 1; }

    unsigned decoded = 0;
    H264Decoder decoder;
    auto onFrame = [&](const H264Decoder::Nv12View& f) {
        if (decoded++ == 0) printf("decoding %ux%u -> %s at %ux%u\n", f.width, f.height, device.c_str(), width, height);
        writer.WritePlanes(f.y, f.uv, f.pitch, f.width, f.height, rotation, mirror, false);
    };

    printf("%zu access units; Ctrl+C to stop\n", units.size());
    const auto frameTime = std::chrono::microseconds(33333); // ponytail: fixed 30 fps; the file's own timing is ignored.
    auto next = std::chrono::steady_clock::now();
    for (unsigned loop = 0;; ++loop) {
        if (decoder.Init(width, height, onFrame) < 0) { fprintf(stderr, "decoder init failed\n"); return 1; }
        unsigned errors = 0;
        for (size_t i = 0; i < units.size(); ++i) {
            if (decoder.Decode(units[i].data(), units[i].size(), int64_t(i) * 33333) < 0) ++errors;
            next += frameTime;
            std::this_thread::sleep_until(next);
        }
        printf("loop %u: %u frames decoded, %u errors\n", loop + 1, decoded, errors);
    }
}
