#include "wifi_crypto.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>

namespace mycam::wifi {

namespace {

// Algorithm providers, opened once (they are thread-safe and cheap to keep).
BCRYPT_ALG_HANDLE Provider(LPCWSTR id, ULONG flags = 0) {
    BCRYPT_ALG_HANDLE h = nullptr;
    return BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&h, id, nullptr, flags)) ? h : nullptr;
}
BCRYPT_ALG_HANDLE Sha1Alg() { static BCRYPT_ALG_HANDLE h = Provider(BCRYPT_SHA1_ALGORITHM); return h; }
BCRYPT_ALG_HANDLE ShaAlg() { static BCRYPT_ALG_HANDLE h = Provider(BCRYPT_SHA256_ALGORITHM); return h; }
BCRYPT_ALG_HANDLE HmacAlg() { static BCRYPT_ALG_HANDLE h = Provider(BCRYPT_SHA256_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG); return h; }
BCRYPT_ALG_HANDLE EcdhAlg() { static BCRYPT_ALG_HANDLE h = Provider(BCRYPT_ECDH_P256_ALGORITHM); return h; }
BCRYPT_ALG_HANDLE GcmAlg() {
    static BCRYPT_ALG_HANDLE h = [] {
        BCRYPT_ALG_HANDLE a = Provider(BCRYPT_AES_ALGORITHM);
        if (a) BCryptSetProperty(a, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
        return a;
    }();
    return h;
}

void PutU32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }

void Nonce(uint32_t dir, uint64_t counter, uint8_t out[12]) {
    PutU32(out, dir);
    PutU32(out + 4, uint32_t(counter >> 32));
    PutU32(out + 8, uint32_t(counter));
}

} // namespace

Bytes Random(size_t n) {
    Bytes out(n);
    BCryptGenRandom(nullptr, out.data(), ULONG(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return out;
}

Bytes Sha1(const Bytes& data) {
    Bytes out(20);
    BCryptHash(Sha1Alg(), nullptr, 0, const_cast<PUCHAR>(data.data()), ULONG(data.size()), out.data(), 20);
    return out;
}

Bytes Sha256(const Bytes& data) {
    Bytes out(32);
    BCryptHash(ShaAlg(), nullptr, 0, const_cast<PUCHAR>(data.data()), ULONG(data.size()), out.data(), 32);
    return out;
}

Bytes HmacSha256(const Bytes& key, const Bytes& data) {
    Bytes out(32);
    BCryptHash(HmacAlg(), const_cast<PUCHAR>(key.data()), ULONG(key.size()), const_cast<PUCHAR>(data.data()),
               ULONG(data.size()), out.data(), 32);
    return out;
}

// --- EcKey ------------------------------------------------------------------------------------------

EcKey::~EcKey() {
    if (key_) BCryptDestroyKey(key_);
}

bool EcKey::Generate() {
    BCRYPT_KEY_HANDLE h = nullptr;
    if (!BCRYPT_SUCCESS(BCryptGenerateKeyPair(EcdhAlg(), &h, 256, 0))) return false;
    if (!BCRYPT_SUCCESS(BCryptFinalizeKeyPair(h, 0))) { BCryptDestroyKey(h); return false; }
    key_ = h;
    return true;
}

bool EcKey::ImportPrivate(const Bytes& d, const Bytes& publicRaw) {
    if (d.size() != 32 || publicRaw.size() != 65) return false;
    Bytes blob(sizeof(BCRYPT_ECCKEY_BLOB) + 96);
    auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    header->dwMagic = BCRYPT_ECDH_PRIVATE_P256_MAGIC;
    header->cbKey = 32;
    memcpy(blob.data() + sizeof(*header), publicRaw.data() + 1, 64);
    memcpy(blob.data() + sizeof(*header) + 64, d.data(), 32);
    BCRYPT_KEY_HANDLE h = nullptr;
    if (!BCRYPT_SUCCESS(BCryptImportKeyPair(EcdhAlg(), nullptr, BCRYPT_ECCPRIVATE_BLOB, &h, blob.data(), ULONG(blob.size()), 0)))
        return false;
    key_ = h;
    return true;
}

Bytes EcKey::PublicRaw() const {
    Bytes blob(sizeof(BCRYPT_ECCKEY_BLOB) + 64);
    ULONG size = 0;
    if (!BCRYPT_SUCCESS(BCryptExportKey(key_, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob.data(), ULONG(blob.size()), &size, 0)))
        return {};
    Bytes out(65);
    out[0] = 4;
    memcpy(out.data() + 1, blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), 64);
    return out;
}

bool EcKey::Agree(const Bytes& peerPublicRaw, Bytes* secret) const {
    if (peerPublicRaw.size() != 65 || peerPublicRaw[0] != 4) return false;
    Bytes blob(sizeof(BCRYPT_ECCKEY_BLOB) + 64);
    auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    header->dwMagic = BCRYPT_ECDH_PUBLIC_P256_MAGIC;
    header->cbKey = 32;
    memcpy(blob.data() + sizeof(*header), peerPublicRaw.data() + 1, 64);
    BCRYPT_KEY_HANDLE peer = nullptr;
    // Import validates that the point is on the curve.
    if (!BCRYPT_SUCCESS(BCryptImportKeyPair(EcdhAlg(), nullptr, BCRYPT_ECCPUBLIC_BLOB, &peer, blob.data(), ULONG(blob.size()), 0)))
        return false;
    BCRYPT_SECRET_HANDLE agreed = nullptr;
    bool ok = BCRYPT_SUCCESS(BCryptSecretAgreement(key_, peer, &agreed, 0));
    if (ok) {
        secret->assign(32, 0);
        ULONG size = 0;
        ok = BCRYPT_SUCCESS(BCryptDeriveKey(agreed, BCRYPT_KDF_RAW_SECRET, nullptr, secret->data(), 32, &size, 0)) && size == 32;
        // CNG returns the raw secret little-endian; Java (and the protocol) use big-endian.
        std::reverse(secret->begin(), secret->end());
        BCryptDestroySecret(agreed);
    }
    BCryptDestroyKey(peer);
    return ok;
}

// --- RecordKey --------------------------------------------------------------------------------------

RecordKey::~RecordKey() {
    if (key_) BCryptDestroyKey(key_);
}

bool RecordKey::Init(const Bytes& key, uint32_t dir) {
    if (key_) BCryptDestroyKey(key_);
    key_ = nullptr;
    dir_ = dir;
    counter_ = 0;
    BCRYPT_KEY_HANDLE h = nullptr;
    if (key.size() != 32 ||
        !BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(GcmAlg(), &h, nullptr, 0, const_cast<PUCHAR>(key.data()), 32, 0)))
        return false;
    key_ = h;
    return true;
}

