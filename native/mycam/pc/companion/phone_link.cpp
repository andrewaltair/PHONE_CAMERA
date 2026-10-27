#include "phone_link.h"

#include "net.h" // Before libusb.h: on Windows that includes the old winsock.h, which clashes with winsock2.h.

#include <libusb.h>
#include <string.h>

#include <chrono>

#include "log.h"
#include "marquee.h"
#include "phone_state.h"
#include "platform.h"
#include "protocol.h"

namespace mycam {

namespace {

constexpr uint16_t kGoogleVid = 0x18D1;
constexpr int kAoaGetProtocol = 51;
constexpr int kAoaSendString = 52;
constexpr int kAoaStart = 53;

// USB vendors that make Android phones. Used together with interface checks to decide which devices
// are worth asking to switch into accessory mode.
constexpr uint16_t kAndroidVendors[] = {
    0x18D1, 0x04E8, 0x2717, 0x2A70, 0x22B8, 0x1004, 0x12D1, 0x22D9, 0x2D95, 0x0FCE, 0x0BB4, 0x0B05,
    0x17EF, 0x19D2, 0x05C6, 0x0E8D, 0x1782, 0x2A45, 0x1BBB, 0x2E04, 0x0489, 0x2B0E, 0x2D96, 0x1EBF,
};

constexpr uint64_t kConsumerWindowMs = 2000;  // App counts as "using the camera" for this long after a request.
constexpr uint64_t kStopGraceMs = 4000;       // Keep the phone camera on briefly between app sessions.

bool IsAccessoryPid(uint16_t pid) { return pid >= 0x2D00 && pid <= 0x2D05; }

std::string DeviceKey(libusb_device* dev, const libusb_device_descriptor& desc) {
    uint8_t ports[8];
    int n = libusb_get_port_numbers(dev, ports, sizeof(ports));
    std::string key = std::to_string(libusb_get_bus_number(dev));
    for (int i = 0; i < n; ++i) key += "." + std::to_string(ports[i]);
    key += ":" + std::to_string(desc.idVendor) + ":" + std::to_string(desc.idProduct);
    return key;
}

} // namespace

void PhoneLink::Run() {
    // Without USB (no UsbDk on Windows, say), USB phones can't be used, but Wi-Fi phones still can.
    const bool ok = InitUsb();
    usbReady_ = ok;
    net::Init();
    // The iPhone page's server. Without it (no certificate, port taken) everything else still works.
    web_ = std::make_unique<WebServer>();
    web_->Start();
    web_->SetPhoneHandler([this](const uint8_t* data, size_t size) {
        if (webSession_) FeedBytes(data, size);
    });
    web_->SetPcId(PcIdHex()); // In the QR code, for the Android app (PROTOCOL.md "QR pairing").
    status_.state = ok ? LinkState::Searching : LinkState::NoDriver;
    Publish();

    while (!quit_) {
        if (usbReady_) ScanOnce();         // Runs a whole USB session when a phone is plugged in.
        if (!quit_) NetScanOnce();          // Runs a whole Wi-Fi session when a phone answers (if enabled).
        if (!quit_) WebScanOnce();          // Runs a whole web session when an iPhone's page is waiting.
        if (!usbReady_ && status_.state == LinkState::Searching && !wireless_ && !(web_ && web_->Listening())) {
            status_.state = LinkState::NoDriver;
            Publish();
        }
        if (status_.wirelessSearch != wireless_) Publish(); // Status text mentions Wi-Fi when it's on.
        for (uint64_t start = NowMs(); !quit_ && NowMs() - start < 1000;) {
            SyncLockPaused();
            WriteStatusFrame(NowMs()); // "Waiting for the phone" while none is connected.
            PollDiscovery(NowMs());     // "Scan for phones" answers arrive promptly.
            if (connectRequest_ != 0 || qrRequest_.ip) break; // Connect (scan list or a QR scan): NetScanOnce runs it.
            PollWeb(StatusWaitMs());             // Serves the iPhone page meanwhile (or just waits).
            if (web_ && web_->PhonePending()) break; // An iPhone's page wants a session: WebScanOnce runs it.
        }
    }
    web_.reset();
    net::Cleanup();
    ExitUsb();
}

void PhoneLink::ScanOnce() {
    // Windows has no driver for a phone in accessory mode, and a driverless device makes UsbDk fail to
    // enumerate anything. Ask the UI to bind WinUSB (one UAC prompt per phone; it sticks afterwards).
    if (UsbNeedsDriver()) {
        uint64_t now = NowMs();
        if (onNeedDriver_ && (lastDriverRequest_ == 0 || now - lastDriverRequest_ > 20000)) {
            lastDriverRequest_ = now;
            Log("scan: accessory-mode phone has no driver, requesting WinUSB binding");
            onNeedDriver_();
        }
        if (status_.state != LinkState::Waiting) {
            status_ = LinkStatus{};
            status_.state = LinkState::Waiting;
            Publish();
        }
        return;
    }

    libusb_device** list = nullptr;
    ssize_t count = libusb_get_device_list(ctx_, &list);
    if (count < 0) {
        static uint64_t lastWarn = 0;
        if (NowMs() - lastWarn > 10000) { Log("scan: device list failed: %s", libusb_error_name(int(count))); lastWarn = NowMs(); }
        return;
    }

    libusb_device* accessory = nullptr;
    std::set<std::string> present;
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) != LIBUSB_SUCCESS) continue;
        if (desc.idVendor == kGoogleVid && IsAccessoryPid(desc.idProduct)) {
            if (!accessory) accessory = list[i];
            continue;
        }
        std::string key = DeviceKey(list[i], desc);
        present.insert(key);
        // Ask each phone once per plug-in; the set entry goes away when it is unplugged.
        if (!probed_.count(key) && LooksLikeAndroid(list[i])) {
            probed_.insert(key);
            SwitchToAccessory(list[i]);
        }
    }
    for (auto it = probed_.begin(); it != probed_.end();) {
        it = present.count(*it) ? std::next(it) : probed_.erase(it);
    }

    if (accessory) {
        libusb_ref_device(accessory);
        libusb_free_device_list(list, 1);
        RunAccessory(accessory);
        libusb_unref_device(accessory);
    } else {
        libusb_free_device_list(list, 1);
        if (status_.state != LinkState::Searching) {
            status_ = LinkStatus{};
            Publish();
        }
    }
}

