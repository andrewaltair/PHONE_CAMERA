#pragma once
// Linux: talking to the running daemon. It listens on $XDG_RUNTIME_DIR/mycam.sock (this user only), so
// `mycam-companion --pair` in any terminal can show the iPhone QR code while the service runs: the Windows
// settings window's "Connect an iPhone", for a daemon with no window.
//
// Lines over the socket; the client's first line picks what the connection is for:
// - "pair": the daemon sends "url <pairing address>" whenever it changes (empty: none yet) and "paired
//   <name>" when a device pairs. Closing the connection stops pairing. The address holds the one-time code,
//   so it only ever goes over this socket, never to the log.
// - "watch" (mycam-tray): the daemon sends a status line (control_proto.h) now and on every change, and
//   takes commands on the same connection until it closes.
// - A command on its own: "pause 0|1", "facing 0|1", "quality 0|1|2", "mirror 0|1", "fill 0|1", "reconnect".

#include <stdint.h>

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "link_status.h"

namespace mycam {

class PhoneLink;

class ControlServer {
public:
    // mirror/fill: the saved picture options (the store's "Mirror"/"Fill"), already applied to the link.
    ControlServer(PhoneLink* link, bool mirror, bool fill) : link_(link), mirror_(mirror), fill_(fill) {}
    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;
    ~ControlServer() { Stop(); }

    bool Start();  // Logs why not (another daemon already owns the socket, no runtime dir...).
    void Stop();
    void OnStatus(const LinkStatus& status); // From PhoneLink's status callback.

private:
    void Loop();
    void Serve(int client);
    void Pair(int client);
    void Watch(int client, std::string buffer);
    void Command(const std::string& line);
    void UpdateStatusLine(); // With lock_ held.

    PhoneLink* link_;
    int listen_ = -1;
    std::string path_;
    std::thread thread_;
    std::mutex lock_;
    std::condition_variable changed_;
    // Guarded by lock_:
    bool stop_ = false;
    int clients_ = 0;             // Connections being served, each on its own thread; Stop waits for them.
    std::string url_;
    uint32_t pairedCount_ = 0;
    std::string pairedName_;
    LinkStatus status_;
    bool mirror_, fill_;
    std::string statusLine_;
    uint64_t statusSeq_ = 0;      // Bumped whenever statusLine_ changes.
};

// `mycam-companion --pair` without --daemon: asks the running daemon to pair and prints its QR codes until a
// device pairs or Ctrl+C. Returns the process exit code.
int RunPairClient(void (*printQr)(const std::string& url));

} // namespace mycam
