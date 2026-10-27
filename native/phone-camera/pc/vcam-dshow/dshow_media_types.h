#pragma once
// AM_MEDIA_TYPE helpers for the DirectShow source filter. No BaseClasses: Windows no longer ships
// strmbase.lib, so these replace the handful of helpers (NewMediaType/FreeMediaType/CopyMediaType)
// CBasePin would otherwise provide.

#include <windows.h>
#include <dshow.h>

namespace mycam {

// Same formats as pc/vcam/media_source.cpp's kFormats, offered through DirectShow instead of Media
// Foundation. Keep the two lists in sync if the phone's capability set changes.
struct DShowFormat { UINT32 width, height, fps; };
constexpr DShowFormat kDShowFormats[] = {
    {1920, 1080, 30}, {1280, 720, 30}, {3840, 2160, 30}, {1920, 1080, 60}, {1280, 720, 60}, {1920, 1080, 120},
    {1280, 720, 120}, {640, 480, 30}, {640, 360, 30},
};

// NV12, {3231564E-0000-0010-8000-00AA00389B71} — the standard FOURCC-derived DirectShow subtype GUID,
// same pixel layout pc/vcam/frame_reader.cpp already renders (tightly packed, Y plane then UV).
constexpr GUID kSubtypeNV12 = {0x3231564e, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

inline DWORD FrameBytes(UINT32 width, UINT32 height) { return width * height * 3 / 2; }

// Allocates a new AM_MEDIA_TYPE (CoTaskMemAlloc'd, including its VIDEOINFOHEADER payload) for a format.
// Caller frees it with FreeMediaType then CoTaskMemFree, or hands ownership to DirectShow.
AM_MEDIA_TYPE* NewMediaType(const DShowFormat& format);

// Frees the internals of a media type (pbFormat, pUnk) without freeing the AM_MEDIA_TYPE struct itself,
// since it is sometimes a member variable rather than a heap pointer.
void FreeMediaType(AM_MEDIA_TYPE* mt);

// Deep-copies src into *dst (zeroing *dst first), allocating a fresh pbFormat block.
bool CopyMediaTypeInto(const AM_MEDIA_TYPE& src, AM_MEDIA_TYPE* dst);

// Matches mt against kDShowFormats by major/subtype/formattype and the embedded width/height/fps.
bool MatchesKnownFormat(const AM_MEDIA_TYPE& mt, DShowFormat* out);

void FillStreamCaps(const DShowFormat& format, VIDEO_STREAM_CONFIG_CAPS* caps);

} // namespace mycam
