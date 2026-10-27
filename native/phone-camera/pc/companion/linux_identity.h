#pragma once

// Product-owned Linux identity and per-user runtime namespace.  The shared
// MyCam-derived POSIX code still reads XDG_CONFIG_HOME; this seam gives that
// code a PHONE_CAMERA-owned parent without changing the desktop session's
// XDG_RUNTIME_DIR or the upstream wire protocol.

#include <string>

namespace mycam {

constexpr char kLinuxProductName[] = "PHONE_CAMERA";
constexpr char kLinuxPackageName[] = "phone-camera";
constexpr char kLinuxDaemonName[] = "phone-camera-companion";
constexpr char kLinuxTrayName[] = "phone-camera-tray";
constexpr char kLinuxServiceName[] = "phone-camera.service";
constexpr char kLinuxRuntimeNamespace[] = "phone-camera";
constexpr char kLinuxConfigNamespace[] = "PHONE_CAMERA";
constexpr char kLinuxSocketName[] = "phone-camera.sock";

// Sets product-owned XDG_CONFIG_HOME and XDG_RUNTIME_DIR namespaces and
// creates them with user-only permissions.  It is idempotent within a
// process.  Returns false when the process has no usable HOME/XDG roots.
bool PrepareLinuxIdentity();

// Resolved product-owned paths.  These do not mutate the environment.
std::string LinuxConfigDir();
std::string LinuxRuntimeDir();
std::string LinuxControlSocketPath();

} // namespace mycam