// During a Wi-Fi session: a phone was just plugged in (or is in accessory mode), so the cable should take
// over. Phones already asked once (probed_) don't count, so a phone left plugged in that refused doesn't
// keep ending the Wi-Fi session.
bool PhoneLink::UsbPhoneArrived() {
    if (!usbReady_) return false;
    if (UsbNeedsDriver()) return true;
    libusb_device** list = nullptr;
    ssize_t count = libusb_get_device_list(ctx_, &list);
    if (count < 0) return false;
    bool arrived = false;
    for (ssize_t i = 0; i < count && !arrived; ++i) {
        libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) != LIBUSB_SUCCESS) continue;
        arrived = (desc.idVendor == kGoogleVid && IsAccessoryPid(desc.idProduct)) ||
                  (!probed_.count(DeviceKey(list[i], desc)) && LooksLikeAndroid(list[i]));
    }
    libusb_free_device_list(list, 1);
    return arrived;
}

bool PhoneLink::LooksLikeAndroid(libusb_device* dev) {
    libusb_device_descriptor desc;
    if (libusb_get_device_descriptor(dev, &desc) != LIBUSB_SUCCESS) return false;
    if (desc.bDeviceClass == LIBUSB_CLASS_HUB) return false;

    bool knownVendor = false;
    for (uint16_t vid : kAndroidVendors) knownVendor |= desc.idVendor == vid;

    libusb_config_descriptor* config = nullptr;
    if (libusb_get_active_config_descriptor(dev, &config) != LIBUSB_SUCCESS) return false;
    bool phoneInterface = false, excluded = false;
    for (int i = 0; i < config->bNumInterfaces; ++i) {
        const libusb_interface& itf = config->interface[i];
        for (int a = 0; a < itf.num_altsetting; ++a) {
            const libusb_interface_descriptor& alt = itf.altsetting[a];
            switch (alt.bInterfaceClass) {
            case LIBUSB_CLASS_HID:
            case LIBUSB_CLASS_PRINTER:
            case LIBUSB_CLASS_MASS_STORAGE:
            case LIBUSB_CLASS_HUB:
            case LIBUSB_CLASS_VIDEO:
                excluded = true; // Keyboards, drives, webcams...: never poke these.
                break;
            case LIBUSB_CLASS_IMAGE: // PTP
                phoneInterface = true;
                break;
            case LIBUSB_CLASS_VENDOR_SPEC:
                // ADB (ff/42/01) or Android MTP (ff/ff/00 with bulk in/out + interrupt).
                if ((alt.bInterfaceSubClass == 0x42 && alt.bInterfaceProtocol == 0x01) ||
                    (alt.bInterfaceSubClass == 0xFF && alt.bInterfaceProtocol == 0x00 && alt.bNumEndpoints == 3)) {
                    phoneInterface = true;
                }
                break;
            }
        }
    }
    libusb_free_config_descriptor(config);
    return !excluded && (phoneInterface || knownVendor);
}

