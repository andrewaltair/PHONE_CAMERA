#include "tls_server.h"

// tls_server.h on OpenSSL, for Linux. The same certificate as the Schannel version (tls_server.cpp): RSA-2048 /
// SHA-256, self-signed, 397 days, renewed 30 days before it ends. It lives in ~/.config/mycam/ (web-cert.pem,
// and web-key.pem readable only by this user). The opaque handle slots in the header hold an SSL_CTX*
// (Credentials) and an SSL* (Session), run over memory BIOs so the byte-in/bytes-out interface stays the same.

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "log.h"

namespace mycam::tls {

namespace {

constexpr long kDay = 24L * 60 * 60;
constexpr int kLifetimeDays = 397;
constexpr int kRenewDays = 30; // Make a new one when the current one has less than this left.

SSL_CTX* CtxOf(const uintptr_t* h) { return reinterpret_cast<SSL_CTX*>(h[0]); }
SSL* SslOf(const uintptr_t* h) { return reinterpret_cast<SSL*>(h[0]); }

// $XDG_CONFIG_HOME/mycam, else ~/.config/mycam; made (0700) if missing. "" if there is no home.
std::string ConfigDir() {
    std::string base;
    if (const char* x = getenv("XDG_CONFIG_HOME"); x && *x) base = x;
    else if (const char* h = getenv("HOME"); h && *h) base = std::string(h) + "/.config";
    else return "";
    mkdir(base.c_str(), 0700);
    const std::string dir = base + "/mycam";
    mkdir(dir.c_str(), 0700);
    return dir;
}

std::vector<in_addr> LocalIpv4s() {
    std::vector<in_addr> out;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
        if (!(a->ifa_flags & IFF_UP) || (a->ifa_flags & IFF_LOOPBACK)) continue;
        out.push_back(reinterpret_cast<const sockaddr_in*>(a->ifa_addr)->sin_addr);
    }
    freeifaddrs(list);
    return out;
}

bool AddExt(X509* cert, int nid, const std::string& value) {
    X509V3_CTX v3;
    X509V3_set_ctx_nodb(&v3);
    X509V3_set_ctx(&v3, cert, cert, nullptr, nullptr, 0);
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &v3, nid, value.c_str());
    const bool ok = ext && X509_add_ext(cert, ext, -1);
    X509_EXTENSION_free(ext);
    return ok;
}

// A new RSA-2048 / SHA-256 self-signed server certificate.
bool CreateCertificate(X509** certOut, EVP_PKEY** keyOut, std::string* why) {
    EVP_PKEY* key = EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", size_t(2048));
    X509* cert = X509_new();
    if (!key || !cert) { *why = "could not make a key"; EVP_PKEY_free(key); X509_free(cert); return false; }
    uint8_t serial[16];
    RAND_bytes(serial, sizeof(serial));
    serial[0] &= 0x7f; // Positive.
    BIGNUM* bn = BN_bin2bn(serial, sizeof(serial), nullptr);
    BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(cert));
    BN_free(bn);
    X509_set_version(cert, 2); // v3
    X509_gmtime_adj(X509_getm_notBefore(cert), -kDay);
    X509_gmtime_adj(X509_getm_notAfter(cert), kDay * kLifetimeDays);
    X509_NAME* name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("MyCam web camera"), -1, -1, 0);
    X509_set_issuer_name(cert, name);
    X509_set_pubkey(cert, key);
    // Names: localhost and every IPv4 address the PC has now. Safari's one-time tap-through doesn't need
    // them to match, but a matching certificate shows a clearer warning.
    std::string san = "DNS:localhost";
    for (const in_addr& ip : LocalIpv4s()) {
        char text[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &ip, text, sizeof(text))) san += std::string(",IP:") + text;
    }
    if (!AddExt(cert, NID_subject_alt_name, san) || !AddExt(cert, NID_ext_key_usage, "serverAuth") ||
        !AddExt(cert, NID_key_usage, "critical,digitalSignature,keyEncipherment") ||
        !AddExt(cert, NID_basic_constraints, "critical,CA:FALSE") || !X509_sign(cert, key, EVP_sha256())) {
        *why = "could not build the certificate";
        EVP_PKEY_free(key);
        X509_free(cert);
        return false;
    }
    *certOut = cert;
    *keyOut = key;
    return true;
}

bool Save(const std::string& dir, X509* cert, EVP_PKEY* key, std::string* why) {
    const std::string keyPath = dir + "/web-key.pem", certPath = dir + "/web-cert.pem";
    const int fd = open(keyPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600); // Only this user.
    FILE* kf = fd >= 0 ? fdopen(fd, "w") : nullptr;
    if (!kf) { if (fd >= 0) close(fd); *why = keyPath + ": " + strerror(errno); return false; }
    bool ok = PEM_write_PrivateKey(kf, key, nullptr, nullptr, 0, nullptr, nullptr);
    ok = fclose(kf) == 0 && ok;
    FILE* cf = fopen(certPath.c_str(), "w");
    ok = ok && cf && PEM_write_X509(cf, cert);
    if (cf) ok = fclose(cf) == 0 && ok;
    if (!ok) *why = "could not save the certificate in " + dir;
    return ok;
}

