// Wireless transport (IMPROVEMENTS.md 11): finds MyCam phones on the local network, pairs with them once
// (6-digit code compared on both screens), then runs the normal protocol over TCP inside AES-256-GCM
// records. See protocol/PROTOCOL.md "Wireless transport" and "Wireless security".
//
// The PC only ever sends (a UDP broadcast, then a TCP connect), so Windows never asks for a firewall rule:
// the phone's unicast answer to a broadcast is allowed back in as a response.

#include <string.h>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "log.h"
#include "net.h"
#include "phone_link.h"
#include "platform.h"
#include "wifi_crypto.h"

namespace mycam {

namespace {

using wifi::Bytes;

// --- Handshake messages: u16 length (type + body) | u8 type | body -----------------------------------
constexpr uint8_t kMsgClientHello = 1, kMsgServerHello = 2, kMsgNonceA = 3, kMsgNonceB = 4, kMsgPcFinished = 5,
                  kMsgPhoneFinished = 6, kMsgReject = 7;
constexpr uint8_t kModePaired = 0, kModePairing = 1, kModeQr = 2;
constexpr uint8_t kFlagForcePair = 0x01, kFlagQr = 0x02; // kFlagQr: PROTOCOL.md "QR pairing".
constexpr uint8_t kRejectRefused = 1;
constexpr uint64_t kStepTimeoutMs = 10000;
constexpr uint64_t kUserTimeoutMs = kPairingAnswerMs + 15000; // The phone gives its user 60 s.

// --- Paired phones: the store's PairedPhones/<phone id> = Key (protected), Name ---------------------------
// (HKCU\Software\MyCam\PairedPhones on Windows.)
constexpr char kPairedKey[] = "PairedPhones";

std::string PairedPath(const Bytes& phoneId) { return std::string(kPairedKey) + "/" + web::Hex(phoneId.data(), phoneId.size()); }

// This PC's random id, made once.
Bytes PcId() {
    Bytes id;
    if (StoreGetBytes("", "PcId", &id) && id.size() == 16) return id;
    id = wifi::Random(16);
    StoreSetBytes("", "PcId", id);
    return id;
}

bool LoadPairKey(const Bytes& phoneId, Bytes* key) {
    Bytes stored;
    if (!StoreGetBytes(PairedPath(phoneId), "Key", &stored) || !Unprotect(stored, key)) return false;
    return key->size() == 32;
}

void SavePairKey(const Bytes& phoneId, const Bytes& key, const std::string& name) {
    Bytes stored;
    if (!Protect(key, &stored)) return;
    StoreSetBytes(PairedPath(phoneId), "Key", stored);
    StoreSetString(PairedPath(phoneId), "Name", name);
}

void ForgetPairKey(const Bytes& phoneId) { StoreDelete(PairedPath(phoneId)); }

// Names of the phones this PC holds a pairing key for. Discovery answers carry only a name (no phone id),
// so "Paired" in the scan list is matched by name: best effort.
std::vector<std::string> PairedNames() {
    std::vector<std::string> names;
    for (const std::string& sub : StoreList(kPairedKey)) {
        std::string name;
        if (StoreGetString(std::string(kPairedKey) + "/" + sub, "Name", &name)) names.push_back(name);
    }
    return names;
}

// --- Socket helpers (the socket is non-blocking) --------------------------------------------------

bool SendAll(net::Socket s, const uint8_t* data, size_t size, uint64_t timeoutMs = 1000) {
    size_t sent = 0;
    const uint64_t deadline = NowMs() + timeoutMs;
    while (sent < size) {
        int n = send(s, reinterpret_cast<const char*>(data + sent), int(size - sent), 0);
        if (n > 0) { sent += size_t(n); continue; }
        if (!net::WouldBlock() || NowMs() > deadline) return false;
        fd_set wr;
        FD_ZERO(&wr);
        FD_SET(s, &wr);
        timeval tv = {0, 50000};
        select(int(s) + 1, nullptr, &wr, nullptr, &tv);
    }
    return true;
}

// Reads exactly `size` bytes, giving up after `timeoutMs` or when `keepWaiting` says so (it runs about
// every 100 ms, so the status pictures keep updating while the phone's user decides).
bool RecvExact(net::Socket s, uint8_t* out, size_t size, uint64_t timeoutMs, const std::function<bool()>& keepWaiting) {
    size_t got = 0;
    const uint64_t deadline = NowMs() + timeoutMs;
    while (got < size) {
        int n = recv(s, reinterpret_cast<char*>(out + got), int(size - got), 0);
        if (n > 0) { got += size_t(n); continue; }
        if (n == 0 || !net::WouldBlock()) return false;
        if (NowMs() > deadline || !keepWaiting()) return false;
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        timeval tv = {0, 100000};
        select(int(s) + 1, &rd, nullptr, nullptr, &tv);
    }
    return true;
}

Bytes Frame(uint8_t type, const Bytes& body) {
    Bytes f = {uint8_t((body.size() + 1) >> 8), uint8_t(body.size() + 1), type};
    f.insert(f.end(), body.begin(), body.end());
    return f;
}

Bytes NameField(const std::string& name) {
    Bytes b = {uint8_t(std::min<size_t>(name.size(), 64))};
    b.insert(b.end(), name.begin(), name.begin() + b[0]);
    return b;
}

constexpr uint16_t kDiscoveryPort = 47801;
constexpr char kAsk[] = "MYCAM?1";
constexpr uint64_t kProbeEveryMs = 2000;
constexpr uint64_t kForgetAfterMs = 6000;      // A phone that stopped answering is gone.
constexpr uint64_t kRetryRefusedMs = 120000;   // The phone said no (or nobody answered): don't nag it.
constexpr uint64_t kRetryLostMs = 2000;        // The connection dropped after working: try again soon.
constexpr uint64_t kRetryUnreachableMs = 10000;
constexpr uint64_t kNever = ~0ull;             // retryAfter for a phone this PC's user refused to pair.
// A phone may ask this PC to connect for a short while after a scan ends, so the discovery socket stays
// open that long: the phone lists this PC for 6 s after its last probe, and taps take a moment.
constexpr uint64_t kInviteGraceMs = 10000;

// Directed broadcast address of every IPv4 network this PC is on (plus the generic one), so the probe
// goes out on Wi-Fi even when Ethernet or a VPN is the default route.
std::vector<uint32_t> BroadcastAddresses() {
    std::vector<uint32_t> out = {htonl(INADDR_BROADCAST)};
    for (const Adapter& a : UpAdapters()) {
        if (a.ifType == 24 /* loopback */ || a.prefix == 0 || a.prefix >= 32) continue;
        const uint32_t mask = htonl(~0u << (32 - a.prefix));
        out.push_back(a.ipv4 | ~mask);
    }
    return out;
}

// Connects with a timeout, then leaves the socket non-blocking.
net::Socket ConnectTcp(uint32_t ip, uint16_t port, int timeoutMs) {
    const net::Socket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == net::kInvalid) return s;
    net::SetNonBlocking(s);
    int noDelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    // Large receive window: a 4K key frame can be several hundred KB.
    int rcv = 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcv), sizeof(rcv));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = ip;
    connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    fd_set wr, ex;
    FD_ZERO(&wr); FD_ZERO(&ex);
    FD_SET(s, &wr); FD_SET(s, &ex);
    timeval tv = {timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    int error = 0;
    socklen_t errorLen = sizeof(error);
    if (select(int(s) + 1, nullptr, &wr, &ex, &tv) != 1 || !FD_ISSET(s, &wr) ||
        // Linux reports a refused connect as writable: the socket error says whether it worked.
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &errorLen) != 0 || error != 0) {
        net::Close(s);
        return net::kInvalid;
    }
    return s;
}

