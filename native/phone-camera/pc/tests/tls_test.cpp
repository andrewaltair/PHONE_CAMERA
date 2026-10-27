// Linux: the OpenSSL tls::Session (companion/tls_server_openssl.cpp) against an OpenSSL client, in memory.
// Checks the certificate is made once and reused, a TLS 1.3 handshake, data both ways, and close_notify.

#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

#include "../companion/tls_server.h"

using mycam::tls::Bytes;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                        \
    } while (0)

// The client's side: an SSL over memory BIOs. Pump() carries bytes both ways until neither side has more.
struct Client {
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    SSL* ssl = nullptr;
    Client() {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr); // Self-signed, like Safari after the tap-through.
        ssl = SSL_new(ctx);
        SSL_set_bio(ssl, BIO_new(BIO_s_mem()), BIO_new(BIO_s_mem()));
        SSL_set_connect_state(ssl);
    }
    ~Client() { SSL_free(ssl); SSL_CTX_free(ctx); }
    Bytes Out() {
        Bytes out;
        uint8_t buf[16384];
        int n;
        while ((n = BIO_read(SSL_get_wbio(ssl), buf, sizeof(buf))) > 0) out.insert(out.end(), buf, buf + n);
        return out;
    }
    void In(const Bytes& b) { if (!b.empty()) BIO_write(SSL_get_rbio(ssl), b.data(), int(b.size())); }
};

bool Pump(Client& c, mycam::tls::Session& s, Bytes* serverPlain) {
    for (int i = 0; i < 20; ++i) {
        SSL_do_handshake(c.ssl);
        const Bytes toServer = c.Out();
        Bytes toClient;
        if (!s.Receive(toServer.data(), toServer.size(), &toClient, serverPlain)) return false;
        c.In(toClient);
        if (toServer.empty() && toClient.empty()) return true;
    }
    return true;
}

} // namespace

int main() {
    char dir[] = "/tmp/mycam-tls-test-XXXXXX";
    if (!mkdtemp(dir)) return 1;
    setenv("XDG_CONFIG_HOME", dir, 1);

    printf("tls: certificate made once, then reused\n");
    mycam::tls::Credentials cred;
    std::string why;
    CHECK(cred.Init(&why));
    const std::string keyPath = std::string(dir) + "/mycam/web-key.pem";
    struct stat st{};
    CHECK(stat(keyPath.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    mycam::tls::Credentials again;
    CHECK(again.Init(&why));
    CHECK(again.Thumbprint() == cred.Thumbprint() && cred.Thumbprint().size() == 40);

    printf("tls: handshake and data both ways\n");
    {
        Client c;
        mycam::tls::Session s(&cred);
        Bytes plain;
        CHECK(Pump(c, s, &plain));
        CHECK(s.Ready() && SSL_is_init_finished(c.ssl));
        CHECK(SSL_version(c.ssl) == TLS1_3_VERSION);
        X509* peer = SSL_get1_peer_certificate(c.ssl);
        CHECK(peer && X509_check_host(peer, "localhost", 0, 0, nullptr) == 1);
        X509_free(peer);

        const char hello[] = "GET / HTTP/1.1\r\n\r\n";
        SSL_write(c.ssl, hello, int(strlen(hello)));
        const Bytes rec = c.Out();
        Bytes reply;
        CHECK(s.Receive(rec.data(), rec.size(), &reply, &plain));
        CHECK(std::string(plain.begin(), plain.end()) == hello);

        Bytes big(100000, 0x5a), out; // Bigger than one TLS record.
        CHECK(s.Encrypt(big.data(), big.size(), &out));
        c.In(reply);
        c.In(out);
        Bytes got(big.size() + 10);
        size_t n = 0;
        int r;
        while ((r = SSL_read(c.ssl, got.data() + n, int(got.size() - n))) > 0) n += size_t(r);
        CHECK(n == big.size() && memcmp(got.data(), big.data(), n) == 0);

        printf("tls: close_notify\n");
        SSL_shutdown(c.ssl);
        const Bytes bye = c.Out();
        Bytes ignored;
        CHECK(!s.Receive(bye.data(), bye.size(), &ignored, &plain));
        CHECK(s.Closed() && !s.Error());
    }

    printf("tls: garbage fails the handshake\n");
    {
        mycam::tls::Session s(&cred);
        const Bytes junk(64, 'x');
        Bytes toSend, plain;
        CHECK(!s.Receive(junk.data(), junk.size(), &toSend, &plain));
        CHECK(s.Error() && !s.Closed());
    }

    std::string cleanup = std::string("rm -rf ") + dir;
    if (system(cleanup.c_str()) != 0) printf("  (could not remove %s)\n", dir);
    printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
