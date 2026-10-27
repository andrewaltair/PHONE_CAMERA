// The iPhone web camera's HTTPS + WebSocket server. See web_server.h and docs/web-camera-format.md.
//
// Routes:
//   GET  /, /app.js, /protocol.js, /style.css   the page, from the exe's resources (companion.rc)
//   POST /pair    t=<one-time code>&name=<device>  -> {"device": "<device token>", "name": "..."}
//   POST /check   d=<device token>                 -> {"ok": true, "busy": false, "name": "..."}
//   GET  /ws?d=<device token>                      WebSocket: one MyCam packet or command per message
// Tokens never reach the log.

#include "net.h"

#include "web_server.h"

#include <stdio.h>
#include <ctype.h>

#include <algorithm>

#include "log.h"
#include "platform.h"
#include "tls_server.h"
#include "wifi_crypto.h"

namespace mycam {

namespace {

constexpr size_t kMaxConns = 48;
constexpr uint64_t kHandshakeMs = 10000;   // TLS + first request.
constexpr uint64_t kHttpIdleMs = 30000;    // Keep-alive connections between requests.
constexpr uint64_t kPhoneIdleMs = 15000;   // The page answers pings, so silence means it's gone.
constexpr uint64_t kPingEveryMs = 5000;
constexpr size_t kMaxPending = 4 * 1024 * 1024; // Unsent bytes before a reader counts as stuck.
constexpr size_t kMaxEarly = 256 * 1024;   // What a page may send before its session starts.
constexpr uint16_t kCloseBusy = 4001;      // Another phone is using MyCam: try again later.
constexpr uint16_t kCloseReplaced = 4002;  // The same device opened a newer connection.
constexpr uint16_t kCloseForgotten = 4003; // The PC forgot this device: scan the QR code again.
constexpr char kDevicesKey[] = "WebDevices"; // In the settings store (HKCU\Software\MyCam\WebDevices on Windows).

// Page files embedded in companion.rc. IDs must match there.
struct PageFile { const char* path; int id; const char* type; };
constexpr PageFile kFiles[] = {
    {"/", 301, "text/html; charset=utf-8"},
    {"/index.html", 301, "text/html; charset=utf-8"},
    {"/app.js", 302, "text/javascript; charset=utf-8"},
    {"/protocol.js", 303, "text/javascript; charset=utf-8"},
    {"/style.css", 304, "text/css; charset=utf-8"},
    {"/icon.png", 305, "image/png"},
};

std::string IpText(uint32_t ip) {
    char text[INET_ADDRSTRLEN] = {};
    in_addr a;
    a.s_addr = ip;
    inet_ntop(AF_INET, &a, text, sizeof(text));
    return text;
}

std::string JsonString(const std::string& s) {
    std::string out = "\"";
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\') { out += '\\'; out += ch; }
        else if (c < 0x20) { char esc[8]; snprintf(esc, sizeof(esc), "\\u%04x", c); out += esc; }
        else out += ch;
    }
    return out + "\"";
}

bool EqualsNoCase(const std::string& a, const char* b) {
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i)
        if (tolower(static_cast<unsigned char>(a[i])) != tolower(static_cast<unsigned char>(b[i]))) return false;
    return i == a.size() && !b[i];
}

} // namespace

struct WebServer::Conn {
    enum class Mode { Http, PhonePending, Phone };
    net::Socket s = net::kInvalid;
    std::unique_ptr<tls::Session> tls;
    Mode mode = Mode::Http;
    web::Bytes in;          // Decrypted request bytes not handled yet (HTTP mode).
    web::Bytes out;         // Encrypted bytes not sent yet.
    size_t outPos = 0;
    web::WsParser ws;
    web::Bytes early;       // Messages from a waiting page, framed back to back (MyCam packets need no more).
    std::string deviceId, deviceName, peer;
    uint32_t localIp = 0;
    uint64_t opened = 0, lastIn = 0;
    bool closeAfterSend = false;
    bool closeSent = false;
    bool dead = false;
};

WebServer::WebServer() : listen_(net::kInvalid) {}

