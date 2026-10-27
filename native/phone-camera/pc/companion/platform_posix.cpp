#include "platform.h"
#include "net.h"

// platform.h and net.h on Linux. The settings store is a directory tree under $XDG_CONFIG_HOME/mycam/store
// (~/.config/mycam/store): one directory per key, one file per value, readable by this user only.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <fstream>
#include <iterator>

namespace mycam {

namespace {

std::string StoreRoot() {
    std::string base;
    if (const char* x = getenv("XDG_CONFIG_HOME"); x && *x) base = x;
    else if (const char* h = getenv("HOME"); h && *h) base = std::string(h) + "/.config";
    else return "";
    return base + "/mycam/store";
}

// Keys and value names become file names: anything but [A-Za-z0-9._-] is refused, so a name can't climb out
// of the store ("..", "/").
bool SafePart(const std::string& s) {
    if (s.empty() || s == "." || s == "..") return false;
    for (char c : s)
        if (!isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_' && c != '-') return false;
    return true;
}

bool SafePath(const std::string& path) {
    if (path.empty()) return true;
    size_t start = 0;
    while (true) {
        const size_t slash = path.find('/', start);
        if (!SafePart(path.substr(start, slash - start))) return false;
        if (slash == std::string::npos) return true;
        start = slash + 1;
    }
}

std::string Dir(const std::string& path) {
    const std::string root = StoreRoot();
    if (root.empty() || !SafePath(path)) return "";
    return path.empty() ? root : root + "/" + path;
}

bool MakeDirs(const std::string& dir) {
    for (size_t i = 1; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == '/') {
            const std::string part = dir.substr(0, i);
            if (mkdir(part.c_str(), 0700) != 0 && errno != EEXIST) return false;
        }
    }
    return true;
}

bool ReadFile(const std::string& path, const std::string& name, std::string* out) {
    const std::string dir = Dir(path);
    if (dir.empty() || !SafePart(name)) return false;
    std::ifstream in(dir + "/" + name, std::ios::binary);
    if (!in) return false;
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// Written to a temporary file, then renamed over the old one, so a crash never leaves half a value.
bool WriteFile(const std::string& path, const std::string& name, const void* data, size_t size) {
    const std::string dir = Dir(path);
    if (dir.empty() || !SafePart(name) || !MakeDirs(dir)) return false;
    const std::string file = dir + "/" + name, temp = file + ".tmp";
    const int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    const bool ok = write(fd, data, size) == ssize_t(size) && fsync(fd) == 0;
    close(fd);
    return ok && rename(temp.c_str(), file.c_str()) == 0;
}

void RemoveTree(const std::string& dir) {
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string name = e->d_name;
            if (name == "." || name == "..") continue;
            const std::string full = dir + "/" + name;
            struct stat st;
            if (lstat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) RemoveTree(full);
            else unlink(full.c_str());
        }
        closedir(d);
    }
    rmdir(dir.c_str());
}

std::string ReadSys(const std::string& path) {
    std::ifstream in(path);
    std::string s;
    std::getline(in, s);
    return s;
}

} // namespace

uint64_t NowMs() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return uint64_t(t.tv_sec) * 1000 + uint64_t(t.tv_nsec) / 1000000;
}

void SleepMs(uint32_t ms) {
    timespec t = {time_t(ms / 1000), long(ms % 1000) * 1000000};
    while (nanosleep(&t, &t) != 0 && errno == EINTR) {}
}

uint64_t UnixTime() { return uint64_t(time(nullptr)); }

