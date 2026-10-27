#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "discovery.h"
#include "packet_parser.h"
#include "link_status.h"
#include "platform_video.h"
#include "protocol.h"
#include "status_images.h"
#include "web_server.h"

struct libusb_context;
struct libusb_device;
struct libusb_device_handle;
struct libusb_transfer;
struct sockaddr_in;

namespace mycam {

namespace wifi { class RecordKey; }

// Forgets every phone paired over Wi-Fi (they must pair again with a code). Settings window.
void ForgetPairedPhones();
// This PC's Wi-Fi id (made once), as 32 hex digits: in CLIENT_HELLO and the QR code.
std::string PcIdHex();

// How long the phone gives its user to answer a pairing request (the PC waits a little longer, see
// kUserTimeoutMs in phone_link_wifi.cpp). The pairing dialog counts this down.
constexpr uint64_t kPairingAnswerMs = 60000;

// Owns all USB traffic. Run() blocks on the calling (worker) thread until Quit() is called.
class PhoneLink {
public:
    using StatusCallback = std::function<void(const LinkStatus&)>;

    // onNeedDriver: a phone in accessory mode has no driver yet; the UI should run the elevated binder.
    PhoneLink(StatusCallback onStatus, std::function<void()> onNeedDriver)
        : onStatus_(std::move(onStatus)), onNeedDriver_(std::move(onNeedDriver)) {}

    void Run();
    void Quit() { quit_ = true; }

    // Thread-safe controls from the UI thread.
    void RequestFacing(int facing) { pendingFacing_ = facing; }
    void SetMirror(bool mirror) { mirror_ = mirror; }
    void SetFill(bool fill) { fill_ = fill; }
    // Queues a v3 camera command (quality, fps, zoom, exposure, torch, focus) for the phone.
    void RequestCommand(uint8_t cmd, uint8_t arg) {
        std::lock_guard<std::mutex> lock(commandLock_);
        commands_.push_back({cmd, arg});
    }
    void RequestReconnect() { reconnect_ = true; }
    // Wireless mode (beta): also look for phones on the local network and connect over Wi-Fi.
    void SetWireless(bool on) { wireless_ = on; }
    // "Scan for phones": broadcast a probe now and list every phone that answers for kScanWindowMs, even
    // while "Find phones on Wi-Fi" is off (the setting doesn't change, and nothing connects by itself then).
    void ScanForPhones() { scanRequest_ = true; }
    // Runs one Wi-Fi session to a phone from the scan list (pairing with a code if it's new), even while
    // "Find phones on Wi-Fi" is off. A plugged-in phone still wins. ipv4 in network order.
    void ConnectTo(uint32_t ipv4) {
        connectAsked_ = true; // Somebody pressed Connect here, as opposed to a phone asking over the network.
        connectRequest_ = ipv4;
    }
    static constexpr uint64_t kScanWindowMs = 6000;
    // A code pairing (6 digits) needs this PC's user too (1.5, PROTOCOL.md "The PC asks too"): Allow lets it
    // go ahead once the phone has allowed it as well; "Don't allow" refuses it, and auto-connect leaves that
    // phone alone until the companion restarts. No effect when not pairing.
    void AllowPairing() { allowPairing_ = true; }
    void CancelPairing() { cancelPairing_ = true; }
    // "Connect an iPhone": show a QR code for `ipv4` (0: the best address) until StopWebPairing().
    void StartWebPairing(uint32_t ipv4) { webPairRequest_ = int64_t(ipv4); }
    void StopWebPairing() { webPairRequest_ = -2; }
    // Forgets a remembered iPhone (it has to scan the QR code again) and closes its connection.
    void ForgetWebDevice(const std::string& id) {
        std::lock_guard<std::mutex> lock(commandLock_);
        webForget_.push_back(id);
    }
    // Pause / resume the camera (the phone stores the choice). Needs a connected phone.
    void RequestPause(bool pause) { pendingPause_ = pause ? 1 : 0; }
    // Windows locked / asleep: keep the phone camera off until unlocked. Independent of RequestPause.
    void SetLockPaused(bool locked) { lockPaused_ = locked; }
    // Pictures shown on the MyCam camera while there is no live video. Call before Run().
    void SetStatusImages(Nv12Image paused, Nv12Image waiting) {
        pausedImage_ = std::move(paused);
        waitingImage_ = std::move(waiting);
        waitingFrame_ = waitingImage_;
    }

private:
    // The USB backend (phone_link_usb_win.cpp: UsbDk + WinUSB; phone_link_usb_posix.cpp: plain libusb).
    bool InitUsb();                                  // Logs how it went; false: no USB phones.
    void ExitUsb();
    bool UsbNeedsDriver();                           // A phone in accessory mode can't be opened yet.
    void RunAccessory(libusb_device* accessory);     // One USB session with a phone in accessory mode.
    const char* BackendName(libusb_context* ctx) const;

