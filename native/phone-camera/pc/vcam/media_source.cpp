#include "media_source.h"

#include <initguid.h>
#include <mfvirtualcamera.h>

namespace mycam {

namespace {

// KSCATEGORY pin name for a video capture stream ({FB6C4281-0353-11d1-905F-0000C0CC16BA}).
constexpr GUID kPinNameVideoCapture = {0xfb6c4281, 0x0353, 0x11d1, {0x90, 0x5f, 0x00, 0x00, 0xc0, 0xcc, 0x16, 0xba}};

constexpr LONGLONG kSecond = 10'000'000; // 100 ns units

// Offered to apps; the first is the default. 4K, 60 and 120 fps match the phone modes (1.3, 120 in 1.3.2).
struct Format { UINT32 width, height, fps; };
constexpr Format kFormats[] = {
    {1920, 1080, 30}, {1280, 720, 30}, {3840, 2160, 30}, {1920, 1080, 60}, {1280, 720, 60}, {1920, 1080, 120},
    {1280, 720, 120}, {640, 480, 30}, {640, 360, 30},
};

HRESULT CreateVideoType(UINT32 width, UINT32 height, UINT32 fps, IMFMediaType** out) {
    ComPtr<IMFMediaType> type;
    HRESULT hr = MFCreateMediaType(&type);
    if (FAILED(hr)) return hr;
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    type->SetUINT32(MF_MT_DEFAULT_STRIDE, width);
    type->SetUINT32(MF_MT_SAMPLE_SIZE, width * height * 3 / 2);
    type->SetUINT32(MF_MT_AVG_BITRATE, width * height * 3 / 2 * 8 * fps);
    *out = type.Detach();
    return S_OK;
}

HRESULT KsNotFound(ULONG* bytesReturned) {
    if (bytesReturned) *bytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

} // namespace

// ---------------------------------------------------------------------------------------------
// MediaStream

HRESULT MediaStream::RuntimeClassInitialize(MediaSource* source, IMFStreamDescriptor* descriptor) {
    source_ = source;
    descriptor_ = descriptor;
    HRESULT hr = MFCreateEventQueue(&queue_);
    if (FAILED(hr)) return hr;
    hr = MFCreateAttributes(&attributes_, 4);
    if (FAILED(hr)) return hr;
    attributes_->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, kPinNameVideoCapture);
    attributes_->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0);
    attributes_->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
    attributes_->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return wake_ ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

MediaStream::~MediaStream() {
    StopThread();
    if (wake_) CloseHandle(wake_);
}

IFACEMETHODIMP MediaStream::GetEvent(DWORD flags, IMFMediaEvent** event) {
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::recursive_mutex> lock(lock_);
        if (shutdown_) return MF_E_SHUTDOWN;
        queue = queue_;
    }
    return queue->GetEvent(flags, event); // May block; must not hold the lock.
}

