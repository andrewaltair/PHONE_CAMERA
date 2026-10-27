#pragma once
// Linux: the status the daemon sends the tray over the control socket (control_posix.h), as one line:
//   status icon=103 pause=1 paused=0 linked=1 facing=0 quality=1 q=110 mirror=0 fill=0<TAB>headline<TAB>detail
// The daemon works out everything the tray menu shows (control_posix.cpp, the same rules as the Windows tray
// menu in main.cpp), so the tray only draws it. Portable, unit-tested in pc/tests.

#include <string>

namespace mycam {

struct TrayStatus {
    int icon = 101;           // TrayIconId (status_text.h).
    bool canPause = false;    // Pause/Resume is offered.
    bool paused = false;
    bool linked = false;      // A phone is connected and has reported its camera (Quality is offered).
    int facing = 0;           // proto::kFacingBack / kFacingFront.
    int quality = -1;         // proto::kQuality720p..4K, -1: none checked.
    bool qualityOk[3] = {};   // Which of 720p, 1080p, 4K this phone offers.
    bool mirror = false;
    bool fill = false;
    std::string headline;     // UTF-8, as the Windows tray shows it.
    std::string detail;
};

std::string FormatTrayStatus(const TrayStatus& s); // Without the newline.
bool ParseTrayStatus(const std::string& line, TrayStatus* s);

} // namespace mycam