    void ScanOnce();
    bool LooksLikeAndroid(libusb_device* dev);
    void SwitchToAccessory(libusb_device* dev);
    void RunSession(libusb_device* dev, libusb_context* ctx);
    libusb_device* FindAccessory(libusb_context* ctx);

    // Shared by USB and Wi-Fi sessions (all on the worker thread).
    void BeginSession(bool wireless, const std::string& phoneName);
    bool SessionLoop(const std::function<void()>& pump); // True if a reconnect was asked for.
    void LeaveSession();      // Before the link closes: STOP the camera if the companion is quitting.
    void ResetAfterSession(); // After the link closed.
    void FeedBytes(const uint8_t* data, size_t size);

    // Wireless (Wi-Fi) transport: UDP discovery, then the same protocol over TCP. PROTOCOL.md "Wireless".
    void NetScanOnce();
    bool EnsureUdp();
    void BroadcastProbe();
    // Discovery without connecting: starts a requested scan, probes while one runs, reads the answers and
    // ends the scan window. Safe inside a session (the SessionLoop calls it).
    void PollDiscovery(uint64_t now);
    void ReadAnswers(uint64_t now);
    void RefreshNearby();            // Paired / connected marks, then Publish() if the list changed.
    bool WifiAllowed() const { return wireless_ || manualSession_; } // A Wi-Fi session may go on.
    void RunManualConnect(uint32_t ip);
    // QR pairing for the Android app (PROTOCOL.md "QR pairing"): probes while the QR code shows, then the
    // session a phone's "MYCAM#1" asked for (qrRequest_).
    void ProbeForQr(uint64_t now);
    void OnQrInvite(const sockaddr_in& from, uint16_t port, const std::string& proof, const std::string& name);
    void RunQrConnect();
    // A new pairing worked (QR, code or iPhone): tells the UI, which says so and shows the camera.
    void NotePaired(const std::string& name, bool wifi);
    // How a Wi-Fi session ended, for when to try that phone again.
    // RefusedHere: this PC's user said no to a code pairing, so auto-connect leaves that phone alone until the
    // companion restarts. TimedOutHere: nobody answered here, which is not a refusal: that phone is asked
    // again after the usual back-off, so a dialog someone simply missed doesn't lose the phone.
    enum class WifiEnd { Ran, Refused, RefusedHere, TimedOutHere, NeedsPairing, Failed };
    // qrCode: the one-time code a phone proved it scanned; the handshake then runs in QR pairing mode.
    WifiEnd RunTcpSession(uintptr_t socket, const std::string& phoneName, bool forcePair,
                          const std::vector<uint8_t>* qrCode = nullptr);
    // Pairing / paired proof / QR pairing, then sets wifiTx_/wifiRx_.
    WifiEnd Handshake(uintptr_t socket, bool forcePair, const std::vector<uint8_t>* qrCode);
    bool ReceiveRecords(uintptr_t socket);               // Decrypts what arrived and feeds the parser.
    bool UsbPhoneArrived(); // During a Wi-Fi session: a phone was plugged in (the cable takes over).
    bool SendTcp(const uint8_t* data, size_t size);

    // iPhone web camera: the page's server (web_server.h), polled from every loop on this thread.
    void PollWeb(uint32_t waitMs);   // Serves pages and handles the window's requests; waits up to waitMs.
    void WebScanOnce();              // Runs a web session if a page is waiting for one.

    bool SendCommand(uint8_t cmd, uint8_t arg = 0);
    void HandlePacket(uint8_t type, uint8_t flags, int64_t ptsUs, const uint8_t* payload, uint32_t length);
    long InitDecoder(uint32_t width, uint32_t height, const char* why); // HRESULT on Windows.
    void OnDecodedFrame(const H264Decoder::Nv12View& frame);
    void Publish();
    void OnInTransfer(libusb_transfer* transfer);
    void LogStats(uint64_t now);
    void SyncLockPaused();
    void WriteStatusFrame(uint64_t now);
    // How long the loops may wait before the next status frame is due (shorter while the marquee runs).
    uint32_t StatusWaitMs() const { return marqueeActive_ ? 66 : 100; }

    StatusCallback onStatus_;
    std::function<void()> onNeedDriver_;
    uint64_t lastDriverRequest_ = 0;
    std::atomic<bool> quit_{false};
    std::atomic<int> pendingFacing_{-1};
    std::atomic<bool> mirror_{false};
    std::atomic<bool> fill_{false};
    std::mutex commandLock_;
    std::vector<std::pair<uint8_t, uint8_t>> commands_;
    std::atomic<bool> reconnect_{false};
    std::atomic<int> pendingPause_{-1};
    std::atomic<bool> lockPaused_{false};
    Nv12Image pausedImage_, waitingImage_;
    uint64_t lastStatusFrame_ = 0;
    Nv12Image waitingFrame_;     // waitingImage_ with the marquee drawn in (worker thread only).
    bool marqueeActive_ = false; // The animated waiting picture is on screen: write it at ~15 fps.

