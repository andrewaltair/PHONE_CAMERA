#include "frame_reader.h"

#include <sddl.h>
#include <string.h>

#include "../common/shared_frame.h"
#include "frame_transform.h"

namespace mycam {

namespace {

constexpr uint64_t kStaleMs = 1500;

} // namespace

FrameReader::FrameReader() = default;

FrameReader::~FrameReader() {
    if (view_) UnmapViewOfFile(view_);
    if (mapping_) CloseHandle(mapping_);
}

bool FrameReader::EnsureMapping() {
    if (view_) return true;

    // Running inside the Frame Server service we can create Global objects; elsewhere just try to open.
    PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, FALSE};
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(kSharedSddl, SDDL_REVISION_1, &sd, nullptr)) {
        sa.lpSecurityDescriptor = sd;
    }
    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, sd ? &sa : nullptr, PAGE_READWRITE, 0, kSharedBytes,
                                  kSharedFrameName);
    if (!mapping_) mapping_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, kSharedFrameName);
    // Global\ needs SeCreateGlobalPrivilege to create — services such as Frame Server have it and create
    // it there on Windows 11, and this side can only open it if a privileged process got there first. On
    // Windows 10 the DirectShow filter runs inside whatever ordinary app opened the camera, and the
    // companion is an ordinary process too, both in the same session — neither can create or see a
    // Global section. Fall back to a session-local one that needs no special privilege (and that
    // pc/companion/frame_writer.cpp falls back to the same way).
    if (!mapping_) {
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, sd ? &sa : nullptr, PAGE_READWRITE, 0, kSharedBytes,
                                      kSharedFrameNameLocal);
    }
    if (!mapping_) mapping_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, kSharedFrameNameLocal);
    if (sd) LocalFree(sd);
    if (!mapping_) return false;

    view_ = MapViewOfFile(mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kSharedBytes);
    if (!view_) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
        return false;
    }
    auto* hdr = static_cast<SharedHeader*>(view_);
    if (hdr->magic != kSharedMagic) {
        // Fresh section: initialise the header. The companion never writes frames before magic is set.
        hdr->version = kSharedVersion;
        hdr->seq = 0;
        hdr->width = hdr->height = 0;
        MemoryBarrier();
        hdr->magic = kSharedMagic;
    }
    return true;
}

bool FrameReader::CopyLatest() {
    if (!EnsureMapping()) return false;
    auto* hdr = static_cast<SharedHeader*>(view_);
    hdr->consumerTick = GetTickCount64();

    for (int attempt = 0; attempt < 3; ++attempt) {
        LONG seq1 = hdr->seq;
        if (seq1 & 1) { YieldProcessor(); continue; }
        if (seq1 == lastSeq_) return true; // Nothing new; keep the frame we have.
        MemoryBarrier();
        UINT32 w = hdr->width, h = hdr->height;
        if (w == 0 || h == 0 || w > kMaxWidth || h > kMaxHeight || (w | h) & 1) return false;
        size_t bytes = size_t(w) * h * 3 / 2;
        if (frame_.size() < bytes) frame_.resize(bytes);
        memcpy(frame_.data(), FrameData(view_), bytes);
        UINT32 rot = hdr->rotation;
        bool mirror = hdr->mirror != 0;
        bool fill = hdr->fill != 0;
        uint64_t tick = hdr->frameTick;
        MemoryBarrier();
        if (hdr->seq != seq1) continue; // Torn read; retry.
        frameWidth_ = w;
        frameHeight_ = h;
        rotation_ = (rot == 90 || rot == 180 || rot == 270) ? rot : 0;
        mirror_ = mirror;
        fill_ = fill;
        frameTick_ = tick;
        lastSeq_ = seq1;
        return true;
    }
    return frameWidth_ != 0;
}

void FrameReader::Render(uint8_t* dst, LONG pitch, UINT32 width, UINT32 height) {
    bool have = CopyLatest() && frameWidth_ && GetTickCount64() - frameTick_ < kStaleMs;
    FillBlackNV12(dst, pitch, width, height);
    if (have) DrawFittedNV12(frame_.data(), frameWidth_, frameHeight_, rotation_, mirror_, dst, pitch, width, height, fill_);
}


bool FrameReader::HasNewFrame() {
    if (!EnsureMapping()) return false;
    const LONG seq = static_cast<SharedHeader*>(view_)->seq;
    return !(seq & 1) && seq != lastSeq_;
}

} // namespace mycam