std::string IpText(uint32_t ip) {
    char text[INET_ADDRSTRLEN] = {};
    in_addr a;
    a.s_addr = ip;
    inet_ntop(AF_INET, &a, text, sizeof(text));
    return text;
}

} // namespace

bool PhoneLink::EnsureUdp() {
    if (udp_ != net::kInvalid) return true;
    const net::Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == net::kInvalid) return false;
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
    net::SetNonBlocking(s);
    sockaddr_in any = {};
    any.sin_family = AF_INET; // Any address, a free port.
    if (bind(s, reinterpret_cast<sockaddr*>(&any), sizeof(any)) != 0) {
        net::Close(s);
        return false;
    }
    udp_ = s;
    return true;
}

void PhoneLink::BroadcastProbe() {
    const std::string message = std::string(kAsk) + " " + ComputerName();
    for (uint32_t addr : BroadcastAddresses()) {
        sockaddr_in to = {};
        to.sin_family = AF_INET;
        to.sin_port = htons(kDiscoveryPort);
        to.sin_addr.s_addr = addr;
        sendto(udp_, message.data(), int(message.size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
    }
}

// Answers: "MYCAM!1 <tcp port> <phone name>". Fills the scan list, and the auto-connect table while
// "Find phones on Wi-Fi" is on.
//
// Also "MYCAM+1 <tcp port> <phone name>": that phone's user picked this PC in its own list and wants a
// session now (PROTOCOL.md "The phone asks for a session"). It is treated exactly like Connect here, so a
// pairing can be started from either screen. It is ignored during a session, and while a connect is already
// queued, so the phone's resends don't pile up.
void PhoneLink::ReadAnswers(uint64_t now) {
    if (udp_ == net::kInvalid) return;
    char buf[512];
    for (int i = 0; i < 64; ++i) {
        sockaddr_in from = {};
        socklen_t fromLen = sizeof(from);
        int n = recvfrom(udp_, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n <= 0) break;
        const std::string text(buf, size_t(n));
        uint16_t port = 0;
        std::string name, proof;
        const bool qr = ParseDiscoveryQrRequest(text, &port, &proof, &name);
        const bool invite = !qr && ParseDiscoveryConnectRequest(text, &port, &name);
        if (!qr && !invite && !ParseDiscoveryReply(text, &port, &name)) continue;
        const uint32_t ip = from.sin_addr.s_addr;
        const std::string ipText = IpText(ip);
        // Whoever sent this chose the name, and it reaches the scan list and the pairing dialog.
        if (!name.empty()) name = web::CleanName(name, ipText);
        if (qr) {
            OnQrInvite(from, port, proof, name);
            continue;
        }
        if (nearby_.Seen(ip, ipText, port, name, now)) nearbyDirty_ = true;
        if (invite) {
            const std::string shown = name.empty() ? ipText : name;
            // During a session the cable or the current phone owns the link; the phone gives up by itself.
            if (sessionIp_ != 0 || handle_ != nullptr) {
                Log("wifi: %s asked to connect, but a session is running", shown.c_str());
                continue;
            }
            if (connectRequest_ != 0) continue; // Already queued (one of the phone's resends).
            Log("wifi: %s (%s:%u) asked this PC to connect", shown.c_str(), ipText.c_str(), unsigned(port));
            // The phone asked, so any back-off from an earlier refusal doesn't apply any more.
            if (auto it = netPhones_.find(ip); it != netPhones_.end()) it->second.retryAfter = 0;
            connectRequest_ = ip;
            continue;
        }
        if (!wireless_) continue;
        NetPhone& phone = netPhones_[ip];
        if (phone.lastSeen == 0) {
            phone.name = name.empty() ? ipText : name;
            Log("wifi: found %s at %s:%u", phone.name.c_str(), ipText.c_str(), unsigned(port));
        }
        phone.port = port;
        phone.lastSeen = now;
    }
}

void PhoneLink::RefreshNearby() {
    if (nearby_.SetConnected(status_.wireless ? sessionIp_ : 0)) nearbyDirty_ = true;
    if (!nearbyDirty_) return;
    const std::vector<std::string> paired = PairedNames();
    nearby_.UpdatePaired([&](const std::string& name) { return std::find(paired.begin(), paired.end(), name) != paired.end(); });
    Publish();
}

void PhoneLink::PollDiscovery(uint64_t now) {
    if (scanRequest_.exchange(false)) {
        if (EnsureUdp()) {
            if (!scanUntil_) Log("wifi: scanning for phones (Find phones on Wi-Fi is %s)", wireless_ ? "on" : "off");
            nearby_.ClearForScan();
            scanUntil_ = now + kScanWindowMs;
            lastProbe_ = 0; // Probe right away.
            nearbyDirty_ = true;
        } else {
            Log("wifi: can't scan: no UDP socket (%d)", net::LastError());
            ++scansDone_;
            nearbyDirty_ = true;
        }
    }
    if (scanUntil_ && now - lastProbe_ >= kProbeEveryMs) {
        lastProbe_ = now;
        BroadcastProbe();
    }
    ProbeForQr(now); // While the QR code shows, so a phone that scans it can answer.
    // Read whenever the socket is open, not only during a scan: a phone's "connect to me" may arrive just
    // after the window closed, and a phone that answered may be asking right now.
    ReadAnswers(now);
    if (scanUntil_ && now >= scanUntil_) {
        scanUntil_ = 0;
        scanEndedAt_ = now;
        ++scansDone_;
        nearbyDirty_ = true;
        Log("wifi: scan finished, %u phone(s) answered", unsigned(nearby_.Phones().size()));
    }
    RefreshNearby();
}

// Connect, from the scan list: one Wi-Fi session to that phone, whatever "Find phones on Wi-Fi" says.
void PhoneLink::RunManualConnect(uint32_t ip) {
    // Which side asked, taken before any early return so the flag never carries into the next connect.
    const bool askedHere = connectAsked_.exchange(false);
    const NearbyPhone* found = nearby_.Find(ip);
    if (!found) {
        Log("wifi: connect: %s isn't in the scan list any more", IpText(ip).c_str());
        return;
    }
    const std::string name = found->name;
    const uint16_t port = found->port;
    // Say which side asked: a "MYCAM+1" from the network looks exactly like Connect from here otherwise, and
    // anyone reading the log afterwards would think somebody at this PC started it.
    Log("wifi: connecting to %s (%s:%u), asked for %s", name.c_str(), IpText(ip).c_str(), unsigned(port),
        askedHere ? "on the PC" : "by the phone over the network");
    manualSession_ = true;
    WifiEnd end = WifiEnd::Failed;
    for (int attempt = 0; attempt < 2 && !quit_; ++attempt) {
        net::Socket s = ConnectTcp(ip, port, 3000);
        if (s == net::kInvalid) {
            Log("wifi: can't connect to %s (%s:%u)", name.c_str(), IpText(ip).c_str(), unsigned(port));
            break;
        }
        end = RunTcpSession(uintptr_t(s), name, attempt > 0);
        if (end != WifiEnd::NeedsPairing) break; // Else: this PC forgot the phone; pair again with a code.
    }
    manualSession_ = false;
    // Auto-connect (search on) mustn't ask a phone again right after a user said no, here or on the phone.
    if (wireless_ && (end == WifiEnd::Refused || end == WifiEnd::TimedOutHere))
        netPhones_[ip].retryAfter = NowMs() + kRetryRefusedMs;
    if (wireless_ && end == WifiEnd::RefusedHere) netPhones_[ip].retryAfter = kNever;
    lastProbe_ = 0;
}

// --- QR pairing for the Android app (PROTOCOL.md "QR pairing") ---------------------------------------------
//
// Nothing connects into the PC: while the QR code shows, it probes (broadcast, plus unicast to the /24 the
// code points at, for networks that drop broadcasts). A phone that scanned the code answers a probe with
// "MYCAM#1 <port> <proof> <name>", and the PC connects out to it as to any phone, with the code mixed into the
// handshake's keys. So it works on any network, "Public" ones included, with no firewall rule.

void PhoneLink::ProbeForQr(uint64_t now) {
    if (!web_ || !web_->Pairing() || now - lastQrProbe_ < kProbeEveryMs || !EnsureUdp()) return;
    lastQrProbe_ = now;
    // The broadcast is shared with the ordinary probe timer, so showing the QR code doesn't double the rate.
    if (now - lastProbe_ >= kProbeEveryMs) {
        lastProbe_ = now;
        BroadcastProbe();
    }
    const uint32_t pairingIp = web_->PairingAddress();
    const uint32_t self = ntohl(pairingIp);
    if (!self) return;
    // The unicast copies, for networks that drop broadcasts. The subnet is whatever the adapter reports, not
    // an assumed /24: on a /16 a /24's worth of guesses misses almost every host. Sweeping a subnet that big
    // isn't practical either (a /16 would be 65k datagrams every 2 s), so there the broadcast is all there is.
    uint8_t prefix = 0;
    for (const web::NetAddress& a : web_->Addresses())
        if (a.ipv4 == pairingIp) prefix = a.prefix;
    if (prefix > 30) return;  // A /31 or /32 has no other host to ask.
    if (prefix < 24) {        // Too wide to ask one by one (0: the adapter didn't say).
        constexpr uint64_t kSweepNoteEveryMs = 60000;
        if (prefix && now - lastSweepNote_ >= kSweepNoteEveryMs) {
            lastSweepNote_ = now;
            Log("wifi: this network is a /%u, too big to probe address by address; relying on the broadcast. "
                "If the phone can't find this PC, put both on the same smaller network.", unsigned(prefix));
        }
        return;
    }
    const uint32_t mask = ~0u << (32 - prefix), net = self & mask, last = net | ~mask;
    const std::string message = std::string(kAsk) + " " + ComputerName();
    for (uint32_t host = net + 1; host < last; ++host) { // Skip the network and broadcast addresses.
        if (host == self) continue;
        sockaddr_in to = {};
        to.sin_family = AF_INET;
        to.sin_port = htons(kDiscoveryPort);
        to.sin_addr.s_addr = htonl(host);
        sendto(udp_, message.data(), int(message.size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
    }
}

// "MYCAM#1 <port> <proof> <name>" from a phone that scanned the QR code. A valid proof queues the session
// (RunQrConnect, from the idle loop); otherwise the phone hears why ("MYCAM-1 code" or "MYCAM-1 busy").
void PhoneLink::OnQrInvite(const sockaddr_in& from, uint16_t port, const std::string& proof, const std::string& name) {
    const uint32_t ip = from.sin_addr.s_addr;
    if (ip == qrRequest_.ip || (sessionIp_ != 0 && ip == sessionIp_)) return; // Its resends while it connects.
    // A connect attempt for some other address is already queued. That is a moment, not a state, and it may
    // well be a replay of this phone's own invite from somewhere else, so say nothing: this phone resends
    // every 2 s and gets in as soon as that attempt finishes. Answering "busy" here would make it give up.
    if (qrRequest_.ip) return;
    Bytes code;
    const std::string current = web_ ? web_->CurrentPairingCode() : std::string();
    const char* refusal = nullptr;
    if (current.empty() || !web::FromBase64Url(current, &code) || !web::SameSecret(wifi::QrInviteProof(code, port), proof))
        refusal = "MYCAM-1 code"; // Used, expired, or another PC's code.
    else if (sessionIp_ != 0 || handle_ != nullptr || webSession_)
        refusal = "MYCAM-1 busy";
    else if (!web_->ClaimPairingCode()) // An iPhone's POST /pair has it mid-flight.
        refusal = "MYCAM-1 busy";
    if (refusal) {
        Log("wifi: %s scanned a QR code, but %s", IpText(ip).c_str(),
            refusal[8] == 'c' ? "it isn't this PC's current code" : "another phone has the session");
        sendto(udp_, refusal, int(strlen(refusal)), 0, reinterpret_cast<const sockaddr*>(&from), sizeof(from));
        return;
    }
    qrRequest_ = QrRequest{ip, port, name.empty() ? IpText(ip) : name, code};
    Log("wifi: %s (%s) scanned the QR code", qrRequest_.name.c_str(), IpText(ip).c_str());
}

void PhoneLink::RunQrConnect() {
    const QrRequest q = qrRequest_;
    qrRequest_ = QrRequest{}; // The session (sessionIp_) covers the phone's resends from here on.
    manualSession_ = true;    // Whether or not "Find phones on Wi-Fi" is on, like Connect.
    net::Socket s = ConnectTcp(q.ip, q.port, 3000);
    WifiEnd end = WifiEnd::Failed;
    if (s == net::kInvalid) Log("wifi: can't connect to %s (%s:%u)", q.name.c_str(), IpText(q.ip).c_str(), unsigned(q.port));
    else end = RunTcpSession(uintptr_t(s), q.name, false, &q.code);
    manualSession_ = false;
    // It didn't pair (unreachable, or whoever answered couldn't prove it holds the code): let go of the code
    // so the phone that really scanned it can try again with its next resend.
    if (end != WifiEnd::Ran && web_) web_->ReleasePairingCode();
    lastProbe_ = 0;
}

void PhoneLink::NotePaired(const std::string& name, bool wifi) {
    pairedName_ = Wide(name);
    pairedWifi_ = wifi;
    ++pairedCount_;
    Publish();
}

void PhoneLink::NetScanOnce() {
    if (qrRequest_.ip) {
        RunQrConnect();
        return;
    }
    if (const uint32_t ip = connectRequest_.exchange(0)) {
        RunManualConnect(ip);
        return;
    }
    if (!wireless_) {
        // The socket lingers after a scan (or the QR code) so a phone that lists this PC can still ask it to
        // connect.
        const bool qrShowing = web_ && web_->Pairing();
        if (udp_ != net::kInvalid && !scanUntil_ && !qrShowing &&
            NowMs() - std::max(scanEndedAt_, lastQrProbe_) > kInviteGraceMs) {
            net::Close(udp_);
            udp_ = net::kInvalid;
            netPhones_.clear();
        }
        return;
    }
    if (!EnsureUdp()) return;
    uint64_t now = NowMs();
    if (now - lastProbe_ >= kProbeEveryMs) {
        lastProbe_ = now;
        BroadcastProbe();
    }
    ReadAnswers(now);
    // While searching, the list follows what answers (between scans too, as probes go out every 2 s).
    if (nearby_.Expire(now, kForgetAfterMs)) nearbyDirty_ = true;
    RefreshNearby();

    // Connect to a phone that answered recently and isn't in back-off.
    for (auto& [ip, phone] : netPhones_) {
        if (now - phone.lastSeen > kForgetAfterMs || now < phone.retryAfter) continue;
        net::Socket s = ConnectTcp(ip, phone.port, 2000);
        if (s == net::kInvalid) {
            Log("wifi: can't connect to %s (%s:%u)", phone.name.c_str(), IpText(ip).c_str(), phone.port);
            phone.retryAfter = now + kRetryUnreachableMs;
            continue;
        }
        const bool forcePair = phone.forcePair;
        phone.forcePair = false;
        switch (RunTcpSession(uintptr_t(s), phone.name, forcePair)) {
        case WifiEnd::Ran: phone.retryAfter = NowMs() + kRetryLostMs; break;
        // The user said no (or nobody answered): don't ask again for a while.
        case WifiEnd::Refused: phone.retryAfter = NowMs() + kRetryRefusedMs; break;
        // Nobody answered on this PC: treat it like any other "not now" and ask again after a while.
        case WifiEnd::TimedOutHere: phone.retryAfter = NowMs() + kRetryRefusedMs; break;
        // This PC's user said no: never by itself again (Connect, or the phone asking, still work).
        case WifiEnd::RefusedHere: phone.retryAfter = kNever; break;
        // Our pairing key doesn't work any more (e.g. this PC forgot the phone): pair again, with a code.
        case WifiEnd::NeedsPairing: phone.forcePair = true; phone.retryAfter = 0; break;
        case WifiEnd::Failed: phone.retryAfter = NowMs() + kRetryUnreachableMs; break;
        }
        lastProbe_ = 0; // Look again right away.
        break;
    }
}

// PROTOCOL.md "Wireless security". A paired phone is proven with the stored pairing key and connects
// without asking. A new phone is paired: the phone commits to its nonce before seeing ours, both sides
// show the same 6-digit code, and the user allows it on the phone. Either way the session keys come from
// a fresh ECDH, so earlier sessions stay safe if a key ever leaks.
PhoneLink::WifiEnd PhoneLink::Handshake(uintptr_t socket, bool forcePair, const Bytes* qrCode) {
    const net::Socket s = socket;
    bool cancelled = false; // The user pressed "Don't allow" on the PC.
    bool allowedHere = false; // ...or "Allow" (1.5: a code pairing needs both users).
    uint64_t codeShownAt = 0; // When the 6-digit code went on screen: this PC's user answers from then.
    cancelPairing_ = false;
    allowPairing_ = false;
    auto keepWaiting = [this, &cancelled, &allowedHere] {
        const uint64_t now = NowMs();
        SyncLockPaused();
        WriteStatusFrame(now);
        if (cancelPairing_.exchange(false) && !status_.pairingCode.empty()) cancelled = true;
        if (allowPairing_.exchange(false) && !status_.pairingCode.empty() && !allowedHere) {
            allowedHere = true;
            status_.pairingAllowed = true;
            Log("wifi: pairing allowed on the PC");
            Publish();
        }
        PollDiscovery(now);
        PollWeb(0);
        return !quit_ && WifiAllowed() && !cancelled;
    };
    Bytes transcript;
    auto sendMsg = [&](uint8_t type, const Bytes& body, bool record) {
        const Bytes f = Frame(type, body);
        if (record) transcript.insert(transcript.end(), f.begin(), f.end());
        return SendAll(s, f.data(), f.size());
    };
    // Returns the body, or false (refused / wrong message / timeout).
    auto readMsg = [&](uint8_t expected, Bytes* body, uint64_t timeoutMs, bool record, bool* rejected) {
        uint8_t head[3];
        if (!RecvExact(s, head, 3, timeoutMs, keepWaiting)) return false;
        const size_t len = size_t(head[0]) << 8 | head[1];
        if (len < 1) return false;
        body->resize(len - 1);
        if (!body->empty() && !RecvExact(s, body->data(), body->size(), kStepTimeoutMs, keepWaiting)) return false;
        if (head[2] == kMsgReject) {
            *rejected = true;
            Log("wifi: the phone refused (reason %d)", body->empty() ? 0 : int((*body)[0]));
            return false;
        }
        if (head[2] != expected) { Log("wifi: unexpected handshake message %d", int(head[2])); return false; }
        if (record) {
            transcript.insert(transcript.end(), head, head + 3);
            transcript.insert(transcript.end(), body->begin(), body->end());
        }
        return true;
    };

    wifi::EcKey key;
    if (!key.Generate()) return WifiEnd::Failed;
    const Bytes pcPub = key.PublicRaw(), npc = wifi::Random(16);
    Bytes hello = wifi::Ascii("MCHS");
    hello.push_back(1);                              // Version
    hello.push_back(uint8_t((forcePair ? kFlagForcePair : 0) | (qrCode ? kFlagQr : 0))); // Flags
    const Bytes pcId = PcId();
    const Bytes name = NameField(ComputerName());
    hello = wifi::Concat({&hello, &pcId, &pcPub, &npc, &name});
    if (!sendMsg(kMsgClientHello, hello, true)) return WifiEnd::Failed;

    bool rejected = false;
    Bytes sh;
    if (!readMsg(kMsgServerHello, &sh, kStepTimeoutMs, true, &rejected)) return rejected ? WifiEnd::Refused : WifiEnd::Failed;
    if (sh.size() < 16 + 65 + 16 + 1 + 1) return WifiEnd::Failed;
    const Bytes phoneId(sh.begin(), sh.begin() + 16), phonePub(sh.begin() + 16, sh.begin() + 81),
                nph(sh.begin() + 81, sh.begin() + 97);
    const uint8_t mode = sh[97];
    // The phone's own name, after the commitment in pairing mode. It names the stored key and the "Paired"
    // message (the discovery name may have been shortened).
    const size_t nameAt = 98 + (mode == kModePairing ? 32 : 0);
    if (sh.size() > nameAt && sh.size() >= nameAt + 1 + sh[nameAt] && sh[nameAt] > 0) {
        const std::string announced(sh.begin() + nameAt + 1, sh.begin() + nameAt + 1 + sh[nameAt]);
        // The phone chooses this text and it lands in the pairing dialog's title, so it is cleaned first:
        // without that, line separators in it would draw outside the title and over the code to compare.
        status_.phoneName = Wide(web::CleanName(announced, IpText(sessionIp_)));
    }
    Bytes z;
    if (!key.Agree(phonePub, &z)) return WifiEnd::Failed;

    Bytes pairKey, keys;
    if (qrCode) {
        // QR pairing (PROTOCOL.md "QR pairing"): the scanned code goes into K, so only the phone that saw this
        // PC's screen can finish. No code to compare and nothing to allow.
        if (mode != kModeQr) {
            Log("wifi: the phone didn't take the QR pairing (mode %d)", int(mode));
            return WifiEnd::Failed;
        }
        pairKey = wifi::QrPairKey(z, *qrCode, npc, nph);
        keys = wifi::SessionKeys(z, pairKey, npc, nph);
    } else if (mode == kModePaired) {
        if (!LoadPairKey(phoneId, &pairKey)) {
            Log("wifi: the phone remembers this PC, but this PC forgot it: pairing again");
            uint8_t reject[] = {0, 2, kMsgReject, 2};
            SendAll(s, reject, sizeof(reject));
            return WifiEnd::NeedsPairing;
        }
        keys = wifi::SessionKeys(z, pairKey, npc, nph);
    } else if (mode == kModePairing && sh.size() >= 98 + 32 + 1) {
        const Bytes commit(sh.begin() + 98, sh.begin() + 130);
        const Bytes na = wifi::Random(16);
        if (!sendMsg(kMsgNonceA, na, true)) return WifiEnd::Failed;
        Bytes nb;
        if (!readMsg(kMsgNonceB, &nb, kStepTimeoutMs, true, &rejected) || nb.size() != 16)
            return rejected ? WifiEnd::Refused : WifiEnd::Failed;
        if (wifi::Commit(nb, phonePub, pcPub) != commit) {
            Log("wifi: the phone's commitment doesn't match: someone may be in between; not pairing");
            return WifiEnd::Failed;
        }
        pairKey = wifi::PairKey(z, na, nb);
        keys = wifi::SessionKeys(z, pairKey, npc, nph);
        const std::string code = wifi::Sas(pcPub, phonePub, na, nb);
        status_.pairingCode = std::wstring(code.begin(), code.begin() + 3) + L" " + std::wstring(code.begin() + 3, code.end());
        Publish();
        codeShownAt = NowMs(); // This PC's user gets their own kPairingAnswerMs from here.
        Log("wifi: pairing with %s, code %s", Utf8(status_.phoneName).c_str(), code.c_str());
    } else {
        return WifiEnd::Failed;
    }

    const Bytes h = wifi::Sha256(transcript);
    const Bytes kFin(keys.begin() + 64, keys.end());
    if (!sendMsg(kMsgPcFinished, wifi::Finished(kFin, "PC", h), false)) return WifiEnd::Failed;
    Bytes phoneFinished;
    bool ok = readMsg(kMsgPhoneFinished, &phoneFinished, mode == kModePairing ? kUserTimeoutMs : kStepTimeoutMs, false, &rejected);
    // A code pairing also needs this PC's user (PROTOCOL.md "The PC asks too"): otherwise anything on the
    // network posing as a phone could approve its own pairing and send video into the camera. The phone may
    // have answered first; the user here gets their own kPairingAnswerMs from when the code appeared, so a
    // slow answer on the phone can't eat this PC's window.
    bool timedOutHere = false;
    if (ok && mode == kModePairing) {
        while (!allowedHere && !cancelled && NowMs() - codeShownAt < kPairingAnswerMs && keepWaiting()) SleepMs(50);
        if (!allowedHere) {
            const uint8_t reject[] = {0, 2, kMsgReject, kRejectRefused};
            SendAll(s, reject, sizeof(reject));
            // Nobody answering is not the same as somebody saying no: a user who never saw the dialog
            // shouldn't lose that phone until the companion restarts (it is asked again after a while).
            timedOutHere = !cancelled;
        }
    }
    status_.pairingCode.clear();
    status_.pairingAllowed = false;
    Publish();
    if (timedOutHere) {
        Log("wifi: nobody answered the pairing on this PC in time");
        return WifiEnd::TimedOutHere;
    }
    if (cancelled) {
        Log("wifi: pairing refused on the PC");
        return WifiEnd::RefusedHere; // The caller closes the socket; auto-connect leaves this phone alone.
    }
    if (!ok) {
        // A paired phone that rejects our proof has a different key for us (it re-paired, or forgot us).
        if (rejected && mode == kModePaired) { ForgetPairKey(phoneId); return WifiEnd::NeedsPairing; }
        return rejected ? WifiEnd::Refused : WifiEnd::Failed;
    }
    if (!wifi::SameMac(phoneFinished, wifi::Finished(kFin, "PH", h))) {
        Log("wifi: the phone's proof is wrong; not connecting");
        return WifiEnd::Failed;
    }
    if (mode == kModePairing || qrCode) {
        const std::string phoneName = Utf8(status_.phoneName);
        SavePairKey(phoneId, pairKey, phoneName);
        Log("wifi: paired %s%s", phoneName.c_str(), qrCode ? " by QR code" : "");
        nearbyDirty_ = true; // "Paired" in the scan list.
        if (qrCode && web_) web_->PairingCodeUsed(); // One phone per code: the QR code shows a new one.
        NotePaired(phoneName, true);
    }
    wifiTx_ = std::make_shared<wifi::RecordKey>();
    wifiRx_ = std::make_shared<wifi::RecordKey>();
    wifiTx_->Init(Bytes(keys.begin(), keys.begin() + 32), wifi::kDirPcToPhone);
    wifiRx_->Init(Bytes(keys.begin() + 32, keys.begin() + 64), wifi::kDirPhoneToPc);
    wifiRxBuf_.clear();
    return WifiEnd::Ran;
}

PhoneLink::WifiEnd PhoneLink::RunTcpSession(uintptr_t socket, const std::string& phoneName, bool forcePair,
                                             const Bytes* qrCode) {
    const net::Socket s = socket;
    BeginSession(true, phoneName);
    Log("session: Wi-Fi connection to %s%s", phoneName.c_str(), forcePair ? " (pairing again)" : "");
    sockaddr_in peer = {};
    socklen_t peerLen = sizeof(peer);
    if (getpeername(s, reinterpret_cast<sockaddr*>(&peer), &peerLen) == 0) {
        sessionIp_ = peer.sin_addr.s_addr;
        nearby_.Seen(sessionIp_, IpText(sessionIp_), ntohs(peer.sin_port), phoneName, NowMs());
        RefreshNearby(); // Shows it as "Connected" in the scan list.
    }
    WifiEnd end = Handshake(socket, forcePair, qrCode);
    if (end == WifiEnd::Ran) {
        Log("session: Wi-Fi link to %s is encrypted", phoneName.c_str());
        tcp_ = socket;
        uint64_t lastUsbCheck = NowMs();
        SessionLoop([&] {
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(s, &rd);
            timeval tv = {0, long(StatusWaitMs()) * 1000};
            if (select(int(s) + 1, &rd, nullptr, nullptr, &tv) > 0 && !ReceiveRecords(socket)) sessionError_ = true;
            const uint64_t now = NowMs();
            if (!WifiAllowed()) {
                Log("wifi: turned off on the PC");
                sessionError_ = true;
            } else if (now - lastUsbCheck > 2000) {
                lastUsbCheck = now;
                if (UsbPhoneArrived()) {
                    Log("wifi: a phone was plugged in; switching to the cable");
                    sessionError_ = true;
                }
            }
        });
        LeaveSession();
    }
    net::Shutdown(s);
    net::Close(s);
    tcp_ = net::kInvalid;
    wifiTx_.reset();
    wifiRx_.reset();
    wifiRxBuf_.clear();
    sessionIp_ = 0;
    ResetAfterSession();
    RefreshNearby();
    return end;
}

// Drains the socket into wifiRxBuf_, then decrypts every complete record into the protocol parser.
bool PhoneLink::ReceiveRecords(uintptr_t socket) {
    const net::Socket s = socket;
    uint8_t buf[64 * 1024];
    for (int i = 0; i < 64; ++i) {
        int n = recv(s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
        if (n > 0) { wifiRxBuf_.insert(wifiRxBuf_.end(), buf, buf + n); continue; }
        if (n == 0) { Log("wifi: the phone closed the connection"); return false; }
        if (net::WouldBlock()) break;
        Log("wifi: connection lost (%d)", net::LastError());
        return false;
    }
    size_t pos = 0;
    Bytes plain;
    while (wifiRxBuf_.size() - pos >= 4) {
        const uint8_t* p = wifiRxBuf_.data() + pos;
        const size_t len = size_t(p[0]) << 24 | size_t(p[1]) << 16 | size_t(p[2]) << 8 | p[3];
        if (len < 16 || len > wifi::kMaxRecord) { Log("wifi: bad record length"); return false; }
        if (wifiRxBuf_.size() - pos - 4 < len) break; // Wait for the rest.
        if (!wifiRx_->Open(p + 4, len, &plain)) { Log("wifi: a record failed its check; dropping the connection"); return false; }
        pos += 4 + len;
        FeedBytes(plain.data(), plain.size());
    }
    wifiRxBuf_.erase(wifiRxBuf_.begin(), wifiRxBuf_.begin() + pos);
    return true;
}

bool PhoneLink::SendTcp(const uint8_t* data, size_t size) {
    Bytes record;
    if (!wifiTx_ || !wifiTx_->Seal(data, size, &record)) return false;
    if (!SendAll(tcp_, record.data(), record.size())) {
        Log("send cmd failed over Wi-Fi (%d)", net::LastError());
        return false;
    }
    return true;
}

std::string PcIdHex() {
    const Bytes id = PcId();
    return web::Hex(id.data(), id.size());
}

void ForgetPairedPhones() { StoreDelete(kPairedKey); }

} // namespace mycam