void PhoneLink::SwitchToAccessory(libusb_device* dev) {
    libusb_device_handle* h = nullptr;
    libusb_device_descriptor desc = {};
    libusb_get_device_descriptor(dev, &desc);
    int openResult = libusb_open(dev, &h);
    Log("scan: phone-like device %04x:%04x, open -> %s", desc.idVendor, desc.idProduct, libusb_error_name(openResult));
    if (openResult != LIBUSB_SUCCESS) return;

    uint8_t version[2] = {};
    int r = libusb_control_transfer(h, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR, kAoaGetProtocol, 0, 0,
                                    version, sizeof(version), 1000);
    Log("scan: AOA protocol query -> %d (version %d)", r, version[0] | version[1] << 8);
    if (r == 2 && (version[0] | version[1] << 8) >= 1) {
        const char* strings[] = {proto::kAccessoryManufacturer, proto::kAccessoryModel, proto::kAccessoryDescription,
                                 proto::kAccessoryVersion, proto::kAccessoryUri, proto::kAccessorySerial};
        bool ok = true;
        for (uint16_t i = 0; i < 6 && ok; ++i) {
            ok = libusb_control_transfer(h, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR, kAoaSendString, 0, i,
                                         (unsigned char*)strings[i], uint16_t(strlen(strings[i]) + 1), 1000) >= 0;
        }
        // The phone disconnects and comes back as a Google accessory device (18d1:2d0x).
        if (ok) libusb_control_transfer(h, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR, kAoaStart, 0, 0, nullptr, 0, 1000);
    }
    libusb_close(h);
}

libusb_device* PhoneLink::FindAccessory(libusb_context* ctx) {
    libusb_device** list = nullptr;
    ssize_t count = libusb_get_device_list(ctx, &list);
    if (count < 0) return nullptr;
    libusb_device* found = nullptr;
    for (ssize_t i = 0; i < count && !found; ++i) {
        libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) == LIBUSB_SUCCESS && desc.idVendor == kGoogleVid &&
            IsAccessoryPid(desc.idProduct)) {
            found = libusb_ref_device(list[i]);
        }
    }
    libusb_free_device_list(list, 1);
    return found;
}