WebServer::~WebServer() {
    for (auto& c : conns_) if (!c->dead) net::Close(c->s);
    if (listen_ != net::kInvalid) net::Close(listen_);
}

bool WebServer::Listening() const { return listen_ != net::kInvalid; }

bool WebServer::Start() {
    LoadDevices();
    cred_ = std::make_unique<tls::Credentials>();
    std::string why;
    if (!cred_->Init(&why)) {
        Log("web: no certificate, so no iPhone page: %s", why.c_str());
        cred_.reset();
        return false;
    }
    return TryListen();
}

// Linux hands out ports 32768-60999 for outgoing connections, so right after boot one can briefly hold kPort.
// Poll keeps retrying until it frees up.
bool WebServer::TryListen() {
    lastListenTry_ = NowMs();
    const net::Socket s = net::ListenTcp(web::kPort, 16);
    if (s == net::kInvalid) {
        if (!listenFailed_)
            Log("web: can't listen on port %u (%d): another program may be using it (retrying every 2 s)",
                unsigned(web::kPort), net::LastError());
        listenFailed_ = true;
        return false;
    }
    listen_ = s;
    RefreshAddresses();
    Log("web: iPhone page listening on port %u (certificate %s), %u remembered device(s)", unsigned(web::kPort),
        cred_->Thumbprint().c_str(), unsigned(book_.Devices().size()));
    return true;
}

void WebServer::Poll(uint32_t timeoutMs) {
    if (listen_ == net::kInvalid && cred_ && NowMs() - lastListenTry_ >= 2000) TryListen();
    if (listen_ == net::kInvalid) {
        if (timeoutMs) SleepMs(timeoutMs);
        return;
    }
    fd_set rd, wr;
    FD_ZERO(&rd);
    FD_ZERO(&wr);
    FD_SET(listen_, &rd);
    int maxFd = int(listen_); // select's first argument: ignored on Windows, the highest fd + 1 on Linux.
    for (auto& c : conns_) {
        if (c->dead) continue;
        FD_SET(c->s, &rd);
        if (c->outPos < c->out.size()) FD_SET(c->s, &wr);
        maxFd = std::max(maxFd, int(c->s));
    }
    timeval tv = {long(timeoutMs / 1000), long(timeoutMs % 1000) * 1000};
    if (select(maxFd + 1, &rd, &wr, nullptr, &tv) > 0) {
        if (FD_ISSET(listen_, &rd)) Accept();
        for (size_t i = 0; i < conns_.size(); ++i) {
            Conn& c = *conns_[i];
            if (!c.dead && FD_ISSET(c.s, &rd)) ReadFrom(c);
            if (!c.dead && FD_ISSET(c.s, &wr)) Flush(c);
        }
    }
    // Messages a page sent while it waited for its session.
    for (auto& c : conns_) {
        if (!c->dead && c->mode == Conn::Mode::Phone && !c->early.empty() && onPhone_) {
            web::Bytes early;
            early.swap(c->early);
            onPhone_(early.data(), early.size());
        }
    }
    Sweep(NowMs());
}

void WebServer::Accept() {
    for (int i = 0; i < 8; ++i) {
        sockaddr_in from = {};
        socklen_t len = sizeof(from);
        const net::Socket s = accept(listen_, reinterpret_cast<sockaddr*>(&from), &len);
        if (s == net::kInvalid) return;
        // ponytail: select() can't watch an fd >= FD_SETSIZE on Linux; the connection limit keeps us far below.
        if (conns_.size() >= kMaxConns) {
            net::Close(s);
            continue;
        }
        net::SetNonBlocking(s);
        int noDelay = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
        int rcv = 1024 * 1024; // A 4K key frame can be several hundred KB.
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcv), sizeof(rcv));
        auto c = std::make_unique<Conn>();
        c->s = s;
        c->tls = std::make_unique<tls::Session>(cred_.get());
        c->peer = IpText(from.sin_addr.s_addr);
        sockaddr_in local = {};
        socklen_t localLen = sizeof(local);
        if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &localLen) == 0) c->localIp = local.sin_addr.s_addr;
        c->opened = c->lastIn = NowMs();
        conns_.push_back(std::move(c));
    }
}

