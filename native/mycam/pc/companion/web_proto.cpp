#include "web_proto.h"

#include <algorithm>
#include <cctype>
#include <cwctype>

#include "wifi_crypto.h"

namespace mycam::web {

namespace {

Bytes Sha1(const std::string& s) { return wifi::Sha1(Bytes(s.begin(), s.end())); }
Bytes Sha256(const std::string& s) { return wifi::Sha256(Bytes(s.begin(), s.end())); }

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t") - a + 1);
}

std::string Base64With(const char* alphabet, bool pad, const uint8_t* p, size_t n) {
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = uint32_t(p[i]) << 16 | (i + 1 < n ? uint32_t(p[i + 1]) << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        out += alphabet[v >> 18 & 63];
        out += alphabet[v >> 12 & 63];
        if (i + 1 < n) out += alphabet[v >> 6 & 63]; else if (pad) out += '=';
        if (i + 2 < n) out += alphabet[v & 63]; else if (pad) out += '=';
    }
    return out;
}

constexpr char kWsGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

} // namespace

// --- Encoding helpers ----------------------------------------------------------------------------------

std::string Base64(const uint8_t* data, size_t size) {
    return Base64With("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/", true, data, size);
}

std::string Base64Url(const uint8_t* data, size_t size) {
    return Base64With("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_", false, data, size);
}

std::string Hex(const uint8_t* data, size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < size; ++i) {
        out += digits[data[i] >> 4];
        out += digits[data[i] & 15];
    }
    return out;
}

bool FromBase64Url(const std::string& s, Bytes* out) {
    if (s.size() % 4 == 1) return false; // No byte count leaves one character over.
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-') return 62;
        if (c == '_') return 63;
        return -1;
    };
    Bytes b;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        const int v = value(c);
        if (v < 0) return false;
        acc = acc << 6 | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            b.push_back(uint8_t(acc >> bits));
        }
    }
    if (acc & ((1u << bits) - 1)) return false; // Leftover bits must be zero (one spelling per value).
    *out = std::move(b);
    return true;
}

std::string UrlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                   std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string QueryParam(const std::string& query, const std::string& name) {
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t end = query.find('&', pos);
        if (end == std::string::npos) end = query.size();
        const std::string pair = query.substr(pos, end - pos);
        const size_t eq = pair.find('=');
        if (UrlDecode(pair.substr(0, eq)) == name) return eq == std::string::npos ? "" : UrlDecode(pair.substr(eq + 1));
        pos = end + 1;
    }
    return "";
}

bool SameSecret(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff |= unsigned(uint8_t(a[i]) ^ uint8_t(b[i]));
    return diff == 0;
}

// --- HTTP ------------------------------------------------------------------------------------------------

