#pragma once
#include <guiddef.h>

namespace mycam {

// COM class of the virtual camera media source (MyCamVCam.dll). Registered by install.ps1 via regsvr32.
#define MYCAM_VCAM_CLSID_STRING L"{7B3C1E2A-9F4D-4C8B-A6E5-2D1F0B9C8A71}"
constexpr GUID CLSID_MyCamVCam = {0x7b3c1e2a, 0x9f4d, 0x4c8b, {0xa6, 0xe5, 0x2d, 0x1f, 0x0b, 0x9c, 0x8a, 0x71}};

constexpr wchar_t kCameraFriendlyName[] = L"MyCam";

// COM class of the Windows 10 virtual camera DirectShow source filter (MyCamVCamDShow.dll). Registered
// by the installer with regsvr32 (see docs/WINDOWS10-SUPPORT.md); reads the same Global\MyCamFrame
// shared memory as CLSID_MyCamVCam above.
#define MYCAM_VCAM_DSHOW_CLSID_STRING L"{9E1D7B3F-2C6A-4E8D-9B47-5A6C8D1E4F2B}"
constexpr GUID CLSID_MyCamVCamDShow = {0x9e1d7b3f, 0x2c6a, 0x4e8d, {0x9b, 0x47, 0x5a, 0x6c, 0x8d, 0x1e, 0x4f, 0x2b}};

} // namespace mycam
