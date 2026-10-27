// Independent, non-privileged activation test for PHONE_CAMERA's two in-proc
// virtual-camera servers.
//
// This file deliberately does not include product headers or call any
// registration entry point.  It loads an explicitly supplied DLL, resolves
// DllGetClassObject, and asks that class factory for the fixed PHONE_CAMERA
// CLSIDs below.  The expected values are kept here so an old MyCam DLL cannot
// make this test pass by sharing stale product constants.

#include <windows.h>
#include <dshow.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <wrl/client.h>

#include <cstdio>
#include <cwchar>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

// A43186E6-3C6E-4FC2-8E62-7193B5AA0FF3
constexpr GUID kPhoneCameraMfClsid = {
    0xA43186E6, 0x3C6E, 0x4FC2, {0x8E, 0x62, 0x71, 0x93, 0xB5, 0xAA, 0x0F, 0xF3}};

// F1CAA121-F8A7-4DFB-BEAB-1DED6A7887EA
constexpr GUID kPhoneCameraDshowClsid = {
    0xF1CAA121, 0xF8A7, 0x4DFB, {0xBE, 0xAB, 0x1D, 0xED, 0x6A, 0x78, 0x87, 0xEA}};

// MEDIASUBTYPE_NV12 is the FOURCC-derived {3231564E-0000-0010-8000-00AA00389B71}.
constexpr GUID kNv12Subtype = {
    0x3231564E, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

using DllGetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);

struct Options {
    std::wstring mode = L"pass";
    std::wstring mfPath;
    std::wstring dshowPath;
};

struct LoadedServer {
    HMODULE module = nullptr;
    DllGetClassObjectFn getClassObject = nullptr;
    std::wstring label;
    std::wstring path;

    ~LoadedServer() {
        if (module) FreeLibrary(module);
    }

    bool Load(const wchar_t* requestedPath, const wchar_t* serverLabel) {
        label = serverLabel;
        path = requestedPath ? requestedPath : L"";
        module = LoadLibraryW(path.c_str());
        if (!module) {
            std::fwprintf(stderr, L"FAIL: LoadLibraryW(%ls) for %ls, win32=%lu\n", path.c_str(), label.c_str(),
                          GetLastError());
            return false;
        }

        getClassObject = reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(module, "DllGetClassObject"));
        if (!getClassObject) {
            std::fwprintf(stderr, L"FAIL: %ls has no DllGetClassObject export\n", path.c_str());
            return false;
        }
        std::wprintf(L"INFO: loaded %ls: %ls\n", label.c_str(), path.c_str());
        return true;
    }
};

bool ParseOptions(int argc, wchar_t** argv, Options* options) {
    if (!options) return false;
    for (int i = 1; i < argc; ++i) {
        const wchar_t* arg = argv[i];
        if (_wcsicmp(arg, L"--mode") == 0 && i + 1 < argc) {
            options->mode = argv[++i];
        } else if (_wcsicmp(arg, L"--mf") == 0 && i + 1 < argc) {
            options->mfPath = argv[++i];
        } else if (_wcsicmp(arg, L"--dshow") == 0 && i + 1 < argc) {
            options->dshowPath = argv[++i];
        } else {
            return false;
        }
    }
    return (options->mode == L"pass" || options->mode == L"red") && !options->mfPath.empty() &&
           !options->dshowPath.empty();
}

void PrintUsage() {
    std::fwprintf(stderr,
                  L"Usage: phone_camera_native_activation_test --mode pass|red --mf <MyCamVCam.dll> "
                  L"--dshow <MyCamVCamDShow.dll>\n");
}

bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

void PrintHr(const wchar_t* operation, HRESULT hr) {
    std::fwprintf(stderr, L"FAIL: %ls: HRESULT=0x%08lX\n", operation, static_cast<unsigned long>(hr));
}

bool IsNv12(const GUID& subtype) { return IsEqualGUID(subtype, kNv12Subtype); }

void FreeAmMediaType(AM_MEDIA_TYPE* mediaType) {
    if (!mediaType) return;
    if (mediaType->pUnk) mediaType->pUnk->Release();
    if (mediaType->pbFormat) CoTaskMemFree(mediaType->pbFormat);
    CoTaskMemFree(mediaType);
}