HttpParse ParseHttpRequest(const uint8_t* data, size_t size, HttpRequest* out, size_t* headSize) {
    const char* text = reinterpret_cast<const char*>(data);
    const size_t scan = std::min(size, kMaxRequestHead);
    size_t end = std::string::npos;
    for (size_t i = 0; i + 3 < scan; ++i) {
        if (text[i] == '\r' && text[i + 1] == '\n' && text[i + 2] == '\r' && text[i + 3] == '\n') { end = i; break; }
    }
    if (end == std::string::npos) return size >= kMaxRequestHead ? HttpParse::Bad : HttpParse::Incomplete;

    const std::string head(text, end);
    *out = HttpRequest{};
    size_t lineEnd = head.find("\r\n");
    const std::string requestLine = head.substr(0, lineEnd);
    const size_t sp1 = requestLine.find(' ');
    const size_t sp2 = sp1 == std::string::npos ? std::string::npos : requestLine.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return HttpParse::Bad;
    out->method = requestLine.substr(0, sp1);
    const std::string target = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::string version = requestLine.substr(sp2 + 1);
    if (out->method.empty() || target.empty() || target[0] != '/' || version.rfind("HTTP/1.", 0) != 0) return HttpParse::Bad;
    const size_t q = target.find('?');
    out->path = target.substr(0, q);
    out->query = q == std::string::npos ? "" : target.substr(q + 1);

    while (lineEnd != std::string::npos) {
        const size_t start = lineEnd + 2;
        lineEnd = head.find("\r\n", start);
        const std::string line = head.substr(start, lineEnd == std::string::npos ? std::string::npos : lineEnd - start);
        if (line.empty()) continue;
        const size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0) return HttpParse::Bad;
        const std::string name = Lower(line.substr(0, colon));
        const std::string value = Trim(line.substr(colon + 1));
        auto it = out->headers.find(name);
        if (it == out->headers.end()) out->headers[name] = value;
        else it->second += ", " + value;
    }

    if (!out->Header("transfer-encoding").empty()) return HttpParse::Bad; // Chunked bodies: never needed here.
    const std::string length = out->Header("content-length");
    if (!length.empty()) {
        if (length.size() > 6 || length.find_first_not_of("0123456789") != std::string::npos) return HttpParse::Bad;
        out->contentLength = size_t(std::stoul(length));
        if (out->contentLength > kMaxRequestBody) return HttpParse::Bad;
    }
    *headSize = end + 4;
    return HttpParse::Done;
}

bool IsWebSocketUpgrade(const HttpRequest& r) {
    if (r.method != "GET" || Lower(r.Header("upgrade")) != "websocket" || r.Header("sec-websocket-version") != "13")
        return false;
    bool upgrade = false;
    const std::string connection = Lower(r.Header("connection"));
    size_t pos = 0;
    while (pos <= connection.size()) {
        size_t comma = connection.find(',', pos);
        if (comma == std::string::npos) comma = connection.size();
        upgrade |= Trim(connection.substr(pos, comma - pos)) == "upgrade";
        pos = comma + 1;
    }
    const std::string key = r.Header("sec-websocket-key");
    return upgrade && key.size() == 24 && key.compare(22, 2, "==") == 0; // 16 bytes, base64.
}

std::string AcceptKey(const std::string& clientKey) {
    const Bytes digest = Sha1(clientKey + kWsGuid);
    return Base64(digest.data(), digest.size());
}

// --- WebSocket frames ----------------------------------------------------------------------------------

Bytes Frame(uint8_t opcode, const uint8_t* payload, size_t size) {
    Bytes f;
    f.reserve(size + 10);
    f.push_back(uint8_t(0x80 | opcode));
    if (size < 126) {
        f.push_back(uint8_t(size));
    } else if (size < 65536) {
        f.push_back(126);
        f.push_back(uint8_t(size >> 8));
        f.push_back(uint8_t(size));
    } else {
        f.push_back(127);
        for (int i = 7; i >= 0; --i) f.push_back(uint8_t(uint64_t(size) >> (8 * i)));
    }
    if (size) f.insert(f.end(), payload, payload + size);
    return f;
}

Bytes CloseFrame(uint16_t code) {
    const uint8_t body[2] = {uint8_t(code >> 8), uint8_t(code)};
    return Frame(kClose, body, sizeof(body));
}

void WsParser::Reset() {
    buf_.clear();
    message_.clear();
    inMessage_ = false;
    messageOpcode_ = 0;
    error_ = nullptr;
    tooBig_ = false;
}

