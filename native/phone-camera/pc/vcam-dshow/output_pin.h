#pragma once
// The filter's single output pin: format negotiation (IAMStreamConfig), the IPin connection dance, and
// the delivery thread that pushes frames downstream. Mirrors pc/vcam/media_source.h's MediaStream, but
// pushes IMediaSample via IMemInputPin::Receive instead of queuing Media Foundation events.

#include <windows.h>
#include <dshow.h>
#include <wrl.h>

#include <atomic>
#include <mutex>
#include <thread>

#include "../vcam/frame_reader.h"
#include "dshow_media_types.h"

namespace mycam {

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::FtmBase;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;

class Filter;

class OutputPin : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IPin, IAMStreamConfig, IKsPropertySet, FtmBase> {
public:
    HRESULT RuntimeClassInitialize(Filter* filter);
    ~OutputPin() override;

    // IPin
    IFACEMETHODIMP Connect(IPin* receivePin, const AM_MEDIA_TYPE* mt) override;
    IFACEMETHODIMP ReceiveConnection(IPin* connector, const AM_MEDIA_TYPE* mt) override;
    IFACEMETHODIMP Disconnect() override;
    IFACEMETHODIMP ConnectedTo(IPin** pin) override;
    IFACEMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* mt) override;
    IFACEMETHODIMP QueryPinInfo(PIN_INFO* info) override;
    IFACEMETHODIMP QueryDirection(PIN_DIRECTION* dir) override;
    IFACEMETHODIMP QueryId(LPWSTR* id) override;
    IFACEMETHODIMP QueryAccept(const AM_MEDIA_TYPE* mt) override;
    IFACEMETHODIMP EnumMediaTypes(IEnumMediaTypes** enumerator) override;
    IFACEMETHODIMP QueryInternalConnections(IPin** apPin, ULONG* nPin) override;
    IFACEMETHODIMP EndOfStream() override;
    IFACEMETHODIMP BeginFlush() override;
    IFACEMETHODIMP EndFlush() override;
    IFACEMETHODIMP NewSegment(REFERENCE_TIME start, REFERENCE_TIME stop, double rate) override;

    // IAMStreamConfig
    IFACEMETHODIMP SetFormat(AM_MEDIA_TYPE* mt) override;
    IFACEMETHODIMP GetFormat(AM_MEDIA_TYPE** mt) override;
    IFACEMETHODIMP GetNumberOfCapabilities(int* count, int* size) override;
    IFACEMETHODIMP GetStreamCaps(int index, AM_MEDIA_TYPE** mt, BYTE* caps) override;

    // IKsPropertySet
    IFACEMETHODIMP Set(REFGUID set, DWORD id, LPVOID instanceData, DWORD instanceLen, LPVOID propData,
                       DWORD dataLen) override;
    IFACEMETHODIMP Get(REFGUID set, DWORD id, LPVOID instanceData, DWORD instanceLen, LPVOID propData,
                       DWORD dataLen, DWORD* bytesReturned) override;
    IFACEMETHODIMP QuerySupported(REFGUID set, DWORD id, DWORD* typeSupport) override;

    // Called by Filter on state transitions (always while the filter holds its own lock).
    HRESULT Activate();
    void Deactivate();
    void SetDeliver(bool deliver);

private:
    void DeliveryLoop();
    HRESULT DeliverSample();
    HRESULT NegotiateAllocator();
    HRESULT TryConnect(IPin* receivePin, const AM_MEDIA_TYPE& candidate);

    std::recursive_mutex lock_;
    Filter* filter_ = nullptr; // Weak: the filter owns the pin.
    ComPtr<IPin> connected_;
    ComPtr<IMemInputPin> connectedMemInput_;
    ComPtr<IMemAllocator> allocator_;
    AM_MEDIA_TYPE currentType_{};
    bool haveType_ = false;
    DShowFormat current_{1920, 1080, 30};
    LONGLONG frameDuration_ = 10'000'000 / 30;

    HANDLE wake_ = nullptr;
    bool running_ = false;
    std::atomic<bool> deliver_{false};
    std::thread thread_;
    LONGLONG streamTime_ = 0;
    uint64_t deliveredCount_ = 0; // Only for the dshow_log.h heartbeat.
    FrameReader reader_;
};

} // namespace mycam
