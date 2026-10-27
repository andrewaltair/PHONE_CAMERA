#include "dshow_log.h"

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>

#include <string>

#include "../common/guids.h"

namespace mycam {

void DShowLog(const char* fmt, ...) {
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    std::wstring dir = std::wstring(base) + L"\\" + kLocalDataDirectory;
    CreateDirectoryW(dir.c_str(), nullptr);
    FILE* file = _wfsopen((dir + L"\\" + kDShowLogFile).c_str(), L"a", _SH_DENYWR);
    if (!file) return;

    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(file, "%02d:%02d:%02d.%03d [pid %lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            GetCurrentProcessId());
    va_list args;
    va_start(args, fmt);
    vfprintf(file, fmt, args);
    va_end(args);
    fputc('\n', file);
    fclose(file);
}

} // namespace mycam