bool WsParser::Feed(const uint8_t* data, size_t size, const OnMessage& onMessage) {
    if (error_) return false;
    buf_.insert(buf_.end(), data, data + size);
    size_t pos = 0;
    bool ok = true;
    while (ok) {
        const size_t avail = buf_.size() - pos;
        if (avail < 2) break;
        const uint8_t* p = buf_.data() + pos;
        const bool fin = (p[0] & 0x80) != 0;
        const uint8_t opcode = p[0] & 0x0F;
        if (p[0] & 0x70) { ok = Fail("reserved bits set (no extensions were agreed)"); break; }
        if (!(p[1] & 0x80)) { ok = Fail("a client frame is not masked"); break; }
        const bool control = opcode >= 8;
        if (control ? opcode > kPong : opcode > kBinary) { ok = Fail("unknown opcode"); break; }
        uint64_t length = p[1] & 0x7F;
        size_t head = 2;
        if (length == 126) {
            if (avail < 4) break;
            length = uint64_t(p[2]) << 8 | p[3];
            head = 4;
        } else if (length == 127) {
            if (avail < 10) break;
            length = 0;
            for (int i = 0; i < 8; ++i) length = length << 8 | p[2 + i];
            if (length >> 63) { ok = Fail("bad length"); break; }
            head = 10;
        }
        if (control && (!fin || length > 125)) { ok = Fail("bad control frame"); break; }
        if (!control) {
            if (opcode == kContinuation && !inMessage_) { ok = Fail("continuation without a message"); break; }
            if (opcode != kContinuation && inMessage_) { ok = Fail("new message inside a fragmented one"); break; }
            if (length > kMaxMessage || message_.size() + length > kMaxMessage) {
                tooBig_ = true;
                ok = Fail("message too big");
                break;
            }
        }
        if (avail < head + 4 + length) break; // Wait for the rest of the frame.
        const uint8_t* mask = p + head;
        uint8_t* body = buf_.data() + pos + head + 4;
        const size_t n = size_t(length);
        for (size_t i = 0; i < n; ++i) body[i] ^= mask[i & 3];
        pos += head + 4 + n;

        if (control) {
            onMessage(opcode, body, n);
        } else if (fin && opcode != kContinuation) {
            onMessage(opcode, body, n); // The usual case: one frame, one message, no copy.
        } else {
            if (opcode != kContinuation) {
                inMessage_ = true;
                messageOpcode_ = opcode;
                message_.clear();
            }
            message_.insert(message_.end(), body, body + n);
            if (fin) {
                inMessage_ = false;
                onMessage(messageOpcode_, message_.data(), message_.size());
                message_.clear();
            }
        }
    }
    buf_.erase(buf_.begin(), buf_.begin() + pos);
    return ok;
}

// --- Tokens ----------------------------------------------------------------------------------------------

std::string TokenBook::NewPairingCode(const Bytes& random16, uint64_t nowMs) {
    code_ = Base64Url(random16.data(), random16.size());
    codeExpires_ = nowMs + kPairingCodeMs;
    return code_;
}

bool TokenBook::UsePairingCode(const std::string& code, uint64_t nowMs) {
    if (code_.empty() || nowMs >= codeExpires_ || !SameSecret(code, code_)) return false;
    code_.clear(); // Once only.
    return true;
}

std::string TokenBook::AddDevice(const std::string& name, const Bytes& random32, const Bytes& random8, uint64_t nowUnix,
                                 WebDevice* added) {
    const std::string token = Base64Url(random32.data(), random32.size());
    WebDevice d;
    d.id = Hex(random8.data(), random8.size());
    d.name = CleanDeviceName(name);
    d.tokenHash = Sha256(token);
    d.addedUnix = nowUnix;
    devices_.push_back(d);
    if (added) *added = d;
    return token;
}

const WebDevice* TokenBook::FindDevice(const std::string& token) const {
    if (token.empty()) return nullptr;
    const Bytes hash = Sha256(token);
    const std::string h(hash.begin(), hash.end());
    const WebDevice* found = nullptr;
    for (const WebDevice& d : devices_) {
        if (SameSecret(h, std::string(d.tokenHash.begin(), d.tokenHash.end())) && !found) found = &d;
    }
    return found;
}

bool TokenBook::Forget(const std::string& id) {
    const auto before = devices_.size();
    devices_.erase(std::remove_if(devices_.begin(), devices_.end(), [&](const WebDevice& d) { return d.id == id; }),
                   devices_.end());
    return devices_.size() != before;
}

