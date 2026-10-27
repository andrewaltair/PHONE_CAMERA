#include "log.h"

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>

#include <mutex>
#include <string>

namespace mycam {

namespace {
std::mutex g_lock;
FILE* g_file = nullptr;
} // namespace

void LogInit() {
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    std::wstring dir = std::wstring(base) + L"\\MyCam";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::lock_guard<std::mutex> lock(g_lock);
    g_file = _wfsopen((dir + L"\\mycam.log").c_str(), L"w", _SH_DENYWR);
}

void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_lock);
    if (!g_file) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_file, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfprintf(g_file, fmt, args);
    va_end(args);
    fputc('\n', g_file);
    fflush(g_file);
}

} // namespace mycam