void PhoneLink::RunSession(libusb_device* dev, libusb_context* ctx) {
    sessionCtx_ = ctx;
    int openResult = libusb_open(dev, &handle_);
    if (openResult != LIBUSB_SUCCESS) {
        static uint64_t lastWarn = 0;
        if (NowMs() - lastWarn > 10000) {
            Log("session: opening accessory failed: %s", libusb_error_name(openResult));
            lastWarn = NowMs();
        }
        handle_ = nullptr;
        return;
    }

    // Interface 0 of the accessory configuration has one bulk IN and one bulk OUT endpoint.
    libusb_config_descriptor* config = nullptr;
    epIn_ = epOut_ = 0;
    if (libusb_get_active_config_descriptor(dev, &config) == LIBUSB_SUCCESS) {
        const libusb_interface_descriptor& alt = config->interface[0].altsetting[0];
        for (int e = 0; e < alt.bNumEndpoints; ++e) {
            const libusb_endpoint_descriptor& ep = alt.endpoint[e];
            if ((ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK) continue;
            if (ep.bEndpointAddress & LIBUSB_ENDPOINT_IN) epIn_ = ep.bEndpointAddress;
            else epOut_ = ep.bEndpointAddress;
        }
        libusb_free_config_descriptor(config);
    }
    if (!epIn_ || !epOut_ || libusb_claim_interface(handle_, 0) != LIBUSB_SUCCESS) {
        libusb_close(handle_);
        handle_ = nullptr;
        return;
    }

    BeginSession(false, "");
    Log("session: accessory opened via %s (in 0x%02x, out 0x%02x)", BackendName(ctx), epIn_, epOut_);

    // Keep several reads queued with no timeout. Timed-out reads get cancelled, and with UsbDk a
    // cancelled read can drop bytes that were already in flight, which corrupts the stream.
    // Frames are decoded inside the read callback, so queue enough reads (256 KB) to hold a whole 4K frame:
    // otherwise the phone's write stalls while a frame decodes, and that delay backs up into its camera.
    constexpr int kQueued = 16;
    constexpr int kReadSize = 16384; // Matches the phone's accessory request size.
    std::vector<std::vector<uint8_t>> buffers(kQueued, std::vector<uint8_t>(kReadSize));
    std::vector<libusb_transfer*> transfers;
    sessionEnding_ = sessionError_ = false;
    inFlight_ = 0;
    for (auto& b : buffers) {
        libusb_transfer* t = libusb_alloc_transfer(0);
        libusb_fill_bulk_transfer(t, handle_, epIn_, b.data(), kReadSize,
                                  [](libusb_transfer* x) { static_cast<PhoneLink*>(x->user_data)->OnInTransfer(x); },
                                  this, 0);
        if (libusb_submit_transfer(t) == LIBUSB_SUCCESS) ++inFlight_;
        else Log("session: submit failed");
        transfers.push_back(t);
    }

    const bool reconnect = SessionLoop([this] {
        timeval tv = {0, long(StatusWaitMs()) * 1000};
        libusb_handle_events_timeout_completed(sessionCtx_, &tv, nullptr);
    });
    // Forces the phone out of accessory mode; it re-enumerates and we switch it back.
    if (reconnect) libusb_reset_device(handle_);
    LeaveSession();

    sessionEnding_ = true;
    for (libusb_transfer* t : transfers) libusb_cancel_transfer(t);
    for (int i = 0; i < 50 && inFlight_ > 0; ++i) {
        timeval tv = {0, 20000};
        libusb_handle_events_timeout_completed(sessionCtx_, &tv, nullptr);
    }
    if (inFlight_ == 0) {
        for (libusb_transfer* t : transfers) libusb_free_transfer(t);
    } // else: leak rather than free transfers libusb still owns.
    libusb_release_interface(handle_, 0);
    libusb_close(handle_);
    handle_ = nullptr;
    ResetAfterSession();
}

void PhoneLink::BeginSession(bool wireless, const std::string& phoneName) {
    parser_.Reset();
    gotHello_ = startSent_ = phoneStreaming_ = false;
    waitKeyFrame_ = true;
    keyFrameWanted_ = false;
    csd_.clear();
    decoder_.Reset();
    lastDecoderFailure_ = 0;
    status_ = LinkStatus{};
    status_.state = LinkState::Waiting;
    status_.wireless = wireless;
    if (web_) web_->SetBusy(true); // One phone at a time: pages that turn up now are told to wait.
    if (!phoneName.empty()) status_.phoneName = Wide(phoneName);
    writer_.SetPhoneState(kPhoneWaiting);
    sessionError_ = false;
    statBytes_ = statPackets_ = statFrames_ = statDecoded_ = statDecodeErrors_ = 0;
    Publish();
}

// The protocol for one connected phone, the same over USB and Wi-Fi: HELLO until it answers, then START /
// STOP as apps use the camera, plus queued commands. `pump` waits up to StatusWaitMs() for data from the phone and
// feeds it in (or sets sessionError_). Everything runs on this thread, so sends never race with reads.
bool PhoneLink::SessionLoop(const std::function<void()>& pump) {
    SendCommand(proto::kCmdHello);
    uint64_t lastHello = NowMs();
    uint64_t lastConsumer = 0;
    uint64_t lastStart = 0;
    uint64_t lastStop = 0;
    lastStats_ = lastHello;

    while (!quit_ && !sessionError_) {
        pump();

        uint64_t now = NowMs();
        LogStats(now);
        SyncLockPaused();
        WriteStatusFrame(now);
        PollDiscovery(now); // A scan may run during a session: it only lists phones, it never connects.
        PollWeb(0);         // The iPhone page stays reachable (it is told the PC is busy).
        if (reconnect_.exchange(false)) return true;
        if (!gotHello_) {
            if (now - lastHello > 2000) {
                SendCommand(proto::kCmdHello);
                lastHello = now;
            }
            continue;
        }

        if (keyFrameWanted_) {
            keyFrameWanted_ = false;
            SendCommand(proto::kCmdKeyFrame);
        }
        int facing = pendingFacing_.exchange(-1);
        if (facing >= 0) SendCommand(proto::kCmdSetFacing, uint8_t(facing));
        {
            std::vector<std::pair<uint8_t, uint8_t>> queued;
            {
                std::lock_guard<std::mutex> lock(commandLock_);
                queued.swap(commands_);
            }
            for (auto& c : queued) SendCommand(c.first, c.second);
        }
        int pause = pendingPause_.exchange(-1);
        if (pause >= 0) {
            Log("session: user %s the camera from the PC", pause ? "paused" : "resumed");
            SendCommand(pause ? proto::kCmdPause : proto::kCmdResume);
        }

        if (writer_.ConsumerActive(kConsumerWindowMs)) lastConsumer = now;
        // Video is wanted only if an app is using the camera and nothing has paused it.
        bool wanted = lastConsumer && now - lastConsumer < kStopGraceMs && !phonePaused_ && !lockPaused_;
        if (wanted && (!startSent_ || (!phoneStreaming_ && now - lastStart > 3000))) {
            // (Re)start the phone camera; repeated if the phone reported an error or never started.
            Log("session: app is using the camera -> START");
            if (!SendCommand(proto::kCmdStart)) break;
            startSent_ = true;
            lastStart = now;
            waitKeyFrame_ = true;
        } else if (!wanted && (startSent_ || (phoneStreaming_ && now - lastStop > 3000))) {
            // Also stops a phone that is streaming without being asked, e.g. after the companion crashed
            // or was killed mid-stream and the phone kept its camera on.
            Log("session: camera not wanted (no app, paused or Windows locked) -> STOP");
            if (!SendCommand(proto::kCmdStop)) break;
            startSent_ = false;
            lastStop = now;
        }
    }
    return false;
}

void PhoneLink::LeaveSession() {
    Log("session: ending (quit=%d, error=%d)", int(quit_.load()), int(sessionError_));
    // Companion exiting: turn the phone camera off rather than leaving it blocked on a full pipe.
    if (quit_ && startSent_) SendCommand(proto::kCmdStop);
}

void PhoneLink::ResetAfterSession() {
    if (web_) web_->SetBusy(false);
    decoder_.Reset();
    phonePaused_ = phoneStreaming_ = false;
    writer_.SetPhoneState(kPhoneNone);
    status_ = LinkStatus{};
    Publish();
}

void PhoneLink::FeedBytes(const uint8_t* data, size_t size) {
    statBytes_ += size;
    parser_.Feed(data, size, [this](const Packet& p) {
        ++statPackets_;
        HandlePacket(p.type, p.flags, p.ptsUs, p.payload, p.length);
    });
}

void PhoneLink::OnInTransfer(libusb_transfer* t) {
    if (t->status == LIBUSB_TRANSFER_COMPLETED || t->status == LIBUSB_TRANSFER_TIMED_OUT) {
        if (t->actual_length > 0) {
            FeedBytes(t->buffer, size_t(t->actual_length));
        }
        if (!sessionEnding_ && libusb_submit_transfer(t) == LIBUSB_SUCCESS) return;
    } else if (t->status != LIBUSB_TRANSFER_CANCELLED) {
        Log("session: read failed (status %d), phone disconnected?", int(t->status));
        sessionError_ = true;
    }
    --inFlight_;
}

void PhoneLink::LogStats(uint64_t now) {
    if (now - lastStats_ < 3000) return;
    double secs = (now - lastStats_) / 1000.0;
    if (statBytes_ || startSent_) {
        Log("stats: %.0f KB/s, %llu packets, %llu video frames in, %llu decoded, %llu decode errors, "
            "decode+copy avg %.1f ms, app using camera=%d",
            statBytes_ / 1024.0 / secs, statPackets_, statFrames_, statDecoded_, statDecodeErrors_,
            statFrames_ ? statDecodeMs_ / statFrames_ : 0.0, int(writer_.ConsumerActive(kConsumerWindowMs)));
    }
    statDecodeMs_ = 0;
    statBytes_ = statPackets_ = statFrames_ = statDecoded_ = statDecodeErrors_ = 0;
    lastStats_ = now;
}

bool PhoneLink::SendCommand(uint8_t cmd, uint8_t arg) {
    uint8_t packet[proto::kCommandSize];
    proto::MakeCommand(packet, proto::Command(cmd), arg);
    if (webSession_) {
        if (webLingering_) return true; // Nobody to tell: the page gets HELLO (and the rest) when it is back.
        if (web_->SendToPhone(packet, sizeof(packet))) return true;
        Log("send cmd %d to the web page failed", int(cmd));
        return false;
    }
    if (tcp_ != ~uintptr_t(0)) return SendTcp(packet, sizeof(packet));
    int sent = 0;
    int r = libusb_bulk_transfer(handle_, epOut_, packet, sizeof(packet), &sent, 1000);
    if (r != LIBUSB_SUCCESS || sent != int(sizeof(packet))) {
        Log("send cmd %d failed: %s", int(cmd), libusb_error_name(r));
        return false;
    }
    return true;
}

void PhoneLink::HandlePacket(uint8_t type, uint8_t flags, int64_t ptsUs, const uint8_t* payload, uint32_t length) {
    switch (type) {
    case proto::kHello:
        gotHello_ = true;
        Log("phone: hello (protocol %d)", length >= 2 ? int(proto::ReadU16(payload)) : -1);
        if (status_.state == LinkState::Waiting) status_.state = LinkState::Idle;
        writer_.SetPhoneState(kPhoneIdle);
        Publish();
        break;

    case proto::kConfig: {
        if (length < 7) return;
        uint32_t w = proto::ReadU16(payload), h = proto::ReadU16(payload + 2);
        sensorOrientation_ = proto::ReadU16(payload + 4);
        status_.facing = payload[6];
        csd_.assign(payload + 7, payload + length);
        if (!decoder_.Ready() || w != configWidth_ || h != configHeight_) {
            configWidth_ = w;
            configHeight_ = h;
            InitDecoder(w, h, "");
        }
        Log("phone: config %ux%u sensor=%d facing=%d csd=%u bytes", w, h, sensorOrientation_, int(payload[6]), unsigned(csd_.size()));
        waitKeyFrame_ = true;
        status_.width = w;
        status_.height = h;
        Publish();
        break;
    }

    case proto::kFrame: {
        if (phonePaused_) return; // Late frames after a pause must never reach the camera.
        ++statFrames_;
        bool key = flags & proto::kFlagKeyFrame;
        if (!phoneStreaming_) {
            // Frames prove the phone is streaming even if its state/config messages never arrived.
            phoneStreaming_ = true;
            status_.state = LinkState::Streaming;
            writer_.SetPhoneState(kPhoneStreaming);
            Publish();
        }
        if (!decoder_.Ready()) {
            // No config received (some encoders only put SPS/PPS inside key frames). The decoder reads
            // the real size from the SPS and reports a format change, so the size here is only a hint.
            // After a failed init, retry (and log) at most every 2 s instead of on every frame.
            if (lastDecoderFailure_ && NowMs() - lastDecoderFailure_ < 2000) return;
            uint32_t w = configWidth_ ? configWidth_ : 1920, h = configHeight_ ? configHeight_ : 1080;
            if (InitDecoder(w, h, " without config (size is a hint)") < 0) return;
            waitKeyFrame_ = true;
        }
        uint64_t now = NowMs();
        if (waitKeyFrame_ && !key) {
            if (now - lastKeyRequest_ > 1000) {
                keyFrameWanted_ = true; // Sent from the session loop: blocking sends fail inside USB callbacks.
                lastKeyRequest_ = now;
            }
            return;
        }
        long hr; // HRESULT on Windows: negative is a failure.
        const auto t0 = std::chrono::steady_clock::now();
        if (key && !csd_.empty()) {
            // Always hand SPS/PPS to the decoder with a key frame so it can (re)start cleanly.
            scratch_.assign(csd_.begin(), csd_.end());
            scratch_.insert(scratch_.end(), payload, payload + length);
            hr = decoder_.Decode(scratch_.data(), scratch_.size(), ptsUs);
        } else {
            hr = decoder_.Decode(payload, length, ptsUs);
        }
        statDecodeMs_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); // Decode + copy out.
        if (hr < 0) {
            if (statDecodeErrors_++ < 3) Log("decoder: frame (key=%d, %u bytes) failed 0x%08X", int(key), length, unsigned(hr));
            waitKeyFrame_ = true;
            if (key && decoder_.OnGpu()) {
                // A key frame comes with SPS/PPS, so it fails only when the GPU can't decode what the phone sends
                // (some can't do 4K or high frame rates). The CPU can, if slowly.
                gpuDecodeFailed_ = true;
                InitDecoder(configWidth_ ? configWidth_ : 1920, configHeight_ ? configHeight_ : 1080, " after a GPU failure");
            }
        } else if (key) {
            waitKeyFrame_ = false;
        }
        break;
    }

    case proto::kCamera:
        if (proto::ParseCameraInfo(payload, length, &status_.camera)) {
            Log("phone: camera %ux%u @ %u fps, zoom %.2f (%.2f-%.2f), ev %d, flags 0x%02x", status_.camera.width,
                status_.camera.height, status_.camera.actualFps, status_.camera.zoomX100 / 100.0,
                status_.camera.zoomMinX100 / 100.0, status_.camera.zoomMaxX100 / 100.0, status_.camera.ev,
                status_.camera.flags);
            Publish();
        }
        break;

    case proto::kLog:
        Log("phone says: %.*s", int(length), reinterpret_cast<const char*>(payload));
        break;

    case proto::kOrient:
        if (length >= 2) deviceRotation_ = proto::ReadU16(payload) % 360;
        break;

    case proto::kState:
        if (length < 1) return;
        Log("phone: state %d", int(payload[0]));
        phoneStreaming_ = payload[0] == proto::kStateStreaming;
        phonePaused_ = payload[0] == proto::kStatePaused;
        if (length >= 2) status_.facing = payload[1];
        status_.state = payload[0] == proto::kStateStreaming ? LinkState::Streaming
                      : payload[0] == proto::kStateError     ? LinkState::PhoneError
                      : payload[0] == proto::kStatePaused    ? LinkState::Paused
                                                             : LinkState::Idle;
        if (!phoneStreaming_) status_.width = status_.height = 0;
        writer_.SetPhoneState(phoneStreaming_ ? kPhoneStreaming : kPhoneIdle);
        Publish();
        break;
    }
}

