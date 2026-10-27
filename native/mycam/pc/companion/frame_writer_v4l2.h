#pragma once
// The Linux FrameWriter: the same Write/WritePlanes as frame_writer.h (Windows shared memory), into a
// v4l2loopback device (/dev/videoN) instead. Apps (browsers, OBS, Zoom) read it as a webcam.

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace mycam {

class FrameWriter {
public:
    // The device and camera size every writer uses (the daemon's --device and --size). Writers created
    // afterwards open it by themselves on first use, and retry every 2 s while it is missing, like the Windows
    // writer waiting for the camera's shared memory.
    static void Configure(const std::string& device, uint32_t width, uint32_t height);

    FrameWriter() = default;
    FrameWriter(const FrameWriter&) = delete;
    FrameWriter& operator=(const FrameWriter&) = delete;
    ~FrameWriter();

    // Opens the device and fixes the camera's format: I420 at width x height (even). The size never changes
    // while the device is open, since apps don't follow a format change mid-stream. Phone frames of any size or
    // orientation are fitted into it. False (with *why) if the device can't be opened or refuses the format.
    bool Open(const std::string& device, uint32_t width, uint32_t height, std::string* why);
    void Close();
    bool IsOpen() const { return fd_ >= 0; }

    // True while an app has the camera open: the phone's camera only runs while someone watches.
    // v4l2loopback keeps no reader count, so this counts the other processes holding the device open
    // (/proc/*/fd), rescanned only when inotify sees the node opened or closed. That is exact, so unlike the
    // Windows camera (which only learns of a reader when it asks for a frame) there is no `withinMs` window
    // on top: the session loop's stop grace alone covers an app closing and reopening the camera.
    bool ConsumerActive(uint64_t /*withinMs*/);
    void SetPhoneState(uint32_t /*state*/) {} // The Windows camera draws its own waiting picture; nothing here.

    void Write(const uint8_t* nv12, uint32_t width, uint32_t height, uint32_t rotation, bool mirror, bool fill = false);
    // Same, from separate Y and UV planes with a row pitch (decoder output).
    void WritePlanes(const uint8_t* y, const uint8_t* uv, ptrdiff_t pitch, uint32_t width, uint32_t height,
                     uint32_t rotation, bool mirror, bool fill);

private:
    bool EnsureOpen();
    void PollReaders(uint64_t now);
    int CountReaders() const;

    int fd_ = -1;
    std::string device_;
    int inotify_ = -1;          // Watches device_ for opens and closes.
    int readers_ = -1;          // Other processes holding it open (-1: not counted yet).
    uint64_t lastScan_ = 0;
    uint64_t lastAttempt_ = 0;
    uint32_t width_ = 0, height_ = 0;
    std::vector<uint8_t> packed_; // Decoder planes repacked tightly (DrawFittedNV12 takes packed NV12).
    std::vector<uint8_t> fitted_; // The camera-sized NV12 frame.
    std::vector<uint8_t> out_;    // ...as I420, what goes to the device.
};

} // namespace mycam
