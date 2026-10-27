#pragma once
// Diagnostic log: %LOCALAPPDATA%\MyCam\mycam.log on Windows (restarted each launch), stderr on Linux
// (log_posix.cpp). Thread-safe.

namespace mycam {

void LogInit();
void Log(const char* fmt, ...);

} // namespace mycam
