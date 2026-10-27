#pragma once
// TLS for the iPhone page's server: Schannel on Windows (tls_server.cpp, built in, so no new dependency),
// OpenSSL on Linux (tls_server_openssl.cpp). The private handle slots below are opaque platform handles.
//
// The certificate is self-signed and made once per user: on Windows it is kept in the current user's
// certificate store ("MyCam web camera", private key not exportable), on Linux in ~/.config/mycam/. Safari warns about it the first time; after one tap
// through, the same certificate is trusted for that address. It is renewed (another tap-through) only when
// it nears its end, 397 days after it was made: iOS refuses TLS certificates that live longer than that.

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

namespace mycam::tls {

using Bytes = std::vector<uint8_t>;

// Server credentials for the certificate (found, or made now). Shared by every connection.
class Credentials {
public:
    Credentials() = default;
    Credentials(const Credentials&) = delete;
    Credentials& operator=(const Credentials&) = delete;
    ~Credentials();
    bool Init(std::string* why);
    void* Handle() { return &cred_[0]; } // CredHandle*
    std::string Thumbprint() const { return thumbprint_; } // SHA-1 hex, for the log.

private:
    uintptr_t cred_[2] = {};
    bool valid_ = false;
    const void* cert_ = nullptr; // PCCERT_CONTEXT
    std::string thumbprint_;
};

// One server-side TLS connection. Bytes from the socket go into Receive(); what it returns in *toSend must
// go back out (handshake messages, alerts); decrypted application data comes out in *plain.
class Session {
public:
    explicit Session(Credentials* credentials) : cred_(credentials) {}
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session();

    // False when the connection must close (bad data, handshake failure). Closed() then says whether the
    // client simply said goodbye (close_notify).
    bool Receive(const uint8_t* data, size_t size, Bytes* toSend, Bytes* plain);
    // Encrypts application data into TLS records (appended to *out).
    bool Encrypt(const uint8_t* data, size_t size, Bytes* out);
    bool Ready() const { return ready_; }
    bool Closed() const { return closed_; }
    const char* Error() const { return error_; }
    long Status() const { return status_; }

private:
    bool Handshake(Bytes* toSend);
    bool Decrypt(Bytes* toSend, Bytes* plain, bool* more);

    Credentials* cred_;
    uintptr_t ctx_[2] = {};
    bool haveCtx_ = false;
    bool ready_ = false;
    bool closed_ = false;
    const char* error_ = nullptr;
    long status_ = 0;
    Bytes in_; // Received bytes not consumed yet.
    unsigned long header_ = 0, trailer_ = 0, maxMessage_ = 0;
};

} // namespace mycam::tls
