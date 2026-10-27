#include "tls_server.h"

#define SECURITY_WIN32
#define SCHANNEL_USE_BLACKLISTS
#include <winsock2.h>
#include <windows.h>
#include <objbase.h>
#include <subauth.h>
#include <sspi.h>
#include <schannel.h>
#include <wincrypt.h>
#include <ncrypt.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>

#include <algorithm>

#include "log.h"

namespace mycam::tls {

namespace {

constexpr wchar_t kFriendlyName[] = L"MyCam web camera";
constexpr ULONGLONG kDay = 24ull * 60 * 60 * 10000000; // In FILETIME units (100 ns).
constexpr int kLifetimeDays = 397;
constexpr int kRenewDays = 30; // Make a new one when the current one has less than this left.

CredHandle* Cred(uintptr_t* h) { return reinterpret_cast<CredHandle*>(h); }
CtxtHandle* Ctx(uintptr_t* h) { return reinterpret_cast<CtxtHandle*>(h); }

ULONGLONG FileTimeNow() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ULARGE_INTEGER{{ft.dwLowDateTime, ft.dwHighDateTime}}.QuadPart;
}

SYSTEMTIME SystemTimeAt(ULONGLONG t) {
    FILETIME ft = {DWORD(t), DWORD(t >> 32)};
    SYSTEMTIME st;
    FileTimeToSystemTime(&ft, &st);
    return st;
}

ULONGLONG ToU64(const FILETIME& ft) { return ULARGE_INTEGER{{ft.dwLowDateTime, ft.dwHighDateTime}}.QuadPart; }

bool IsMyCam(PCCERT_CONTEXT c) {
    wchar_t name[64] = {};
    DWORD size = sizeof(name);
    return CertGetCertificateContextProperty(c, CERT_FRIENDLY_NAME_PROP_ID, name, &size) && wcscmp(name, kFriendlyName) == 0;
}

bool HasPrivateKey(PCCERT_CONTEXT c) {
    HCRYPTPROV_OR_NCRYPT_KEY_HANDLE key = 0;
    DWORD spec = 0;
    BOOL mustFree = FALSE;
    if (!CryptAcquireCertificatePrivateKey(c, CRYPT_ACQUIRE_ONLY_NCRYPT_KEY_FLAG | CRYPT_ACQUIRE_SILENT_FLAG, nullptr, &key,
                                           &spec, &mustFree))
        return false;
    if (mustFree) NCryptFreeObject(key);
    return true;
}

// Deletes a certificate and its private key.
void DeleteWithKey(PCCERT_CONTEXT c) {
    HCRYPTPROV_OR_NCRYPT_KEY_HANDLE key = 0;
    DWORD spec = 0;
    BOOL mustFree = FALSE;
    if (CryptAcquireCertificatePrivateKey(c, CRYPT_ACQUIRE_ONLY_NCRYPT_KEY_FLAG | CRYPT_ACQUIRE_SILENT_FLAG, nullptr, &key,
                                          &spec, &mustFree)) {
        NCryptDeleteKey(key, 0); // Frees the handle too.
    }
    CertDeleteCertificateFromStore(c); // Frees the context.
}

std::string ThumbprintOf(PCCERT_CONTEXT c) {
    BYTE hash[20];
    DWORD size = sizeof(hash);
    if (!CertGetCertificateContextProperty(c, CERT_SHA1_HASH_PROP_ID, hash, &size)) return "?";
    static const char digits[] = "0123456789ABCDEF";
    std::string out;
    for (DWORD i = 0; i < size; ++i) { out += digits[hash[i] >> 4]; out += digits[hash[i] & 15]; }
    return out;
}

std::vector<uint32_t> LocalIpv4s() {
    std::vector<uint32_t> out;
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    auto* list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    }
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size) != NO_ERROR) return out;
    for (auto* a = list; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next)
            out.push_back(reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr);
    }
    return out;
}

struct Encoded {
    BYTE* data = nullptr;
    DWORD size = 0;
    ~Encoded() { if (data) LocalFree(data); }
};