    std::atomic<bool> wireless_{false};
    std::atomic<bool> cancelPairing_{false};
    std::atomic<bool> allowPairing_{false};
    bool usbReady_ = false;
    uintptr_t udp_ = ~uintptr_t(0);       // SOCKET for discovery (INVALID_SOCKET when closed).
    uintptr_t tcp_ = ~uintptr_t(0);       // SOCKET of the current Wi-Fi session.
    uint64_t lastProbe_ = 0;
    struct NetPhone { uint16_t port = 0; std::string name; uint64_t lastSeen = 0, retryAfter = 0; bool forcePair = false; };
    // Encrypted Wi-Fi session (PROTOCOL.md "Wireless security"): record keys per direction, received bytes.
    std::shared_ptr<wifi::RecordKey> wifiTx_, wifiRx_;
    std::vector<uint8_t> wifiRxBuf_;
    std::map<uint32_t, NetPhone> netPhones_; // By IPv4 address (network order), from discovery answers.
    std::atomic<bool> scanRequest_{false};
    std::atomic<uint32_t> connectRequest_{0};
    std::atomic<bool> connectAsked_{false}; // connectRequest_ came from Connect here, not from the network.
    bool manualSession_ = false;  // The current Wi-Fi session was asked for with Connect (search may be off).
    // A phone that scanned the QR code asked for a session ("MYCAM#1", PROTOCOL.md "QR pairing"): where to
    // connect and the code it proved it has (ip 0: nothing asked).
    struct QrRequest { uint32_t ip = 0; uint16_t port = 0; std::string name; std::vector<uint8_t> code; } qrRequest_;
    uint64_t lastQrProbe_ = 0;    // Probes go out every 2 s while the QR code shows.
    uint64_t lastSweepNote_ = 0;  // Last time the log said this network is too big to probe host by host.
    // Successful new pairings so far (QR, code or iPhone), so the UI can say "Paired with ...".
    uint32_t pairedCount_ = 0;
    std::wstring pairedName_;
    bool pairedWifi_ = false;
    uint64_t scanUntil_ = 0;      // End of the running scan window (0: none).
    uint64_t scanEndedAt_ = 0;    // When the last scan window closed: the discovery socket lingers after it.
    uint32_t scansDone_ = 0;
    uint32_t sessionIp_ = 0;      // Peer of the current Wi-Fi session (0: none).
    bool nearbyDirty_ = false;    // nearby_ changed since the last Publish().
    NearbyList nearby_;           // What the window lists.

    std::unique_ptr<WebServer> web_;
    bool webSession_ = false;     // The current session's phone is a web page.
    bool webLingering_ = false;   // ...whose connection iOS dropped while it was paused: waiting for it.
    std::string webDeviceId_;     // ...and which remembered device it is.
    uint32_t webGeneration_ = ~0u;
    std::atomic<int64_t> webPairRequest_{-1}; // -1 nothing, -2 stop, else start (IPv4).
    std::vector<std::string> webForget_;     // Guarded by commandLock_.

    libusb_context* ctx_ = nullptr;       // UsbDk backend: switches phones into accessory mode.
    libusb_context* winusbCtx_ = nullptr; // WinUSB backend: streams from accessory-mode phones (WinUSB-bound).
    libusb_context* sessionCtx_ = nullptr;
    std::set<std::string> probed_;

    // Session state (worker thread only).
    libusb_device_handle* handle_ = nullptr;
    unsigned char epIn_ = 0, epOut_ = 0;
    PacketParser parser_;
    bool gotHello_ = false;
    bool startSent_ = false;
    bool phoneStreaming_ = false;
    bool phonePaused_ = false;
    bool waitKeyFrame_ = true;
    std::vector<uint8_t> csd_;
    std::vector<uint8_t> scratch_;
    uint32_t configWidth_ = 0, configHeight_ = 0;
    int sensorOrientation_ = 0;
    int deviceRotation_ = 0;
    uint64_t lastKeyRequest_ = 0;
    uint64_t lastFrameTick_ = 0;

    // Queued bulk-IN reads (never time out, so no data is lost to cancellation).
    int inFlight_ = 0;
    bool sessionEnding_ = false;
    bool sessionError_ = false;
    bool keyFrameWanted_ = false;

    // Diagnostics, logged every few seconds.
    uint64_t statBytes_ = 0, statPackets_ = 0, statFrames_ = 0, statDecoded_ = 0, statDecodeErrors_ = 0;
    double statDecodeMs_ = 0; // Total decode time in the current stats window (latency measurement).
    uint64_t lastStats_ = 0;

    H264Decoder decoder_;
    bool gpuDecodeFailed_ = false; // Once the GPU has failed a key frame, the CPU decodes for the rest of this run.
    uint64_t lastDecoderFailure_ = 0; // NowMs() of the last failed decoder init (0: none), to back off retries.
    FrameWriter writer_;
    LinkStatus status_;
};

} // namespace mycam
