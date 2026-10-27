#include "frame_writer.h"

#include <sddl.h>
#include <string.h>

#include "../common/shared_frame.h"

namespace mycam {

FrameWriter::~FrameWriter() { Close(); }

void FrameWriter::Close() {
    if (view_) UnmapViewOfFile(view_);
    if (mapping_) CloseHandle(mapping_);
    view_ = nullptr;
    mapping_ = nullptr;
}

bool FrameWriter::EnsureMapping() {
    if (view_ && !onLocal_) return true;
    uint64_t now = GetTickCount64();
    if (now - lastAttempt_ < 500) return view_ != nullptr; // Don't hammer OpenFileMapping.
    lastAttempt_ = now;

    // The Frame Server camera (Windows 11) creates Global\MyCamFrame, but only while an app uses it. So even
    // after falling back to the session-local section below, keep looking for it: otherwise a companion that
    // started before any app opened the camera would write into a section nobody reads (a black camera, and
    // no app ever counted as using it).
    if (HANDLE global = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, kSharedFrameName)) {
        if (void* view = MapViewOfFile(global, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kSharedBytes)) {
            Close();
            mapping_ = global;
            view_ = view;
            onLocal_ = false;
            return true;
        }
        CloseHandle(global);
    }
    if (view_) return true; // Still only the local section (Windows 10's DirectShow camera reads that one).

    // Windows 10 (docs/WINDOWS10-SUPPORT.md): the DirectShow camera is hosted inside whatever
    // ordinary app opened it (not a privileged service like Frame Server), so neither it nor this
    // companion can create or see the Global\ section above (see frame_reader.cpp). Create/open the
    // session-local fallback instead — unlike Global\, either side can create this one, so it
    // doesn't matter which one gets there first.
    PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, FALSE};
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(kSharedSddl, SDDL_REVISION_1, &sd, nullptr)) {
        sa.lpSecurityDescriptor = sd;
    }
    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, sd ? &sa : nullptr, PAGE_READWRITE, 0, kSharedBytes,
                                  kSharedFrameNameLocal);
    if (!mapping_) mapping_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, kSharedFrameNameLocal);
    if (sd) LocalFree(sd);
    if (!mapping_) return false;
    view_ = MapViewOfFile(mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kSharedBytes);
    if (!view_) {
        Close();
        return false;
    }
    onLocal_ = true;
    return true;
}

bool FrameWriter::ConsumerActive(uint64_t withinMs) {
    if (!EnsureMapping()) return false;
    auto* hdr = static_cast<SharedHeader*>(view_);
    if (hdr->magic != kSharedMagic) return false;
    uint64_t tick = hdr->consumerTick;
    return tick != 0 && GetTickCount64() - tick < withinMs;
}

void FrameWriter::SetPhoneState(uint32_t state) {
    if (!EnsureMapping()) return;
    static_cast<SharedHeader*>(view_)->phoneState = state;
}

void FrameWriter::Write(const uint8_t* nv12, uint32_t width, uint32_t height, uint32_t rotation, bool mirror, bool fill) {
    WritePlanes(nv12, nv12 + size_t(width) * height, width, width, height, rotation, mirror, fill);
}

void FrameWriter::WritePlanes(const uint8_t* y, const uint8_t* uv, ptrdiff_t pitch, uint32_t width, uint32_t height,
                              uint32_t rotation, bool mirror, bool fill) {
    if (width > kMaxWidth || height > kMaxHeight || !EnsureMapping()) return;
    auto* hdr = static_cast<SharedHeader*>(view_);
    if (hdr->magic != kSharedMagic) return;

    InterlockedIncrement(&hdr->seq); // Odd: writing.
    hdr->width = width;
    hdr->height = height;
    hdr->rotation = rotation;
    hdr->mirror = mirror ? 1 : 0;
    hdr->fill = fill ? 1 : 0;
    uint8_t* dst = FrameData(view_);
    if (pitch == ptrdiff_t(width)) {
        memcpy(dst, y, size_t(width) * height);
        memcpy(dst + size_t(width) * height, uv, size_t(width) * height / 2);
    } else {
        for (uint32_t r = 0; r < height; ++r) memcpy(dst + size_t(r) * width, y + r * pitch, width);
        uint8_t* dstUV = dst + size_t(width) * height;
        for (uint32_t r = 0; r < height / 2; ++r) memcpy(dstUV + size_t(r) * width, uv + r * pitch, width);
    }
    hdr->frameTick = GetTickCount64();
    InterlockedIncrement(&hdr->seq); // Even: stable.
}

} // namespace mycam