bool Encode(LPCSTR type, const void* info, Encoded* out) {
    return CryptEncodeObjectEx(X509_ASN_ENCODING, type, info, CRYPT_ENCODE_ALLOC_FLAG, nullptr, &out->data, &out->size) != 0;
}

// A new RSA-2048 / SHA-256 self-signed server certificate, added to the user's store.
PCCERT_CONTEXT CreateCertificate(HCERTSTORE store, std::string* why) {
    NCRYPT_PROV_HANDLE prov = 0;
    if (NCryptOpenStorageProvider(&prov, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS) { *why = "no key storage provider"; return nullptr; }
    // A unique key name, so a new certificate never reuses (or deletes) an older one's key.
    GUID guid;
    CoCreateGuid(&guid);
    wchar_t keyName[96];
    swprintf_s(keyName, L"MyCam web camera %08lX%04X%04X", guid.Data1, guid.Data2, guid.Data3);
    NCRYPT_KEY_HANDLE key = 0;
    SECURITY_STATUS st = NCryptCreatePersistedKey(prov, &key, BCRYPT_RSA_ALGORITHM, keyName, 0, 0);
    if (st == ERROR_SUCCESS) {
        DWORD bits = 2048, usage = NCRYPT_ALLOW_SIGNING_FLAG | NCRYPT_ALLOW_DECRYPT_FLAG;
        NCryptSetProperty(key, NCRYPT_LENGTH_PROPERTY, reinterpret_cast<PBYTE>(&bits), sizeof(bits), 0);
        NCryptSetProperty(key, NCRYPT_KEY_USAGE_PROPERTY, reinterpret_cast<PBYTE>(&usage), sizeof(usage), 0);
        st = NCryptFinalizeKey(key, NCRYPT_SILENT_FLAG);
    }
    NCryptFreeObject(prov);
    if (st != ERROR_SUCCESS) {
        if (key) NCryptFreeObject(key);
        *why = "could not make a key (" + std::to_string(st) + ")";
        return nullptr;
    }

    BYTE subject[256];
    DWORD subjectSize = sizeof(subject);
    CertStrToNameW(X509_ASN_ENCODING, L"CN=MyCam web camera", CERT_X500_NAME_STR, nullptr, subject, &subjectSize, nullptr);
    CERT_NAME_BLOB subjectBlob = {subjectSize, subject};

    // Names: localhost and every IPv4 address the PC has now. Safari's one-time tap-through doesn't need
    // them to match, but a matching certificate shows a clearer warning.
    std::vector<uint32_t> ips = LocalIpv4s();
    std::vector<CERT_ALT_NAME_ENTRY> names;
    CERT_ALT_NAME_ENTRY local = {};
    local.dwAltNameChoice = CERT_ALT_NAME_DNS_NAME;
    local.pwszDNSName = const_cast<LPWSTR>(L"localhost");
    names.push_back(local);
    for (uint32_t& ip : ips) {
        CERT_ALT_NAME_ENTRY e = {};
        e.dwAltNameChoice = CERT_ALT_NAME_IP_ADDRESS;
        e.IPAddress = {4, reinterpret_cast<BYTE*>(&ip)};
        names.push_back(e);
    }
    CERT_ALT_NAME_INFO san = {DWORD(names.size()), names.data()};
    LPSTR serverAuth = const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH);
    CERT_ENHKEY_USAGE eku = {1, &serverAuth};
    BYTE kuBits = CERT_DIGITAL_SIGNATURE_KEY_USAGE | CERT_KEY_ENCIPHERMENT_KEY_USAGE;
    CRYPT_BIT_BLOB ku = {1, &kuBits, 0};
    CERT_BASIC_CONSTRAINTS2_INFO basic = {FALSE, FALSE, 0};
    Encoded sanEnc, ekuEnc, kuEnc, basicEnc;
    if (!Encode(X509_ALTERNATE_NAME, &san, &sanEnc) || !Encode(X509_ENHANCED_KEY_USAGE, &eku, &ekuEnc) ||
        !Encode(X509_KEY_USAGE, &ku, &kuEnc) || !Encode(X509_BASIC_CONSTRAINTS2, &basic, &basicEnc)) {
        NCryptFreeObject(key);
        *why = "could not encode the certificate";
        return nullptr;
    }
    CERT_EXTENSION ext[] = {
        {const_cast<LPSTR>(szOID_SUBJECT_ALT_NAME2), FALSE, {sanEnc.size, sanEnc.data}},
        {const_cast<LPSTR>(szOID_ENHANCED_KEY_USAGE), FALSE, {ekuEnc.size, ekuEnc.data}},
        {const_cast<LPSTR>(szOID_KEY_USAGE), TRUE, {kuEnc.size, kuEnc.data}},
        {const_cast<LPSTR>(szOID_BASIC_CONSTRAINTS2), TRUE, {basicEnc.size, basicEnc.data}},
    };
    CERT_EXTENSIONS exts = {DWORD(std::size(ext)), ext};

    CRYPT_KEY_PROV_INFO provInfo = {};
    provInfo.pwszContainerName = keyName;
    provInfo.pwszProvName = const_cast<LPWSTR>(MS_KEY_STORAGE_PROVIDER);
    CRYPT_ALGORITHM_IDENTIFIER alg = {const_cast<LPSTR>(szOID_RSA_SHA256RSA), {}};
    const ULONGLONG now = FileTimeNow();
    SYSTEMTIME start = SystemTimeAt(now - kDay), end = SystemTimeAt(now + kDay * kLifetimeDays);
    PCCERT_CONTEXT cert = CertCreateSelfSignCertificate(key, &subjectBlob, 0, &provInfo, &alg, &start, &end, &exts);
    if (!cert) {
        *why = "CertCreateSelfSignCertificate failed (" + std::to_string(GetLastError()) + ")";
        NCryptDeleteKey(key, 0);
        return nullptr;
    }
    NCryptFreeObject(key);
    CRYPT_DATA_BLOB friendly = {DWORD(sizeof(kFriendlyName)), reinterpret_cast<BYTE*>(const_cast<wchar_t*>(kFriendlyName))};
    CertSetCertificateContextProperty(cert, CERT_FRIENDLY_NAME_PROP_ID, 0, &friendly);
    PCCERT_CONTEXT stored = nullptr;
    if (!CertAddCertificateContextToStore(store, cert, CERT_STORE_ADD_NEW, &stored)) {
        *why = "could not store the certificate (" + std::to_string(GetLastError()) + ")";
        CertFreeCertificateContext(cert);
        return nullptr;
    }
    CertFreeCertificateContext(cert);
    return stored;
}