long PhoneLink::InitDecoder(uint32_t width, uint32_t height, const char* why) {
    const long hr = decoder_.Init(width, height, [this](const H264Decoder::Nv12View& f) { OnDecodedFrame(f); },
                               !gpuDecodeFailed_);
    Log("decoder: init %ux%u%s on the %s -> 0x%08X", width, height, why, decoder_.OnGpu() ? "GPU" : "CPU", unsigned(hr));
    lastDecoderFailure_ = hr < 0 ? NowMs() : 0; // hr < 0: FAILED(hr)
    return hr;
}

void PhoneLink::OnDecodedFrame(const H264Decoder::Nv12View& frame) {
    // Camera2's JPEG-orientation rule: how far to rotate the sensor image clockwise to look upright.
    int rotation = status_.facing == proto::kFacingFront ? (sensorOrientation_ - deviceRotation_ + 360) % 360
                                                         : (sensorOrientation_ + deviceRotation_) % 360;
    rotation = (rotation + 45) / 90 * 90 % 360;
    ++statDecoded_;
    if (phonePaused_ || lockPaused_) return; // Privacy: nothing reaches the camera while paused or locked.
    writer_.WritePlanes(frame.y, frame.uv, frame.pitch, frame.width, frame.height, uint32_t(rotation), mirror_, fill_);
    lastFrameTick_ = NowMs();
}

