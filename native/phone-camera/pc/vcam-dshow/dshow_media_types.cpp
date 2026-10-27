#include "dshow_media_types.h"

#include <string.h>

namespace mycam {

namespace {

constexpr LONGLONG kSecond = 10'000'000; // 100 ns units
constexpr DWORD kFourCCNV12 = 0x3231564E; // 'N','V','1','2' packed as a BITMAPINFOHEADER biCompression FOURCC.

void FillVideoInfo(const DShowFormat& format, VIDEOINFOHEADER* info) {
    ZeroMemory(info, sizeof(*info));
    info->AvgTimePerFrame = kSecond / format.fps;
    info->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info->bmiHeader.biWidth = LONG(format.width);
    info->bmiHeader.biHeight = LONG(format.height);
    info->bmiHeader.biPlanes = 1;
    info->bmiHeader.biBitCount = 12; // NV12: 8 bpp Y + 4 bpp averaged chroma.
    info->bmiHeader.biCompression = kFourCCNV12;
    info->bmiHeader.biSizeImage = FrameBytes(format.width, format.height);
}

} // namespace

AM_MEDIA_TYPE* NewMediaType(const DShowFormat& format) {
    auto* mt = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
    if (!mt) return nullptr;
    ZeroMemory(mt, sizeof(*mt));
    mt->majortype = MEDIATYPE_Video;
    mt->subtype = kSubtypeNV12;
    mt->bFixedSizeSamples = TRUE;
    mt->bTemporalCompression = FALSE;
    mt->lSampleSize = FrameBytes(format.width, format.height);
    mt->formattype = FORMAT_VideoInfo;
    mt->cbFormat = sizeof(VIDEOINFOHEADER);
    mt->pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(sizeof(VIDEOINFOHEADER)));
    if (!mt->pbFormat) {
        CoTaskMemFree(mt);
        return nullptr;
    }
    FillVideoInfo(format, reinterpret_cast<VIDEOINFOHEADER*>(mt->pbFormat));
    return mt;
}

void FreeMediaType(AM_MEDIA_TYPE* mt) {
    if (!mt) return;
    if (mt->pbFormat) {
        CoTaskMemFree(mt->pbFormat);
        mt->pbFormat = nullptr;
    }
    mt->cbFormat = 0;
    if (mt->pUnk) {
        mt->pUnk->Release();
        mt->pUnk = nullptr;
    }
}

bool CopyMediaTypeInto(const AM_MEDIA_TYPE& src, AM_MEDIA_TYPE* dst) {
    ZeroMemory(dst, sizeof(*dst));
    *dst = src;
    dst->pbFormat = nullptr;
    dst->pUnk = nullptr;
    if (src.cbFormat && src.pbFormat) {
        dst->pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(src.cbFormat));
        if (!dst->pbFormat) return false;
        memcpy(dst->pbFormat, src.pbFormat, src.cbFormat);
    }
    if (src.pUnk) src.pUnk->AddRef();
    return true;
}

bool MatchesKnownFormat(const AM_MEDIA_TYPE& mt, DShowFormat* out) {
    if (mt.majortype != MEDIATYPE_Video || mt.subtype != kSubtypeNV12 || mt.formattype != FORMAT_VideoInfo) {
        return false;
    }
    if (!mt.pbFormat || mt.cbFormat < sizeof(VIDEOINFOHEADER)) return false;
    const auto* info = reinterpret_cast<const VIDEOINFOHEADER*>(mt.pbFormat);
    const UINT32 width = UINT32(info->bmiHeader.biWidth);
    const UINT32 height = UINT32(labs(info->bmiHeader.biHeight));
    const UINT32 fps = info->AvgTimePerFrame > 0 ? UINT32(kSecond / info->AvgTimePerFrame) : 0;
    for (const auto& format : kDShowFormats) {
        if (format.width == width && format.height == height && format.fps == fps) {
            if (out) *out = format;
            return true;
        }
    }
    return false;
}

void FillStreamCaps(const DShowFormat& format, VIDEO_STREAM_CONFIG_CAPS* caps) {
    ZeroMemory(caps, sizeof(*caps));
    caps->guid = FORMAT_VideoInfo;
    caps->VideoStandard = AnalogVideo_None;
    caps->InputSize.cx = caps->MinCroppingSize.cx = caps->MaxCroppingSize.cx = LONG(format.width);
    caps->InputSize.cy = caps->MinCroppingSize.cy = caps->MaxCroppingSize.cy = LONG(format.height);
    caps->CropGranularityX = caps->CropGranularityY = 1;
    caps->CropAlignX = caps->CropAlignY = 1;
    caps->MinOutputSize = caps->MaxOutputSize = caps->MinCroppingSize;
    caps->OutputGranularityX = caps->OutputGranularityY = 1;
    caps->MinBitsPerSecond = caps->MaxBitsPerSecond =
        LONG(FrameBytes(format.width, format.height)) * 8 * LONG(format.fps);
    caps->MinFrameInterval = caps->MaxFrameInterval = kSecond / format.fps;
}

} // namespace mycam