IFACEMETHODIMP MediaStream::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) {
    std::lock_guard<std::recursive_mutex> lock(lock_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return queue_->BeginGetEvent(callback, state);
}

IFACEMETHODIMP MediaStream::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) {
    std::lock_guard<std::recursive_mutex> lock(lock_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return queue_->EndGetEvent(result, event);
}

IFACEMETHODIMP MediaStream::QueueEvent(MediaEventType type, REFGUID extType, HRESULT status, const PROPVARIANT* value) {
    std::lock_guard<std::recursive_mutex> lock(lock_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return queue_->QueueEventParamVar(type, extType, status, value);
}

IFACEMETHODIMP MediaStream::GetMediaSource(IMFMediaSource** source) {
    if (!source) return E_POINTER;
    std::lock_guard<std::recursive_mutex> lock(lock_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return source_->QueryInterface(IID_PPV_ARGS(source));
}

IFACEMETHODIMP MediaStream::GetStreamDescriptor(IMFStreamDescriptor** descriptor) {
    if (!descriptor) return E_POINTER;
    std::lock_guard<std::recursive_mutex> lock(lock_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return descriptor_.CopyTo(descriptor);
}

IFACEMETHODIMP MediaStream::RequestSample(IUnknown* token) {
    std::lock_guard<std::recursive_mutex> lock(lock_);
    if (shutdown_) return MF_E_SHUTDOWN;
    if (state_ != MF_STREAM_STATE_RUNNING) return MF_E_MEDIA_SOURCE_WRONGSTATE;
    pendingTokens_.emplace_back(token);
    return S_OK;
}

IFACEMETHODIMP MediaStream::SetStreamState(MF_STREAM_STATE state) {
    std::lock_guard<std::recursive_mutex> lock(lock_);
    if (shutdown_) return MF_E_SHUTDOWN;
    switch (state) {
    case MF_STREAM_STATE_STOPPED:
        return Stop();
    case MF_STREAM_STATE_PAUSED:
        if (state_ != MF_STREAM_STATE_RUNNING) return MF_E_INVALID_STATE_TRANSITION;
        state_ = state;
        return S_OK;
    case MF_STREAM_STATE_RUNNING: {
        if (state_ == MF_STREAM_STATE_RUNNING) return S_OK;
        ComPtr<IMFMediaTypeHandler> handler;
        ComPtr<IMFMediaType> type;
        HRESULT hr = descriptor_->GetMediaTypeHandler(&handler);
        if (SUCCEEDED(hr)) hr = handler->GetCurrentMediaType(&type);
        if (SUCCEEDED(hr)) hr = Start(type.Get(), nullptr);
        return hr;
    }
    default:
        return E_INVALIDARG;
    }
}

IFACEMETHODIMP MediaStream::GetStreamState(MF_STREAM_STATE* state) {
    if (!state) return E_POINTER;
    std::lock_guard<std::recursive_mutex> lock(lock_);
    if (shutdown_) return MF_E_SHUTDOWN;
    *state = state_;
    return S_OK;
}

IFACEMETHODIMP MediaStream::KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG* returned) { return KsNotFound(returned); }
IFACEMETHODIMP MediaStream::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG* returned) { return KsNotFound(returned); }
IFACEMETHODIMP MediaStream::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG* returned) { return KsNotFound(returned); }

void MediaStream::SetAllocator(IMFVideoSampleAllocatorEx* allocator) {
    std::lock_guard<std::recursive_mutex> lock(lock_);
    allocator_ = allocator;
}

HRESULT MediaStream::Start(IMFMediaType* type, const PROPVARIANT* startTime) {
    std::lock_guard<std::recursive_mutex> lock(lock_);
    if (shutdown_) return MF_E_SHUTDOWN;
    if (!type) return E_INVALIDARG;
    HRESULT hr = MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width_, &height_);
    if (FAILED(hr)) return hr;
    UINT32 num = 30, den = 1;
    MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &num, &den);
    frameDuration_ = (num && den) ? kSecond * den / num : kSecond / 30;

    if (allocator_ && FAILED(allocator_->InitializeSampleAllocator(10, type))) {
        allocator_.Reset(); // Fall back to plain memory samples.
    }

    state_ = MF_STREAM_STATE_RUNNING;
    if (!running_) {
        running_ = true;
        thread_ = std::thread(&MediaStream::DeliveryLoop, this);
    }
    return queue_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, startTime);
}

HRESULT MediaStream::Stop() {
    // The delivery thread keeps ticking but idles while stopped; it is joined on Shutdown.
    std::lock_guard<std::recursive_mutex> lock(lock_);
    if (shutdown_) return MF_E_SHUTDOWN;
    state_ = MF_STREAM_STATE_STOPPED;
    pendingTokens_.clear();
    if (allocator_) allocator_->UninitializeSampleAllocator();
    return queue_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
}

void MediaStream::Shutdown() {
    StopThread();
    std::lock_guard<std::recursive_mutex> lock(lock_);
    if (shutdown_) return;
    shutdown_ = true;
    state_ = MF_STREAM_STATE_STOPPED;
    pendingTokens_.clear();
    if (queue_) queue_->Shutdown();
    if (allocator_) allocator_->UninitializeSampleAllocator();
    allocator_.Reset();
    source_ = nullptr;
}

