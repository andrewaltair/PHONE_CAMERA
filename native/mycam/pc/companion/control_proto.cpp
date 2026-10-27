#include "control_proto.h"

#include <stdio.h>

namespace mycam {

std::string FormatTrayStatus(const TrayStatus& s) {
    char fields[160];
    snprintf(fields, sizeof(fields), "status icon=%d pause=%d paused=%d linked=%d facing=%d quality=%d q=%d%d%d mirror=%d fill=%d",
             s.icon, s.canPause, s.paused, s.linked, s.facing, s.quality, s.qualityOk[0], s.qualityOk[1],
             s.qualityOk[2], s.mirror, s.fill);
    // Tabs separate the texts; status texts never hold one, but a phone's name could.
    auto clean = [](std::string t) {
        for (char& c : t) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        return t;
    };
    return std::string(fields) + "\t" + clean(s.headline) + "\t" + clean(s.detail);
}

bool ParseTrayStatus(const std::string& line, TrayStatus* s) {
    const size_t tab1 = line.find('\t');
    const size_t tab2 = tab1 == std::string::npos ? tab1 : line.find('\t', tab1 + 1);
    if (tab2 == std::string::npos) return false;
    TrayStatus t;
    int pause, paused, linked, q0, q1, q2, mirror, fill;
    if (sscanf(line.substr(0, tab1).c_str(),
               "status icon=%d pause=%d paused=%d linked=%d facing=%d quality=%d q=%1d%1d%1d mirror=%d fill=%d",
               &t.icon, &pause, &paused, &linked, &t.facing, &t.quality, &q0, &q1, &q2, &mirror, &fill) != 11) {
        return false;
    }
    t.canPause = pause;
    t.paused = paused;
    t.linked = linked;
    t.qualityOk[0] = q0;
    t.qualityOk[1] = q1;
    t.qualityOk[2] = q2;
    t.mirror = mirror;
    t.fill = fill;
    t.headline = line.substr(tab1 + 1, tab2 - tab1 - 1);
    t.detail = line.substr(tab2 + 1);
    *s = t;
    return true;
}

} // namespace mycam
