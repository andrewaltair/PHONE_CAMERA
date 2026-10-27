#include "linux_device.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

namespace mycam {

namespace {

constexpr char kProductCard[] = "PHONE_CAMERA";
std::mutex g_policyLock;
LinuxCameraPolicy g_policy;

std::string CardText(const v4l2_capability& caps) {
    const char* card = reinterpret_cast<const char*>(caps.card);
    return std::string(card, strnlen(card, sizeof(caps.card)));
}

bool IsVideoNodeName(const char* name) {
    if (strncmp(name, "video", 5) != 0 || !name[5]) return false;
    for (const char* p = name + 5; *p; ++p)
        if (*p < '0' || *p > '9') return false;
    return true;
}

bool QueryFd(int fd, std::string* card, std::string* why) {
    v4l2_capability caps = {};
    const bool queried = ioctl(fd, VIDIOC_QUERYCAP, &caps) == 0;
    const int error = errno;
    if (!queried) {
        *why = std::string("not a V4L2 device (") + strerror(error) + ")";
        return false;
    }
    const uint32_t capabilities = caps.device_caps ? caps.device_caps : caps.capabilities;
    if ((capabilities & V4L2_CAP_VIDEO_OUTPUT) == 0) {
        *why = "V4L2 device is not a video-output loopback";
        return false;
    }
    *card = CardText(caps);
    return true;
}

bool QueryNode(const std::string& path, std::string* card, std::string* why) {
    struct stat st = {};
    if (stat(path.c_str(), &st) != 0) {
        *why = path + ": " + strerror(errno);
        return false;
    }
    if (!S_ISCHR(st.st_mode)) {
        *why = path + ": not a character device";
        return false;
    }
    // Match FrameWriter's write-side access. A node that can only be read is
    // not a usable output camera for this daemon.
    const int fd = open(path.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        *why = path + ": cannot inspect V4L2 capabilities (" + strerror(errno) + ")";
        return false;
    }
    const bool ok = QueryFd(fd, card, why);
    close(fd);
    if (!ok) *why = path + ": " + *why;
    return ok;
}

} // namespace

bool LinuxCameraPolicyAllows(std::string_view card, bool explicitOverride) {
    return explicitOverride || card == kProductCard;
}

void SetLinuxCameraPolicy(bool explicitOverride, std::string_view requiredCard) {
    std::lock_guard<std::mutex> lock(g_policyLock);
    g_policy.explicitOverride = explicitOverride;
    g_policy.requiredCard = requiredCard.empty() ? kProductCard : std::string(requiredCard);
}

LinuxCameraPolicy CurrentLinuxCameraPolicy() {
    std::lock_guard<std::mutex> lock(g_policyLock);
    return g_policy;
}

bool ValidateLinuxCameraFd(int fd, const LinuxCameraPolicy& policy, std::string* card, std::string* why) {
    if (fd < 0 || !card || !why) return false;
    card->clear();
    why->clear();
    if (!QueryFd(fd, card, why)) return false;
    if (!policy.explicitOverride && *card != policy.requiredCard) {
        *why = "V4L2 card label is " + (*card == "" ? std::string("empty") : *card) + "; expected " +
               policy.requiredCard;
        return false;
    }
    return true;
}

bool ResolveLinuxCamera(const std::string& requested, bool explicitOverride, LinuxCameraSelection* selection,
                       std::string* why) {
    if (!selection || !why) return false;
    *selection = {};
    why->clear();
    if (explicitOverride) {
        if (requested.empty()) {
            *why = "--device was supplied without a path";
            return false;
        }
        std::string card;
        if (!QueryNode(requested, &card, why)) return false;
        selection->path = requested;
        selection->card = std::move(card);
        selection->explicitOverride = true;
        return LinuxCameraPolicyAllows(selection->card, true);
    }
    if (!requested.empty()) {
        *why = "internal error: an implicit camera path was provided";
        return false;
    }

    std::vector<LinuxCameraSelection> owned;
    DIR* dir = opendir("/dev");
    if (!dir) {
        *why = std::string("cannot scan /dev for a PHONE_CAMERA loopback: ") + strerror(errno);
        return false;
    }
    while (dirent* entry = readdir(dir)) {
        if (!IsVideoNodeName(entry->d_name)) continue;
        const std::string path = std::string("/dev/") + entry->d_name;
        std::string card, ignored;
        if (!QueryNode(path, &card, &ignored) || !LinuxCameraPolicyAllows(card, false)) continue;
        owned.push_back({path, std::move(card), false});
    }
    closedir(dir);
    std::sort(owned.begin(), owned.end(), [](const LinuxCameraSelection& a, const LinuxCameraSelection& b) {
        return a.path < b.path;
    });
    if (owned.size() == 1) {
        *selection = owned.front();
        return true;
    }
    if (owned.empty()) {
        *why = "no V4L2 video-output device is labelled PHONE_CAMERA; create an owner-approved loopback "
               "with that card label or pass --device /dev/videoN explicitly";
    } else {
        *why = "multiple V4L2 devices are labelled PHONE_CAMERA; pass --device /dev/videoN explicitly";
    }
    return false;
}

} // namespace mycam