void MediaStream::StopThread() {
    // Must be called without lock_ held: the delivery thread takes it every tick.
    if (!thread_.joinable()) return;
    {
        std::lock_guard<std::recursive_mutex> lock(lock_);
        running_ = false;
    }
    SetEvent(wake_);
    thread_.join();
}

void MediaStream::DeliveryLoop() {
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    // Poll every 2 ms and hand a new phone frame to the app as soon as it arrives, instead of on a fixed
    // tick (which added up to one frame of delay). Without new frames, repeat the last one at the app's
    // frame rate; never deliver faster than 3/4 of the frame interval.
    constexpr LONGLONG kPoll = 20'000; // 2 ms in 100 ns units
    LONGLONG lastDelivery = 0;
    for (;;) {
        LARGE_INTEGER due;
        due.QuadPart = -kPoll;
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        HANDLE handles[] = {wake_, timer};
        WaitForMultipleObjects(2, handles, FALSE, INFINITE);

        std::lock_guard<std::recursive_mutex> lock(lock_);
        if (!running_) break;
        if (state_ != MF_STREAM_STATE_RUNNING || pendingTokens_.empty()) continue;
        const LONGLONG now = MFGetSystemTime(), since = now - lastDelivery;
        const bool due_ = since >= frameDuration_ || (since >= frameDuration_ * 3 / 4 && reader_.HasNewFrame());
        if (!due_) continue;
        ComPtr<IUnknown> token = pendingTokens_.front();
        pendingTokens_.pop_front();
        HRESULT hr = DeliverSample(token.Get());
        if (FAILED(hr)) queue_->QueueEventParamVar(MEError, GUID_NULL, hr, nullptr);
        lastDelivery = now;
    }
    CloseHandle(timer);
}

HRESULT MediaStream::DeliverSample(IUnknown* token) {
    const DWORD size = width_ * height_ * 3 / 2;
    ComPtr<IMFSample> sample;
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr;
    if (allocator_) {
        hr = allocator_->AllocateSample(&sample);
        if (SUCCEEDED(hr)) hr = sample->GetBufferByIndex(0, &buffer);
    } else {
        hr = MFCreateSample(&sample);
        if (SUCCEEDED(hr)) hr = MFCreateMemoryBuffer(size, &buffer);
        if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer.Get());
    }
    if (FAILED(hr)) return hr;

    ComPtr<IMF2DBuffer2> buffer2d;
    if (SUCCEEDED(buffer.As(&buffer2d))) {
        BYTE* scanline0 = nullptr;
        BYTE* start = nullptr;
        LONG pitch = 0;
        DWORD length = 0;
        hr = buffer2d->Lock2DSize(MF2DBuffer_LockFlags_Write, &scanline0, &pitch, &start, &length);
        if (FAILED(hr)) return hr;
        reader_.Render(scanline0, pitch, width_, height_);
        buffer2d->Unlock2D();
    } else {
        BYTE* data = nullptr;
        DWORD maxLength = 0;
        hr = buffer->Lock(&data, &maxLength, nullptr);
        if (FAILED(hr)) return hr;
        if (maxLength >= size) reader_.Render(data, LONG(width_), width_, height_);
        buffer->Unlock();
        buffer->SetCurrentLength(size);
    }

    sample->SetSampleTime(MFGetSystemTime());
    sample->SetSampleDuration(frameDuration_);
    if (token) sample->SetUnknown(MFSampleExtension_Token, token);
    return queue_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
}

// ---------------------------------------------------------------------------------------------
// MediaSource

