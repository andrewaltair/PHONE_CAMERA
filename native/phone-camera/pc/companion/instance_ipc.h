#pragma once

#include <windows.h>

namespace mycam {

// Private message used only by PHONE_CAMERA.exe instances in this user's window station.
constexpr UINT kOpenSettingsMessage = WM_APP + 4;

inline bool RequestSettingsWindow(HWND hwnd) {
    return hwnd && IsWindow(hwnd) && PostMessageW(hwnd, kOpenSettingsMessage, 0, 0) != FALSE;
}

} // namespace mycam
