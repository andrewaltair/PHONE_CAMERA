#include "app_settings.h"

#include <shellapi.h>

#include <string>

namespace mycam {

namespace {
constexpr wchar_t kSettingsKey[] = L"Software\\MyCam";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"MyCam";
} // namespace

DWORD ReadSetting(const wchar_t* name, DWORD fallback) {
    DWORD value = fallback, size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value;
}

void WriteSetting(const wchar_t* name, DWORD value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_DWORD, &value, sizeof(value));
}

bool AutostartEnabled() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

void SetAutostart(bool enable) {
    if (enable) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(path) + L"\"";
        RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, cmd.c_str(), DWORD((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        HKEY key;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
            RegDeleteValueW(key, kRunValue);
            RegCloseKey(key);
        }
    }
}

void OpenLogFolder() {
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    std::wstring dir = std::wstring(base) + L"\\MyCam";
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

} // namespace mycam
