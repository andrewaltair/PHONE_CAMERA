#include "wifi_crypto.h"

// The handshake and key schedule, built on the primitives each platform's backend supplies
// (wifi_crypto.cpp on Windows CNG, wifi_crypto_openssl.cpp elsewhere).

#include <stdio.h>
#include <string.h>

namespace mycam::wifi {

Bytes Concat(std::initializer_list<const Bytes*> parts) {
    Bytes out;
    for (const Bytes* p : parts) out.insert(out.end(), p->begin(), p->end());
    return out;
}

Bytes Ascii(const char* s) { return Bytes(s, s + strlen(s)); }

Bytes Hkdf(const Bytes& ikm, const Bytes& salt, const Bytes& info, size_t length) {
    const Bytes prk = HmacSha256(salt.empty() ? Bytes(32, 0) : salt, ikm);
    Bytes out, t;
    for (uint8_t i = 1; out.size() < length; ++i) {
        Bytes block = t;
        block.insert(block.end(), info.begin(), info.end());
        block.push_back(i);
        t = HmacSha256(prk, block);
        out.insert(out.end(), t.begin(), t.end());
    }
    out.resize(length);
    return out;
}

Bytes Commit(const Bytes& nb, const Bytes& phonePub, const Bytes& pcPub) {
    const Bytes label = Ascii("MyCam commit v1");
    return Sha256(Concat({&label, &nb, &phonePub, &pcPub}));
}

std::string Sas(const Bytes& pcPub, const Bytes& phonePub, const Bytes& na, const Bytes& nb) {
    const Bytes h = Sha256(Concat({&pcPub, &phonePub, &na, &nb}));
    const uint32_t v = uint32_t(h[0]) << 24 | uint32_t(h[1]) << 16 | uint32_t(h[2]) << 8 | h[3];
    char code[8];
    snprintf(code, sizeof(code), "%06u", unsigned(v % 1000000u));
    return code;
}

Bytes PairKey(const Bytes& ecdh, const Bytes& na, const Bytes& nb) {
    return Hkdf(ecdh, Concat({&na, &nb}), Ascii("MyCam pair v1"), 32);
}

Bytes SessionKeys(const Bytes& ecdh, const Bytes& pairKey, const Bytes& npc, const Bytes& nph) {
    return Hkdf(Concat({&ecdh, &pairKey}), Concat({&npc, &nph}), Ascii("MyCam session v1"), 96);
}

Bytes Finished(const Bytes& kFin, const char* label, const Bytes& transcript) {
    const Bytes l = Ascii(label);
    return HmacSha256(kFin, Concat({&l, &transcript}));
}

bool SameMac(const Bytes& a, const Bytes& b) {
    if (a.size() != b.size()) return false;
    unsigned diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff |= unsigned(a[i] ^ b[i]);
    return diff == 0;
}

Bytes QrPairKey(const Bytes& ecdh, const Bytes& code, const Bytes& npc, const Bytes& nph) {
    return Hkdf(Concat({&ecdh, &code}), Concat({&npc, &nph}), Ascii("MyCam qr pair v1"), 32);
}

std::string QrInviteProof(const Bytes& code, uint16_t tcpPort) {
    const Bytes label = Ascii("MyCam qr invite v1"), p = {uint8_t(tcpPort >> 8), uint8_t(tcpPort)};
    const Bytes mac = HmacSha256(code, Concat({&label, &p}));
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < 16; ++i) { out += digits[mac[i] >> 4]; out += digits[mac[i] & 15]; }
    return out;
}

} // namespace mycam::wifi