void WebServer::ReadFrom(Conn& c) {
    uint8_t buf[64 * 1024];
    for (int i = 0; i < 32 && !c.dead; ++i) {
        const int n = recv(c.s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
        if (n == 0) { Close(c, "closed by the page"); return; }
        if (n < 0) {
            if (net::WouldBlock()) return;
            Close(c, "connection lost");
            return;
        }
        c.lastIn = NowMs();
        web::Bytes toSend, plain;
        const bool ok = c.tls->Receive(buf, size_t(n), &toSend, &plain);
        if (!toSend.empty()) SendRaw(c, toSend.data(), toSend.size());
        if (!ok) {
            if (c.tls->Closed()) Close(c, "the page said goodbye");
            else if (!c.tls->Ready()) Close(c, nullptr); // Typically Safari before the certificate is accepted.
            else Close(c, c.tls->Error());
            return;
        }
        if (plain.empty()) continue;
        if (c.mode == Conn::Mode::Http) {
            c.in.insert(c.in.end(), plain.begin(), plain.end());
            HandleHttp(c);
        } else {
            HandleWs(c, plain.data(), plain.size());
        }
    }
}

void WebServer::HandleHttp(Conn& c) {
    while (!c.dead && c.mode == Conn::Mode::Http && !c.in.empty()) {
        web::HttpRequest r;
        size_t head = 0;
        const web::HttpParse p = web::ParseHttpRequest(c.in.data(), c.in.size(), &r, &head);
        if (p == web::HttpParse::Incomplete) return;
        if (p == web::HttpParse::Bad) {
            SendResponse(c, 400, "Bad Request", "text/plain", "bad request\n", "Connection: close\r\n");
            c.closeAfterSend = true;
            return;
        }
        if (c.in.size() < head + r.contentLength) return; // The body hasn't all arrived.
        const std::string body(c.in.begin() + head, c.in.begin() + head + r.contentLength);
        c.in.erase(c.in.begin(), c.in.begin() + head + r.contentLength);
        HandleRequest(c, r, body);
        if (EqualsNoCase(r.Header("connection"), "close")) c.closeAfterSend = true;
    }
    // After an upgrade, anything left over already belongs to the WebSocket.
    if (!c.dead && c.mode != Conn::Mode::Http && !c.in.empty()) {
        web::Bytes rest;
        rest.swap(c.in);
        HandleWs(c, rest.data(), rest.size());
    }
}

void WebServer::HandleRequest(Conn& c, const web::HttpRequest& r, const std::string& body) {
    if (r.path == "/ws") { HandleUpgrade(c, r); return; }
    if (r.method == "POST" && r.path == "/pair") { HandlePair(c, r, body); return; }
    if (r.method == "POST" && r.path == "/check") {
        const web::WebDevice* d = book_.FindDevice(web::QueryParam(body, "d"));
        if (!d) {
            SendResponse(c, 403, "Forbidden", "application/json", "{\"ok\":false,\"error\":\"unknown device\"}");
            return;
        }
        // Busy unless this device's own (older) connection is the one in the way.
        bool other = false, own = false;
        for (auto& o : conns_) {
            if (o->dead || o->mode == Conn::Mode::Http) continue;
            if (o->deviceId != d->id) other = true;
            else own |= o->mode == Conn::Mode::Phone;
        }
        const bool busy = other || (busy_ && !own && d->id != busyExcept_);
        SendResponse(c, 200, "OK", "application/json",
                     std::string("{\"ok\":true,\"busy\":") + (busy ? "true" : "false") + ",\"name\":" + JsonString(d->name) + "}");
        return;
    }
    if (r.method != "GET" && r.method != "HEAD") {
        SendResponse(c, 405, "Method Not Allowed", "text/plain", "method not allowed\n");
        return;
    }
    for (const PageFile& f : kFiles) {
        if (r.path != f.path) continue;
        const std::string data = ResourceData(f.id);
        if (data.empty()) break;
        std::string extra;
        if (f.id == 301) {
            // The page only talks to this PC.
            extra = "Content-Security-Policy: default-src 'self'; connect-src 'self' wss:; img-src 'self' data:; "
                    "style-src 'self'; script-src 'self'; frame-ancestors 'none'; base-uri 'none'\r\n";
        }
        SendResponse(c, 200, "OK", f.type, r.method == "HEAD" ? std::string() : data, extra);
        return;
    }
    SendResponse(c, 404, "Not Found", "text/plain", "not found\n");
}

void WebServer::HandlePair(Conn& c, const web::HttpRequest&, const std::string& body) {
    const uint64_t now = NowMs();
    if (codeClaimed_) { // An Android phone scanned this code first and is pairing with it now.
        Log("web: refused to pair %s: a phone is already using this code", c.peer.c_str());
        SendResponse(c, 409, "Conflict", "application/json", "{\"ok\":false,\"error\":\"code\"}");
        return;
    }
    if (!book_.UsePairingCode(web::QueryParam(body, "t"), now)) {
        Log("web: refused to pair %s: the code is wrong, used or expired", c.peer.c_str());
        SendResponse(c, 403, "Forbidden", "application/json", "{\"ok\":false,\"error\":\"code\"}");
        return;
    }
    web::WebDevice d;
    const std::string token =
        book_.AddDevice(web::QueryParam(body, "name"), wifi::Random(32), wifi::Random(8), UnixTime(), &d);
    SaveDevice(d);
    newlyPairedId_ = d.id;
    Log("web: paired %s at %s", d.name.c_str(), c.peer.c_str());
    if (pairing_) MakeCode(); // Ready for the next device.
    ++generation_;
    SendResponse(c, 200, "OK", "application/json",
                 "{\"ok\":true,\"device\":" + JsonString(token) + ",\"name\":" + JsonString(d.name) + "}");
}

void WebServer::HandleUpgrade(Conn& c, const web::HttpRequest& r) {
    if (!web::IsWebSocketUpgrade(r)) {
        SendResponse(c, 400, "Bad Request", "text/plain", "expected a WebSocket upgrade\n");
        return;
    }
    // Only this PC's own page may open the link.
    const std::string origin = r.Header("origin");
    if (!origin.empty() && origin != "https://" + r.Header("host")) {
        Log("web: refused a camera link from another site (%s)", c.peer.c_str());
        SendResponse(c, 403, "Forbidden", "text/plain", "wrong origin\n");
        return;
    }
    const web::WebDevice* d = book_.FindDevice(web::QueryParam(r.query, "d"));
    if (!d) {
        Log("web: refused a camera link from %s: no valid device token", c.peer.c_str());
        SendResponse(c, 403, "Forbidden", "text/plain", "unknown device\n");
        return;
    }
    // A newer connection from the same device replaces the old one (Safari reloaded, Wi-Fi changed...).
    // If the old one was running the session, that session ends with it and this one takes over.
    bool other = false, replacedSession = false;
    for (auto& o : conns_) {
        if (o.get() == &c || o->dead || o->mode == Conn::Mode::Http) continue;
        if (o->deviceId != d->id) { other = true; continue; }
        replacedSession |= o->mode == Conn::Mode::Phone;
        const web::Bytes bye = web::CloseFrame(kCloseReplaced);
        web::Bytes enc;
        if (o->tls->Encrypt(bye.data(), bye.size(), &enc)) SendRaw(*o, enc.data(), enc.size());
        Flush(*o);
        Close(*o, "replaced by a newer connection");
    }
    if (other || (busy_ && !replacedSession && d->id != busyExcept_)) {
        SendResponse(c, 503, "Service Unavailable", "text/plain", "busy with another phone\n", "Retry-After: 5\r\n");
        return;
    }
    const std::string accept = web::AcceptKey(r.Header("sec-websocket-key"));
    const std::string head = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                             "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
    web::Bytes enc;
    if (!c.tls->Encrypt(reinterpret_cast<const uint8_t*>(head.data()), head.size(), &enc)) {
        Close(c, "TLS failed");
        return;
    }
    SendRaw(c, enc.data(), enc.size());
    c.mode = Conn::Mode::PhonePending;
    c.deviceId = d->id;
    c.deviceName = d->name;
    c.lastIn = NowMs();
    ++generation_;
    Log("web: %s opened the camera link from %s", d->name.c_str(), c.peer.c_str());
}

void WebServer::HandleWs(Conn& c, const uint8_t* data, size_t size) {
    const bool ok = c.ws.Feed(data, size, [&](uint8_t op, const uint8_t* p, size_t n) {
        if (c.dead) return;
        switch (op) {
        case web::kBinary:
            if (c.mode == Conn::Mode::Phone && onPhone_) onPhone_(p, n);
            else if (c.early.size() + n <= kMaxEarly) c.early.insert(c.early.end(), p, p + n);
            break;
        case web::kPing: {
            const web::Bytes pong = web::Frame(web::kPong, p, n);
            web::Bytes enc;
            if (c.tls->Encrypt(pong.data(), pong.size(), &enc)) SendRaw(c, enc.data(), enc.size());
            break;
        }
        case web::kClose: {
            if (!c.closeSent) {
                const web::Bytes bye = web::CloseFrame(1000);
                web::Bytes enc;
                if (c.tls->Encrypt(bye.data(), bye.size(), &enc)) SendRaw(c, enc.data(), enc.size());
                c.closeSent = true;
            }
            c.closeAfterSend = true;
            break;
        }
        default: break; // Text and pong: nothing to do (lastIn already moved).
        }
    });
    if (!ok && !c.dead) {
        Log("web: %s broke the WebSocket rules (%s)", c.deviceName.c_str(), c.ws.Error());
        const web::Bytes bye = web::CloseFrame(c.ws.TooBig() ? 1009 : 1002);
        web::Bytes enc;
        if (c.tls->Encrypt(bye.data(), bye.size(), &enc)) SendRaw(c, enc.data(), enc.size());
        Flush(c);
        Close(c, nullptr);
    }
}

void WebServer::SendRaw(Conn& c, const uint8_t* data, size_t size) {
    if (c.dead) return;
    c.out.insert(c.out.end(), data, data + size);
    Flush(c);
}

void WebServer::SendResponse(Conn& c, int code, const char* reason, const std::string& type, const std::string& body,
                             const std::string& extraHeaders) {
    char status[64];
    snprintf(status, sizeof(status), "HTTP/1.1 %d %s\r\n", code, reason);
    std::string head = status;
    head += "Content-Type: " + type + "\r\n";
    head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    head += "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n";
    head += extraHeaders;
    head += "\r\n";
    const std::string all = head + body;
    web::Bytes enc;
    if (!c.tls->Encrypt(reinterpret_cast<const uint8_t*>(all.data()), all.size(), &enc)) {
        Close(c, "TLS failed");
        return;
    }
    SendRaw(c, enc.data(), enc.size());
}

void WebServer::Flush(Conn& c) {
    while (!c.dead && c.outPos < c.out.size()) {
        const int n = send(c.s, reinterpret_cast<const char*>(c.out.data() + c.outPos), int(c.out.size() - c.outPos), 0);
        if (n > 0) { c.outPos += size_t(n); continue; }
        if (!net::WouldBlock()) Close(c, "send failed");
        break;
    }
    if (c.dead) return;
    if (c.outPos == c.out.size()) {
        c.out.clear();
        c.outPos = 0;
        if (c.closeAfterSend) Close(c, nullptr);
    } else if (c.out.size() - c.outPos > kMaxPending) {
        Close(c, "it stopped reading");
    } else if (c.outPos > 64 * 1024) {
        c.out.erase(c.out.begin(), c.out.begin() + c.outPos);
        c.outPos = 0;
    }
}

void WebServer::Close(Conn& c, const char* why) {
    if (c.dead) return;
    if (c.mode != Conn::Mode::Http) {
        Log("web: %s's camera link closed%s%s", c.deviceName.c_str(), why ? ": " : "", why ? why : "");
        ++generation_;
    }
    net::Shutdown(c.s);
    net::Close(c.s);
    c.dead = true;
}

void WebServer::Sweep(uint64_t now) {
    const bool ping = now - lastPing_ >= kPingEveryMs;
    if (ping) lastPing_ = now;
    for (auto& cp : conns_) {
        Conn& c = *cp;
        if (c.dead) continue;
        if (c.mode == Conn::Mode::Http) {
            if (!c.tls->Ready() ? now - c.opened > kHandshakeMs : now - c.lastIn > kHttpIdleMs) Close(c, nullptr);
            continue;
        }
        if (now - c.lastIn > kPhoneIdleMs) {
            Close(c, "no answer for 15 s");
            continue;
        }
        if (ping) {
            const web::Bytes f = web::Frame(web::kPing, nullptr, 0);
            web::Bytes enc;
            if (c.tls->Encrypt(f.data(), f.size(), &enc)) SendRaw(c, enc.data(), enc.size());
        }
    }
    conns_.erase(std::remove_if(conns_.begin(), conns_.end(), [](const std::unique_ptr<Conn>& c) { return c->dead; }),
                 conns_.end());
    if (pairing_ && !book_.HasPairingCode(now)) {
        MakeCode(); // Ran out (or was used): the QR code shows a fresh one.
        ++generation_;
    }
}

// --- The phone ---------------------------------------------------------------------------------------

void WebServer::SetBusy(bool busy, const std::string& except) {
    busy_ = busy;
    busyExcept_ = busy ? except : std::string();
    if (!busy) return;
    for (auto& c : conns_) {
        if (c->dead || c->mode != Conn::Mode::PhonePending || (!except.empty() && c->deviceId == except)) continue;
        const web::Bytes bye = web::CloseFrame(kCloseBusy);
        web::Bytes enc;
        if (c->tls->Encrypt(bye.data(), bye.size(), &enc)) SendRaw(*c, enc.data(), enc.size());
        Flush(*c);
        Close(*c, "another phone is connected");
    }
}

bool WebServer::PhonePending() const {
    for (auto& c : conns_) if (!c->dead && c->mode == Conn::Mode::PhonePending) return true;
    return false;
}

bool WebServer::TakePhone(std::string* id, std::string* name, std::string* link) {
    for (auto& c : conns_) {
        if (c->dead || c->mode != Conn::Mode::PhonePending) continue;
        c->mode = Conn::Mode::Phone;
        *id = c->deviceId;
        *name = c->deviceName;
        *link = LinkOf(c->localIp);
        ++generation_;
        return true;
    }
    return false;
}

bool WebServer::PhoneConnected() const {
    for (auto& c : conns_) if (!c->dead && c->mode == Conn::Mode::Phone) return true;
    return false;
}

bool WebServer::SendToPhone(const uint8_t* data, size_t size) {
    for (auto& c : conns_) {
        if (c->dead || c->mode != Conn::Mode::Phone) continue;
        const web::Bytes f = web::Frame(web::kBinary, data, size);
        web::Bytes enc;
        if (!c->tls->Encrypt(f.data(), f.size(), &enc)) return false;
        SendRaw(*c, enc.data(), enc.size());
        return !c->dead;
    }
    return false;
}

void WebServer::EndPhone() {
    for (auto& c : conns_) {
        if (c->dead || c->mode != Conn::Mode::Phone) continue;
        const web::Bytes bye = web::CloseFrame(1000);
        web::Bytes enc;
        if (c->tls->Encrypt(bye.data(), bye.size(), &enc)) SendRaw(*c, enc.data(), enc.size());
        Flush(*c);
        Close(*c, nullptr);
    }
}

std::string WebServer::LinkOf(uint32_t localIp) const {
    for (const web::NetAddress& a : addresses_) {
        if (a.ipv4 != localIp) continue;
        switch (a.kind) {
        case web::NetKind::WiFi: return "Wi-Fi";
        case web::NetKind::Ethernet: return "network";
        case web::NetKind::IPhoneUsb: return "USB hotspot";
        default: return "network";
        }
    }
    return "network";
}

// --- Pairing ---------------------------------------------------------------------------------------------

void WebServer::RefreshAddresses() {
    std::vector<web::NetAddress> list;
    for (const Adapter& a : UpAdapters()) {
        web::NetAddress n;
        n.ipv4 = a.ipv4;
        n.text = IpText(n.ipv4);
        n.adapter = a.name;
        n.kind = web::ClassifyAdapter(a.ifType, a.description, ntohl(n.ipv4));
        n.prefix = a.prefix;
        list.push_back(std::move(n));
    }
    web::SortAddresses(&list);
    addresses_ = std::move(list);
}

void WebServer::MakeCode() { book_.NewPairingCode(wifi::Random(16), NowMs()); }

void WebServer::StartPairing(uint32_t ipv4) {
    RefreshAddresses();
    pairingIp_ = 0;
    for (const web::NetAddress& a : addresses_) if (a.ipv4 == ipv4) pairingIp_ = ipv4;
    if (!pairingIp_ && !addresses_.empty()) pairingIp_ = addresses_[0].ipv4;
    pairing_ = true;
    MakeCode();
    ++generation_;
    if (!pairingIp_) Log("web: pairing asked for, but this PC has no network address a phone could reach");
}

void WebServer::StopPairing() {
    if (!pairing_) return;
    pairing_ = false;
    book_.ClearPairingCode();
    ++generation_;
}

std::string WebServer::PairingUrl() const {
    if (!pairing_ || !pairingIp_ || listen_ == net::kInvalid || book_.PairingCode().empty()) return "";
    return "https://" + IpText(pairingIp_) + ":" + std::to_string(web::kPort) + "/?t=" + book_.PairingCode() +
           (pcId_.empty() ? "" : "&pc=" + pcId_);
}

std::string WebServer::CurrentPairingCode() const {
    return pairing_ && book_.HasPairingCode(NowMs()) ? book_.PairingCode() : std::string();
}

void WebServer::PairingCodeUsed() {
    codeClaimed_ = false;
    if (!pairing_) return;
    MakeCode();
    ++generation_;
}

bool WebServer::ClaimPairingCode() {
    if (codeClaimed_) return false;
    codeClaimed_ = true;
    return true;
}

std::string WebServer::TakeNewlyPaired(const std::string& deviceId) {
    if (deviceId.empty() || deviceId != newlyPairedId_) return "";
    newlyPairedId_.clear();
    return deviceId;
}

// --- Remembered devices: the store's WebDevices/<id> = TokenHash, Name, Added -------------------------------

std::vector<web::WebDevice> WebServer::Devices() const { return book_.Devices(); }

void WebServer::LoadDevices() {
    std::vector<web::WebDevice> list;
    for (const std::string& id : StoreList(kDevicesKey)) {
        const std::string path = std::string(kDevicesKey) + "/" + id;
        web::WebDevice d;
        d.id = id;
        if (!StoreGetBytes(path, "TokenHash", &d.tokenHash) || d.tokenHash.size() != 32) continue;
        StoreGetString(path, "Name", &d.name);
        StoreGetU64(path, "Added", &d.addedUnix);
        list.push_back(std::move(d));
    }
    std::sort(list.begin(), list.end(), [](const web::WebDevice& a, const web::WebDevice& b) { return a.addedUnix < b.addedUnix; });
    book_.SetDevices(std::move(list));
}

void WebServer::SaveDevice(const web::WebDevice& d) {
    const std::string path = std::string(kDevicesKey) + "/" + d.id;
    StoreSetBytes(path, "TokenHash", d.tokenHash);
    StoreSetString(path, "Name", d.name);
    StoreSetU64(path, "Added", d.addedUnix);
}

void WebServer::Forget(const std::string& id) {
    if (!book_.Forget(id)) return;
    StoreDelete(std::string(kDevicesKey) + "/" + id);
    for (auto& c : conns_) {
        if (c->dead || c->deviceId != id) continue;
        const web::Bytes bye = web::CloseFrame(kCloseForgotten);
        web::Bytes enc;
        if (c->tls->Encrypt(bye.data(), bye.size(), &enc)) SendRaw(*c, enc.data(), enc.size());
        Flush(*c);
        Close(*c, "forgotten on the PC");
    }
    ++generation_;
    Log("web: forgot a device");
}

} // namespace mycam