HRESULT MediaSource::RuntimeClassInitialize(IMFAttributes* activatorAttributes) {
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) return hr;
    mfStarted_ = true;

    hr = MFCreateEventQueue(&queue_);
    if (FAILED(hr)) return hr;
    hr = MFCreateAttributes(&attributes_, 4);
    if (FAILED(hr)) return hr;
    if (activatorAttributes) activatorAttributes->CopyAllItems(attributes_.Get());

    IMFMediaType* types[ARRAYSIZE(kFormats)] = {};
    for (size_t i = 0; i < ARRAYSIZE(kFormats); ++i) {
        hr = CreateVideoType(kFormats[i].width, kFormats[i].height, kFormats[i].fps, &types[i]);
        if (FAILED(hr)) break;
    }
    ComPtr<IMFStreamDescriptor> sd;
    if (SUCCEEDED(hr)) hr = MFCreateStreamDescriptor(0, ARRAYSIZE(types), types, &sd);
    if (SUCCEEDED(hr)) {
        ComPtr<IMFMediaTypeHandler> handler;
        hr = sd->GetMediaTypeHandler(&handler);
        if (SUCCEEDED(hr)) hr = handler->SetCurrentMediaType(types[0]);
    }
    for (IMFMediaType* t : types) if (t) t->Release();
    if (FAILED(hr)) return hr;

    hr = Microsoft::WRL::MakeAndInitialize<MediaStream>(&stream_, this, sd.Get());
    if (FAILED(hr)) return hr;
    // Frame Server reads stream attributes from the descriptor too.
    stream_->Attributes()->CopyAllItems(sd.Get());

    IMFStreamDescriptor* sds[] = {sd.Get()};
    hr = MFCreatePresentationDescriptor(1, sds, &descriptor_);
    if (SUCCEEDED(hr)) hr = descriptor_->SelectStream(0);
    return hr;
}

MediaSource::~MediaSource() {
    Shutdown();
    if (mfStarted_) MFShutdown();
}

IFACEMETHODIMP MediaSource::GetEvent(DWORD flags, IMFMediaEvent** event) {
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::recursive_mutex> lock(lock_);
        if (shutdown_) return MF_E_SHUTDOWN;
        queue = queue_;
    }
    return queue->GetEvent(flags, event);
}

