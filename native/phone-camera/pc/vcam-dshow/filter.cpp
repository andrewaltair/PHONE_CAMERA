#include "filter.h"

#include <algorithm>

#include "dshow_log.h"

namespace mycam {

namespace {

// Single-pin IEnumPins; the filter has exactly one output pin, so Next/Skip/Reset are trivial.
class PinEnumerator : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IEnumPins, FtmBase> {
public:
    HRESULT RuntimeClassInitialize(IPin* pin) {
        pin_ = pin;
        return S_OK;
    }

    IFACEMETHODIMP Next(ULONG count, IPin** pins, ULONG* fetched) override {
        if (!pins) return E_POINTER;
        ULONG n = 0;
        if (index_ == 0 && count > 0) {
            pin_->AddRef();
            pins[0] = pin_.Get();
            n = 1;
            index_ = 1;
        }
        if (fetched) *fetched = n;
        return n == count ? S_OK : S_FALSE;
    }
    IFACEMETHODIMP Skip(ULONG count) override {
        if (count == 0) return S_OK;
        bool hadRoom = index_ < 1;
        index_ = std::min<ULONG>(index_ + count, 1);
        return (hadRoom && count == 1) ? S_OK : S_FALSE;
    }
    IFACEMETHODIMP Reset() override {
        index_ = 0;
        return S_OK;
    }
    IFACEMETHODIMP Clone(IEnumPins** out) override {
        if (!out) return E_POINTER;
        ComPtr<PinEnumerator> clone;
        HRESULT hr = Microsoft::WRL::MakeAndInitialize<PinEnumerator>(&clone, pin_.Get());
        if (SUCCEEDED(hr)) {
            clone->index_ = index_;
            hr = clone.CopyTo(out);
        }
        return hr;
    }

private:
    ComPtr<IPin> pin_;
    ULONG index_ = 0;
};

} // namespace

HRESULT Filter::RuntimeClassInitialize() {
    DShowLog("Filter: created");
    return Microsoft::WRL::MakeAndInitialize<OutputPin>(&pin_, this);
}

IFACEMETHODIMP Filter::GetClassID(CLSID* clsid) {
    if (!clsid) return E_POINTER;
    *clsid = CLSID_PhoneCameraVCamDShow;
    return S_OK;
}

IFACEMETHODIMP Filter::Stop() {
    std::lock_guard<std::recursive_mutex> lock(lock_);
    DShowLog("Filter::Stop (was state %d)", int(state_));
    if (state_ != State_Stopped) pin_->Deactivate();
    state_ = State_Stopped;
    return S_OK;
}

IFACEMETHODIMP Filter::Pause() {
    std::lock_guard<std::recursive_mutex> lock(lock_);
    DShowLog("Filter::Pause (was state %d)", int(state_));
    if (state_ == State_Stopped) {
        HRESULT hr = pin_->Activate();
        DShowLog("Filter::Pause -> pin Activate() = 0x%08X", unsigned(hr));
        if (FAILED(hr)) return hr;
    }
    state_ = State_Paused;
    pin_->SetDeliver(false);
    return S_OK;
}

IFACEMETHODIMP Filter::Run(REFERENCE_TIME) {
    std::lock_guard<std::recursive_mutex> lock(lock_);
    DShowLog("Filter::Run (was state %d)", int(state_));
    if (state_ == State_Stopped) {
        HRESULT hr = pin_->Activate();
        DShowLog("Filter::Run -> pin Activate() = 0x%08X", unsigned(hr));
        if (FAILED(hr)) return hr;
    }
    state_ = State_Running;
    pin_->SetDeliver(true);
    return S_OK;
}

IFACEMETHODIMP Filter::GetState(DWORD, FILTER_STATE* state) {
    if (!state) return E_POINTER;
    std::lock_guard<std::recursive_mutex> lock(lock_);
    *state = state_;
    return S_OK;
}

IFACEMETHODIMP Filter::SetSyncSource(IReferenceClock* clock) {
    std::lock_guard<std::recursive_mutex> lock(lock_);
    clock_ = clock;
    return S_OK;
}

IFACEMETHODIMP Filter::GetSyncSource(IReferenceClock** clock) {
    if (!clock) return E_POINTER;
    std::lock_guard<std::recursive_mutex> lock(lock_);
    return clock_.CopyTo(clock);
}

IFACEMETHODIMP Filter::EnumPins(IEnumPins** enumerator) {
    if (!enumerator) return E_POINTER;
    ComPtr<PinEnumerator> e;
    HRESULT hr = Microsoft::WRL::MakeAndInitialize<PinEnumerator>(&e, static_cast<IPin*>(pin_.Get()));
    if (FAILED(hr)) return hr;
    return e.CopyTo(enumerator);
}

IFACEMETHODIMP Filter::FindPin(LPCWSTR id, IPin** pin) {
    if (!id || !pin) return E_POINTER;
    if (wcscmp(id, L"Capture") != 0) {
        *pin = nullptr;
        return VFW_E_NOT_FOUND;
    }
    return pin_.CopyTo(pin);
}

IFACEMETHODIMP Filter::QueryFilterInfo(FILTER_INFO* info) {
    if (!info) return E_POINTER;
    std::lock_guard<std::recursive_mutex> lock(lock_);
    wcsncpy_s(info->achName, name_, _TRUNCATE);
    info->pGraph = graph_;
    if (graph_) graph_->AddRef();
    return S_OK;
}

IFACEMETHODIMP Filter::JoinFilterGraph(IFilterGraph* graph, LPCWSTR name) {
    std::lock_guard<std::recursive_mutex> lock(lock_);
    DShowLog("Filter::JoinFilterGraph(%ls)", graph ? L"<graph>" : L"<leaving graph>");
    graph_ = graph; // Not AddRef'd: filters must not keep the graph alive.
    if (name) {
        wcsncpy_s(name_, name, _TRUNCATE);
    } else {
        name_[0] = L'\0';
    }
    return S_OK;
}

IFACEMETHODIMP Filter::QueryVendorInfo(LPWSTR*) { return E_NOTIMPL; }

IFACEMETHODIMP_(ULONG) Filter::GetMiscFlags() { return AM_FILTER_MISC_FLAGS_IS_SOURCE; }

} // namespace mycam
