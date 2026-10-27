#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <dshow.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>

#include "../common/guids.h"

using Microsoft::WRL::ComPtr;

namespace {

using DllGetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);

bool Check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

bool TestMfClassFactory(const wchar_t* path) {
    HMODULE module = LoadLibraryW(path);
    if (!Check(module != nullptr, "LoadLibrary(MF virtual camera)")) return false;
    auto getClassObject = reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(module, "DllGetClassObject"));
    bool ok = Check(getClassObject != nullptr, "MF DllGetClassObject export");
    ComPtr<IClassFactory> factory;
    if (ok) ok = Check(SUCCEEDED(getClassObject(mycam::CLSID_PhoneCameraVCam, IID_PPV_ARGS(&factory))),
                      "MF class factory lookup by PHONE_CAMERA CLSID");
    ComPtr<IMFActivate> activate;
    if (ok) ok = Check(SUCCEEDED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&activate))),
                      "MF class factory creates IMFActivate");
    if (ok) ok = Check(activate.Get() != nullptr, "MF activator is a valid IMFActivate");
    activate.Reset();
    factory.Reset();
    FreeLibrary(module);
    return ok;
}

bool TestDshowClassFactory(const wchar_t* path) {
    HMODULE module = LoadLibraryW(path);
    if (!Check(module != nullptr, "LoadLibrary(DirectShow virtual camera)")) return false;
    auto getClassObject = reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(module, "DllGetClassObject"));
    bool ok = Check(getClassObject != nullptr, "DirectShow DllGetClassObject export");
    ComPtr<IClassFactory> factory;
    if (ok) ok = Check(SUCCEEDED(getClassObject(mycam::CLSID_PhoneCameraVCamDShow, IID_PPV_ARGS(&factory))),
                      "DirectShow class factory lookup by PHONE_CAMERA CLSID");
    ComPtr<IBaseFilter> filter;
    if (ok) ok = Check(SUCCEEDED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&filter))),
                      "DirectShow class factory creates IBaseFilter");
    if (ok) {
        ComPtr<IPersist> persist;
        ok = Check(SUCCEEDED(filter.As(&persist)), "DirectShow filter exposes IPersist");
        CLSID classId = {};
        if (ok) ok = Check(SUCCEEDED(persist->GetClassID(&classId)) && classId == mycam::CLSID_PhoneCameraVCamDShow,
                           "DirectShow filter class ID matches PHONE_CAMERA CLSID");
    }
    filter.Reset();
    factory.Reset();
    FreeLibrary(module);
    return ok;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (!Check(argc == 3, "expected MF and DirectShow DLL paths")) return 1;
    HRESULT mf = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (!Check(SUCCEEDED(mf), "MFStartup")) return 1;
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (!Check(SUCCEEDED(co) || co == RPC_E_CHANGED_MODE, "COM initialization")) {
        MFShutdown();
        return 1;
    }
    bool ok = TestMfClassFactory(argv[1]) && TestDshowClassFactory(argv[2]);
    if (SUCCEEDED(co)) CoUninitialize();
    MFShutdown();
    return ok ? 0 : 1;
}
