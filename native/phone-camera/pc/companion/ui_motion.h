#pragma once
// Motion helpers for the companion UI (docs/redesign.md §6.1, §6.3): easing curves, a time-based tween and the
// XP green progress bar's chunk maths. Pure C++, unit-tested in pc/tests.

#include <stdint.h>

#include <algorithm>
#include <cmath>

namespace mycam::ui {

// One timing system (docs/redesign-v2.md §12): short, standard and page, all on the same ease-out curve, and
// nothing over 300 ms. Reduced motion turns every duration into 0 (see SettingsWindow::Impl::Dur).
constexpr float kShortMs = 120;     // Dropdown open, pressed-state release.
constexpr float kToggleMs = 150;    // Toggle thumb and track.
constexpr float kStandardMs = 200;  // Status crossfade, nav pill, preview crossfade.
constexpr float kPageMs = 250;      // Page transition, badge pop.
constexpr float kPageSlideDip = 8;  // How far a page slides while it fades.

inline float Clamp01(float t) { return t < 0 ? 0.f : t > 1 ? 1.f : t; }
inline float EaseOut(float t) { t = Clamp01(t); return 1 - (1 - t) * (1 - t) * (1 - t); }
inline float EaseInOut(float t) {
    t = Clamp01(t);
    return t < 0.5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3.f) / 2;
}
// Ease-out with a slight overshoot (the badge "pop"): rises past 1 near the end and settles at exactly 1.
inline float BackOut(float t) {
    t = Clamp01(t);
    constexpr float c1 = 1.70158f, c3 = c1 + 1;
    return 1 + c3 * std::pow(t - 1, 3.f) + c1 * std::pow(t - 1, 2.f);
}
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

// A value animating from `from` to `to` over `durationMs`, started at `startMs`. With duration 0 (reduced
// motion) it jumps straight to `to`.
struct Tween {
    float from = 0, to = 0;
    uint64_t startMs = 0;
    float durationMs = 0;

    void Start(float target, uint64_t now, float duration, float current) {
        from = current;
        to = target;
        startMs = now;
        durationMs = duration;
    }
    void Set(float value) { from = to = value; durationMs = 0; }
    // Interruptible (§12): retargets from where the value is now instead of snapping. Does nothing when
    // the tween is already heading there, so a repaint never restarts it.
    void Retarget(float target, uint64_t now, float duration, float (*ease)(float) = EaseInOut) {
        if (to == target) return;
        Start(target, now, duration, Value(now, ease));
    }
    float Progress(uint64_t now) const {
        return durationMs <= 0 ? 1.f : Clamp01(float(now - std::min(now, startMs)) / durationMs);
    }
    float Value(uint64_t now, float (*ease)(float) = EaseInOut) const { return Lerp(from, to, ease(Progress(now))); }
    bool Running(uint64_t now) const { return Progress(now) < 1.f; }
};

// --- XP progress bar (§6.1) --------------------------------------------------------------------
// Chunks are `chunk` wide with `gap` between them, inside a track of `trackWidth` (both DIPs).

constexpr int kMarqueeChunks = 3;
constexpr float kMarqueePassMs = 2000;  // One pass of the three chunks across the track.
constexpr float kMarqueePauseMs = 400;  // Then a pause before the next pass.

// Left edge of the marquee's 3-chunk block at time `ms`, relative to the track's left edge. The block
// enters from the left (negative), leaves on the right, then nothing is shown during the pause (returns
// a value >= trackWidth). Positions snap to whole chunk steps, as XP's did.
inline float MarqueeOffset(uint64_t ms, float trackWidth, float chunk, float gap) {
    const float step = chunk + gap;
    const float block = kMarqueeChunks * step;
    const float cycle = kMarqueePassMs + kMarqueePauseMs;
    const float t = std::fmod(float(ms), cycle);
    if (t >= kMarqueePassMs) return trackWidth; // Pause: off the track.
    const float travel = trackWidth + block;     // From fully left of the track to fully right of it.
    const float x = -block + travel * (t / kMarqueePassMs);
    return std::floor(x / step) * step;
}

// How many whole chunks a determinate bar shows for `fraction` (0..1): XP fills in whole steps.
inline int DeterminateChunks(float fraction, float trackWidth, float chunk, float gap) {
    const int total = std::max(1, int((trackWidth + gap) / (chunk + gap)));
    return std::clamp(int(std::floor(Clamp01(fraction) * total + 0.0001f)), 0, total);
}

// "0:42" for a countdown in milliseconds (rounded up, so it reads 0:01 until the very end).
inline void FormatCountdown(uint64_t remainingMs, wchar_t* out, size_t outCount) {
    const unsigned secs = unsigned((remainingMs + 999) / 1000);
    if (outCount < 6) { if (outCount) out[0] = 0; return; }
    const unsigned m = std::min(secs / 60, 9u), s = secs % 60;
    out[0] = wchar_t(L'0' + m);
    out[1] = L':';
    out[2] = wchar_t(L'0' + s / 10);
    out[3] = wchar_t(L'0' + s % 10);
    out[4] = 0;
}

} // namespace mycam::ui
