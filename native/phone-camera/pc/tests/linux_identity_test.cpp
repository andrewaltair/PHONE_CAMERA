#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <string>

#include "../companion/linux_identity.h"

int main() {
    using namespace mycam;
    int failures = 0;
    if (std::string(kLinuxProductName) != "PHONE_CAMERA") ++failures;
    if (std::string(kLinuxPackageName) != "phone-camera") ++failures;
    if (std::string(kLinuxDaemonName) != "phone-camera-companion") ++failures;
    if (std::string(kLinuxTrayName) != "phone-camera-tray") ++failures;
    if (std::string(kLinuxServiceName) != "phone-camera.service") ++failures;
    if (std::string(kLinuxSocketName) != "phone-camera.sock") ++failures;

    char root[] = "/tmp/phone-camera-identity-XXXXXX";
    if (!mkdtemp(root)) return 1;
    setenv("XDG_CONFIG_HOME", root, 1);
    setenv("XDG_RUNTIME_DIR", root, 1);
    unsetenv("PHONE_CAMERA_CONFIG_HOME");
    unsetenv("PHONE_CAMERA_RUNTIME_DIR");

    const std::string config = std::string(root) + "/PHONE_CAMERA";
    const std::string runtime = std::string(root) + "/phone-camera";
    if (LinuxConfigDir() != config || LinuxRuntimeDir() != runtime ||
        LinuxControlSocketPath() != runtime + "/phone-camera.sock")
        ++failures;
    if (!PrepareLinuxIdentity()) {
        ++failures;
    } else {
        struct stat st = {};
        if (stat(config.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || (st.st_mode & 077) != 0) ++failures;
        if (stat(runtime.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || (st.st_mode & 077) != 0) ++failures;
        if (std::string(getenv("XDG_CONFIG_HOME")) != config || std::string(getenv("XDG_RUNTIME_DIR")) != root)
            ++failures;
    }
    rmdir(config.c_str());
    rmdir(runtime.c_str());
    rmdir(root);
    return failures ? 1 : 0;
}
