#include "output_pin.h"

#include <ks.h>
#include <ksmedia.h>
#include <ksproxy.h>

#include <algorithm>

#include "dshow_log.h"
#include "filter.h"

namespace mycam {

namespace {

// Enumerates the fixed format list through IPin::EnumMediaTypes; separate from IAMStreamConfig's
// GetStreamCaps, which most apps use instead, but some graph-building code still walks this.
class MediaTypeEnumerator : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IEnumMediaTypes, FtmBase> {
public:
    HRESULT RuntimeClassInitialize() { return S_OK; }

    IFACEMETHODIMP Next(ULONG count, AM_MEDIA_TYPE** types, ULONG* fetched) override {
        if (!types) return E_POINTER;
        ULONG n = 0;
        while (n < count && index_ < ARRAYSIZE(kDShowFormats)) {
            AM_MEDIA_TYPE* mt = NewMediaType(kDShowFormats[index_]);
            if (!mt) break;
            types[n++] = mt;
            ++index_;
        }
        if (fetched) *fetched = n;
        return n == count ? S_OK : S_FALSE;
    }
    IFACEMETHODIMP Skip(ULONG count) override {
        index_ = std::min<size_t>(index_ + count, ARRAYSIZE(kDShowFormats));
        return index_ < ARRAYSIZE(kDShowFormats) ? S_OK : S_FALSE;
    }
    IFACEMETHODIMP Reset() override {
        index_ = 0;
        return S_OK;
    }
    IFACEMETHODIMP Clone(IEnumMediaTypes** out) override {
        if (!out) return E_POINTER;
        ComPtr<MediaTypeEnumerator> clone;
        HRESULT hr = Microsoft::WRL::MakeAndInitialize<MediaTypeEnumerator>(&clone);
        if (SUCCEEDED(hr)) {
            clone->index_ = index_;
            hr = clone.CopyTo(out);
        }
        return hr;
    }

private:
    size_t index_ = 0;
};

} // namespace

HRESULT OutputPin::RuntimeClassInitialize(Filter* filter) {
    filter_ = filter;
    current_ = kDShowFormats[0];
    frameDuration_ = 10'000'000LL / current_.fps;
    AM_MEDIA_TYPE* defaultType = NewMediaType(current_);
    if (!defaultType) return E_OUTOFMEMORY;
    bool ok = CopyMediaTypeInto(*defaultType, &currentType_);
    FreeMediaType(defaultType);
    CoTaskMemFree(defaultType);
    if (!ok) return E_OUTOFMEMORY;
    haveType_ = true;
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return wake_ ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

OutputPin::~OutputPin() {
    Deactivate();
    if (wake_) CloseHandle(wake_);
    FreeMediaType(&currentType_);
}

// ---------------------------------------------------------------------------------------------
// IPin

IFACEMETHODIMP OutputPin::Connect(IPin* receivePin, const AM_MEDIA_TYPE* mt) {
    if (!receivePin) return E_POINTER;
    std::lock_guard<std::recursive_mutex> lock(lock_);
    DShowLog("OutputPin::Connect(mt=%s)", mt && mt->majortype != GUID_NULL ? "specific" : "none");
    if (connected_) return VFW_E_ALREADY_CONNECTED;

    if (mt && mt->majortype != GUID_NULL) {
        HRESULT hr = TryConnect(receivePin, *mt);
        DShowLog("OutputPin::Connect(specific type) = 0x%08X", unsigned(hr));
        return hr;
    }

    HRESULT hr = TryConnect(receivePin, currentType_);
    DShowLog("OutputPin::Connect(default %ux%u@%u) = 0x%08X", current_.width, current_.height, current_.fps,
             unsigned(hr));
    if (SUCCEEDED(hr)) return hr;
    for (const auto& format : kDShowFormats) {
        AM_MEDIA_TYPE* candidate = NewMediaType(format);
        if (!candidate) continue;
        hr = TryConnect(receivePin, *candidate);
        DShowLog("OutputPin::Connect(try %ux%u@%u) = 0x%08X", format.width, format.height, format.fps, unsigned(hr));
        FreeMediaType(candidate);
        CoTaskMemFree(candidate);
        if (SUCCEEDED(hr)) return hr;
    }
    DShowLog("OutputPin::Connect: no format accepted");
    return VFW_E_NO_ACCEPTABLE_TYPES;
}

HRESULT OutputPin::TryConnect(IPin* receivePin, const AM_MEDIA_TYPE& candidate) {
    DShowFormat format;
    if (!MatchesKnownFormat(candidate, &format)) return VFW_E_TYPE_NOT_ACCEPTED;
    // No QueryAccept pre-check: ReceiveConnection does its own type validation, and some input pins
    // (seen with real capture apps) answer QueryAccept ambiguously before a type is negotiated even
    // though ReceiveConnection with that exact type succeeds. Matches the real base-class algorithm
    // (CBaseOutputPin::AttemptConnection), which never calls QueryAccept on the peer either.
    HRESULT hr = receivePin->ReceiveConnection(static_cast<IPin*>(this), &candidate);
    if (FAILED(hr)) return hr;

    AM_MEDIA_TYPE stored{};
    if (!CopyMediaTypeInto(candidate, &stored)) {
        receivePin->Disconnect();
        return E_OUTOFMEMORY;
    }
    FreeMediaType(&currentType_);
    currentType_ = stored;
    haveType_ = true;
    current_ = format;
    frameDuration_ = 10'000'000LL / format.fps;
    connected_ = receivePin;

    hr = NegotiateAllocator();
    if (FAILED(hr)) {
        receivePin->Disconnect();
        connected_.Reset();
        return hr;
    }
    return S_OK;
}

IFACEMETHODIMP OutputPin::ReceiveConnection(IPin*, const AM_MEDIA_TYPE*) {
    return E_UNEXPECTED; // We are the output (initiating) side of every connection we take part in.
}

IFACEMETHODIMP OutputPin::Disconnect() {
    std::lock_guard<std::recursive_mutex> lock(lock_);
    if (!connected_) return S_FALSE;
    Deactivate();
    connected_.Reset();
    connectedMemInput_.Reset();
    allocator_.Reset();
    return S_OK;
}

IFACEMETHODIMP OutputPin::ConnectedTo(IPin** pin) {
    if (!pin) return E_POINTER;
    std::lock_guard<std::recursive_mutex> lock(lock_);
    if (!connected_) {
        *pin = nullptr;
        return VFW_E_NOT_CONNECTED;
    }
    return connected_.CopyTo(pin);
}

IFACEMETHODIMP OutputPin::ConnectionMediaType(AM_MEDIA_TYPE* mt) {
    if (!mt) return E_POINTER;
    std::lock_guard<std::recursive_mutex> lock(lock_);
    if (!connected_) return VFW_E_NOT_CONNECTED;
    return CopyMediaTypeInto(currentType_, mt) ? S_OK : E_OUTOFMEMORY;
}

IFACEMETHODIMP OutputPin::QueryPinInfo(PIN_INFO* info) {
    if (!info) return E_POINTER;
    std::lock_guard<std::recursive_mutex> lock(lock_);
    info->dir = PINDIR_OUTPUT;
    info->pFilter = static_cast<IBaseFilter*>(filter_);
    if (info->pFilter) info->pFilter->AddRef();
    wcscpy_s(info->achName, L"Capture");
    return S_OK;
}

IFACEMETHODIMP OutputPin::QueryDirection(PIN_DIRECTION* dir) {
    if (!dir) return E_POINTER;
    *dir = PINDIR_OUTPUT;
    return S_OK;
}

IFACEMETHODIMP OutputPin::QueryId(LPWSTR* id) {
    if (!id) return E_POINTER;
    constexpr wchar_t kId[] = L"Capture";
    *id = static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(kId)));
    if (!*id) return E_OUTOFMEMORY;
    wcscpy_s(*id, ARRAYSIZE(kId), kId);
    return S_OK;
}

