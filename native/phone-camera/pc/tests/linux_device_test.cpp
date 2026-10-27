#include <fcntl.h>
#include <string>
#include <unistd.h>

#include "../companion/linux_device.h"

int main() {
    using namespace mycam;
    if (!LinuxCameraPolicyAllows("PHONE_CAMERA", false)) return 1;
    if (LinuxCameraPolicyAllows("MyCam", false)) return 1;
    if (LinuxCameraPolicyAllows("", false)) return 1;
    if (!LinuxCameraPolicyAllows("MyCam", true)) return 1;
    if (!LinuxCameraPolicyAllows("", true)) return 1;

    SetLinuxCameraPolicy(false, "PHONE_CAMERA");
    LinuxCameraPolicy policy = CurrentLinuxCameraPolicy();
    if (policy.explicitOverride || policy.requiredCard != "PHONE_CAMERA") return 1;
    SetLinuxCameraPolicy(true, "MyCam");
    policy = CurrentLinuxCameraPolicy();
    if (!policy.explicitOverride || policy.requiredCard != "MyCam") return 1;

    LinuxCameraSelection selection;
    std::string why;
    SetLinuxCameraPolicy(false, "PHONE_CAMERA");
    if (ResolveLinuxCamera("", true, &selection, &why)) return 1;
    if (why.find("--device") == std::string::npos) return 1;
    if (ResolveLinuxCamera("/dev/null", true, &selection, &why)) return 1;
    if (why.find("V4L2") == std::string::npos) return 1;
    const int fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (fd < 0) return 1;
    std::string card;
    if (ValidateLinuxCameraFd(fd, CurrentLinuxCameraPolicy(), &card, &why)) return 1;
    close(fd);
    return 0;
}
