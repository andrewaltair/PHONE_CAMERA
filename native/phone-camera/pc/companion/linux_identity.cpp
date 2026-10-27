#include "linux_identity.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <string>

namespace mycam {

namespace {

std::string Env(const char* name) {
    const char* value = getenv(name);
    return value && *value ? value : std::string();
}

std::string ConfigRoot() {
    const std::string configured = Env("PHONE_CAMERA_CONFIG_HOME");
    if (!configured.empty()) return configured;
    const std::string base = Env("XDG_CONFIG_HOME");
    if (!base.empty()) return base + "/" + kLinuxConfigNamespace;
    const std::string home = Env("HOME");
    return home.empty() ? std::string() : home + "/.config/" + kLinuxConfigNamespace;
}

std::string RuntimeRoot() {
    const std::string configured = Env("PHONE_CAMERA_RUNTIME_DIR");
    if (!configured.empty()) return configured;
    const std::string base = Env("XDG_RUNTIME_DIR");
    return base.empty() ? std::string() : base + "/" + kLinuxRuntimeNamespace;
}

bool MakeDir(const std::string& path, mode_t mode) {
    if (path.empty()) return false;
    std::string current;
    if (path[0] == '/') current = "/";
    size_t start = path[0] == '/' ? 1 : 0;
    while (start <= path.size()) {
        const size_t slash = path.find('/', start);
        const std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!part.empty()) {
            if (!current.empty() && current.back() != '/') current += '/';
            current += part;
            if (mkdir(current.c_str(), mode) != 0 && errno != EEXIST) return false;
            // Only the product-owned leaf gets tightened. Never chmod a
            // shared parent such as /tmp or ~/.config.
            if (slash == std::string::npos && chmod(current.c_str(), mode) != 0) return false;
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

} // namespace

std::string LinuxConfigDir() { return ConfigRoot(); }

std::string LinuxRuntimeDir() { return RuntimeRoot(); }

std::string LinuxControlSocketPath() {
    const std::string dir = LinuxRuntimeDir();
    return dir.empty() ? std::string() : dir + "/" + kLinuxSocketName;
}

bool PrepareLinuxIdentity() {
    const std::string config = LinuxConfigDir();
    const std::string runtime = LinuxRuntimeDir();
    if (config.empty() || runtime.empty()) return false;
    if (!MakeDir(config, 0700) || !MakeDir(runtime, 0700)) return false;
    if (setenv("PHONE_CAMERA_CONFIG_HOME", config.c_str(), 1) != 0 ||
        setenv("PHONE_CAMERA_RUNTIME_DIR", runtime.c_str(), 1) != 0 ||
        // tls_server_openssl.cpp is an inherited seam; point its standard XDG
        // lookup at our already-isolated parent. The desktop's
        // XDG_RUNTIME_DIR stays unchanged because GTK/AppIndicator may use it
        // for the user's D-Bus and desktop sockets.
        setenv("XDG_CONFIG_HOME", config.c_str(), 1) != 0)
        return false;
    return true;
}

} // namespace mycam
