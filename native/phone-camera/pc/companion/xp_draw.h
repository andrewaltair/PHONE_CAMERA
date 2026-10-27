#pragma once
// Windows XP "Luna Blue" drawing kit for the companion's custom-drawn windows (docs/redesign.md §3, §6).
//
// Surface: an ID2D1DeviceContext on a DXGI flip-model swap chain shown through DirectComposition, so the
// window can have transparent rounded top corners (the XP silhouette) and use Direct2D effects (Gaussian
// blur for the frosted preview chips). Hardware D3D11 device, WARP if that fails; device loss rebuilds
// everything (Generation() changes, so callers drop cached bitmaps).
//
// Painter: the XP widgets (title bar, caption buttons, push buttons, checkboxes, radios, task-pane groups,
// the green progress bar, pills, badges, focus rectangles) and DirectWrite text with measuring. In high
// contrast it draws flat system colours with no gloss, blur or animation.

#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <initializer_list>
#include <map>
#include <string>
#include <tuple>

#include "ui_layout.h"

namespace mycam::xp {

using Microsoft::WRL::ComPtr;
using ui::Box;
using ui::Size;

// The clock every animation in the companion's windows runs on. GetTickCount64 only moves in steps of the
// system timer tick (~15.6 ms), which is coarser than a frame: consecutive frames read the same value, so
// a 200 ms tween plays as a handful of jumps instead of a slide. The performance counter is smooth.
inline uint64_t NowMs() {
    static const LONGLONG freq = [] {
        LARGE_INTEGER f = {};
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    if (freq <= 0) return GetTickCount64();
    LARGE_INTEGER c = {};
    QueryPerformanceCounter(&c);
    // Seconds and remainder separately: counter * 1000 could overflow on a very long uptime.
    return uint64_t(c.QuadPart / freq) * 1000 + uint64_t((c.QuadPart % freq) * 1000 / freq);
}

// --- Surface ----------------------------------------------------------------------------------------

class Surface {
public:
    // The window must have WS_EX_NOREDIRECTIONBITMAP.
    void Attach(HWND hwnd) { hwnd_ = hwnd; }
    // Starts a frame at the window's current size and DPI. Returns nullptr if no device could be made.
    ID2D1DeviceContext* Begin(float dpiScale);
    // Ends the frame and presents it. Handles device loss (the next Begin rebuilds): returns false when the
    // device was lost, so the caller repaints (DirectComposition shows nothing until it does).
    bool End();
    void Reset(); // Drop all device resources (window destroyed, or device lost).

    ID2D1Factory1* Factory();
    ID2D1DeviceContext* Context() const { return dc_.Get(); }
    int Generation() const { return generation_; }
    // The last Present found the window fully covered, so it returned at once instead of waiting for the
    // vertical blank. An animation must then be paced by a timer, or it would spin.
    bool Occluded() const { return occluded_; }

private:
    bool CreateDevice();
    bool EnsureSize(UINT w, UINT h, float dpiScale);

    HWND hwnd_ = nullptr;
    ComPtr<ID2D1Factory1> factory_;
    ComPtr<ID3D11Device> d3d_;
    ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> dc_;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<ID2D1Bitmap1> target_;
    ComPtr<IDCompositionDevice> dcomp_;
    ComPtr<IDCompositionTarget> dcompTarget_;
    ComPtr<IDCompositionVisual> visual_;
    UINT width_ = 0, height_ = 0;
    float dpiScale_ = 1;
    int generation_ = 0;
    bool occluded_ = false;
};

// --- Text ---------------------------------------------------------------------------------------------

// Body text is Segoe UI (docs/redesign-v2.md §4); only the window caption keeps Trebuchet MS Bold, so the XP
// chrome still reads as XP while the interior is a clean modern app.
enum class Font {
    Body,        // Segoe UI 9 pt
    BodyBold,    // Segoe UI Semibold 9 pt (card titles, buttons)
    Caption,     // Trebuchet MS Bold 10 pt (window title: XP chrome, unchanged)
    Instruction, // Segoe UI Semibold 13 pt (page title, status headline)
    Code,        // Segoe UI Semibold 24 pt (pairing code)
    Small,       // Segoe UI 8 pt (subtitles, addresses, capability reasons)
    Count
};

enum class Align { Leading, Center, Trailing };

// --- Colours ------------------------------------------------------------------------------------------

struct Palette {
    bool highContrast = false;
    D2D1_COLOR_F surface, text, subtle, disabledText, link, linkHot, selection, selectionText;
    D2D1_COLOR_F paneTop, paneBottom, groupBody, groupBorder, groupTitle, heroTitle;
    D2D1_COLOR_F frame, focus, card;
    // v2 flat kit (docs/redesign-v2.md §4).
    D2D1_COLOR_F accent, accentHot, accentDown, accentText; // Flat blue #1E6FE8 -> #1760D0, white text.
    D2D1_COLOR_F pageBg, navBg, cardBorder, ctlFill, ctlBorder, ctlHotBorder, trackOff, trackOffBorder;
};

inline D2D1_COLOR_F Rgb(UINT32 rgb, float a = 1.f) { return D2D1::ColorF(rgb, a); }
D2D1_COLOR_F SysColor(int index, float a = 1.f);

struct Stop {
    float pos;
    UINT32 rgb;
    float alpha = 1.f;
};

// --- Painter ------------------------------------------------------------------------------------------

struct ButtonState {
    bool hot = false, pressed = false, disabled = false, focused = false, isDefault = false;
    // How pressed the flat components look, 0..1. The press itself is immediate (`pressed`); the release
    // eases back to 0 (docs/redesign-v2.md §12.5), so the window hands the fading value in here.
    float press = 0;
};

enum class ButtonStyle { Normal, Green };
enum class BadgeKind { None, Play, Pause, Error };
enum class Chevron { None, Up, Down };

// Flat v2 buttons: a blue primary, a white secondary with a grey border, and a borderless one.
enum class FlatStyle { Primary, Secondary, Ghost };

// Flat line icons, 1.5 px stroke (docs/redesign-v2.md §4). Drawn, never bitmaps, so they stay crisp at any DPI
// and follow the ink colour. The app's own mark is never drawn here: it comes from the generated icons
// (§11), which the window loads with LoadIconBitmap.
enum class Glyph { Home, Devices, Camera, Settings, About, Usb, Wifi, Phone, Refresh, Monitor, Pause, Play };

class Painter {
public:
    void Init(IDWriteFactory* dwrite, float textScale);
    void SetTextScale(float textScale);
    void SetPalette(bool highContrast);
    const Palette& Colors() const { return pal_; }
    bool HighContrast() const { return pal_.highContrast; }

    // Per frame.
    void BeginFrame(ID2D1DeviceContext* dc, ID2D1Factory1* factory) { dc_ = dc; factory_ = factory; }
    ID2D1DeviceContext* Dc() const { return dc_; }

    // Text.
    Size Measure(Font font, const std::wstring& text, float maxWidth = 10000.f);
    float LineHeight(Font font);
    void Text(Font font, const std::wstring& text, const Box& box, D2D1_COLOR_F color, Align align = Align::Leading,
              int underline = -1, bool ellipsis = false, bool vcenter = false);
    IDWriteTextFormat* Format(Font font) const { return formats_[int(font)].Get(); }
    void ClearTextCache() { measureCache_.clear(); layoutCache_.clear(); }

    // Primitives.
    void Fill(const Box& b, D2D1_COLOR_F c);
    void Line(float x0, float y0, float x1, float y1, D2D1_COLOR_F c, float width = 1.f);
    void Frame(const Box& b, D2D1_COLOR_F c, float width = 1.f); // Pixel-aligned rectangle outline.
    void VGradient(const Box& b, std::initializer_list<Stop> stops, float radius = 0);
    void HGradient(const Box& b, std::initializer_list<Stop> stops);
    ComPtr<ID2D1LinearGradientBrush> Gradient(float x0, float y0, float x1, float y1, std::initializer_list<Stop> stops);
    ComPtr<ID2D1PathGeometry> TopRounded(const Box& b, float radius);
    ID2D1SolidColorBrush* Brush(D2D1_COLOR_F c);

    // XP widgets.
    void TitleBar(const Box& b, bool active, float cornerRadius);
    void CaptionButton(const Box& b, bool isClose, const ButtonState& s);
    void PushButton(const Box& b, const std::wstring& label, int underline, const ButtonState& s,
                    ButtonStyle style = ButtonStyle::Normal, Font font = Font::BodyBold);
    void Checkbox(const Box& box13, bool checked, const ButtonState& s);
    void Radio(const Box& box13, bool selected, const ButtonState& s, float dotScale = 1.f);
    void GroupHeader(const Box& b, const std::wstring& title, bool hero, Chevron chevron, float chevronTurn, bool hot);
    void GroupBody(const Box& b, bool hero);
    // Green XP progress bar. marqueeOffset: left edge of the 3-chunk block (NaN-free); determinate when
    // chunks >= 0 (that many chunks from the left).
    void ProgressTrack(const Box& b);
    void ProgressMarquee(const Box& b, float offset);
    void ProgressChunks(const Box& b, int chunks);
    static constexpr float kChunk = 8.f, kChunkGap = 2.f;
    void LivePill(const Box& b, float glow /* 0..1 sweep position, <0 none */, float opacity = 1.f);
    void Badge(float cx, float cy, float radius, BadgeKind kind, float scale = 1.f);
    void FocusRect(const Box& b);
    void SunkenFrame(const Box& b);

    // --- v2 flat components (docs/redesign-v2.md §4) -----------------------------------------------------
    void RoundRect(const Box& b, float radius, D2D1_COLOR_F fill);
    void RoundFrame(const Box& b, float radius, D2D1_COLOR_F line, float width = 1.f);
    // White card: 1 px #E3E8EF border, radius 10, soft shadow under it.
    void Card(const Box& b, float radius = 10.f, float shadow = 1.f);
    void FlatButton(const Box& b, const std::wstring& label, int underline, const ButtonState& s,
                    FlatStyle style = FlatStyle::Primary, Font font = Font::BodyBold);
    // The sidebar's selected row: a filled blue pill (white text and icon are drawn by the caller).
    void NavPill(const Box& b, float opacity = 1.f);
    // Combo box: white, rounded, with a chevron; `open` draws the pressed/open look.
    void Combo(const Box& b, const std::wstring& text, const ButtonState& s, bool open);
    void DropPanel(const Box& b);                                   // The open list's white panel.
    void DropRow(const Box& b, bool selected, bool hot);            // One row's background.
    void Switch(const Box& track, float on, const ButtonState& s);  // Toggle switch; `on` 0..1 animates.
    void SliderRail(const Box& rail, float fraction, const ButtonState& s); // Rail + filled part + thumb.
    void DarkPill(const Box& b, const std::wstring& text, Font font = Font::BodyBold); // Preview chips.
    void Icon(Glyph g, const Box& b, D2D1_COLOR_F ink, float stroke = 1.5f);
    void FocusRing(const Box& b, float radius); // Focus indication for the flat controls.

private:
    struct FontSpec {
        const wchar_t* family;
        const wchar_t* fallback;
        DWRITE_FONT_WEIGHT weight;
        float points;
    };
    void CreateFormats();

    IDWriteFactory* dwrite_ = nullptr;
    ID2D1DeviceContext* dc_ = nullptr;
    ID2D1Factory1* factory_ = nullptr;
    float textScale_ = 1.f;
    Palette pal_;
    ComPtr<IDWriteTextFormat> formats_[int(Font::Count)];
    ComPtr<IDWriteTextFormat> ellipsisFormat_[int(Font::Count)];
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1DeviceContext> brushOwner_; // Held, so a new context after device loss can't reuse its address.
    std::map<std::tuple<int, int, std::wstring>, Size> measureCache_;
    // Drawn text layouts, reused between frames. Making one per string per frame is the per-frame cost
    // that shows up during a page transition, when two pages are drawn at once.
    std::map<std::tuple<int, int, int, int, int, std::wstring>, ComPtr<IDWriteTextLayout>> layoutCache_;
};

// Loads an icon resource at an exact pixel size as a D2D bitmap.
ComPtr<ID2D1Bitmap> LoadIconBitmap(ID2D1DeviceContext* dc, int resourceId, int px);

// Accessibility settings that change how the windows draw (docs/redesign.md §6.3, §8.4).
bool ReducedMotion();       // SPI_GETCLIENTAREAANIMATION is off.
bool HighContrastOn();      // SPI_GETHIGHCONTRAST.
bool KeyboardCuesAlways();  // SPI_GETKEYBOARDCUES: always underline access keys.
float TextScaleFactor();    // Settings > Accessibility > Text size (1.0 .. 2.25).

// The XP window silhouette: Luna title bar with rounded top corners (transparent outside them), blue
// side and bottom borders, the app icon and the white Trebuchet caption with its dark shadow.
constexpr float kTitleBarH = 30.f, kBorder = 3.f, kCornerRadius = 8.f;
void WindowChrome(Painter& p, float width, float height, const std::wstring& caption, bool active, ID2D1Bitmap* icon);
// Caption button boxes (DIPs): close at the right, minimise left of it.
Box CloseButtonBox(float width);
Box MinimizeButtonBox(float width);

} // namespace mycam::xp
