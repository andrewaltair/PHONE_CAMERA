#pragma once
// Crypto for the wireless link (PROTOCOL.md "Wireless security"): P-256 ECDH, HKDF-SHA256 and AES-256-GCM
// records. The primitives come from Windows CNG (wifi_crypto.cpp) or OpenSSL (wifi_crypto_openssl.cpp); the
// handshake built on them is in wifi_crypto_common.cpp. Byte-compatible with the phone's WifiCrypto.kt; both are checked
// against protocol/golden.txt "wifi.*", generated with the JDK.

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace mycam::wifi {

using Bytes = std::vector<uint8_t>;

constexpr uint32_t kDirPcToPhone = 0;
constexpr uint32_t kDirPhoneToPc = 1;
constexpr uint32_t kMaxRecord = 16 * 1024 * 1024;

Bytes Concat(std::initializer_list<const Bytes*> parts);
Bytes Ascii(const char* s);
Bytes Random(size_t n);
Bytes Sha1(const Bytes& data); // WebSocket handshake only (web_proto.cpp), not the wireless link.
Bytes Sha256(const Bytes& data);
Bytes HmacSha256(const Bytes& key, const Bytes& data);
Bytes Hkdf(const Bytes& ikm, const Bytes& salt, const Bytes& info, size_t length); // RFC 5869

// Handshake values (names match golden.txt).
Bytes Commit(const Bytes& nb, const Bytes& phonePub, const Bytes& pcPub);
std::string Sas(const Bytes& pcPub, const Bytes& phonePub, const Bytes& na, const Bytes& nb); // 6 digits
Bytes PairKey(const Bytes& ecdh, const Bytes& na, const Bytes& nb);
Bytes SessionKeys(const Bytes& ecdh, const Bytes& pairKey, const Bytes& npc, const Bytes& nph); // 96 bytes
Bytes Finished(const Bytes& kFin, const char* label, const Bytes& transcript);
// Equal contents, in time that doesn't depend on where they differ. For comparing a MAC against one a phone
// sent: operator== on a vector stops at the first difference.
bool SameMac(const Bytes& a, const Bytes& b);
// QR pairing (PROTOCOL.md "QR pairing"). The pairing key, with the QR code's one-time code (16 bytes) mixed in
// so only a phone that saw the PC's screen can finish the handshake: HKDF(z | code, Npc | Nph, "MyCam qr pair
// v1", 32). Golden "qr.pair_key".
Bytes QrPairKey(const Bytes& ecdh, const Bytes& code, const Bytes& npc, const Bytes& nph);
// The proof in the phone's "MYCAM#1" invite: hex of the first 16 bytes of HMAC(code, "MyCam qr invite v1" |
// tcp port, u16 BE). Golden "qr.invite_proof".
std::string QrInviteProof(const Bytes& code, uint16_t tcpPort);

// An ephemeral P-256 key pair.
class EcKey {
public:
    EcKey() = default;
    EcKey(const EcKey&) = delete;
    EcKey& operator=(const EcKey&) = delete;
    ~EcKey();
    bool Generate();
    bool ImportPrivate(const Bytes& d, const Bytes& publicRaw); // For tests.
    Bytes PublicRaw() const;                                    // 0x04 | X | Y
    bool Agree(const Bytes& peerPublicRaw, Bytes* secret) const; // X coordinate, big-endian (like Java)
private:
    void* key_ = nullptr; // BCRYPT_KEY_HANDLE
};

// AES-256-GCM with the record nonce (u32 direction | u64 counter, big-endian).
class RecordKey {
public:
    RecordKey() = default;
    RecordKey(const RecordKey&) = delete;
    RecordKey& operator=(const RecordKey&) = delete;
    ~RecordKey();
    bool Init(const Bytes& key, uint32_t dir);
    // Appends one record (u32 BE ciphertext length, ciphertext, tag) to *out.
    bool Seal(const uint8_t* plain, size_t size, Bytes* out);
    // Decrypts one record body (ciphertext + tag). False if it was tampered with or out of order.
    bool Open(const uint8_t* body, size_t size, Bytes* plain);
private:
    void* key_ = nullptr; // BCRYPT_KEY_HANDLE
    uint32_t dir_ = 0;
    uint64_t counter_ = 0;
};

} // namespace mycam::wifi
