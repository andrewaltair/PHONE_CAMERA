#include "decoder_ffmpeg.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <string.h>

namespace mycam {

H264Decoder::~H264Decoder() { Reset(); }

long H264Decoder::Init(uint32_t width, uint32_t height, FrameCallback onFrame, bool) {
    Reset();
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) return AVERROR_DECODER_NOT_FOUND;
    ctx_ = avcodec_alloc_context3(codec);
    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    if (!ctx_ || !frame_ || !packet_) { Reset(); return AVERROR(ENOMEM); }
    ctx_->width = int(width);
    ctx_->height = int(height);
    // A live camera: output each frame as soon as it decodes. Frame threading would hold frames back to fill
    // its pipeline, so only slice threads.
    ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx_->thread_type = FF_THREAD_SLICE;
    ctx_->thread_count = 0; // Auto.
    if (const int r = avcodec_open2(ctx_, codec, nullptr); r < 0) { Reset(); return r; }
    onFrame_ = std::move(onFrame);
    return 0;
}

void H264Decoder::Reset() {
    sws_freeContext(sws_);
    sws_ = nullptr;
    av_packet_free(&packet_);
    av_frame_free(&frame_);
    avcodec_free_context(&ctx_);
    onFrame_ = nullptr;
}

long H264Decoder::Decode(const uint8_t* data, size_t size, int64_t ptsUs) {
    if (!ctx_ || size == 0) return AVERROR(EINVAL);
    input_.resize(size + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(input_.data(), data, size);
    memset(input_.data() + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    packet_->data = input_.data();
    packet_->size = int(size);
    packet_->pts = ptsUs;
    const int sent = avcodec_send_packet(ctx_, packet_);
    if (sent < 0 && sent != AVERROR(EAGAIN)) return sent;
    while (avcodec_receive_frame(ctx_, frame_) == 0) {
        EmitFrame();
        av_frame_unref(frame_);
    }
    return 0;
}

void H264Decoder::EmitFrame() {
    const int w = frame_->width & ~1, h = frame_->height & ~1; // NV12 needs even sizes.
    if (w <= 0 || h <= 0 || !onFrame_) return;
    // Software H.264 decodes to planar YUV 4:2:0; swscale interleaves it into NV12. A full-range stream (the
    // iPhone's) comes as the deprecated yuvj420p: name it yuv420p and say full range explicitly, or swscale
    // rebuilds its context on every frame. Either way the output is limited range, what the camera declares.
    AVPixelFormat format = AVPixelFormat(frame_->format);
    bool full = frame_->color_range == AVCOL_RANGE_JPEG;
    if (format == AV_PIX_FMT_YUVJ420P) { format = AV_PIX_FMT_YUV420P; full = true; }
    if (!sws_ || format != swsFormat_ || w != swsWidth_ || h != swsHeight_ || full != swsFull_) {
        sws_freeContext(sws_);
        sws_ = sws_getContext(w, h, format, w, h, AV_PIX_FMT_NV12, SWS_POINT, nullptr, nullptr, nullptr);
        if (!sws_) return;
        const int* bt601 = sws_getCoefficients(SWS_CS_ITU601);
        sws_setColorspaceDetails(sws_, bt601, full ? 1 : 0, bt601, 0, 0, 1 << 16, 1 << 16);
        swsFormat_ = format;
        swsWidth_ = w;
        swsHeight_ = h;
        swsFull_ = full;
    }
    nv12_.resize(size_t(w) * h * 3 / 2);
    uint8_t* planes[4] = {nv12_.data(), nv12_.data() + size_t(w) * h, nullptr, nullptr};
    const int pitches[4] = {w, w, 0, 0};
    sws_scale(sws_, frame_->data, frame_->linesize, 0, h, planes, pitches);
    onFrame_(Nv12View{planes[0], planes[1], w, uint32_t(w), uint32_t(h)});
}

} // namespace mycam
