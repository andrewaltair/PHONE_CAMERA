#pragma once
// Per-user settings (HKCU\Software\MyCam) and the "Start with Windows" Run entry.

#include <windows.h>

namespace mycam {

DWORD ReadSetting(const wchar_t* name, DWORD fallback);
void WriteSetting(const wchar_t* name, DWORD value);

bool AutostartEnabled();
void SetAutostart(bool enable);

// Opens %LOCALAPPDATA%\MyCam (where mycam.log lives) in Explorer.
void OpenLogFolder();

} // namespace mycam
