#pragma once
// Diagnostic log: %LOCALAPPDATA%\PHONE_CAMERA\PHONE_CAMERA.log on Windows (restarted each launch), stderr on Linux
// (log_posix.cpp). Thread-safe.

namespace mycam {

void LogInit();
void Log(const char* fmt, ...);

} // namespace mycam