IFACEMETHODIMP OutputPin::QueryAccept(const AM_MEDIA_TYPE* mt) {
    if (!mt) return E_POINTER;
    DShowFormat format;
    return MatchesKnownFormat(*mt, &format) ? S_OK : S_FALSE;
}

IFACEMETHODIMP OutputPin::EnumMediaTypes(IEnumMediaTypes** enumerator) {
    if (!enumerator) return E_POINTER;
    ComPtr<MediaTypeEnumerator> e;
    HRESULT hr = Microsoft::WRL::MakeAndInitialize<MediaTypeEnumerator>(&e);
    if (FAILED(hr)) return hr;
    return e.CopyTo(enumerator);
}

IFACEMETHODIMP OutputPin::QueryInternalConnections(IPin**, ULONG* nPin) {
    if (nPin) *nPin = 0;
    return E_NOTIMPL;
}

// Flush/segment notifications are things an output pin issues to its downstream peer, never receives
// itself — matching the classic DirectShow base classes, which return E_UNEXPECTED here too.
IFACEMETHODIMP OutputPin::EndOfStream() { return E_UNEXPECTED; }
IFACEMETHODIMP OutputPin::BeginFlush() { return E_UNEXPECTED; }
IFACEMETHODIMP OutputPin::EndFlush() { return E_UNEXPECTED; }
IFACEMETHODIMP OutputPin::NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) { return E_UNEXPECTED; }

// ---------------------------------------------------------------------------------------------
// IAMStreamConfig

