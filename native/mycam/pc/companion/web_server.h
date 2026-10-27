#pragma once
// The iPhone web camera's server (docs/AIYAN-IPHONE.md part 2): HTTPS on port 47900 serving the page from
// the exe's resources, POST /pair (QR code -> device token) and one WebSocket endpoint, /ws.
//
// Single-threaded and polled: PhoneLink's worker thread calls Poll() from its loops, the same thread that
// runs the session loop, so nothing here needs a lock. A page's WebSocket is "the phone" of a web session:
// its binary messages are MyCam packets (one per message) and go to the handler PhoneLink sets.

#include <stdint.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "web_proto.h"

namespace mycam {

namespace tls { class Credentials; }

class WebServer {
public:
    WebServer();
    ~WebServer();
    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    // Makes (or loads) the certificate and starts listening. Logs why it couldn't.
    bool Start();
    bool Listening() const;

    // Accepts connections, serves pages and handshakes, and reads the phone's messages, waiting up to
    // timeoutMs for something to happen.
    void Poll(uint32_t timeoutMs);

    // --- The phone (a page with an open /ws) ---------------------------------------------------------
    // A phone's session is running: new pages get "busy" and retry, and one waiting to be picked up is told
    // to try again later. `except` (a device id) may still connect: the web session's own iPhone, coming
    // back after iOS dropped its connection.
    void SetBusy(bool busy, const std::string& except = std::string());
    // A page opened /ws and waits for a session.
    bool PhonePending() const;
    // Makes the waiting page the session's phone: which device, its name, and how it is connected ("Wi-Fi"...).
    bool TakePhone(std::string* id, std::string* name, std::string* link);
    bool PhoneConnected() const;
    bool SendToPhone(const uint8_t* data, size_t size);
    void EndPhone();
    void SetPhoneHandler(std::function<void(const uint8_t*, size_t)> handler) { onPhone_ = std::move(handler); }

    // --- Pairing (the settings window's "Connect an iPhone") ----------------------------------------
    // Starts pairing with a new one-time code, for the QR code at `ipv4` (0: the best address). Pairing
    // stays on until StopPairing(), with a new code whenever one is used or runs out.
    void StartPairing(uint32_t ipv4);
    void StopPairing();
    bool Pairing() const { return pairing_; }
    // https://<address>:47900/?t=<code>&pc=<PC id> while pairing, else "". Holds the code: never log it.
    std::string PairingUrl() const;
    // The PC id (32 hex digits) the QR code carries, so the Android app knows which PC it scanned
    // (PROTOCOL.md "QR pairing"). Set once by PhoneLink, which owns the id.
    void SetPcId(const std::string& hex) { pcId_ = hex; }
    // The Android app pairs over Wi-Fi with the QR code's one-time code (PROTOCOL.md "QR pairing"): the code
    // while it is valid ("" otherwise), and "used" after a successful pairing (the QR code shows a new one).
    std::string CurrentPairingCode() const;
    void PairingCodeUsed();
    // An Android pairing has taken the current code and is finishing its handshake: POST /pair is refused
    // meanwhile, so one displayed code can't also become an iPhone's device token. Claim() is false when
    // somebody already holds it. Release() when the handshake didn't work out; PairingCodeUsed() when it did.
    bool ClaimPairingCode();
    void ReleasePairingCode() { codeClaimed_ = false; }
    // A page that paired with POST /pair has just opened its session: its device id (once; "" otherwise), so
    // PhoneLink can say "paired" rather than only "connected".
    std::string TakeNewlyPaired(const std::string& deviceId);
    uint64_t PairingExpires() const { return book_.PairingCodeExpires(); } // GetTickCount64 time.
    std::vector<web::NetAddress> Addresses() const { return addresses_; }
    uint32_t PairingAddress() const { return pairingIp_; }
    // Bumped whenever the code, the device list or the phone changes, so PhoneLink knows to publish.
    uint32_t Generation() const { return generation_; }

    std::vector<web::WebDevice> Devices() const;
    void Forget(const std::string& id); // Also closes that device's connection.

private:
    struct Conn;
    void Accept();
    void ReadFrom(Conn& c);
    void HandleHttp(Conn& c);
    void HandleRequest(Conn& c, const web::HttpRequest& r, const std::string& body);
    void HandleUpgrade(Conn& c, const web::HttpRequest& r);
    void HandlePair(Conn& c, const web::HttpRequest& r, const std::string& body);
    void HandleWs(Conn& c, const uint8_t* data, size_t size);
    void SendRaw(Conn& c, const uint8_t* data, size_t size);
    void SendResponse(Conn& c, int code, const char* reason, const std::string& type, const std::string& body,
                      const std::string& extraHeaders = "");
    void Flush(Conn& c);
    void Close(Conn& c, const char* why);
    void Sweep(uint64_t now);
    bool TryListen();
    void RefreshAddresses();
    void MakeCode();
    void LoadDevices();
    void SaveDevice(const web::WebDevice& d);
    std::string LinkOf(uint32_t localIp) const;

    uintptr_t listen_;
    std::unique_ptr<tls::Credentials> cred_;
    uint64_t lastListenTry_ = 0;
    bool listenFailed_ = false; // Logged once, not every retry.
    std::vector<std::unique_ptr<Conn>> conns_;
    std::function<void(const uint8_t*, size_t)> onPhone_;
    web::TokenBook book_;
    std::vector<web::NetAddress> addresses_;
    bool busy_ = false;
    std::string busyExcept_;
    bool pairing_ = false;
    std::string pcId_;           // In the QR code (`pc`).
    bool codeClaimed_ = false;   // An Android QR pairing is using the current code right now.
    std::string newlyPairedId_;  // The device the last POST /pair added, until its session starts.
    uint32_t pairingIp_ = 0;
    uint32_t generation_ = 0;
    uint64_t lastPing_ = 0;
};

} // namespace mycam
