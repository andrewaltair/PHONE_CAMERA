// MyCamVCam.dll: COM server for the MyCam virtual camera media source.
// Frame Server creates CLSID_MyCamVCam as an IMFActivate and activates the media source from it.

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <wrl.h>
#include <wrl/module.h>

#include <string>

#include "../common/guids.h"
#include "media_source.h"

using namespace Microsoft::WRL;

namespace mycam {

class DECLSPEC_UUID("7B3C1E2A-9F4D-4C8B-A6E5-2D1F0B9C8A71") Activator
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ChainInterfaces<IMFActivate, IMFAttributes>, FtmBase> {
public:
    HRESULT RuntimeClassInitialize() { return MFCreateAttributes(&attrs_, 1); }

    // IMFActivate
    IFACEMETHODIMP ActivateObject(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        ComPtr<MediaSource> source;
        HRESULT hr = MakeAndInitialize<MediaSource>(&source, attrs_.Get());
        if (FAILED(hr)) return hr;
        hr = source->QueryInterface(riid, object);
        if (SUCCEEDED(hr)) source_ = source;
        return hr;
    }
    IFACEMETHODIMP ShutdownObject() override {
        if (source_) source_->Shutdown();
        source_.Reset();
        return S_OK;
    }
    IFACEMETHODIMP DetachObject() override {
        source_.Reset();
        return S_OK;
    }

    // IMFAttributes: delegate everything to attrs_.
    IFACEMETHODIMP GetItem(REFGUID k, PROPVARIANT* v) override { return attrs_->GetItem(k, v); }
    IFACEMETHODIMP GetItemType(REFGUID k, MF_ATTRIBUTE_TYPE* t) override { return attrs_->GetItemType(k, t); }
    IFACEMETHODIMP CompareItem(REFGUID k, REFPROPVARIANT v, BOOL* r) override { return attrs_->CompareItem(k, v, r); }
    IFACEMETHODIMP Compare(IMFAttributes* a, MF_ATTRIBUTES_MATCH_TYPE m, BOOL* r) override { return attrs_->Compare(a, m, r); }
    IFACEMETHODIMP GetUINT32(REFGUID k, UINT32* v) override { return attrs_->GetUINT32(k, v); }
    IFACEMETHODIMP GetUINT64(REFGUID k, UINT64* v) override { return attrs_->GetUINT64(k, v); }
    IFACEMETHODIMP GetDouble(REFGUID k, double* v) override { return attrs_->GetDouble(k, v); }
    IFACEMETHODIMP GetGUID(REFGUID k, GUID* v) override { return attrs_->GetGUID(k, v); }
    IFACEMETHODIMP GetStringLength(REFGUID k, UINT32* l) override { return attrs_->GetStringLength(k, l); }
    IFACEMETHODIMP GetString(REFGUID k, LPWSTR v, UINT32 s, UINT32* l) override { return attrs_->GetString(k, v, s, l); }
    IFACEMETHODIMP GetAllocatedString(REFGUID k, LPWSTR* v, UINT32* l) override { return attrs_->GetAllocatedString(k, v, l); }
    IFACEMETHODIMP GetBlobSize(REFGUID k, UINT32* s) override { return attrs_->GetBlobSize(k, s); }
    IFACEMETHODIMP GetBlob(REFGUID k, UINT8* b, UINT32 s, UINT32* l) override { return attrs_->GetBlob(k, b, s, l); }
    IFACEMETHODIMP GetAllocatedBlob(REFGUID k, UINT8** b, UINT32* s) override { return attrs_->GetAllocatedBlob(k, b, s); }
    IFACEMETHODIMP GetUnknown(REFGUID k, REFIID r, LPVOID* v) override { return attrs_->GetUnknown(k, r, v); }
    IFACEMETHODIMP SetItem(REFGUID k, REFPROPVARIANT v) override { return attrs_->SetItem(k, v); }
    IFACEMETHODIMP DeleteItem(REFGUID k) override { return attrs_->DeleteItem(k); }
    IFACEMETHODIMP DeleteAllItems() override { return attrs_->DeleteAllItems(); }
    IFACEMETHODIMP SetUINT32(REFGUID k, UINT32 v) override { return attrs_->SetUINT32(k, v); }
    IFACEMETHODIMP SetUINT64(REFGUID k, UINT64 v) override { return attrs_->SetUINT64(k, v); }
    IFACEMETHODIMP SetDouble(REFGUID k, double v) override { return attrs_->SetDouble(k, v); }
    IFACEMETHODIMP SetGUID(REFGUID k, REFGUID v) override { return attrs_->SetGUID(k, v); }
    IFACEMETHODIMP SetString(REFGUID k, LPCWSTR v) override { return attrs_->SetString(k, v); }
    IFACEMETHODIMP SetBlob(REFGUID k, const UINT8* b, UINT32 s) override { return attrs_->SetBlob(k, b, s); }
    IFACEMETHODIMP SetUnknown(REFGUID k, IUnknown* v) override { return attrs_->SetUnknown(k, v); }
    IFACEMETHODIMP LockStore() override { return attrs_->LockStore(); }
    IFACEMETHODIMP UnlockStore() override { return attrs_->UnlockStore(); }
    IFACEMETHODIMP GetCount(UINT32* c) override { return attrs_->GetCount(c); }
    IFACEMETHODIMP GetItemByIndex(UINT32 i, GUID* k, PROPVARIANT* v) override { return attrs_->GetItemByIndex(i, k, v); }
    IFACEMETHODIMP CopyAllItems(IMFAttributes* d) override { return attrs_->CopyAllItems(d); }

private:
    ComPtr<IMFAttributes> attrs_;
    ComPtr<MediaSource> source_;
};

CoCreatableClass(Activator);

} // namespace mycam

namespace {

HMODULE g_module = nullptr;

std::wstring ClsidKey() { return std::wstring(L"Software\\Classes\\CLSID\\") + MYCAM_VCAM_CLSID_STRING; }

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
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

// Registers under HKLM (Frame Server runs as a service and does not see per-user registrations).
STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(g_module, path, MAX_PATH)) return HRESULT_FROM_WIN32(GetLastError());

    std::wstring key = ClsidKey() + L"\\InprocServer32";
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
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, ClsidKey().c_str(), 0, KEY_WRITE, &clsidKey) == ERROR_SUCCESS) {
        const wchar_t name[] = L"MyCam Virtual Camera";
        RegSetValueExW(clsidKey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(name), sizeof(name));
        RegCloseKey(clsidKey);
    }
    return S_OK;
}

STDAPI DllUnregisterServer() {
    LSTATUS st = RegDeleteTreeW(HKEY_LOCAL_MACHINE, ClsidKey().c_str());
    return (st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND) ? S_OK : HRESULT_FROM_WIN32(st);
}