IFACEMETHODIMP MediaSource::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) {
    std::lock_guard<std::recursive_mutex> lock(lock_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return queue_->BeginGetEvent(callback, state);
}

IFACEMETHODIMP MediaSource::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) {
    std::lock_guard<std::recursive_mutex> lock(lock_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return queue_->EndGetEvent(result, event);
}

IFACEMETHODIMP MediaSource::QueueEvent(MediaEventType type, REFGUID extType, HRESULT status, const PROPVARIANT* value) {
    std::lock_guard<std::recursive_mutex> lock(lock_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return queue_->QueueEventParamVar(type, extType, status, value);
}

IFACEMETHODIMP MediaSource::GetCharacteristics(DWORD* characteristics) {
    if (!characteristics) return E_POINTER;
    std::lock_guard<std::recursive_mutex> lock(lock_);
    if (shutdown_) return MF_E_SHUTDOWN;
    *characteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

IFACEMETHODIMP MediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor) {
    if (!descriptor) return E_POINTER;
    std::lock_guard<std::recursive_mutex> lock(lock_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return descriptor_->Clone(descriptor);
}

IFACEMETHODIMP MediaSource::Start(IMFPresentationDescriptor* descriptor, const GUID* timeFormat,
                                  const PROPVARIANT* startPosition) {
    if (!descriptor) return E_INVALIDARG;
    if (timeFormat && *timeFormat != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;
    std::lock_guard<std::recursive_mutex> lock(lock_);
    if (shutdown_) return MF_E_SHUTDOWN;

    BOOL selected = FALSE;
    ComPtr<IMFStreamDescriptor> sd;
    HRESULT hr = descriptor->GetStreamDescriptorByIndex(0, &selected, &sd);
    if (FAILED(hr)) return hr;

    PROPVARIANT start;
    PropVariantInit(&start);
    start.vt = VT_I8;
    start.hVal.QuadPart = MFGetSystemTime();

    if (selected) {
        ComPtr<IMFMediaTypeHandler> handler;
        ComPtr<IMFMediaType> type;
        hr = sd->GetMediaTypeHandler(&handler);
        if (SUCCEEDED(hr)) hr = handler->GetCurrentMediaType(&type);
        if (FAILED(hr)) return hr;

        // Keep our own descriptor's current type in sync with what the client picked.
        ComPtr<IMFStreamDescriptor> ours;
        ComPtr<IMFMediaTypeHandler> ourHandler;
        BOOL ignored;
        if (SUCCEEDED(descriptor_->GetStreamDescriptorByIndex(0, &ignored, &ours)) &&
            SUCCEEDED(ours->GetMediaTypeHandler(&ourHandler))) {
            ourHandler->SetCurrentMediaType(type.Get());
        }

        hr = queue_->QueueEventParamUnk(streamAnnounced_ ? MEUpdatedStream : MENewStream, GUID_NULL, S_OK,
                                        static_cast<IMFMediaStream*>(stream_.Get()));
        if (FAILED(hr)) return hr;
        streamAnnounced_ = true;
        hr = stream_->Start(type.Get(), &start);
        if (FAILED(hr)) return hr;
    }
    hr = queue_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &start);
    PropVariantClear(&start);
    return hr;
}

IFACEMETHODIMP MediaSource::Stop() {
    std::lock_guard<std::recursive_mutex> lock(lock_);
    if (shutdown_) return MF_E_SHUTDOWN;
    HRESULT hr = stream_->Stop();
    if (FAILED(hr)) return hr;
    return queue_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
}

IFACEMETHODIMP MediaSource::Pause() { return MF_E_INVALID_STATE_TRANSITION; }

IFACEMETHODIMP MediaSource::Shutdown() {
    ComPtr<MediaStream> stream;
    {
        std::lock_guard<std::recursive_mutex> lock(lock_);
        if (shutdown_) return MF_E_SHUTDOWN;
        shutdown_ = true;
        stream = stream_;
        if (queue_) queue_->Shutdown();
    }
    if (stream) stream->Shutdown();
    return S_OK;
}

IFACEMETHODIMP MediaSource::GetSourceAttributes(IMFAttributes** attributes) {
    if (!attributes) return E_POINTER;
    std::lock_guard<std::recursive_mutex> lock(lock_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return attributes_.CopyTo(attributes);
}

IFACEMETHODIMP MediaSource::GetStreamAttributes(DWORD streamId, IMFAttributes** attributes) {
    if (!attributes) return E_POINTER;
    std::lock_guard<std::recursive_mutex> lock(lock_);
    if (shutdown_) return MF_E_SHUTDOWN;
    if (streamId != 0) return E_FAIL;
    *attributes = stream_->Attributes();
    (*attributes)->AddRef();
    return S_OK;
}

IFACEMETHODIMP MediaSource::SetD3DManager(IUnknown*) {
    // We render on the CPU into system-memory samples; nothing to do.
    return S_OK;
}

IFACEMETHODIMP MediaSource::GetService(REFGUID, REFIID, LPVOID* object) {
    if (object) *object = nullptr;
    return MF_E_UNSUPPORTED_SERVICE;
}

IFACEMETHODIMP MediaSource::KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG* returned) { return KsNotFound(returned); }
IFACEMETHODIMP MediaSource::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG* returned) { return KsNotFound(returned); }
IFACEMETHODIMP MediaSource::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG* returned) { return KsNotFound(returned); }

IFACEMETHODIMP MediaSource::SetDefaultAllocator(DWORD outputStreamId, IUnknown* allocator) {
    std::lock_guard<std::recursive_mutex> lock(lock_);
    if (shutdown_) return MF_E_SHUTDOWN;
    if (outputStreamId != 0) return E_INVALIDARG;
    ComPtr<IMFVideoSampleAllocatorEx> ex;
    if (allocator) allocator->QueryInterface(IID_PPV_ARGS(&ex));
    stream_->SetAllocator(ex.Get());
    return S_OK;
}

IFACEMETHODIMP MediaSource::GetAllocatorUsage(DWORD outputStreamId, DWORD* inputStreamId, MFSampleAllocatorUsage* usage) {
    if (!inputStreamId || !usage) return E_POINTER;
    std::lock_guard<std::recursive_mutex> lock(lock_);
    if (shutdown_) return MF_E_SHUTDOWN;
    if (outputStreamId != 0) return E_INVALIDARG;
    *inputStreamId = outputStreamId;
    *usage = MFSampleAllocatorUsage_UsesProvidedAllocator;
    return S_OK;
}

} // namespace mycam
