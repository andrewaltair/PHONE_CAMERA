#include "control_posix.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "capabilities.h"
#include "control_proto.h"
#include "log.h"
#include "phone_link.h"
#include "platform.h"
#include "protocol.h"
#include "status_text.h"

namespace mycam {

namespace {

std::string SocketPath() {
    const char* dir = getenv("XDG_RUNTIME_DIR"); // Per user, 0700, made by systemd at login.
    return dir && *dir ? std::string(dir) + "/mycam.sock" : std::string();
}

int Connect(const std::string& path) {
    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof(addr.sun_path)) return -1;
    memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    const int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s >= 0 && connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(s);
        return -1;
    }
    return s;
}

bool SendLine(int s, const std::string& line) {
    const std::string text = line + "\n";
    return send(s, text.data(), text.size(), MSG_NOSIGNAL) == ssize_t(text.size());
}

// One line (without the newline) into *line; false when the connection ended. *buffer keeps what follows.
bool ReadLine(int s, std::string* buffer, std::string* line, int timeoutMs) {
    for (;;) {
        if (const size_t nl = buffer->find('\n'); nl != std::string::npos) {
            *line = buffer->substr(0, nl);
            buffer->erase(0, nl + 1);
            return true;
        }
        pollfd p = {s, POLLIN, 0};
        if (poll(&p, 1, timeoutMs) <= 0) return false;
        char chunk[512];
        const ssize_t n = recv(s, chunk, sizeof(chunk), 0);
        if (n <= 0 || buffer->size() > 4096) return false;
        buffer->append(chunk, size_t(n));
    }
}

TrayStatus MakeTrayStatus(const LinkStatus& status, bool mirror, bool fill) {
    TrayStatus t;
    // No camera registration to fail on Linux: the v4l2loopback device is opened by the daemon itself.
    const StatusView view = DescribeStatus(status, true);
    t.icon = view.icon;
    t.headline = Utf8(view.headline);
    t.detail = Utf8(view.detail);
    // The Windows tray menu's rules (main.cpp ShowMenu).
    const bool connected = status.state >= LinkState::Idle;
    t.canPause = connected && !status.lockPaused && !status.webAway;
    t.paused = status.state == LinkState::Paused;
    t.linked = connected && status.camera.valid;
    t.facing = status.facing;
    t.quality = t.linked && status.camera.quality <= proto::kQuality4K ? status.camera.quality : -1;
    for (int q = 0; q < 3; ++q) t.qualityOk[q] = caps::QualityEnabled(t.linked, status.camera, uint8_t(q));
    t.mirror = mirror;
    t.fill = fill;
    return t;
}

} // namespace

bool ControlServer::Start() {
    path_ = SocketPath();
    if (path_.empty()) {
        Log("control: no XDG_RUNTIME_DIR, so mycam-companion --pair can't reach this daemon");
        return false;
    }
    if (const int other = Connect(path_); other >= 0) {
        close(other);
        Log("control: another mycam-companion already answers on %s", path_.c_str());
        path_.clear();
        return false;
    }
    unlink(path_.c_str()); // Left over from a daemon that didn't exit cleanly.
    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    if (path_.size() >= sizeof(addr.sun_path)) return false;
    memcpy(addr.sun_path, path_.c_str(), path_.size() + 1);
    listen_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_ < 0 || bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listen_, 2) != 0) {
        Log("control: can't listen on %s: %s", path_.c_str(), strerror(errno));
        if (listen_ >= 0) close(listen_);
        listen_ = -1;
        path_.clear();
        return false;
    }
    chmod(path_.c_str(), 0600);
    {
        std::lock_guard<std::mutex> lock(lock_);
        UpdateStatusLine(); // A tray that connects before the first status change gets one too.
    }
    thread_ = std::thread([this] { Loop(); });
    return true;
}

void ControlServer::Stop() {
    std::unique_lock<std::mutex> lock(lock_);
    stop_ = true;
    changed_.notify_all();
    lock.unlock();
    if (thread_.joinable()) thread_.join();
    lock.lock();
    changed_.wait(lock, [this] { return clients_ == 0; }); // Each notices stop_ within 500 ms.
    lock.unlock();
    if (listen_ >= 0) close(listen_);
    listen_ = -1;
    if (!path_.empty()) unlink(path_.c_str());
    path_.clear();
}

void ControlServer::OnStatus(const LinkStatus& status) {
    {
        std::lock_guard<std::mutex> lock(lock_);
        url_ = Utf8(status.webUrl);
        if (status.pairedCount != pairedCount_) pairedName_ = Utf8(status.pairedName);
        pairedCount_ = status.pairedCount;
        status_ = status;
        UpdateStatusLine();
    }
    changed_.notify_all();
}

void ControlServer::UpdateStatusLine() {
    std::string line = FormatTrayStatus(MakeTrayStatus(status_, mirror_, fill_));
    if (line != statusLine_) {
        statusLine_ = std::move(line);
        ++statusSeq_;
    }
}

