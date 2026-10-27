#include <stddef.h>

#include <algorithm>
#include <string_view>

#include "../companion/linux_status_frame.h"
#include "../companion/marquee.h"

int main() {
    using namespace mycam;
    const std::string_view texts[] = {kWaitingBrandText,      kWaitingHeadlineText, kWaitingConnectText,
                                      kWaitingAndroidText,     kWaitingIphoneText,    kWaitingProgressText};
    for (const std::string_view text : texts)
        for (const char c : text)
            if (c != ' ' && !LinuxWaitingGlyphAvailable(c)) return 1;

    const Nv12Image base = BuildPhoneCameraWaitingFrame();
    if (base.width != kLinuxWaitingFrameWidth || base.height != kLinuxWaitingFrameHeight) return 1;
    if (base.data.size() != size_t(base.width) * base.height * 3 / 2) return 1;

    const MarqueeGeometry g = MarqueeGeometryFor(base.width, base.height);
    if (!g.valid() || g.x != 490 || g.y != 447 || g.w != 300 || g.h != 18) return 1;
    const uint8_t* y = base.data.data();
    if (y[size_t(450) * base.width + 600] <= 200) return 1; // white track interior
    if (y[size_t(430) * base.width + 600] >= 180) return 1; // dark panel above the track

    Nv12Image paused = base;
    DrawMarquee(paused, base, kMarqueePassSec + 0.1); // pause phase keeps the neutral track unchanged
    if (paused.data != base.data) return 1;
    Nv12Image moving = base;
    DrawMarquee(moving, base, 1.0); // Mid-pass: all three chunks have entered the track.
    if (moving.data == base.data) return 1;
    return 0;
}
