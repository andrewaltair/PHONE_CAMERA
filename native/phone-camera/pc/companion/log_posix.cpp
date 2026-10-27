#include "log.h"

// Linux: the daemon logs to stderr, which the systemd journal (or the terminal) keeps.

#include <stdarg.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>

#include <mutex>

namespace mycam {

namespace {
std::mutex g_lock;
} // namespace

void LogInit() {}

void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_lock);
    timeval now;
    gettimeofday(&now, nullptr);
    tm t;
    localtime_r(&now.tv_sec, &t);
    fprintf(stderr, "%02d:%02d:%02d.%03d ", t.tm_hour, t.tm_min, t.tm_sec, int(now.tv_usec / 1000));
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

} // namespace mycam