bool RecordKey::Seal(const uint8_t* plain, size_t size, Bytes* out) {
    uint8_t nonce[12], tag[16];
    Nonce(dir_, counter_, nonce);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = nonce;
    info.cbNonce = sizeof(nonce);
    info.pbTag = tag;
    info.cbTag = sizeof(tag);
    const size_t start = out->size();
    out->resize(start + 4 + size + 16);
    uint8_t* p = out->data() + start;
    PutU32(p, uint32_t(size + 16));
    ULONG written = 0;
    if (!BCRYPT_SUCCESS(BCryptEncrypt(key_, const_cast<PUCHAR>(plain), ULONG(size), &info, nullptr, 0, p + 4,
                                      ULONG(size), &written, 0))) {
        out->resize(start);
        return false;
    }
    memcpy(p + 4 + size, tag, 16);
    ++counter_;
    return true;
}

bool RecordKey::Open(const uint8_t* body, size_t size, Bytes* plain) {
    if (size < 16) return false;
    uint8_t nonce[12], tag[16];
    Nonce(dir_, counter_, nonce);
    memcpy(tag, body + size - 16, 16);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = nonce;
    info.cbNonce = sizeof(nonce);
    info.pbTag = tag;
    info.cbTag = sizeof(tag);
    plain->resize(size - 16);
    ULONG written = 0;
    if (!BCRYPT_SUCCESS(BCryptDecrypt(key_, const_cast<PUCHAR>(body), ULONG(size - 16), &info, nullptr, 0,
                                      plain->data(), ULONG(plain->size()), &written, 0)))
        return false; // STATUS_AUTH_TAG_MISMATCH: tampered, wrong key or out of order.
    ++counter_;
    return true;
}

} // namespace mycam::wifi
