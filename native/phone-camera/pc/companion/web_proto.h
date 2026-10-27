#pragma once
// The iPhone web camera's plumbing (docs/web-camera-format.md): HTTP requests, WebSocket frames (RFC 6455),
// the upgrade handshake and the pairing tokens. Pure C++ (no sockets, no clock): web_server.cpp does the
// I/O, and pc/tests checks everything here.

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace mycam::web {

using Bytes = std::vector<uint8_t>;

constexpr uint16_t kPort = 47900;
// One WebSocket message carries one MyCam packet: a 20-byte header and at most 8 MiB of payload.
constexpr size_t kMaxMessage = 20 + 8 * 1024 * 1024;
constexpr size_t kMaxRequestHead = 8 * 1024;  // Request line and headers.
constexpr size_t kMaxRequestBody = 4 * 1024;  // POST /pair is a few dozen bytes.
constexpr uint64_t kPairingCodeMs = 5 * 60 * 1000; // The QR code's token works once, for this long.

// --- Encoding helpers ----------------------------------------------------------------------------------

std::string Base64(const uint8_t* data, size_t size);
std::string Base64Url(const uint8_t* data, size_t size); // No padding: safe in a URL as it is.
std::string Hex(const uint8_t* data, size_t size);
// The reverse of Base64Url(): false on anything that isn't exactly that encoding.
bool FromBase64Url(const std::string& s, Bytes* out);
std::string UrlDecode(const std::string& s);
// The value of `name` in "a=1&b=2" (URL-decoded), or "" when it isn't there.
std::string QueryParam(const std::string& query, const std::string& name);
// Equal contents, in time that doesn't depend on where they differ.
bool SameSecret(const std::string& a, const std::string& b);

// --- HTTP ------------------------------------------------------------------------------------------------

struct HttpRequest {
    std::string method, path, query;            // "GET", "/ws", "d=..." (path and query split at '?')
    std::map<std::string, std::string> headers; // Names lower-case, values trimmed.
    size_t contentLength = 0;

    std::string Header(const std::string& lowerName) const {
        auto it = headers.find(lowerName);
        return it == headers.end() ? std::string() : it->second;
    }
};

enum class HttpParse { Incomplete, Done, Bad };

// Parses the request line and headers at the start of `data`. On Done, *headSize is the number of bytes
// they took; the body (contentLength bytes) follows. Bad: malformed, too long, or a body too big.
HttpParse ParseHttpRequest(const uint8_t* data, size_t size, HttpRequest* out, size_t* headSize);

// A valid RFC 6455 opening handshake: GET, "Upgrade: websocket", "Connection" listing "upgrade",
// "Sec-WebSocket-Version: 13" and a 16-byte Sec-WebSocket-Key.
bool IsWebSocketUpgrade(const HttpRequest& r);

// Sec-WebSocket-Accept for a Sec-WebSocket-Key: base64(SHA-1(key + RFC 6455 GUID)).
std::string AcceptKey(const std::string& clientKey);

// --- WebSocket frames ----------------------------------------------------------------------------------

enum Opcode : uint8_t { kContinuation = 0, kText = 1, kBinary = 2, kClose = 8, kPing = 9, kPong = 10 };

// One unmasked frame with FIN set, as a server sends it.
Bytes Frame(uint8_t opcode, const uint8_t* payload, size_t size);
Bytes CloseFrame(uint16_t code);

// Reads client frames: checks masking and lengths, unmasks, joins fragmented messages and passes control
// frames on as they come (they may arrive between fragments). Feed() returns false on a protocol error
// (Error() says which); the connection must then close with code 1002 (or 1009 for a message too big).
class WsParser {
public:
    using OnMessage = std::function<void(uint8_t opcode, const uint8_t* data, size_t size)>;

    bool Feed(const uint8_t* data, size_t size, const OnMessage& onMessage);
    const char* Error() const { return error_; }
    bool TooBig() const { return tooBig_; }
    void Reset();

private:
    bool Fail(const char* why) { error_ = why; return false; }

    Bytes buf_;              // Bytes of a frame that hasn't fully arrived yet.
    Bytes message_;          // Fragments of a data message so far.
    uint8_t messageOpcode_ = 0;
    bool inMessage_ = false;
    const char* error_ = nullptr;
    bool tooBig_ = false;
};

// --- Tokens ----------------------------------------------------------------------------------------------
// The QR code holds a one-time pairing code (128 bits, kPairingCodeMs). The page trades it for a device
// token (256 bits) with POST /pair, keeps that in its storage, and opens /ws?d=<device token> with it, now
// and every later time. The PC stores only a SHA-256 of each device token, so its registry can't be used
// to connect. Nothing here ever goes to the log.

struct WebDevice {
    std::string id;       // 16 hex digits, random: names the registry key.
    std::string name;     // "iPhone", from the page.
    Bytes tokenHash;      // SHA-256 of the device token.
    uint64_t addedUnix = 0;
};

class TokenBook {
public:
    // A new pairing code (from 16 random bytes) replaces the previous one. Returns it, base64url.
    std::string NewPairingCode(const Bytes& random16, uint64_t nowMs);
    void ClearPairingCode() { code_.clear(); }
    bool HasPairingCode(uint64_t nowMs) const { return !code_.empty() && nowMs < codeExpires_; }
    uint64_t PairingCodeExpires() const { return codeExpires_; }
    const std::string& PairingCode() const { return code_; }
    // True once for the current code before it expires; the code is then used up.
    bool UsePairingCode(const std::string& code, uint64_t nowMs);

    // Adds a device for 32 random bytes (the token) and 8 more (the id). Returns the token, base64url.
    std::string AddDevice(const std::string& name, const Bytes& random32, const Bytes& random8, uint64_t nowUnix,
                          WebDevice* added);
    // The device a token belongs to, or nullptr.
    const WebDevice* FindDevice(const std::string& token) const;
    bool Forget(const std::string& id);
    void SetDevices(std::vector<WebDevice> devices) { devices_ = std::move(devices); }
    const std::vector<WebDevice>& Devices() const { return devices_; }

private:
    std::string code_;
    uint64_t codeExpires_ = 0;
    std::vector<WebDevice> devices_;
};

// A name a phone sent, cut to what the PC can safely show: valid UTF-8 only, no control characters, no line
// or paragraph separators and no bidirectional overrides, at most 40 bytes. `fallback` is used when nothing
// is left. A remote name ends up in the pairing dialog's title, so everything that reaches a window or the
// registry goes through this first.
std::string CleanName(const std::string& name, const std::string& fallback);
// The page's own name ("iPhone"), with that default.
std::string CleanDeviceName(const std::string& name);

// --- Addresses -------------------------------------------------------------------------------------------
// The PC's IPv4 addresses the QR code may point at, best first: Wi-Fi, then Ethernet, then an iPhone's
// USB hotspot, then anything else. VPNs, virtual switches and link-local addresses are left out.

enum class NetKind { WiFi, Ethernet, IPhoneUsb, Other, Skip };

struct NetAddress {
    uint32_t ipv4 = 0;  // Network order.
    std::string text;   // "192.168.1.20"
    std::wstring adapter;
    NetKind kind = NetKind::Other;
    uint8_t prefix = 0; // On-link prefix length (24 for a /24); 0 when the adapter didn't say.
};

// How an adapter counts, from its IANA interface type, its description and the address.
NetKind ClassifyAdapter(uint32_t ifType, const std::wstring& description, uint32_t ipv4HostOrder);
void SortAddresses(std::vector<NetAddress>* list);

} // namespace mycam::web
