#include "frame_writer_v4l2.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../vcam/frame_transform.h"
#include "log.h"
#include "platform.h"

namespace mycam {

namespace {
std::string g_device;
uint32_t g_width = 0, g_height = 0;
} // namespace

void FrameWriter::Configure(const std::string& device, uint32_t width, uint32_t height) {
    g_device = device;
    g_width = width;
    g_height = height;
}

bool FrameWriter::EnsureOpen() {
    if (fd_ >= 0) return true;
    const uint64_t now = NowMs();
    if (g_device.empty() || (lastAttempt_ && now - lastAttempt_ < 2000)) return false;
    const bool first = lastAttempt_ == 0;
    lastAttempt_ = now;
    std::string why;
    if (Open(g_device, g_width, g_height, &why)) {
        Log("camera: writing %ux%u to %s", g_width, g_height, g_device.c_str());
        return true;
    }
    if (first) Log("camera: %s (retrying every 2 s)", why.c_str());
    return false;
}

FrameWriter::~FrameWriter() {
    Close();
    if (inotify_ >= 0) close(inotify_);
}

bool FrameWriter::ConsumerActive(uint64_t) {
    if (!EnsureOpen()) return false;
    PollReaders(NowMs());
    return readers_ > 0;
}

void FrameWriter::PollReaders(uint64_t now) {
    if (inotify_ < 0) {
        inotify_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (inotify_ >= 0 && inotify_add_watch(inotify_, device_.c_str(), IN_OPEN | IN_CLOSE) < 0) {
            close(inotify_);
            inotify_ = -1;
        }
    }
    bool changed = readers_ < 0 || inotify_ < 0; // Without inotify, the safety-net rescan below is all there is.
    alignas(inotify_event) char buf[4096];
    while (inotify_ >= 0 && read(inotify_, buf, sizeof(buf)) > 0) changed = true;
    // A scan costs ~15 ms (every fd of every process), so only after an open or close, plus a slow safety net
    // in case an event was missed (the queue overflowed, or a reader was already there).
    if (!changed && now - lastScan_ < 10000) return;
    lastScan_ = now;
    const int count = CountReaders();
    if (count != readers_) {
        if (count > 0) Log("camera: %d app%s watching", count, count == 1 ? "" : "s");
        else if (readers_ > 0) Log("camera: no app watching");
    }
    readers_ = count;
}

int FrameWriter::CountReaders() const {
    struct stat dev;
    if (stat(device_.c_str(), &dev) != 0) return 0;
    const std::string self = std::to_string(getpid());
    int count = 0;
    DIR* proc = opendir("/proc");
    if (!proc) return 0;
    while (dirent* p = readdir(proc)) {
        if (p->d_name[0] < '1' || p->d_name[0] > '9' || self == p->d_name) continue; // Processes, not us.
        const std::string fdDir = std::string("/proc/") + p->d_name + "/fd";
        DIR* fds = opendir(fdDir.c_str()); // Other users' processes are unreadable: apps run as this user.
        if (!fds) continue;
        bool holds = false;
        while (dirent* f = readdir(fds)) {
            struct stat st;
            if (f->d_name[0] != '.' && stat((fdDir + "/" + f->d_name).c_str(), &st) == 0 && S_ISCHR(st.st_mode) &&
                st.st_rdev == dev.st_rdev) {
                holds = true;
                break;
            }
        }
        closedir(fds);
        count += holds;
    }
    closedir(proc);
    return count;
}

bool FrameWriter::Open(const std::string& device, uint32_t width, uint32_t height, std::string* why) {
    Close();
    if (width == 0 || height == 0 || width % 2 || height % 2) { *why = "size must be even and non-zero"; return false; }
    fd_ = open(device.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd_ < 0) { *why = device + ": " + strerror(errno); return false; }

    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    fmt.fmt.pix.bytesperline = width;
    fmt.fmt.pix.sizeimage = width * height * 3 / 2;
    fmt.fmt.pix.colorspace = V4L2_COLORSPACE_SMPTE170M; // BT.601 limited range, what the phone's video decodes to.
    if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
        *why = device + ": not a v4l2loopback output (" + strerror(errno) + ")";
        Close();
        return false;
    }
    if (fmt.fmt.pix.width != width || fmt.fmt.pix.height != height || fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUV420) {
        *why = device + ": refused " + std::to_string(width) + "x" + std::to_string(height) + " I420 (already in use?)";
        Close();
        return false;
    }
    width_ = width;
    height_ = height;
    device_ = device;
    fitted_.resize(size_t(width) * height * 3 / 2);
    out_.resize(fitted_.size());
    return true;
}

void FrameWriter::Close() {
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
}

void FrameWriter::Write(const uint8_t* nv12, uint32_t width, uint32_t height, uint32_t rotation, bool mirror, bool fill) {
    if (fd_ < 0 && !EnsureOpen()) return;
    if (width == 0 || height == 0) return;
    FillBlackNV12(fitted_.data(), width_, width_, height_);
    DrawFittedNV12(nv12, width, height, rotation, mirror, fitted_.data(), width_, width_, height_, fill);
    Nv12ToI420(fitted_.data(), width_, height_, out_.data());
    // One write() is one frame. A failed or short write (no reader keeping up) just drops this frame.
    if (write(fd_, out_.data(), out_.size()) < 0 && errno != EAGAIN) {
        // The device went away (module unloaded): stop writing until reopened.
        if (errno == ENODEV || errno == EBADF) Close();
    }
}

void FrameWriter::WritePlanes(const uint8_t* y, const uint8_t* uv, ptrdiff_t pitch, uint32_t width, uint32_t height,
                              uint32_t rotation, bool mirror, bool fill) {
    if (pitch == ptrdiff_t(width) && uv == y + size_t(width) * height) {
        Write(y, width, height, rotation, mirror, fill); // Already packed and contiguous (the FFmpeg decoder's output).
        return;
    }
    packed_.resize(size_t(width) * height * 3 / 2);
    for (uint32_t r = 0; r < height; ++r) memcpy(packed_.data() + size_t(r) * width, y + r * pitch, width);
    uint8_t* dstUv = packed_.data() + size_t(width) * height;
    for (uint32_t r = 0; r < height / 2; ++r) memcpy(dstUv + size_t(r) * width, uv + r * pitch, width);
    Write(packed_.data(), width, height, rotation, mirror, fill);
}

} // namespace mycam
