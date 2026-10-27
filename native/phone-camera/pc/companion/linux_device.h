#pragma once

#include <string>
#include <string_view>

namespace mycam {

struct LinuxCameraSelection {
    std::string path;
    std::string card;
    bool explicitOverride = false;
};

struct LinuxCameraPolicy {
    bool explicitOverride = false;
    std::string requiredCard = "PHONE_CAMERA";
};

// The automatic path may use only a loopback node that advertises the
// product-owned card label. An explicit --device is an owner-granted escape
// hatch for a pre-existing/shared loopback setup and is still V4L2-validated.
bool LinuxCameraPolicyAllows(std::string_view card, bool explicitOverride);

// FrameWriter uses this policy again on every write-side open. The default is
// fail-closed automatic selection, so tools that forget to configure a policy
// cannot write to an arbitrary video node.
void SetLinuxCameraPolicy(bool explicitOverride, std::string_view requiredCard);
LinuxCameraPolicy CurrentLinuxCameraPolicy();

// Query capabilities/card on the already-open write FD. This must run before
// VIDIOC_S_FMT and must not reopen the path, closing the replacement race
// between path preflight and the actual writer.
bool ValidateLinuxCameraFd(int fd, const LinuxCameraPolicy& policy, std::string* card, std::string* why);

// Resolves and validates the camera before PhoneLink starts. With no explicit
// path, exactly one /dev/video* node labelled PHONE_CAMERA must exist. With an
// explicit path, the caller owns the assignment and the node only needs to be
// a writable V4L2 video-output device.
bool ResolveLinuxCamera(const std::string& requested, bool explicitOverride, LinuxCameraSelection* selection,
                       std::string* why);

} // namespace mycam
