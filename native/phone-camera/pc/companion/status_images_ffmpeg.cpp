#include "status_images.h"

// LoadStatusImage on Linux: the same PNGs (compiled in, web_assets.cpp) decoded by FFmpeg instead of WIC,
// then the same BgraToNV12 as Windows, so both cameras show identical pictures.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

#include "../vcam/frame_transform.h"
#include "platform.h"

namespace mycam {

Nv12Image LoadStatusImage(int resourceId) {
    Nv12Image image;
    const std::string png = ResourceData(resourceId);
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_PNG);
    if (png.empty() || !codec) return image;
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    std::vector<uint8_t> input(png.begin(), png.end());
    input.resize(png.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    if (ctx && packet && frame && avcodec_open2(ctx, codec, nullptr) == 0) {
        packet->data = input.data();
        packet->size = int(png.size());
        if (avcodec_send_packet(ctx, packet) == 0 && avcodec_receive_frame(ctx, frame) == 0) {
            const int w = frame->width & ~1, h = frame->height & ~1;
            SwsContext* sws = w && h ? sws_getContext(frame->width, frame->height, AVPixelFormat(frame->format), w, h,
                                                      AV_PIX_FMT_BGRA, SWS_POINT, nullptr, nullptr, nullptr)
                                     : nullptr;
            if (sws) {
                std::vector<uint8_t> bgra(size_t(w) * h * 4);
                uint8_t* planes[4] = {bgra.data(), nullptr, nullptr, nullptr};
                const int pitches[4] = {w * 4, 0, 0, 0};
                sws_scale(sws, frame->data, frame->linesize, 0, frame->height, planes, pitches);
                sws_freeContext(sws);
                image.width = uint32_t(w);
                image.height = uint32_t(h);
                image.data.resize(size_t(w) * h * 3 / 2);
                BgraToNV12(bgra.data(), w * 4, uint32_t(w), uint32_t(h), image.data.data());
            }
        }
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&ctx);
    return image;
}

} // namespace mycam