bool CheckMfClassFactoryIsUnavailable(const std::wstring& path) {
    LoadedServer server;
    if (!server.Load(path.c_str(), L"baseline MF virtual camera")) return false;

    ComPtr<IClassFactory> factory;
    const HRESULT hr = server.getClassObject(kPhoneCameraMfClsid, IID_IClassFactory,
                                              reinterpret_cast<void**>(factory.GetAddressOf()));
    if (SUCCEEDED(hr) || factory) {
        std::fwprintf(stderr, L"FAIL: baseline MF DLL accepted new PHONE_CAMERA CLSID (HRESULT=0x%08lX)\n",
                      static_cast<unsigned long>(hr));
        return false;
    }
    std::wprintf(L"RED: baseline MF class-factory lookup rejected new CLSID (HRESULT=0x%08lX)\n",
                 static_cast<unsigned long>(hr));
    return true;
}

bool CheckDshowClassFactoryIsUnavailable(const std::wstring& path) {
    LoadedServer server;
    if (!server.Load(path.c_str(), L"baseline DirectShow virtual camera")) return false;

    ComPtr<IClassFactory> factory;
    const HRESULT hr = server.getClassObject(kPhoneCameraDshowClsid, IID_IClassFactory,
                                              reinterpret_cast<void**>(factory.GetAddressOf()));
    if (SUCCEEDED(hr) || factory) {
        std::fwprintf(stderr, L"FAIL: baseline DirectShow DLL accepted new PHONE_CAMERA CLSID (HRESULT=0x%08lX)\n",
                      static_cast<unsigned long>(hr));
        return false;
    }
    std::wprintf(L"RED: baseline DirectShow class-factory lookup rejected new CLSID (HRESULT=0x%08lX)\n",
                 static_cast<unsigned long>(hr));
    return true;
}

bool CheckMfActivation(const std::wstring& path) {
    LoadedServer server;
    if (!server.Load(path.c_str(), L"PHONE_CAMERA MF virtual camera")) return false;

    bool ok = true;
    ComPtr<IClassFactory> factory;
    HRESULT hr = server.getClassObject(kPhoneCameraMfClsid, IID_IClassFactory,
                                       reinterpret_cast<void**>(factory.GetAddressOf()));
    if (FAILED(hr) || !factory) {
        PrintHr(L"MF DllGetClassObject(new PHONE_CAMERA CLSID, IID_IClassFactory)", hr);
        return false;
    }
    std::wprintf(L"PASS: MF class factory resolved new PHONE_CAMERA CLSID\n");

    ComPtr<IMFActivate> activate;
    hr = factory->CreateInstance(nullptr, IID_IMFActivate, reinterpret_cast<void**>(activate.GetAddressOf()));
    if (FAILED(hr) || !activate) {
        PrintHr(L"MF IClassFactory::CreateInstance(IID_IMFActivate)", hr);
        return false;
    }
    std::wprintf(L"PASS: MF class factory created IMFActivate\n");

    ComPtr<IMFMediaSource> source;
    hr = activate->ActivateObject(IID_IMFMediaSource, reinterpret_cast<void**>(source.GetAddressOf()));
    if (FAILED(hr) || !source) {
        PrintHr(L"IMFActivate::ActivateObject(IID_IMFMediaSource)", hr);
        activate->ShutdownObject();
        return false;
    }
    std::wprintf(L"PASS: IMFActivate created IMFMediaSource\n");

    ComPtr<IMFPresentationDescriptor> presentation;
    hr = source->CreatePresentationDescriptor(&presentation);
    if (FAILED(hr) || !presentation) {
        PrintHr(L"IMFMediaSource::CreatePresentationDescriptor", hr);
        source.Reset();
        activate->ShutdownObject();
        return false;
    }

    DWORD streamCount = 0;
    hr = presentation->GetStreamDescriptorCount(&streamCount);
    if (FAILED(hr) || streamCount == 0) {
        PrintHr(L"IMFPresentationDescriptor::GetStreamDescriptorCount", hr);
        ok = false;
    } else {
        bool foundVideoNv12 = false;
        bool foundSelectedVideoNv12 = false;
        for (DWORD index = 0; index < streamCount; ++index) {
            BOOL selected = FALSE;
            ComPtr<IMFStreamDescriptor> stream;
            hr = presentation->GetStreamDescriptorByIndex(index, &selected, &stream);
            if (FAILED(hr) || !stream) {
                PrintHr(L"IMFPresentationDescriptor::GetStreamDescriptorByIndex", hr);
                ok = false;
                break;
            }

            ComPtr<IMFMediaTypeHandler> handler;
            hr = stream->GetMediaTypeHandler(&handler);
            if (FAILED(hr) || !handler) {
                PrintHr(L"IMFStreamDescriptor::GetMediaTypeHandler", hr);
                ok = false;
                break;
            }

            GUID major = GUID_NULL;
            hr = handler->GetMajorType(&major);
            if (FAILED(hr)) {
                PrintHr(L"IMFMediaTypeHandler::GetMajorType", hr);
                ok = false;
                break;
            }

            DWORD typeCount = 0;
            hr = handler->GetMediaTypeCount(&typeCount);
            if (FAILED(hr) || typeCount == 0) {
                PrintHr(L"IMFMediaTypeHandler::GetMediaTypeCount", hr);
                ok = false;
                break;
            }

            for (DWORD typeIndex = 0; typeIndex < typeCount; ++typeIndex) {
                ComPtr<IMFMediaType> type;
                hr = handler->GetMediaTypeByIndex(typeIndex, &type);
                if (FAILED(hr) || !type) {
                    PrintHr(L"IMFMediaTypeHandler::GetMediaTypeByIndex", hr);
                    ok = false;
                    break;
                }
                GUID subtype = GUID_NULL;
                if (SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
                    IsEqualGUID(major, MFMediaType_Video) && IsNv12(subtype)) {
                    foundVideoNv12 = true;
                    if (selected) foundSelectedVideoNv12 = true;
                }
            }
            if (!ok) break;

            ComPtr<IMFMediaType> currentType;
            hr = handler->GetCurrentMediaType(&currentType);
            if (FAILED(hr) || !currentType) {
                PrintHr(L"IMFMediaTypeHandler::GetCurrentMediaType", hr);
                ok = false;
                break;
            }
            GUID currentMajor = GUID_NULL;
            GUID currentSubtype = GUID_NULL;
            if (FAILED(currentType->GetGUID(MF_MT_MAJOR_TYPE, &currentMajor)) ||
                FAILED(currentType->GetGUID(MF_MT_SUBTYPE, &currentSubtype)) ||
                !IsEqualGUID(currentMajor, MFMediaType_Video) || !IsNv12(currentSubtype)) {
                std::fwprintf(stderr, L"FAIL: current MF media type is not Video/NV12\n");
                ok = false;
                break;
            }
        }

        if (ok && !foundVideoNv12) {
            std::fwprintf(stderr, L"FAIL: presentation descriptor has no Video/NV12 media type\n");
            ok = false;
        }
        if (ok && !foundSelectedVideoNv12) {
            std::fwprintf(stderr, L"FAIL: no selected Video/NV12 stream in presentation descriptor\n");
            ok = false;
        }
        if (ok) {
            std::wprintf(L"PASS: IMFPresentationDescriptor exposes selected Video/NV12 stream\n");
        }
    }

    // Do not call Start/RequestSample: this test validates activation and
    // descriptors only.  Releasing the source first lets the activator own
    // and shut down its last reference without starting a delivery thread.
    presentation.Reset();
    source.Reset();
    const HRESULT shutdownHr = activate->ShutdownObject();
    if (FAILED(shutdownHr) && shutdownHr != MF_E_SHUTDOWN) {
        PrintHr(L"IMFActivate::ShutdownObject", shutdownHr);
        ok = false;
    }
    return ok;
}

