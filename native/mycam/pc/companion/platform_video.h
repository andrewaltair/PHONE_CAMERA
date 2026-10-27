#pragma once
// The decoder and the camera writer for this platform. Both sides have the same H264Decoder and FrameWriter
// interfaces: Media Foundation and the Frame Server camera's shared memory on Windows, FFmpeg and
// v4l2loopback on Linux.

#ifdef _WIN32
#include "decoder.h"
#include "frame_writer.h"
#else
#include "decoder_ffmpeg.h"
#include "frame_writer_v4l2.h"
#endif