IFACEMETHODIMP OutputPin::SetFormat(AM_MEDIA_TYPE* mt) {
    if (!mt) return E_POINTER;
    std::lock_guard<std::recursive_mutex> lock(lock_);
    if (connected_) return VFW_E_ALREADY_CONNECTED;
    DShowFormat format;
    if (!MatchesKnownFormat(*mt, &format)) return E_INVALIDARG;
    AM_MEDIA_TYPE stored{};
    if (!CopyMediaTypeInto(*mt, &stored)) return E_OUTOFMEMORY;
    FreeMediaType(&currentType_);
    currentType_ = stored;
    haveType_ = true;
    current_ = format;
    frameDuration_ = 10'000'000LL / format.fps;
    return S_OK;
}

IFACEMETHODIMP OutputPin::GetFormat(AM_MEDIA_TYPE** mt) {
    if (!mt) return E_POINTER;
    std::lock_guard<std::recursive_mutex> lock(lock_);
    AM_MEDIA_TYPE* out = NewMediaType(current_);
    if (!out) return E_OUTOFMEMORY;
    *mt = out;
    return S_OK;
}

IFACEMETHODIMP OutputPin::GetNumberOfCapabilities(int* count, int* size) {
    if (!count || !size) return E_POINTER;
    *count = int(ARRAYSIZE(kDShowFormats));
    *size = sizeof(VIDEO_STREAM_CONFIG_CAPS);
    return S_OK;
}

IFACEMETHODIMP OutputPin::GetStreamCaps(int index, AM_MEDIA_TYPE** mt, BYTE* caps) {
    if (!mt || !caps) return E_POINTER;
    if (index < 0 || index >= int(ARRAYSIZE(kDShowFormats))) return S_FALSE;
    AM_MEDIA_TYPE* out = NewMediaType(kDShowFormats[index]);
    if (!out) return E_OUTOFMEMORY;
    FillStreamCaps(kDShowFormats[index], reinterpret_cast<VIDEO_STREAM_CONFIG_CAPS*>(caps));
    *mt = out;
    return S_OK;
}

// ---------------------------------------------------------------------------------------------
// IKsPropertySet (apps query AMPROPERTY_PIN_CATEGORY to tell a capture pin from other pin kinds)

IFACEMETHODIMP OutputPin::Set(REFGUID, DWORD, LPVOID, DWORD, LPVOID, DWORD) { return E_NOTIMPL; }