// The MyCam certificate in CurrentUser\My: the newest usable one, or a new one. Old ones are removed.
PCCERT_CONTEXT LoadOrCreate(std::string* why) {
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER, L"MY");
    if (!store) { *why = "can't open the certificate store"; return nullptr; }
    const ULONGLONG renewAfter = FileTimeNow() + kDay * kRenewDays;
    std::vector<PCCERT_CONTEXT> mine;
    for (PCCERT_CONTEXT c = nullptr; (c = CertEnumCertificatesInStore(store, c)) != nullptr;)
        if (IsMyCam(c)) mine.push_back(CertDuplicateCertificateContext(c));
    PCCERT_CONTEXT best = nullptr;
    for (PCCERT_CONTEXT c : mine) {
        const bool usable = ToU64(c->pCertInfo->NotAfter) > renewAfter && HasPrivateKey(c);
        if (usable && (!best || ToU64(c->pCertInfo->NotAfter) > ToU64(best->pCertInfo->NotAfter))) best = c;
    }
    for (PCCERT_CONTEXT c : mine) {
        if (c == best) continue;
        DeleteWithKey(c); // Expiring or broken: a new one replaces it.
    }
    if (!best) {
        best = CreateCertificate(store, why);
        if (best) Log("web: made a new self-signed certificate (valid %d days)", kLifetimeDays);
    }
    CertCloseStore(store, 0);
    return best;
}

} // namespace

// --- Credentials -----------------------------------------------------------------------------------------

