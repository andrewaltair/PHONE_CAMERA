#include "wifi_crypto.h"

// The primitives from wifi_crypto.h on OpenSSL 3 (libcrypto), for builds without Windows CNG. Must stay
// byte-compatible with wifi_crypto.cpp: both are checked against protocol/golden.txt "wifi.*".

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/param_build.h>
#include <openssl/rand.h>

#include <string.h>

namespace mycam::wifi {

namespace {

void PutU32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }

void Nonce(uint32_t dir, uint64_t counter, uint8_t out[12]) {
    PutU32(out, dir);
    PutU32(out + 4, uint32_t(counter >> 32));
    PutU32(out + 8, uint32_t(counter));
}

Bytes Digest(const EVP_MD* md, const Bytes& data) {
    Bytes out(EVP_MD_get_size(md));
    unsigned size = 0;
    EVP_Digest(data.data(), data.size(), out.data(), &size, md, nullptr);
    return out;
}

// A P-256 key from its raw parts: the public point (0x04 | X | Y) and, for a key pair, the private scalar.
EVP_PKEY* FromRaw(const Bytes& publicRaw, const Bytes* d) {
    OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
    BIGNUM* priv = d ? BN_bin2bn(d->data(), int(d->size()), nullptr) : nullptr;
    EVP_PKEY* key = nullptr;
    if (bld && (!d || priv) &&
        OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0) &&
        OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY, publicRaw.data(), publicRaw.size()) &&
        (!priv || OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_PRIV_KEY, priv))) {
        OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(bld);
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
        if (params && ctx && EVP_PKEY_fromdata_init(ctx) > 0)
            EVP_PKEY_fromdata(ctx, &key, d ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY, params);
        EVP_PKEY_CTX_free(ctx);
        OSSL_PARAM_free(params);
    }
    BN_clear_free(priv);
    OSSL_PARAM_BLD_free(bld);
    return key;
}

EVP_PKEY* Pkey(void* p) { return static_cast<EVP_PKEY*>(p); }

} // namespace

Bytes Random(size_t n) {
    Bytes out(n);
    RAND_bytes(out.data(), int(n));
    return out;
}

Bytes Sha1(const Bytes& data) { return Digest(EVP_sha1(), data); }
Bytes Sha256(const Bytes& data) { return Digest(EVP_sha256(), data); }

Bytes HmacSha256(const Bytes& key, const Bytes& data) {
    Bytes out(32);
    unsigned size = 0;
    HMAC(EVP_sha256(), key.data(), int(key.size()), data.data(), data.size(), out.data(), &size);
    return out;
}

// --- EcKey ------------------------------------------------------------------------------------------

EcKey::~EcKey() {
    EVP_PKEY_free(Pkey(key_));
}

bool EcKey::Generate() {
    EVP_PKEY* key = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256");
    if (!key) return false;
    EVP_PKEY_free(Pkey(key_));
    key_ = key;
    return true;
}

bool EcKey::ImportPrivate(const Bytes& d, const Bytes& publicRaw) {
    if (d.size() != 32 || publicRaw.size() != 65) return false;
    EVP_PKEY* key = FromRaw(publicRaw, &d);
    if (!key) return false;
    EVP_PKEY_free(Pkey(key_));
    key_ = key;
    return true;
}

Bytes EcKey::PublicRaw() const {
    if (!key_) return {};
    unsigned char* raw = nullptr;
    const size_t size = EVP_PKEY_get1_encoded_public_key(Pkey(key_), &raw); // Uncompressed: 0x04 | X | Y.
    Bytes out;
    if (size == 65) out.assign(raw, raw + size);
    OPENSSL_free(raw);
    return out;
}

bool EcKey::Agree(const Bytes& peerPublicRaw, Bytes* secret) const {
    if (!key_ || peerPublicRaw.size() != 65 || peerPublicRaw[0] != 4) return false;
    EVP_PKEY* peer = FromRaw(peerPublicRaw, nullptr);
    if (!peer) return false;
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(Pkey(key_), nullptr);
    size_t size = 32;
    secret->assign(32, 0);
    // validate_peer = 1: rejects a point that is not on the curve. The secret is X, big-endian (like Java).
    const bool ok = ctx && EVP_PKEY_derive_init(ctx) > 0 && EVP_PKEY_derive_set_peer_ex(ctx, peer, 1) > 0 &&
                    EVP_PKEY_derive(ctx, secret->data(), &size) > 0 && size == 32;
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(peer);
    return ok;
}

// --- RecordKey --------------------------------------------------------------------------------------
// key_ holds a copy of the 32-byte AES key; each record gets its own cipher context (a nonce per record).

RecordKey::~RecordKey() {
    if (key_) OPENSSL_clear_free(key_, 32);
}

bool RecordKey::Init(const Bytes& key, uint32_t dir) {
    if (key_) OPENSSL_clear_free(key_, 32);
    key_ = nullptr;
    dir_ = dir;
    counter_ = 0;
    if (key.size() != 32) return false;
    key_ = OPENSSL_memdup(key.data(), 32);
    return key_ != nullptr;
}

bool RecordKey::Seal(const uint8_t* plain, size_t size, Bytes* out) {
    if (!key_) return false;
    uint8_t nonce[12];
    Nonce(dir_, counter_, nonce);
    const size_t start = out->size();
    out->resize(start + 4 + size + 16);
    uint8_t* p = out->data() + start;
    PutU32(p, uint32_t(size + 16));
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int len = 0, fin = 0;
    const bool ok = ctx && EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, static_cast<uint8_t*>(key_), nonce) &&
                    EVP_EncryptUpdate(ctx, p + 4, &len, plain, int(size)) &&
                    EVP_EncryptFinal_ex(ctx, p + 4 + len, &fin) &&
                    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, p + 4 + size);
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) { out->resize(start); return false; }
    ++counter_;
    return true;
}

bool RecordKey::Open(const uint8_t* body, size_t size, Bytes* plain) {
    if (!key_ || size < 16) return false;
    uint8_t nonce[12], tag[16];
    Nonce(dir_, counter_, nonce);
    memcpy(tag, body + size - 16, 16);
    plain->resize(size - 16);
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int len = 0, fin = 0;
    const bool ok = ctx && EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, static_cast<uint8_t*>(key_), nonce) &&
                    EVP_DecryptUpdate(ctx, plain->data(), &len, body, int(size - 16)) &&
                    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag) &&
                    EVP_DecryptFinal_ex(ctx, plain->data() + len, &fin) > 0; // Fails: tampered, wrong key or out of order.
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return false;
    ++counter_;
    return true;
}

} // namespace mycam::wifi