void PhoneLink::Publish() {
    status_.lockPaused = lockPaused_;
    status_.wirelessSearch = wireless_;
    status_.scanning = scanUntil_ != 0;
    status_.scansDone = scansDone_;
    status_.nearbyPhones = nearby_.Phones();
    nearbyDirty_ = false;
    status_.pairedCount = pairedCount_;
    status_.pairedName = pairedName_;
    status_.pairedWifi = pairedWifi_;
    if (web_) {
        status_.webReady = web_->Listening();
        const std::string url = web_->PairingUrl();
        status_.webUrl.assign(url.begin(), url.end()); // ASCII.
        status_.webCodeExpires = url.empty() ? 0 : web_->PairingExpires();
        status_.webAddresses = web_->Addresses();
        status_.webAddress = web_->PairingAddress();
        status_.webDevices.clear();
        for (const web::WebDevice& d : web_->Devices()) {
            LinkStatus::WebDeviceRow row;
            row.id = d.id;
            row.name = Wide(d.name);
            row.addedUnix = d.addedUnix;
            row.connected = webSession_ && d.id == webDeviceId_;
            status_.webDevices.push_back(std::move(row));
        }
        webGeneration_ = web_->Generation();
    }
    if (onStatus_) onStatus_(status_);
}

void PhoneLink::SyncLockPaused() {
    if (status_.lockPaused != lockPaused_) {
        Log("Windows %s: camera %s", lockPaused_ ? "locked" : "unlocked", lockPaused_ ? "paused" : "may resume");
        Publish();
    }
}