void ControlServer::Loop() {
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(lock_);
            if (stop_) return;
        }
        pollfd p = {listen_, POLLIN, 0};
        if (poll(&p, 1, 500) <= 0) continue;
        const int client = accept4(listen_, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) continue;
        {
            std::lock_guard<std::mutex> lock(lock_);
            if (clients_ >= 8) { close(client); continue; } // Only this user can connect; a cap all the same.
            ++clients_;
        }
        // Its own thread: the tray stays connected while a terminal pairs.
        std::thread([this, client] {
            Serve(client);
            close(client);
            std::lock_guard<std::mutex> lock(lock_);
            --clients_;
            changed_.notify_all();
        }).detach();
    }
}

void ControlServer::Serve(int client) {
    std::string buffer, first;
    if (!ReadLine(client, &buffer, &first, 2000)) return;
    if (first == "pair") Pair(client);
    else if (first == "watch") Watch(client, buffer);
    else Command(first);
}

void ControlServer::Pair(int client) {
    Log("control: pairing asked for from a terminal");
    link_->StartWebPairing(0);
    std::unique_lock<std::mutex> lock(lock_);
    const uint32_t pairedBefore = pairedCount_;
    std::string sent = "\n"; // Not a possible address: the first one always goes out.
    uint32_t pairedSeen = pairedBefore;
    while (!stop_) {
        if (url_ != sent) {
            sent = url_;
            if (!SendLine(client, "url " + sent)) break;
        }
        if (pairedCount_ != pairedSeen) {
            pairedSeen = pairedCount_;
            if (!SendLine(client, "paired " + pairedName_)) break;
        }
        changed_.wait_for(lock, std::chrono::milliseconds(500));
        pollfd p = {client, POLLIN, 0};
        if (poll(&p, 1, 0) > 0) break; // The client hung up (it never sends more).
    }
    lock.unlock();
    link_->StopWebPairing();
    Log("control: pairing from the terminal ended");
}

void ControlServer::Watch(int client, std::string buffer) {
    uint64_t sent = 0;
    for (;;) {
        std::string line;
        {
            std::unique_lock<std::mutex> lock(lock_);
            if (stop_) return;
            if (statusSeq_ == sent) changed_.wait_for(lock, std::chrono::milliseconds(500));
            if (statusSeq_ != sent) {
                sent = statusSeq_;
                line = statusLine_;
            }
        }
        if (!line.empty() && !SendLine(client, line)) return;
        pollfd p = {client, POLLIN, 0};
        if (poll(&p, 1, 0) <= 0) continue;
        char chunk[512];
        const ssize_t n = recv(client, chunk, sizeof(chunk), 0);
        if (n <= 0 || buffer.size() > 4096) return; // Closed (the tray quit), or not a tray.
        buffer.append(chunk, size_t(n));
        for (size_t nl; (nl = buffer.find('\n')) != std::string::npos; buffer.erase(0, nl + 1)) {
            Command(buffer.substr(0, nl));
        }
    }
}

void ControlServer::Command(const std::string& line) {
    char name[16] = {};
    int arg = -1;
    if (sscanf(line.c_str(), "%15s %d", name, &arg) < 1) return;
    const std::string cmd = name;
    if (cmd == "pause" && (arg == 0 || arg == 1)) link_->RequestPause(arg);
    else if (cmd == "facing" && (arg == proto::kFacingBack || arg == proto::kFacingFront)) link_->RequestFacing(arg);
    else if (cmd == "quality" && arg >= proto::kQuality720p && arg <= proto::kQuality4K) {
        link_->RequestCommand(proto::kCmdSetQuality, uint8_t(arg));
    } else if (cmd == "reconnect") link_->RequestReconnect();
    else if ((cmd == "mirror" || cmd == "fill") && (arg == 0 || arg == 1)) {
        // Saved like the Windows tray saves them, and applied at the next frame.
        if (cmd == "mirror") link_->SetMirror(arg);
        else link_->SetFill(arg);
        StoreSetU64("", cmd == "mirror" ? "Mirror" : "Fill", uint64_t(arg));
        {
            std::lock_guard<std::mutex> lock(lock_);
            (cmd == "mirror" ? mirror_ : fill_) = arg;
            UpdateStatusLine();
        }
        changed_.notify_all();
    } else {
        Log("control: unknown command"); // Not the line itself: it came from outside the daemon.
    }
}

int RunPairClient(void (*printQr)(const std::string& url)) {
    const int s = Connect(SocketPath());
    if (s < 0) {
        fprintf(stderr, "MyCam isn't running. Start it with: systemctl --user start mycam\n"
                        "(or run mycam-companion --daemon --pair)\n");
        return 1;
    }
    if (!SendLine(s, "pair")) { close(s); return 1; }
    std::string buffer, line;
    while (ReadLine(s, &buffer, &line, -1)) {
        if (line.rfind("url ", 0) == 0) {
            const std::string url = line.substr(4);
            if (url.empty()) printf("Waiting for a network address a phone could reach...\n");
            else printQr(url);
            fflush(stdout);
        } else if (line.rfind("paired ", 0) == 0) {
            printf("Paired with %s. It can connect any time now; this window can close.\n", line.substr(7).c_str());
            close(s);
            return 0;
        } else if (line.rfind("error ", 0) == 0) {
            fprintf(stderr, "MyCam: %s\n", line.substr(6).c_str());
            break;
        }
    }
    close(s);
    fprintf(stderr, "MyCam stopped answering.\n");
    return 1;
}

} // namespace mycam