// The saved certificate if it is usable (not near its end, matches its key), else a new one (saved).
bool LoadOrCreate(X509** certOut, EVP_PKEY** keyOut, std::string* why) {
    const std::string dir = ConfigDir();
    if (dir.empty()) { *why = "no home directory for the certificate"; return false; }
    X509* cert = nullptr;
    EVP_PKEY* key = nullptr;
    if (FILE* f = fopen((dir + "/web-cert.pem").c_str(), "r")) { cert = PEM_read_X509(f, nullptr, nullptr, nullptr); fclose(f); }
    if (FILE* f = fopen((dir + "/web-key.pem").c_str(), "r")) { key = PEM_read_PrivateKey(f, nullptr, nullptr, nullptr); fclose(f); }
    time_t renewAfter = time(nullptr) + kDay * kRenewDays;
    if (cert && key && X509_cmp_time(X509_get0_notAfter(cert), &renewAfter) > 0 && X509_check_private_key(cert, key)) {
        *certOut = cert;
        *keyOut = key;
        return true;
    }
    X509_free(cert); // Expiring or broken: a new one replaces it.
    EVP_PKEY_free(key);
    ERR_clear_error();
    if (!CreateCertificate(&cert, &key, why)) return false;
    if (!Save(dir, cert, key, why)) { X509_free(cert); EVP_PKEY_free(key); return false; }
    Log("web: made a new self-signed certificate (valid %d days)", kLifetimeDays);
    *certOut = cert;
    *keyOut = key;
    return true;
}

std::string ThumbprintOf(X509* cert) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned size = 0;
    if (!X509_digest(cert, EVP_sha1(), hash, &size)) return "?";
    static const char digits[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned i = 0; i < size; ++i) { out += digits[hash[i] >> 4]; out += digits[hash[i] & 15]; }
    return out;
}

// Moves what OpenSSL wrote (handshake messages, alerts, records) out of the session's write BIO.
void Drain(SSL* ssl, Bytes* out) {
    BIO* w = SSL_get_wbio(ssl);
    uint8_t buf[16 * 1024];
    int n;
    while ((n = BIO_read(w, buf, sizeof(buf))) > 0) out->insert(out->end(), buf, buf + n);
}

} // namespace

// --- Credentials -----------------------------------------------------------------------------------------

Credentials::~Credentials() {
    if (valid_) SSL_CTX_free(CtxOf(cred_));
}

bool Credentials::Init(std::string* why) {
    X509* cert = nullptr;
    EVP_PKEY* key = nullptr;
    if (!LoadOrCreate(&cert, &key, why)) return false;
    thumbprint_ = ThumbprintOf(cert);
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    const bool ok = ctx && SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) && SSL_CTX_use_certificate(ctx, cert) &&
                    SSL_CTX_use_PrivateKey(ctx, key);
    X509_free(cert); // The context holds its own references.
    EVP_PKEY_free(key);
    if (!ok) {
        SSL_CTX_free(ctx);
        *why = "OpenSSL refused the certificate";
        return false;
    }
    cred_[0] = reinterpret_cast<uintptr_t>(ctx);
    valid_ = true;
    return true;
}

// --- Session ---------------------------------------------------------------------------------------------

Session::~Session() {
    if (haveCtx_) SSL_free(SslOf(ctx_));
}

bool Session::Receive(const uint8_t* data, size_t size, Bytes* toSend, Bytes* plain) {
    if (error_ || closed_) return false;
    if (!haveCtx_) {
        SSL* ssl = SSL_new(CtxOf(static_cast<uintptr_t*>(cred_->Handle())));
        if (!ssl) { error_ = "TLS: no session"; return false; }
        SSL_set_bio(ssl, BIO_new(BIO_s_mem()), BIO_new(BIO_s_mem()));
        SSL_set_accept_state(ssl);
        ctx_[0] = reinterpret_cast<uintptr_t>(ssl);
        haveCtx_ = true;
    }
    SSL* ssl = SslOf(ctx_);
    if (size) BIO_write(SSL_get_rbio(ssl), data, int(size));
    if (!ready_ && !Handshake(toSend)) return false;
    if (!ready_) return true; // Needs more bytes.
    uint8_t buf[16 * 1024];
    for (;;) {
        const int n = SSL_read(ssl, buf, sizeof(buf));
        if (n > 0) { plain->insert(plain->end(), buf, buf + n); continue; }
        status_ = SSL_get_error(ssl, n);
        Drain(ssl, toSend); // Post-handshake messages (TLS 1.3 key updates) may need an answer.
        if (status_ == SSL_ERROR_WANT_READ) return true;
        if (status_ == SSL_ERROR_ZERO_RETURN) closed_ = true; // close_notify from the client.
        else error_ = "TLS record could not be decrypted";
        ERR_clear_error();
        return false;
    }
}

bool Session::Handshake(Bytes* toSend) {
    SSL* ssl = SslOf(ctx_);
    const int r = SSL_do_handshake(ssl);
    Drain(ssl, toSend); // Handshake messages, or the alert explaining a failure.
    if (r == 1) { ready_ = true; return true; }
    status_ = SSL_get_error(ssl, r);
    if (status_ == SSL_ERROR_WANT_READ) return true;
    error_ = "TLS handshake failed";
    ERR_clear_error();
    return false;
}

bool Session::Encrypt(const uint8_t* data, size_t size, Bytes* out) {
    if (!ready_) return false;
    if (size == 0) return true;
    SSL* ssl = SslOf(ctx_);
    const int n = SSL_write(ssl, data, int(size)); // A memory BIO takes it all at once.
    if (n <= 0 || size_t(n) != size) {
        status_ = SSL_get_error(ssl, n);
        error_ = "TLS encrypt failed";
        ERR_clear_error();
        return false;
    }
    Drain(ssl, out);
    return true;
}

} // namespace mycam::tls
