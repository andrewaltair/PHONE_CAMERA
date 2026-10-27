#pragma once
// Wi-Fi discovery bookkeeping (PROTOCOL.md "Wireless transport"): parsing the phones' answers and the
// "Scan for phones" list the window shows. Pure C++ (no Windows types), unit-tested in pc/tests.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace mycam {

// One phone that answered a discovery probe.
struct NearbyPhone {
    uint32_t ip = 0;          // IPv4, network order (the key: one entry per address).
    std::string ipText;       // "192.168.1.23"
    uint16_t port = 0;        // Its TCP port for the session.
    std::string name;         // UTF-8, as the phone announced it (the address if it sent none).
    bool paired = false;      // This PC has a pairing key stored under that name (best effort).
    bool connected = false;   // The current Wi-Fi session is with this phone.
    uint64_t lastSeen = 0;    // GetTickCount64() of its last answer.
};

// "<tag> <tcp port> <phone name>" -> port and name (name may be empty). False if it isn't that message.
// `tag` is "MYCAM!1" (an answer to a probe) or "MYCAM+1" (the phone asking the PC to connect).
// `maxRest` caps what follows the port. It is 64 for the plain messages, where that is the name; the QR
// invite spends 33 of its own on the proof and a space, so it asks for more (see ParseDiscoveryQrRequest).
inline bool ParseDiscoveryMessage(const std::string& text, const char* tag, uint16_t* port, std::string* name,
                                  size_t maxRest = 64) {
    const size_t tagLen = std::strlen(tag);
    if (text.compare(0, tagLen, tag) != 0) return false;
    if (text.size() < tagLen + 1 || text[tagLen] != ' ') return false; // "MYCAM!10 ..." isn't ours.
    const size_t portStart = tagLen + 1;
    size_t portEnd = portStart;
    while (portEnd < text.size() && text[portEnd] >= '0' && text[portEnd] <= '9') ++portEnd;
    if (portEnd == portStart || portEnd - portStart > 5) return false;
    if (portEnd < text.size() && text[portEnd] != ' ') return false;
    const long p = std::strtol(text.substr(portStart, portEnd - portStart).c_str(), nullptr, 10);
    if (p <= 0 || p > 65535) return false;
    *port = uint16_t(p);
    std::string n = portEnd < text.size() ? text.substr(portEnd + 1) : std::string();
    while (!n.empty() && (n.back() == ' ' || n.back() == '\r' || n.back() == '\n' || n.back() == '\0')) n.pop_back();
    if (n.size() > maxRest) n.resize(maxRest);
    *name = n;
    return true;
}

// A phone's answer to a probe: "MYCAM!1 <tcp port> <phone name>".
inline bool ParseDiscoveryReply(const std::string& text, uint16_t* port, std::string* name) {
    return ParseDiscoveryMessage(text, "MYCAM!1", port, name);
}

// A phone asking this PC to connect to it: "MYCAM+1 <tcp port> <phone name>" (PROTOCOL.md "The phone asks
// for a session"). Same body as an answer, so a pairing can be started from the phone's own list.
inline bool ParseDiscoveryConnectRequest(const std::string& text, uint16_t* port, std::string* name) {
    return ParseDiscoveryMessage(text, "MYCAM+1", port, name);
}

// A phone that scanned this PC's QR code asking it to connect (PROTOCOL.md "QR pairing"):
// "MYCAM#1 <tcp port> <proof: 32 hex digits> <phone name>". The proof is lower-cased.
inline bool ParseDiscoveryQrRequest(const std::string& text, uint16_t* port, std::string* proof, std::string* name) {
    std::string rest;
    // 32 for the proof, 1 for the space, 64 for the name: the same room for a name as the other messages.
    if (!ParseDiscoveryMessage(text, "MYCAM#1", port, &rest, 32 + 1 + 64)) return false;
    const size_t space = rest.find(' ');
    std::string p = rest.substr(0, space);
    if (p.size() != 32) return false;
    for (char& c : p) {
        if (c >= 'A' && c <= 'F') c = char(c - 'A' + 'a');
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    *proof = p;
    *name = space == std::string::npos ? std::string() : rest.substr(space + 1);
    return true;
}

// The phones found on the network, in the order they first answered, one per IP address, at most kMax.
class NearbyList {
public:
    static constexpr size_t kMax = 16;

    // Records an answer. Returns true if the visible list changed (a new phone, or a new name or port).
    bool Seen(uint32_t ip, const std::string& ipText, uint16_t port, const std::string& name, uint64_t now) {
        for (auto& p : phones_) {
            if (p.ip != ip) continue;
            const std::string shown = name.empty() ? ipText : name;
            const bool changed = p.port != port || p.name != shown;
            p.port = port;
            p.name = shown;
            p.lastSeen = now;
            return changed;
        }
        NearbyPhone p;
        p.ip = ip;
        p.ipText = ipText;
        p.port = port;
        p.name = name.empty() ? ipText : name;
        p.lastSeen = now;
        if (phones_.size() >= kMax) {
            // Full: replace the one heard from longest ago (never the connected one).
            auto oldest = phones_.end();
            for (auto it = phones_.begin(); it != phones_.end(); ++it) {
                if (it->connected) continue;
                if (oldest == phones_.end() || it->lastSeen < oldest->lastSeen) oldest = it;
            }
            if (oldest == phones_.end()) return false;
            phones_.erase(oldest);
        }
        phones_.push_back(p);
        return true;
    }

    // Drops phones that haven't answered for maxAgeMs (the connected one stays). True if any went.
    bool Expire(uint64_t now, uint64_t maxAgeMs) {
        const size_t before = phones_.size();
        phones_.erase(std::remove_if(phones_.begin(), phones_.end(),
                                     [&](const NearbyPhone& p) { return !p.connected && now - p.lastSeen > maxAgeMs; }),
                      phones_.end());
        return phones_.size() != before;
    }

    // Marks the phone of the current Wi-Fi session (ip 0: none). True if anything changed.
    bool SetConnected(uint32_t ip) {
        bool changed = false;
        for (auto& p : phones_) {
            const bool c = ip != 0 && p.ip == ip;
            changed |= p.connected != c;
            p.connected = c;
        }
        return changed;
    }

    // Sets `paired` from a predicate on the phone name. True if anything changed.
    template <typename F> bool UpdatePaired(F&& isPaired) {
        bool changed = false;
        for (auto& p : phones_) {
            const bool v = isPaired(p.name);
            changed |= p.paired != v;
            p.paired = v;
        }
        return changed;
    }

    const NearbyPhone* Find(uint32_t ip) const {
        for (const auto& p : phones_) if (p.ip == ip) return &p;
        return nullptr;
    }

    void Clear() { phones_.clear(); }
    // A new scan starts from an empty list, except for the phone already connected.
    void ClearForScan() {
        phones_.erase(std::remove_if(phones_.begin(), phones_.end(), [](const NearbyPhone& p) { return !p.connected; }),
                      phones_.end());
    }
    const std::vector<NearbyPhone>& Phones() const { return phones_; }

private:
    std::vector<NearbyPhone> phones_;
};

// The sentence under a finished scan (a live region: Narrator reads it).
inline std::wstring ScanResultSentence(size_t found) {
    if (found == 0) return L"No phone found. On the phone, open PHONE_CAMERA and turn on “Use over Wi-Fi”. Both must be on the same Wi-Fi.";
    if (found == 1) return L"Found 1 phone.";
    return L"Found " + std::to_wstring(found) + L" phones.";
}

} // namespace mycam
