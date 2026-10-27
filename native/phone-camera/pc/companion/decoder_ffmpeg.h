#pragma once
// The Linux H264Decoder: the same interface as decoder.h (Media Foundation, Windows), on FFmpeg's libavcodec,
// linked dynamically against the distro's package (MyCam is MIT; libavcodec is LGPL/GPL). Results are 0 or a
// negative AVERROR, as the Windows one returns an HRESULT (also a long, negative on failure).

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <vector>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace mycam {

// Decodes H.264 Annex-B access units into tightly packed NV12 frames.
class H264Decoder {
public:
    // A decoded NV12 frame, valid only during the callback: Y and UV planes with a shared row pitch.
    struct Nv12View {
        const uint8_t* y;
        const uint8_t* uv;
        ptrdiff_t pitch;
        uint32_t width, height;
    };
    using FrameCallback = std::function<void(const Nv12View&)>;

    H264Decoder() = default;
    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;
    ~H264Decoder();

    // width/height are the phone's configured size; the stream's own SPS wins if it differs.
    // gpu is ignored: FFmpeg decodes on the CPU (the Windows decoder falls back to its CPU path).
    long Init(uint32_t width, uint32_t height, FrameCallback onFrame, bool gpu = true);
    void Reset();
    bool Ready() const { return ctx_ != nullptr; }
    bool OnGpu() const { return false; }

    // One access unit. Negative if the decoder rejected it (corrupt data); later keyframes recover.
    long Decode(const uint8_t* data, size_t size, int64_t ptsUs);

private:
    void EmitFrame();

    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    SwsContext* sws_ = nullptr;
    int swsFormat_ = -1, swsWidth_ = 0, swsHeight_ = 0; // What sws_ was made for.
    bool swsFull_ = false;
    FrameCallback onFrame_;
    std::vector<uint8_t> input_; // The access unit plus the zero padding libavcodec reads past the end.
    std::vector<uint8_t> nv12_;  // The current frame, converted from whatever libavcodec produced.
};

} // namespace mycam