Credentials::~Credentials() {
    if (valid_) FreeCredentialsHandle(Cred(cred_));
    if (cert_) CertFreeCertificateContext(static_cast<PCCERT_CONTEXT>(cert_));
}

bool Credentials::Init(std::string* why) {
    PCCERT_CONTEXT cert = LoadOrCreate(why);
    if (!cert) return false;
    cert_ = cert;
    thumbprint_ = ThumbprintOf(cert);

    TLS_PARAMETERS params = {};
    params.grbitDisabledProtocols = SP_PROT_SSL3_SERVER | SP_PROT_TLS1_0_SERVER | SP_PROT_TLS1_1_SERVER;
    SCH_CREDENTIALS sc = {};
    sc.dwVersion = SCH_CREDENTIALS_VERSION;
    sc.cCreds = 1;
    sc.paCred = &cert;
    sc.dwFlags = SCH_USE_STRONG_CRYPTO;
    sc.cTlsParameters = 1;
    sc.pTlsParameters = &params;
    TimeStamp expiry;
    SECURITY_STATUS st = AcquireCredentialsHandleW(nullptr, const_cast<LPWSTR>(UNISP_NAME_W), SECPKG_CRED_INBOUND, nullptr,
                                                   &sc, nullptr, nullptr, Cred(cred_), &expiry);
    if (st != SEC_E_OK) {
        char text[64];
        sprintf_s(text, "Schannel refused the certificate (0x%08lX)", unsigned long(st));
        *why = text;
        return false;
    }
    valid_ = true;
    return true;
}

// --- Session ---------------------------------------------------------------------------------------------

Session::~Session() {
    if (haveCtx_) DeleteSecurityContext(Ctx(ctx_));
}

bool Session::Receive(const uint8_t* data, size_t size, Bytes* toSend, Bytes* plain) {
    if (error_ || closed_) return false;
    in_.insert(in_.end(), data, data + size);
    for (;;) {
        if (!ready_) {
            if (in_.empty()) return true;
            const size_t before = in_.size();
            if (!Handshake(toSend)) return false;
            if (!ready_ && in_.size() == before) return true; // Needs more bytes.
            continue;
        }
        bool more = false;
        if (!Decrypt(toSend, plain, &more)) return false;
        if (!more) return true;
    }
}

bool Session::Handshake(Bytes* toSend) {
    SecBuffer inBufs[2] = {{ULONG(in_.size()), SECBUFFER_TOKEN, in_.data()}, {0, SECBUFFER_EMPTY, nullptr}};
    SecBufferDesc inDesc = {SECBUFFER_VERSION, 2, inBufs};
    SecBuffer outBufs[2] = {{0, SECBUFFER_TOKEN, nullptr}, {0, SECBUFFER_ALERT, nullptr}};
    SecBufferDesc outDesc = {SECBUFFER_VERSION, 2, outBufs};
    ULONG attrs = 0;
    const ULONG req = ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY | ASC_REQ_EXTENDED_ERROR |
                      ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM;
    SECURITY_STATUS st = AcceptSecurityContext(Cred(reinterpret_cast<uintptr_t*>(cred_->Handle())),
                                               haveCtx_ ? Ctx(ctx_) : nullptr, &inDesc, req, 0, Ctx(ctx_), &outDesc,
                                               &attrs, nullptr);
    status_ = st;
    for (SecBuffer& b : outBufs) {
        if (b.pvBuffer) {
            if (b.cbBuffer) toSend->insert(toSend->end(), static_cast<uint8_t*>(b.pvBuffer), static_cast<uint8_t*>(b.pvBuffer) + b.cbBuffer);
            FreeContextBuffer(b.pvBuffer);
        }
    }
    if (st == SEC_E_INCOMPLETE_MESSAGE) return true; // Keep in_ and wait for more.
    if (st == SEC_E_OK || st == SEC_I_CONTINUE_NEEDED) haveCtx_ = true;
    if (FAILED(st)) {
        error_ = "TLS handshake failed";
        return false;
    }
    // Whatever the call didn't use stays for the next step (or is the first application data).
    if (inBufs[1].BufferType == SECBUFFER_EXTRA && inBufs[1].cbBuffer > 0 && inBufs[1].cbBuffer <= in_.size())
        in_.erase(in_.begin(), in_.end() - inBufs[1].cbBuffer);
    else
        in_.clear();
    if (st == SEC_E_OK) {
        SecPkgContext_StreamSizes sizes = {};
        if (QueryContextAttributesW(Ctx(ctx_), SECPKG_ATTR_STREAM_SIZES, &sizes) != SEC_E_OK) {
            error_ = "TLS: no stream sizes";
            return false;
        }
        header_ = sizes.cbHeader;
        trailer_ = sizes.cbTrailer;
        maxMessage_ = sizes.cbMaximumMessage;
        ready_ = true;
    }
    return true;
}