// wchar_t is UTF-32 here. Invalid UTF-8 becomes U+FFFD.
std::wstring Wide(const std::string& utf8) {
    std::wstring out;
    for (size_t i = 0; i < utf8.size();) {
        const unsigned char c = utf8[i];
        const int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        uint32_t cp = len == 1 ? c : len == 2 ? c & 0x1F : len == 3 ? c & 0x0F : c & 0x07;
        bool ok = len > 0 && i + len <= utf8.size();
        for (int k = 1; ok && k < len; ++k) {
            const unsigned char cc = utf8[i + k];
            ok = (cc >> 6) == 2;
            cp = cp << 6 | (cc & 0x3F);
        }
        if (!ok || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { out += L'\xFFFD'; ++i; continue; }
        out += wchar_t(cp);
        i += size_t(len);
    }
    return out;
}

std::string Utf8(const std::wstring& wide) {
    std::string out;
    for (wchar_t wc : wide) {
        uint32_t cp = uint32_t(wc);
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) { out += char(0xC0 | cp >> 6); out += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += char(0xE0 | cp >> 12); out += char(0x80 | (cp >> 6 & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
        else { out += char(0xF0 | cp >> 18); out += char(0x80 | (cp >> 12 & 0x3F)); out += char(0x80 | (cp >> 6 & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    }
    return out;
}

std::string ComputerName() {
    char name[256] = {};
    if (gethostname(name, sizeof(name) - 1) != 0 || !name[0]) return "PC";
    std::string s = name;
    if (const size_t dot = s.find('.'); dot != std::string::npos) s.resize(dot); // The host, not the domain.
    if (s.size() > 63) s.resize(63);
    return s;
}

// Linux has no IANA interface types or adapter descriptions, so they are made up from sysfs in the terms
// web::ClassifyAdapter knows: a wireless directory means Wi-Fi; no backing device means virtual (bridges,
// Docker, VPN tunnels); the iPhone's USB hotspot driver (ipheth) reads as "Apple Mobile Device Ethernet".
std::vector<Adapter> UpAdapters() {
    std::vector<Adapter> out;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || !(a->ifa_flags & IFF_UP)) continue;
        Adapter ad;
        ad.ipv4 = reinterpret_cast<const sockaddr_in*>(a->ifa_addr)->sin_addr.s_addr;
        if (a->ifa_netmask) {
            const uint32_t mask = ntohl(reinterpret_cast<const sockaddr_in*>(a->ifa_netmask)->sin_addr.s_addr);
            ad.prefix = uint8_t(__builtin_popcount(mask));
        }
        const std::string sys = std::string("/sys/class/net/") + a->ifa_name;
        struct stat st;
        char link[256] = {};
        const ssize_t n = readlink((sys + "/device/driver").c_str(), link, sizeof(link) - 1);
        std::string driver = n > 0 ? std::string(link, size_t(n)) : "";
        if (const size_t slash = driver.rfind('/'); slash != std::string::npos) driver = driver.substr(slash + 1);
        if (a->ifa_flags & IFF_LOOPBACK) ad.ifType = 24;
        else if (stat((sys + "/wireless").c_str(), &st) == 0) ad.ifType = 71;
        else if (ReadSys(sys + "/type") == "65534") ad.ifType = 131; // tun
        else if (stat((sys + "/device").c_str(), &st) != 0) ad.ifType = 53; // No hardware behind it.
        else ad.ifType = 6;
        ad.name = Wide(a->ifa_name);
        ad.description = Wide(driver == "ipheth" ? "Apple Mobile Device Ethernet (ipheth)"
                                                 : std::string(a->ifa_name) + (driver.empty() ? "" : " (" + driver + ")"));
        out.push_back(std::move(ad));
    }
    freeifaddrs(list);
    return out;
}

bool StoreGetBytes(const std::string& path, const std::string& name, std::vector<uint8_t>* value) {
    std::string raw;
    if (!ReadFile(path, name, &raw)) return false;
    value->assign(raw.begin(), raw.end());
    return true;
}

bool StoreSetBytes(const std::string& path, const std::string& name, const std::vector<uint8_t>& value) {
    return WriteFile(path, name, value.data(), value.size());
}

bool StoreGetString(const std::string& path, const std::string& name, std::string* value) {
    return ReadFile(path, name, value);
}

bool StoreSetString(const std::string& path, const std::string& name, const std::string& value) {
    return WriteFile(path, name, value.data(), value.size());
}

bool StoreGetU64(const std::string& path, const std::string& name, uint64_t* value) {
    std::string raw;
    if (!ReadFile(path, name, &raw) || raw.empty()) return false;
    char* end = nullptr;
    const unsigned long long v = strtoull(raw.c_str(), &end, 10);
    if (*end != '\0' && *end != '\n') return false;
    *value = v;
    return true;
}

bool StoreSetU64(const std::string& path, const std::string& name, uint64_t value) {
    const std::string text = std::to_string(value) + "\n";
    return WriteFile(path, name, text.data(), text.size());
}

std::vector<std::string> StoreList(const std::string& path) {
    std::vector<std::string> out;
    const std::string dir = Dir(path);
    if (dir.empty()) return out;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string name = e->d_name;
            struct stat st;
            if (SafePart(name) && stat((dir + "/" + name).c_str(), &st) == 0 && S_ISDIR(st.st_mode)) out.push_back(name);
        }
        closedir(d);
    }
    return out;
}

void StoreDelete(const std::string& path) {
    const std::string dir = Dir(path);
    if (!dir.empty() && !path.empty()) RemoveTree(dir);
}

// ponytail: stored as-is, protected only by the store's 0600/0700 permissions (like ~/.ssh keys). Use the
// Secret Service (libsecret) if a desktop keyring is wanted.
bool Protect(const std::vector<uint8_t>& plain, std::vector<uint8_t>* out) { *out = plain; return true; }
bool Unprotect(const std::vector<uint8_t>& stored, std::vector<uint8_t>* plain) { *plain = stored; return true; }

// --- net.h -----------------------------------------------------------------------------------------------

namespace net {

bool Init() { return true; }
void Cleanup() {}
void Close(Socket s) { close(int(s)); }
void Shutdown(Socket s) { shutdown(int(s), SHUT_RDWR); }
void SetNonBlocking(Socket s) { fcntl(int(s), F_SETFL, fcntl(int(s), F_GETFL) | O_NONBLOCK); }
bool WouldBlock() { return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS; }
int LastError() { return errno; }

Socket ListenTcp(uint16_t port, int backlog) {
    const int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    if (s < 0) return kInvalid;
    int on = 1; // Restarting the daemon must not wait out TIME_WAIT.
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s, backlog) != 0) {
        const int error = errno;
        close(s);
        errno = error; // For the caller's log.
        return kInvalid;
    }
    SetNonBlocking(Socket(s));
    return Socket(s);
}

} // namespace net

} // namespace mycam
