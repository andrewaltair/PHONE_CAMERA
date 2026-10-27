// The iPhone web camera as a third transport (docs/AIYAN-IPHONE.md, docs/web-camera-format.md): a page in
// Safari sends MyCam packets over a secure WebSocket, one per message. Like RunTcpSession, a web session is
// only a pump around the shared SessionLoop: WebServer::Poll() feeds the page's messages to FeedBytes, and
// SendCommand() writes commands back. Everything runs on the worker thread.

#include "log.h"
#include "phone_link.h"
#include "platform.h"

namespace mycam {

namespace {
// How long a paused page may be gone (the iPhone locked, Safari in the background) and still pick its session
// up again. Meanwhile apps keep seeing the "Camera paused" picture.
constexpr uint64_t kWebLingerMs = 5 * 60 * 1000;
} // namespace

void PhoneLink::PollWeb(uint32_t waitMs) {
    if (!web_) {
        if (waitMs) SleepMs(waitMs);
        return;
    }
    const int64_t pair = webPairRequest_.exchange(-1);
    if (pair == -2) web_->StopPairing();
    else if (pair >= 0) web_->StartPairing(uint32_t(pair));
    std::vector<std::string> forget;
    {
        std::lock_guard<std::mutex> lock(commandLock_);
        forget.swap(webForget_);
    }
    for (const std::string& id : forget) {
        web_->Forget(id);
        if (webSession_ && id == webDeviceId_) { // Its session ends too, even while it waits to come back.
            Log("web: the connected device was forgotten; ending its session");
            sessionError_ = true;
        }
    }
    web_->Poll(waitMs);
    if (web_->Generation() != webGeneration_) Publish();
}

void PhoneLink::WebScanOnce() {
    if (!web_ || !web_->PhonePending()) return;
    std::string id, name, link;
    if (!web_->TakePhone(&id, &name, &link)) return;
    webSession_ = true;
    webDeviceId_ = id;
    BeginSession(true, name);
    web_->SetBusy(true, id); // Other pages wait; this iPhone may reconnect (see kWebLingerMs).
    status_.web = true;
    status_.webLink.assign(link.begin(), link.end()); // ASCII.
    Publish();
    Log("session: %s connected over %s (web page)", name.c_str(), link.c_str());
    if (!web_->TakeNewlyPaired(id).empty()) NotePaired(name, false); // It scanned the QR code just now.

    uint64_t lastUsbCheck = NowMs(), lingerSince = 0;
    webLingering_ = false;
    SessionLoop([&] {
        PollWeb(StatusWaitMs());
        const uint64_t now = NowMs();
        if (!web_->PhoneConnected()) {
            std::string id2, name2, link2;
            // While this session runs, the server only lets its own device in: a page waiting now is this
            // iPhone again (Safari reloaded the page, or it came back from the background).
            if (web_->PhonePending() && web_->TakePhone(&id2, &name2, &link2)) {
                Log("web: %s is back (over %s)", name2.c_str(), link2.c_str());
                webLingering_ = false;
                status_.webAway = false;
                parser_.Reset();
                gotHello_ = startSent_ = phoneStreaming_ = false;
                waitKeyFrame_ = true;
                status_.webLink.assign(link2.begin(), link2.end());
                Publish();
            } else if (!webLingering_) {
                if (!phonePaused_) {
                    Log("web: the page went away");
                    sessionError_ = true;
                    return;
                }
                // iOS drops the connection of a page in the background: it said "paused" first, so keep the
                // session (and the paused picture) for a while in case it comes back.
                Log("web: the paused page went away (iPhone locked, or Safari in the background); waiting for it");
                webLingering_ = true;
                lingerSince = now;
                status_.webAway = true;
                Publish();
            } else if (now - lingerSince > kWebLingerMs) {
                Log("web: the page didn't come back");
                sessionError_ = true;
                return;
            }
        }
        if (now - lastUsbCheck > 2000) {
            lastUsbCheck = now;
            if (UsbPhoneArrived()) { // The cable wins, as with Android over Wi-Fi.
                Log("web: a phone was plugged in; switching to the cable");
                sessionError_ = true;
            }
        }
    });
    LeaveSession();
    web_->EndPhone();
    webSession_ = webLingering_ = false;
    webDeviceId_.clear();
    ResetAfterSession();
}

} // namespace mycam