bool Session::Decrypt(Bytes* toSend, Bytes* plain, bool* more) {
    *more = false;
    if (in_.empty()) return true;
    SecBuffer bufs[4] = {{ULONG(in_.size()), SECBUFFER_DATA, in_.data()}, {0, SECBUFFER_EMPTY, nullptr},
                         {0, SECBUFFER_EMPTY, nullptr}, {0, SECBUFFER_EMPTY, nullptr}};
    SecBufferDesc desc = {SECBUFFER_VERSION, 4, bufs};
    SECURITY_STATUS st = DecryptMessage(Ctx(ctx_), &desc, 0, nullptr);
    status_ = st;
    if (st == SEC_E_INCOMPLETE_MESSAGE) return true;
    if (st == SEC_I_CONTEXT_EXPIRED) {
        closed_ = true; // close_notify from the client.
        return false;
    }
    if (st != SEC_E_OK && st != SEC_I_RENEGOTIATE) {
        error_ = "TLS record could not be decrypted";
        return false;
    }
    Bytes extra;
    for (SecBuffer& b : bufs) {
        if (b.BufferType == SECBUFFER_DATA && b.cbBuffer)
            plain->insert(plain->end(), static_cast<uint8_t*>(b.pvBuffer), static_cast<uint8_t*>(b.pvBuffer) + b.cbBuffer);
        else if (b.BufferType == SECBUFFER_EXTRA && b.cbBuffer)
            extra.assign(static_cast<uint8_t*>(b.pvBuffer), static_cast<uint8_t*>(b.pvBuffer) + b.cbBuffer);
    }
    in_.swap(extra);
    if (st == SEC_I_RENEGOTIATE) {
        // TLS 1.3 post-handshake message (a key update, say): Schannel wants AcceptSecurityContext again with
        // whatever followed it, even if that is nothing.
        ready_ = false;
        if (!Handshake(toSend)) return false;
        *more = !in_.empty();
        return true;
    }
    *more = !in_.empty();
    return true;
}

bool Session::Encrypt(const uint8_t* data, size_t size, Bytes* out) {
    if (!ready_) return false;
    size_t done = 0;
    Bytes record;
    while (done < size) {
        const ULONG chunk = ULONG(std::min<size_t>(size - done, maxMessage_));
        record.resize(header_ + chunk + trailer_);
        memcpy(record.data() + header_, data + done, chunk);
        SecBuffer bufs[4] = {{header_, SECBUFFER_STREAM_HEADER, record.data()},
                             {chunk, SECBUFFER_DATA, record.data() + header_},
                             {trailer_, SECBUFFER_STREAM_TRAILER, record.data() + header_ + chunk},
                             {0, SECBUFFER_EMPTY, nullptr}};
        SecBufferDesc desc = {SECBUFFER_VERSION, 4, bufs};
        SECURITY_STATUS st = EncryptMessage(Ctx(ctx_), 0, &desc, 0);
        if (st != SEC_E_OK) {
            status_ = st;
            error_ = "TLS encrypt failed";
            return false;
        }
        for (int i = 0; i < 3; ++i) {
            const uint8_t* p = static_cast<uint8_t*>(bufs[i].pvBuffer);
            out->insert(out->end(), p, p + bufs[i].cbBuffer);
        }
        done += chunk;
    }
    return true;
}

} // namespace mycam::tls