// While there is no live video, keep the MyCam camera showing a picture instead of black (the camera
// treats frames older than 1.5 s as stale, so refresh a few times a second). The waiting picture carries
// the green marquee bar, redrawn at ~15 fps while an app is watching; the paused picture stays still.
void PhoneLink::WriteStatusFrame(uint64_t now) {
    const bool live = phoneStreaming_ && !phonePaused_ && !lockPaused_ && lastFrameTick_ && now - lastFrameTick_ < 1000;
    const bool paused = phonePaused_ || lockPaused_;
    marqueeActive_ = !live && !paused && !waitingImage_.empty() && writer_.ConsumerActive(kConsumerWindowMs);
    // 55 ms rather than 66: GetTickCount64 moves in ~16 ms steps and the loops wake every 66 ms meanwhile.
    if (now - lastStatusFrame_ < (marqueeActive_ ? 55u : 400u)) return;
    if (live) return;
    const Nv12Image& image = paused ? pausedImage_ : waitingImage_;
    if (image.empty()) return;
    lastStatusFrame_ = now;
    if (!paused) {
        DrawMarquee(waitingFrame_, waitingImage_, double(now % 86400000ull) / 1000.0);
        writer_.Write(waitingFrame_.data.data(), waitingFrame_.width, waitingFrame_.height, 0, false);
        return;
    }
    writer_.Write(image.data.data(), image.width, image.height, 0, false);
}

} // namespace mycam