IFACEMETHODIMP OutputPin::Get(REFGUID set, DWORD id, LPVOID, DWORD, LPVOID propData, DWORD dataLen,
                              DWORD* bytesReturned) {
    if (set != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
    if (id != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
    if (!propData) {
        if (bytesReturned) *bytesReturned = sizeof(GUID);
        return S_OK;
    }
    if (dataLen < sizeof(GUID)) return E_UNEXPECTED;
    *static_cast<GUID*>(propData) = PIN_CATEGORY_CAPTURE;
    if (bytesReturned) *bytesReturned = sizeof(GUID);
    return S_OK;
}

IFACEMETHODIMP OutputPin::QuerySupported(REFGUID set, DWORD id, DWORD* typeSupport) {
    if (set != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
    if (id != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
    if (typeSupport) *typeSupport = KSPROPERTY_SUPPORT_GET;
    return S_OK;
}

// ---------------------------------------------------------------------------------------------
// Delivery (driven by Filter::Run/Pause/Stop, which always call these without the pin's own lock_)

HRESULT OutputPin::NegotiateAllocator() {
    ComPtr<IMemInputPin> memInput;
    HRESULT hr = connected_->QueryInterface(IID_PPV_ARGS(&memInput));
    if (FAILED(hr)) {
        DShowLog("NegotiateAllocator: peer has no IMemInputPin, 0x%08X", unsigned(hr));
        return hr;
    }

    ComPtr<IMemAllocator> allocator;
    hr = memInput->GetAllocator(&allocator);
    DShowLog("NegotiateAllocator: peer GetAllocator() = 0x%08X%s", unsigned(hr), allocator ? "" : " (none)");
    if (FAILED(hr) || !allocator) {
        hr = CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&allocator));
        DShowLog("NegotiateAllocator: CoCreateInstance(CLSID_MemoryAllocator) = 0x%08X", unsigned(hr));
        if (FAILED(hr)) return hr;
    }

    ALLOCATOR_PROPERTIES request{}, actual{};
    request.cBuffers = 2;
    request.cbBuffer = LONG(FrameBytes(current_.width, current_.height));
    request.cbAlign = 1;
    hr = allocator->SetProperties(&request, &actual);
    DShowLog("NegotiateAllocator: SetProperties(%ld bytes x %ld) = 0x%08X -> got %ld x %ld", request.cbBuffer,
             request.cBuffers, unsigned(hr), actual.cbBuffer, actual.cBuffers);
    if (FAILED(hr)) return hr;
    hr = memInput->NotifyAllocator(allocator.Get(), FALSE);
    DShowLog("NegotiateAllocator: NotifyAllocator() = 0x%08X", unsigned(hr));
    if (FAILED(hr)) return hr;

    allocator_ = allocator;
    connectedMemInput_ = memInput;
    return S_OK;
}

HRESULT OutputPin::Activate() {
    std::lock_guard<std::recursive_mutex> lock(lock_);
    if (!connected_) {
        DShowLog("OutputPin::Activate: not connected, nothing to do");
        return S_OK;
    }
    if (allocator_) {
        HRESULT hr = allocator_->Commit();
        DShowLog("OutputPin::Activate: allocator Commit() = 0x%08X", unsigned(hr));
        if (FAILED(hr)) return hr;
    }
    if (!running_) {
        running_ = true;
        thread_ = std::thread(&OutputPin::DeliveryLoop, this);
        DShowLog("OutputPin::Activate: delivery thread started");
    }
    return S_OK;
}

void OutputPin::Deactivate() {
    {
        std::lock_guard<std::recursive_mutex> lock(lock_);
        running_ = false;
    }
    if (wake_) SetEvent(wake_);
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::recursive_mutex> lock(lock_);
    if (allocator_) allocator_->Decommit();
    streamTime_ = 0;
}

void OutputPin::SetDeliver(bool deliver) { deliver_ = deliver; }

void OutputPin::DeliveryLoop() {
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    // Same 2 ms poll as pc/vcam/media_source.cpp's MediaStream::DeliveryLoop: deliver a phone frame the
    // moment it arrives instead of on a fixed tick, but never faster than 3/4 of the frame interval.
    constexpr LONGLONG kPoll = 20'000; // 2 ms in 100 ns units
    ULONGLONG lastDeliveryMs = 0;
    for (;;) {
        LARGE_INTEGER due;
        due.QuadPart = -kPoll;
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        HANDLE handles[] = {wake_, timer};
        WaitForMultipleObjects(2, handles, FALSE, INFINITE);

        bool run;
        {
            std::lock_guard<std::recursive_mutex> lock(lock_);
            run = running_;
        }
        if (!run) break;
        if (!deliver_.load()) continue;

        const ULONGLONG now = GetTickCount64();
        const LONGLONG sinceMs = LONGLONG(now - lastDeliveryMs);
        const LONGLONG frameDurationMs = std::max<LONGLONG>(1, frameDuration_ / 10'000);
        const bool due_ = sinceMs >= frameDurationMs || (sinceMs >= frameDurationMs * 3 / 4 && reader_.HasNewFrame());
        if (!due_) continue;
        DeliverSample();
        lastDeliveryMs = now;
    }
    CloseHandle(timer);
}

HRESULT OutputPin::DeliverSample() {
    ComPtr<IMemAllocator> allocator;
    ComPtr<IMemInputPin> memInput;
    UINT32 width, height;
    LONGLONG duration;
    {
        std::lock_guard<std::recursive_mutex> lock(lock_);
        if (!allocator_ || !connectedMemInput_) return E_FAIL;
        allocator = allocator_;
        memInput = connectedMemInput_;
        width = current_.width;
        height = current_.height;
        duration = frameDuration_;
    }

    ComPtr<IMediaSample> sample;
    HRESULT hr = allocator->GetBuffer(&sample, nullptr, nullptr, 0); // May block; must not hold lock_.
    if (FAILED(hr)) {
        DShowLog("DeliverSample: GetBuffer() = 0x%08X", unsigned(hr));
        return hr;
    }
    BYTE* data = nullptr;
    hr = sample->GetPointer(&data);
    if (FAILED(hr)) {
        DShowLog("DeliverSample: GetPointer() = 0x%08X", unsigned(hr));
        return hr;
    }

    reader_.Render(data, LONG(width), width, height);
    sample->SetActualDataLength(LONG(FrameBytes(width, height)));
    REFERENCE_TIME start = streamTime_, stop = streamTime_ + duration;
    sample->SetTime(&start, &stop);
    sample->SetSyncPoint(TRUE);
    streamTime_ += duration;

    hr = memInput->Receive(sample.Get());
    // First delivery, then roughly once every 5 s (at the stream's own frame rate): proof of life
    // without flooding the log at full frame rate.
    const LONGLONG heartbeatEvery = std::max<LONGLONG>(1, 50'000'000LL / std::max<LONGLONG>(1, duration));
    if (FAILED(hr) || deliveredCount_ == 0 || deliveredCount_ % uint64_t(heartbeatEvery) == 0) {
        DShowLog("DeliverSample #%llu: Receive() = 0x%08X", (unsigned long long)deliveredCount_, unsigned(hr));
    }
    ++deliveredCount_;
    return hr;
}

} // namespace mycam