namespace {

// Code points that would let a name reach outside its own line in a window: the line and paragraph
// separators, and the bidirectional overrides and isolates (which can reverse the text around them).
bool Unshowable(uint32_t cp) {
    return cp == 0x2028 || cp == 0x2029 ||                 // Line / paragraph separator.
           cp == 0x200E || cp == 0x200F ||                  // Left-to-right / right-to-left mark.
           (cp >= 0x202A && cp <= 0x202E) ||                // Embedding and override.
           (cp >= 0x2066 && cp <= 0x2069) ||                // Isolates.
           cp == 0xFEFF;                                    // Zero-width no-break space.
}

// The code point of a `len`-byte UTF-8 sequence starting at `s`.
uint32_t CodePoint(const char* s, size_t len) {
    const uint8_t c = uint8_t(s[0]);
    uint32_t cp = len == 1 ? c : len == 2 ? (c & 0x1Fu) : len == 3 ? (c & 0x0Fu) : (c & 0x07u);
    for (size_t k = 1; k < len; ++k) cp = cp << 6 | (uint8_t(s[k]) & 0x3Fu);
    return cp;
}

} // namespace

std::string CleanName(const std::string& name, const std::string& fallback) {
    std::string out;
    for (size_t i = 0; i < name.size();) {
        const uint8_t c = uint8_t(name[i]);
        size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (len == 0 || i + len > name.size()) break; // Not UTF-8: stop here.
        bool valid = true;
        for (size_t k = 1; k < len; ++k) valid &= (uint8_t(name[i + k]) & 0xC0) == 0x80;
        if (!valid) break;
        if (len == 1 && (c < 0x20 || c == 0x7F)) { i += 1; continue; } // Control characters go.
        if (len > 1 && Unshowable(CodePoint(name.data() + i, len))) { i += len; continue; }
        if (out.size() + len > 40) break;
        out.append(name, i, len);
        i += len;
    }
    out = Trim(out);
    return out.empty() ? fallback : out;
}

std::string CleanDeviceName(const std::string& name) { return CleanName(name, "iPhone"); }

// --- Addresses -------------------------------------------------------------------------------------------

NetKind ClassifyAdapter(uint32_t ifType, const std::wstring& description, uint32_t ip) {
    std::wstring d = description;
    for (wchar_t& c : d) c = wchar_t(std::towlower(c));
    auto has = [&](const wchar_t* s) { return d.find(s) != std::wstring::npos; };
    if (ifType == 24 /* loopback */ || (ip >> 16) == 0xA9FE /* 169.254/16 */ || (ip >> 24) == 127) return NetKind::Skip;
    // Virtual switches of VMs and containers: a phone can never reach them.
    if (has(L"hyper-v") || has(L"vethernet") || has(L"vmware") || has(L"virtualbox") || has(L"wsl") || has(L"docker"))
        return NetKind::Skip;
    if (has(L"apple mobile device")) return NetKind::IPhoneUsb;
    // VPNs and tunnels: listed last (a phone on the same VPN could still use them).
    if (ifType == 131 /* tunnel */ || ifType == 23 /* ppp */ || ifType == 53 /* virtual */ || has(L"vpn") ||
        has(L"tap-") || has(L"wireguard") || has(L"wintun") || has(L"tailscale") || has(L"zerotier"))
        return NetKind::Other;
    if (ifType == 71) return NetKind::WiFi;
    if (ifType == 6) return NetKind::Ethernet;
    return NetKind::Other;
}

void SortAddresses(std::vector<NetAddress>* list) {
    list->erase(std::remove_if(list->begin(), list->end(), [](const NetAddress& a) { return a.kind == NetKind::Skip; }),
                list->end());
    std::stable_sort(list->begin(), list->end(),
                     [](const NetAddress& a, const NetAddress& b) { return int(a.kind) < int(b.kind); });
}

} // namespace mycam::web
