#include <windows.h>

#include "../companion/instance_ipc.h"

namespace {

bool g_received = false;

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == mycam::kOpenSettingsMessage) {
        g_received = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int wmain() {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    constexpr wchar_t kClassName[] = L"PHONE_CAMERAInstanceIpcTest";
    WNDCLASSW klass = {};
    klass.lpfnWndProc = WindowProc;
    klass.hInstance = instance;
    klass.lpszClassName = kClassName;
    if (!RegisterClassW(&klass)) return 1;
    HWND hwnd = CreateWindowExW(0, kClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    if (!hwnd) {
        UnregisterClassW(kClassName, instance);
        return 1;
    }
    const bool queued = mycam::RequestSettingsWindow(hwnd);
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    DestroyWindow(hwnd);
    UnregisterClassW(kClassName, instance);
    return queued && g_received ? 0 : 1;
}
