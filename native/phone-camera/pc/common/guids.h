#pragma once
#include <guiddef.h>

namespace mycam {

// PHONE_CAMERA owns these process, storage, camera and certificate identities.  The wire protocol and
// the internal namespace remain MyCam-compatible so existing Android/iPhone clients keep working.
constexpr wchar_t kProductName[] = L"PHONE_CAMERA";
constexpr wchar_t kCompanionWindowClass[] = L"PHONE_CAMERACompanion";
constexpr wchar_t kCompanionMutexName[] = L"Local\\PHONE_CAMERACompanion";
constexpr wchar_t kSettingsKey[] = L"Software\\PHONE_CAMERA";
constexpr wchar_t kRunValue[] = L"PHONE_CAMERA";
constexpr wchar_t kLocalDataDirectory[] = L"PHONE_CAMERA";
constexpr wchar_t kCompanionLogFile[] = L"PHONE_CAMERA.log";
constexpr wchar_t kDShowLogFile[] = L"PHONE_CAMERA-dshow.log";
constexpr wchar_t kDriverTaskName[] = L"PHONE_CAMERA phone driver";
constexpr wchar_t kFirewallRuleName[] = L"PHONE_CAMERA iPhone camera";
constexpr wchar_t kCertificateFriendlyName[] = L"PHONE_CAMERA web camera";

// COM class of the virtual camera media source (MyCamVCam.dll). Registered by install.ps1 via regsvr32.
#define PHONE_CAMERA_VCAM_CLSID_STRING L"{A43186E6-3C6E-4FC2-8E62-7193B5AA0FF3}"
constexpr GUID CLSID_PhoneCameraVCam = {0xa43186e6, 0x3c6e, 0x4fc2, {0x8e, 0x62, 0x71, 0x93, 0xb5, 0xaa, 0x0f, 0xf3}};

constexpr wchar_t kCameraFriendlyName[] = L"PHONE_CAMERA";

// COM class of the Windows 10 virtual camera DirectShow source filter (MyCamVCamDShow.dll). Registered
// by the installer with regsvr32 (see docs/WINDOWS10-SUPPORT.md); reads the same Global\PHONE_CAMERAFrame
// shared memory as CLSID_PhoneCameraVCam above.
#define PHONE_CAMERA_VCAM_DSHOW_CLSID_STRING L"{F1CAA121-F8A7-4DFB-BEAB-1DED6A7887EA}"
constexpr GUID CLSID_PhoneCameraVCamDShow = {0xf1caa121, 0xf8a7, 0x4dfb, {0xbe, 0xab, 0x1d, 0xed, 0x6a, 0x78, 0x87, 0xea}};

} // namespace mycam
