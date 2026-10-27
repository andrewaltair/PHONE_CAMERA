#pragma once
// The DirectShow source filter itself: COM/graph plumbing (IBaseFilter, IMediaFilter) delegated mostly
// to the single OutputPin. Registered as a Video Capture Source so Zoom/Discord/OBS/Chromium list
// "PHONE_CAMERA" as a camera on Windows 10, where the Frame Server media source (pc/vcam/) does not exist.
// See docs/WINDOWS10-SUPPORT.md.

#include <windows.h>
#include <dshow.h>
#include <wrl.h>
#include <wrl/client.h>

#include <mutex>

#include "../common/guids.h"
#include "output_pin.h"

namespace mycam {

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::FtmBase;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::ChainInterfaces;

class DECLSPEC_UUID("F1CAA121-F8A7-4DFB-BEAB-1DED6A7887EA") Filter
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ChainInterfaces<IBaseFilter, IMediaFilter, IPersist>,
                          IAMFilterMiscFlags, FtmBase> {
public:
    HRESULT RuntimeClassInitialize();

    // IPersist
    IFACEMETHODIMP GetClassID(CLSID* clsid) override;

    // IMediaFilter
    IFACEMETHODIMP Stop() override;
    IFACEMETHODIMP Pause() override;
    IFACEMETHODIMP Run(REFERENCE_TIME start) override;
    IFACEMETHODIMP GetState(DWORD timeoutMs, FILTER_STATE* state) override;
    IFACEMETHODIMP SetSyncSource(IReferenceClock* clock) override;
    IFACEMETHODIMP GetSyncSource(IReferenceClock** clock) override;

    // IBaseFilter
    IFACEMETHODIMP EnumPins(IEnumPins** enumerator) override;
    IFACEMETHODIMP FindPin(LPCWSTR id, IPin** pin) override;
    IFACEMETHODIMP QueryFilterInfo(FILTER_INFO* info) override;
    IFACEMETHODIMP JoinFilterGraph(IFilterGraph* graph, LPCWSTR name) override;
    IFACEMETHODIMP QueryVendorInfo(LPWSTR* vendorInfo) override;

    // IAMFilterMiscFlags
    IFACEMETHODIMP_(ULONG) GetMiscFlags() override;

private:
    std::recursive_mutex lock_;
    FILTER_STATE state_ = State_Stopped;
    ComPtr<IReferenceClock> clock_;
    IFilterGraph* graph_ = nullptr; // Weak: filters must not hold a reference to their own graph.
    wchar_t name_[128] = L"";
    ComPtr<OutputPin> pin_;
};

} // namespace mycam