bool CheckDshowActivation(const std::wstring& path) {
    LoadedServer server;
    if (!server.Load(path.c_str(), L"PHONE_CAMERA DirectShow virtual camera")) return false;

    bool ok = true;
    ComPtr<IClassFactory> factory;
    HRESULT hr = server.getClassObject(kPhoneCameraDshowClsid, IID_IClassFactory,
                                       reinterpret_cast<void**>(factory.GetAddressOf()));
    if (FAILED(hr) || !factory) {
        PrintHr(L"DirectShow DllGetClassObject(new PHONE_CAMERA CLSID, IID_IClassFactory)", hr);
        return false;
    }
    std::wprintf(L"PASS: DirectShow class factory resolved new PHONE_CAMERA CLSID\n");

    ComPtr<IBaseFilter> filter;
    hr = factory->CreateInstance(nullptr, IID_IBaseFilter, reinterpret_cast<void**>(filter.GetAddressOf()));
    if (FAILED(hr) || !filter) {
        PrintHr(L"DirectShow IClassFactory::CreateInstance(IID_IBaseFilter)", hr);
        return false;
    }
    std::wprintf(L"PASS: DirectShow class factory created IBaseFilter\n");

    ComPtr<IPersist> persist;
    hr = filter.As(&persist);
    if (FAILED(hr) || !persist) {
        PrintHr(L"IBaseFilter::QueryInterface(IID_IPersist)", hr);
        ok = false;
    } else {
        CLSID classId = CLSID_NULL;
        hr = persist->GetClassID(&classId);
        if (FAILED(hr) || !IsEqualGUID(classId, kPhoneCameraDshowClsid)) {
            PrintHr(L"IPersist::GetClassID(new PHONE_CAMERA CLSID)", hr);
            ok = false;
        } else {
            std::wprintf(L"PASS: IBaseFilter reports the new PHONE_CAMERA CLSID\n");
        }
    }

    ComPtr<IEnumPins> pins;
    if (ok) {
        hr = filter->EnumPins(&pins);
        if (FAILED(hr) || !pins) {
            PrintHr(L"IBaseFilter::EnumPins", hr);
            ok = false;
        }
    }

    ComPtr<IPin> pin;
    if (ok) {
        ULONG fetched = 0;
        hr = pins->Next(1, pin.GetAddressOf(), &fetched);
        if (FAILED(hr) || fetched != 1 || !pin) {
            PrintHr(L"IEnumPins::Next", hr);
            ok = false;
        } else {
            PIN_DIRECTION direction = PINDIR_INPUT;
            hr = pin->QueryDirection(&direction);
            if (FAILED(hr) || direction != PINDIR_OUTPUT) {
                PrintHr(L"IPin::QueryDirection(output)", hr);
                ok = false;
            }
        }
    }

    if (ok) {
        LPWSTR pinId = nullptr;
        hr = pin->QueryId(&pinId);
        if (FAILED(hr) || !pinId || _wcsicmp(pinId, L"Capture") != 0) {
            PrintHr(L"IPin::QueryId(Capture)", hr);
            ok = false;
        }
        if (pinId) CoTaskMemFree(pinId);
    }

    if (ok) {
        ComPtr<IEnumMediaTypes> mediaTypes;
        hr = pin->EnumMediaTypes(&mediaTypes);
        if (FAILED(hr) || !mediaTypes) {
            PrintHr(L"IPin::EnumMediaTypes", hr);
            ok = false;
        } else {
            AM_MEDIA_TYPE* mediaType = nullptr;
            ULONG fetched = 0;
            hr = mediaTypes->Next(1, &mediaType, &fetched);
            if (FAILED(hr) || fetched != 1 || !mediaType) {
                PrintHr(L"IEnumMediaTypes::Next", hr);
                ok = false;
            } else if (!IsEqualGUID(mediaType->majortype, MEDIATYPE_Video) ||
                       !IsNv12(mediaType->subtype)) {
                std::fwprintf(stderr, L"FAIL: DirectShow pin does not expose Video/NV12\n");
                ok = false;
            } else {
                std::wprintf(L"PASS: DirectShow output pin exposes Video/NV12\n");
            }
            FreeAmMediaType(mediaType);
        }
    }

    return ok;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    Options options;
    if (!ParseOptions(argc, argv, &options)) {
        PrintUsage();
        return 2;
    }

    if (!FileExists(options.mfPath) || !FileExists(options.dshowPath)) {
        std::fwprintf(stderr,
                      L"SKIP: supplied DLL path is missing; no activation result is claimed.\n  MF: %ls\n  "
                      L"DirectShow: %ls\n",
                      options.mfPath.c_str(), options.dshowPath.c_str());
        return 2;
    }

    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE) {
        PrintHr(L"CoInitializeEx(COINIT_MULTITHREADED)", comHr);
        return 1;
    }
    const bool ownsCom = SUCCEEDED(comHr);

    const HRESULT mfHr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(mfHr)) {
        PrintHr(L"MFStartup(MF_VERSION, MFSTARTUP_FULL)", mfHr);
        if (ownsCom) CoUninitialize();
        return 1;
    }

    std::wprintf(L"INFO: mode=%ls; COM/MF initialized; no registration entry point is called\n", options.mode.c_str());
    bool ok = false;
    if (options.mode == L"red") {
        const bool mfRed = CheckMfClassFactoryIsUnavailable(options.mfPath);
        const bool dshowRed = CheckDshowClassFactoryIsUnavailable(options.dshowPath);
        ok = mfRed && dshowRed;
        std::wprintf(ok ? L"EXPECTED RED: legacy DLLs reject both new PHONE_CAMERA CLSIDs\n"
                        : L"UNEXPECTED: baseline red check did not prove both CLSID mismatches\n");
    } else {
        const bool mfPass = CheckMfActivation(options.mfPath);
        const bool dshowPass = CheckDshowActivation(options.dshowPath);
        ok = mfPass && dshowPass;
        std::wprintf(ok ? L"GREEN: MF and DirectShow native activation checks passed\n"
                        : L"RED: one or more native activation checks failed\n");
    }

    MFShutdown();
    if (ownsCom) CoUninitialize();
    return ok ? 0 : 1;
}
