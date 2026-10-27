// MyCamVCamDShow.dll: the Windows 10 fallback for pc/vcam/'s Frame Server camera (see
// docs/WINDOWS10-SUPPORT.md). Registers CLSID_MyCamVCamDShow both as a plain COM server and, through
// IFilterMapper2, as a DirectShow Video Capture Source so legacy and modern capture APIs alike list
// "MyCam" as a camera.

#include <windows.h>
#include <dshow.h>
#include <wrl.h>
#include <wrl/module.h>

#include <string>

#include "dshow_media_types.h"
#include "filter.h"

using namespace Microsoft::WRL;

namespace mycam {

CoCreatableClass(Filter);

namespace {

HMODULE g_module = nullptr;

std::wstring ClsidKey() { return std::wstring(L"Software\\Classes\\CLSID\\") + MYCAM_VCAM_DSHOW_CLSID_STRING; }

// Filter Mapper "instance" string under CLSID_VideoInputDeviceCategory\Instance\<...>. Reusing our own
// CLSID string keeps Register/Unregister symmetric without round-tripping a generated moniker.
constexpr wchar_t kInstanceString[] = MYCAM_VCAM_DSHOW_CLSID_STRING;

HRESULT RegisterWithFilterMapper() {
    ComPtr<IFilterMapper2> mapper;
    HRESULT hr = CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mapper));
    if (FAILED(hr)) return hr;

    REGPINTYPES pinType{&MEDIATYPE_Video, &kSubtypeNV12};
    REGFILTERPINS2 pin{};
    pin.dwFlags = REG_PINFLAG_B_OUTPUT;
    pin.cInstances = 1;
    pin.nMediaTypes = 1;
    pin.lpMediaType = &pinType;
    pin.clsPinCategory = &PIN_CATEGORY_CAPTURE;

    REGFILTER2 filter{};
    filter.dwVersion = 2; // REGFILTERPINS2 (needed for clsPinCategory), not the older REGFILTERPINS.
    filter.dwMerit = MERIT_DO_NOT_USE + 1; // Listed by name, but never auto-picked for a generic "any video source".
    filter.cPins2 = 1;
    filter.rgPins2 = &pin;

    return mapper->RegisterFilter(CLSID_MyCamVCamDShow, kCameraFriendlyName, nullptr, &CLSID_VideoInputDeviceCategory,
                                  kInstanceString, &filter);
}

void UnregisterFromFilterMapper() {
    ComPtr<IFilterMapper2> mapper;
    if (SUCCEEDED(CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mapper)))) {
        mapper->UnregisterFilter(&CLSID_VideoInputDeviceCategory, kInstanceString, CLSID_MyCamVCamDShow);
    }
}

} // namespace

} // namespace mycam

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        mycam::g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* object) {
    return Module<InProc>::GetModule().GetClassObject(clsid, riid, object);
}

STDAPI DllCanUnloadNow() {
    return Module<InProc>::GetModule().Terminate() ? S_OK : S_FALSE;
}

// Registers under HKLM, same as MyCamVCam.dll: regsvr32 for this filter always runs elevated (the
// installer, or an admin uninstall step), and a per-machine camera should not depend on which user
// happens to register it.
STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(mycam::g_module, path, MAX_PATH)) return HRESULT_FROM_WIN32(GetLastError());

    std::wstring key = mycam::ClsidKey() + L"\\InprocServer32";
    HKEY hkey;
    LSTATUS st = RegCreateKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &hkey, nullptr);
    if (st != ERROR_SUCCESS) return HRESULT_FROM_WIN32(st);
    st = RegSetValueExW(hkey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(path),
                        DWORD((wcslen(path) + 1) * sizeof(wchar_t)));
    const wchar_t model[] = L"Both";
    if (st == ERROR_SUCCESS) {
        st = RegSetValueExW(hkey, L"ThreadingModel", 0, REG_SZ, reinterpret_cast<const BYTE*>(model), sizeof(model));
    }
    RegCloseKey(hkey);
    if (st != ERROR_SUCCESS) return HRESULT_FROM_WIN32(st);

    HKEY clsidKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, mycam::ClsidKey().c_str(), 0, KEY_WRITE, &clsidKey) == ERROR_SUCCESS) {
        const wchar_t name[] = L"MyCam Virtual Camera (DirectShow)";
        RegSetValueExW(clsidKey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(name), sizeof(name));
        RegCloseKey(clsidKey);
    }

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HRESULT hr = mycam::RegisterWithFilterMapper();
    if (SUCCEEDED(hrCo)) CoUninitialize();
    return hr;
}

STDAPI DllUnregisterServer() {
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    mycam::UnregisterFromFilterMapper();
    if (SUCCEEDED(hrCo)) CoUninitialize();

    LSTATUS st = RegDeleteTreeW(HKEY_LOCAL_MACHINE, mycam::ClsidKey().c_str());
    return (st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND) ? S_OK : HRESULT_FROM_WIN32(st);
}
