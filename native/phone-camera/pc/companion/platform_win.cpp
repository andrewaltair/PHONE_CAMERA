#include "platform.h"
#include "net.h"

// platform.h and net.h on Windows. The code moved here from phone_link*.cpp and web_server.cpp unchanged in
// behaviour: the same registry keys and value types, the same DPAPI protection.

#include <windows.h>
#include <iphlpapi.h>
#include <wincrypt.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <iterator>

#include "../common/guids.h"

namespace mycam {

namespace {

std::wstring KeyPath(const std::string& path) {
    std::wstring key = kSettingsKey;
    if (!path.empty()) {
        std::wstring sub = Wide(path);
        std::replace(sub.begin(), sub.end(), L'/', L'\\');
        key += L"\\" + sub;
    }
    return key;
}

bool GetValue(const std::string& path, const std::string& name, DWORD flags, std::vector<uint8_t>* out) {
    const std::wstring key = KeyPath(path), value = Wide(name);
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), value.c_str(), flags, nullptr, nullptr, &size) != ERROR_SUCCESS) return false;
    out->resize(size);
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), value.c_str(), flags, nullptr, out->data(), &size) != ERROR_SUCCESS) return false;
    out->resize(size);
    return true;
}

bool SetValue(const std::string& path, const std::string& name, DWORD type, const void* data, DWORD size) {
    return RegSetKeyValueW(HKEY_CURRENT_USER, KeyPath(path).c_str(), Wide(name).c_str(), type, data, size) == ERROR_SUCCESS;
}

} // namespace

uint64_t NowMs() { return GetTickCount64(); }
void SleepMs(uint32_t ms) { Sleep(ms); }
uint64_t UnixTime() { return uint64_t(_time64(nullptr)); }

std::wstring Wide(const std::string& utf8) {
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), w.data(), int(w.size()));
    return w;
}

std::string Utf8(const std::wstring& w) {
    std::string s(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), int(s.size()), nullptr, nullptr);
    return s;
}

std::string ComputerName() {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(name, &n)) return "PC";
    std::string utf8 = Utf8(std::wstring(name, n));
    if (utf8.size() > 63) utf8.resize(63); // As before: at most 63 bytes.
    return utf8;
}

std::vector<Adapter> UpAdapters() {
    std::vector<Adapter> out;
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
        if (a->OperStatus != IfOperStatusUp) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            Adapter ad;
            ad.ipv4 = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr;
            ad.prefix = u->OnLinkPrefixLength <= 32 ? uint8_t(u->OnLinkPrefixLength) : 0;
            ad.ifType = a->IfType;
            ad.name = a->FriendlyName ? a->FriendlyName : L"";
            ad.description = a->Description ? a->Description : L"";
            out.push_back(std::move(ad));
        }
    }
    return out;
}

bool StoreGetBytes(const std::string& path, const std::string& name, std::vector<uint8_t>* value) {
    return GetValue(path, name, RRF_RT_REG_BINARY, value);
}

bool StoreSetBytes(const std::string& path, const std::string& name, const std::vector<uint8_t>& value) {
    return SetValue(path, name, REG_BINARY, value.data(), DWORD(value.size()));
}

bool StoreGetString(const std::string& path, const std::string& name, std::string* value) {
    std::vector<uint8_t> raw;
    if (!GetValue(path, name, RRF_RT_REG_SZ, &raw)) return false;
    std::wstring w(reinterpret_cast<const wchar_t*>(raw.data()), raw.size() / sizeof(wchar_t));
    while (!w.empty() && w.back() == L'\0') w.pop_back();
    *value = Utf8(w);
    return true;
}

bool StoreSetString(const std::string& path, const std::string& name, const std::string& value) {
    const std::wstring w = Wide(value);
    return SetValue(path, name, REG_SZ, w.c_str(), DWORD((w.size() + 1) * sizeof(wchar_t)));
}

bool StoreGetU64(const std::string& path, const std::string& name, uint64_t* value) {
    std::vector<uint8_t> raw;
    if (!GetValue(path, name, RRF_RT_REG_QWORD, &raw) || raw.size() != sizeof(uint64_t)) return false;
    memcpy(value, raw.data(), sizeof(uint64_t));
    return true;
}

bool StoreSetU64(const std::string& path, const std::string& name, uint64_t value) {
    const ULONGLONG v = value;
    return SetValue(path, name, REG_QWORD, &v, sizeof(v));
}

std::vector<std::string> StoreList(const std::string& path) {
    std::vector<std::string> out;
    HKEY root;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, KeyPath(path).c_str(), 0, KEY_READ, &root) != ERROR_SUCCESS) return out;
    wchar_t sub[256];
    for (DWORD i = 0;; ++i) {
        DWORD len = DWORD(std::size(sub));
        if (RegEnumKeyExW(root, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        out.push_back(Utf8(std::wstring(sub, len)));
    }
    RegCloseKey(root);
    return out;
}

void StoreDelete(const std::string& path) { RegDeleteTreeW(HKEY_CURRENT_USER, KeyPath(path).c_str()); }

bool Protect(const std::vector<uint8_t>& plain, std::vector<uint8_t>* out) {
    DATA_BLOB in = {DWORD(plain.size()), const_cast<BYTE*>(plain.data())}, blob = {};
    if (!CryptProtectData(&in, L"MyCam phone pairing", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &blob)) return false;
    out->assign(blob.pbData, blob.pbData + blob.cbData);
    LocalFree(blob.pbData);
    return true;
}

bool Unprotect(const std::vector<uint8_t>& stored, std::vector<uint8_t>* plain) {
    DATA_BLOB in = {DWORD(stored.size()), const_cast<BYTE*>(stored.data())}, out = {};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    plain->assign(out.pbData, out.pbData + out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}

std::string ResourceData(int id) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    const void* p = g ? LockResource(g) : nullptr;
    return p ? std::string(static_cast<const char*>(p), SizeofResource(nullptr, r)) : std::string();
}

// --- net.h -----------------------------------------------------------------------------------------------

namespace net {

bool Init() {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}
void Cleanup() { WSACleanup(); }
void Close(Socket s) { closesocket(SOCKET(s)); }
void Shutdown(Socket s) { shutdown(SOCKET(s), SD_BOTH); }
void SetNonBlocking(Socket s) {
    u_long nonBlocking = 1;
    ioctlsocket(SOCKET(s), FIONBIO, &nonBlocking);
}
bool WouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
int LastError() { return WSAGetLastError(); }

Socket ListenTcp(uint16_t port, int backlog) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return kInvalid;
    BOOL exclusive = TRUE; // Nobody else may take the port from under us.
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s, backlog) != 0) {
        const int error = WSAGetLastError();
        closesocket(s);
        WSASetLastError(error); // For the caller's log.
        return kInvalid;
    }
    SetNonBlocking(s);
    return s;
}

} // namespace net

} // namespace mycam
