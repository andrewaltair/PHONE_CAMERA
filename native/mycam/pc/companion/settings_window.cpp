#include "settings_window.h"

#include <d2d1effects.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "app_settings.h"
#include "capabilities.h"
#include "preview_source.h"
#include "protocol.h"
#include "status_images.h"
#include "status_text.h"
#include "uia_provider.h"
#include "ui_layout.h"
#include "ui_motion.h"
#include "xp_draw.h"
#include "../third_party/qrcodegen/qrcodegen.hpp"

namespace mycam {

using namespace ui;
using xp::Align;
using xp::ButtonState;
using xp::FlatStyle;
using xp::Font;
using xp::Glyph;
using xp::Rgb;
using Microsoft::WRL::ComPtr;

namespace {

// Window geometry, DIPs: 720 x 500 still fits a 720 DIP work area (CLAUDE.md). The v2 shell is two
// columns (docs/redesign-v2.md §2.1): a white sidebar and a content area that shows one page.
constexpr float kW = 720, kH = 500;
constexpr float kNavW = 150;
constexpr float kPad = 16;      // Content padding.
constexpr float kCardPad = 14;  // Inside a card.
constexpr float kNavItemH = 32;
constexpr float kRowH = 30;     // Flat control height (button, combo).
constexpr float kGap = 10;

constexpr UINT_PTR kTimerAnim = 1, kTimerPreview = 2;
constexpr uint64_t kApplyTimeoutMs = 4000;
constexpr uint64_t kUsbScanMs = 2500; // How long "Scan for USB Devices" reports that it is looking.

struct NavDef {
    int id;
    const wchar_t* label;
    Glyph glyph;
    Page page;
};
const NavDef kNav[] = {
    {kNavHome, L"&Home", Glyph::Home, Page::Home},
    {kNavDevices, L"De&vices", Glyph::Devices, Page::Devices},
    {kNavCamera, L"&Camera", Glyph::Camera, Page::Camera},
    {kNavSettings, L"Settin&gs", Glyph::Settings, Page::Settings},
    {kNavAbout, L"&About", Glyph::About, Page::About},
};

// One element as laid out and drawn.
struct Item {
    Element e;
    std::wstring label;    // What is drawn (the accessible name may say more).
    std::wstring sub;      // Second line: a card's description, a device's address.
    float alpha = 1;
    int drawGroup = kNone; // The card it is drawn in (usually e.parent).
    Glyph glyph = Glyph::Home;
    bool hasGlyph = false;
    FlatStyle style = FlatStyle::Secondary;
    Box extra;             // The switch track, the slider rail, the pill box, ...
};

// One row of the device list, as drawn (the row's Element carries the accessible name).
struct DeviceRow {
    std::wstring name, where, status;
    uint32_t ipv4 = 0;
    bool usb = false, connected = false;
    std::string webId;    // A remembered iPhone (web page): its device id, for Forget.
    int card = kNone;     // The card this row is laid out in (Connected or Available).
    bool firstInCard = false; // No divider above the first row of a card.
    Box glyphBox, nameBox, whereBox, transportBox;
};

// A white card (docs/redesign-v2.md §4). Cards are Elements too (UIA Group), so this is only its geometry.
struct CardGeo {
    int id;
    Box box;
};

// Static text that isn't an element of its own (control labels).
struct Deco {
    Box box;
    std::wstring text;
    Font font;
    bool subtle;
    bool dim;
    int underline = -1;
};

std::wstring Wide(const std::string& utf8) {
    std::wstring w(size_t(MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), w.data(), int(w.size()));
    return w;
}

xp::BadgeKind BadgeFor(int icon) {
    switch (icon) {
    case kIconStreaming: return xp::BadgeKind::Play;
    case kIconPaused: return xp::BadgeKind::Pause;
    case kIconError: return xp::BadgeKind::Error;
    default: return xp::BadgeKind::None;
    }
}

bool IsFlat(Kind k) {
    return k == Kind::Button || k == Kind::Dropdown || k == Kind::Toggle || k == Kind::Slider || k == Kind::Nav;
}

} // namespace

struct SettingsWindow::Impl {
    SettingsWindow* owner = nullptr;
    SettingsModel* model = nullptr;
    HWND hwnd = nullptr;
    float scale = 1.f, forcedScale = 0;
    bool active = true;

    xp::Surface surface;
    xp::Painter paint;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<ID2D1Bitmap> appIcon16, appIcon48, statusIcon;
    int statusIconId = 0, iconGen = -1;
    float iconScale = 0;

    // Layout (rebuilt by Layout()).
    std::vector<Item> items;
    std::vector<Deco> decos;
    std::vector<CardGeo> cards;
    std::vector<DeviceRow> deviceRows;
    Box nav, content, contentView, previewBox;
    float scroll = 0, contentH = 0;
    size_t lastSignature = 0;
    float lineH = 13, smallH = 12;

    // Pages (docs/redesign-v2.md §2.1): the window is a small router.
    Page page = Page::Home, returnPage = Page::Home;
    Tween pageAnim;   // 0 -> 1 while the incoming page arrives.
    Tween navPill;    // The pill's top edge, in DIPs.
    bool navPillSet = false;
    // The outgoing page, drawn from the last frame's layout while it fades out.
    std::vector<Item> fadeItems;
    std::vector<Deco> fadeDecos;
    std::vector<CardGeo> fadeCards;
    std::vector<DeviceRow> fadeRows;
    Box fadePreview = {};

    // Screen readers (UI Automation, uia_provider.h). Created with the window, disconnected on destroy.
    std::unique_ptr<UiaHost> uia;

    // Interaction.
    int hot = kNone, pressed = kNone, focus = kNone;
    bool keyboardCues = false, tracking = false, sysKeyHandled = false;
    bool thumbDrag = false, thumbHot = false;
    float dragStartY = 0, dragStartScroll = 0;
    int sliderDrag = kNone;
    int released = kNone;
    Tween pressFade;  // The released button's pressed look eases back (§12.5).

    // Dropdowns.
    int openDrop = kNone, dropHot = kNone;
    Tween dropAnim;

    // Toggles, sliders and other per-element values that animate.
    std::map<int, Tween> valueAnim;

    // Motion and accessibility settings.
    bool reducedMotion = false;
    Tween statusFade, badgePop, liveGlow;
    StatusView view = {}, prevView = {};
    bool viewShown = false, wasLive = false;
    uint64_t pairingSince = 0;
    std::wstring pairingCode;
    uint64_t applyingSince = 0;
    int applyKind = 0, applyValue = 0; // 1 quality, 2 fps, 3 facing
    // "Scan for phones": pressed at (so the marquee shows before the link picks the request up).
    uint64_t scanPressedAt = 0, usbScanAt = 0;
    uint32_t scansAtPress = 0;
    // "Connect an iPhone": the address the QR code points at (0: the best one) and the code's modules.
    uint32_t webIp = 0;
    std::wstring qrUrl;
    std::vector<uint8_t> qrModules;
    int qrSize = 0;

    // Preview.
    PreviewSource preview;
    std::vector<uint8_t> previewPixels;
    bool previewPending = false, previewFresh = false, previewOpen = false;
    Tween previewFade; // 0: status picture, 1: live frames.
    ComPtr<ID2D1Bitmap1> liveBitmap, artBitmap;
    UINT pw = 0, ph = 0;     // Preview size in pixels.
    int artKind = -1;
    UINT artW = 0, artH = 0;
    Nv12Image pausedImage, waitingImage;
    bool imagesLoaded = false;
    ComPtr<ID2D1Effect> artBlur;
    int effectGen = -1;

    static uint64_t Now() { return xp::NowMs(); }
    float Dur(float ms) const { return reducedMotion || paint.HighContrast() ? 0.f : ms; }

    // --- State ---------------------------------------------------------------------------------------

    LinkStatus Status() const { return model->status(); }
    static bool Connected(const LinkStatus& s) { return s.state >= LinkState::Idle && s.camera.valid; }

    bool PauseEnabled(const LinkStatus& s) const { return s.state >= LinkState::Idle && !s.lockPaused && !s.webAway; }

    bool Enabled(int id, const LinkStatus& s) const {
        const proto::CameraInfo& c = s.camera;
        const bool connected = Connected(s);
        const bool known = caps::Known(connected, c);
        switch (id) {
        case kPause: return PauseEnabled(s);
        case kBack: case kFront: return true;
        case kDdCamera: return true;
        case kQ720: return caps::QualityEnabled(connected, c, proto::kQuality720p);
        case kQ1080: return caps::QualityEnabled(connected, c, proto::kQuality1080p);
        case kQ4K: return caps::QualityEnabled(connected, c, proto::kQuality4K);
        case kFps30: return caps::FpsEnabled(connected, c, 30);
        case kFps60: return caps::FpsEnabled(connected, c, 60);
        case kFps120: return caps::FpsEnabled(connected, c, 120);
        // The lists open even with no phone, so the choices can be seen; each entry is still gated on what
        // the connected phone reports (CLAUDE.md "Works on every phone"), and a note says so.
        case kDdQuality: case kDdFps: return true;
        case kZoomSlider: return known && c.zoomMaxX100 > c.zoomMinX100;
        case kEvValue: return known && c.evMax > c.evMin;
        case kFocusAuto: case kFocusLock: return caps::FocusEnabled(known, c);
        case kTorch: return caps::TorchEnabled(known, c);
        case kAuto: return caps::AutoEnabled(known, c);
        default: return true;
        }
    }

    bool Applying(const LinkStatus& s, uint64_t now) {
        if (!applyingSince) return false;
        const bool done = now - applyingSince > kApplyTimeoutMs || !Connected(s) ||
                          (applyKind == 1 && s.camera.quality == applyValue) ||
                          (applyKind == 2 && s.camera.fps == applyValue) || (applyKind == 3 && s.facing == applyValue);
        if (done) applyingSince = 0;
        return !done;
    }

    void StartApplying(int kind, int value, const LinkStatus& s) {
        const bool already = (kind == 1 && s.camera.quality == value) || (kind == 2 && s.camera.fps == value) ||
                             (kind == 3 && s.facing == value);
        if (already || !Connected(s)) return;
        applyingSince = Now();
        applyKind = kind;
        applyValue = value;
    }

    // Picks up a new status: crossfade, badge pop, LIVE glow (docs/redesign.md §6.3).
    void UpdateStatus() {
        const uint64_t now = Now();
        const LinkStatus s = Status();
        const StatusView v = DescribeStatus(s, model->cameraRegistered(), model->backendName());
        if (!viewShown) {
            view = v;
            viewShown = true;
        } else if (v.headline != view.headline || v.icon != view.icon || v.facts != view.facts || v.hint != view.hint) {
            prevView = view;
            statusFade.Start(1, now, Dur(kStandardMs), 0);
            if (v.icon != view.icon) badgePop.Start(1, now, Dur(kPageMs), 0);
            view = v;
        }
        const bool live = s.state == LinkState::Streaming && !s.lockPaused;
        if (live && !wasLive && Dur(600) > 0) liveGlow.Start(1, now, 600, 0);
        wasLive = live;
        if (s.pairingCode != pairingCode) {
            const bool had = !pairingCode.empty();
            pairingCode = s.pairingCode;
            pairingSince = pairingCode.empty() ? 0 : now;
            // Pairing is a page, not a modal dialog, while the window is open (§2.4).
            if (!pairingCode.empty() && page != Page::Pairing) GoTo(Page::Pairing);
            else if (pairingCode.empty() && had && page == Page::Pairing) GoTo(returnPage);
        }
        Applying(s, now);
    }

    // --- Page router ---------------------------------------------------------------------------------

    void GoTo(Page next) {
        if (next == page) return;
        // The QR code (and its one-time code) exists only while its page shows.
        if (page == Page::IPhone && model->stopWebPairing) model->stopWebPairing();
        if (next == Page::IPhone && model->startWebPairing) model->startWebPairing(webIp);
        if (page != Page::Pairing) returnPage = page;
        // The outgoing page keeps the layout it already had and fades out over it (§12).
        fadeItems = items;
        fadeDecos = decos;
        fadeCards = cards;
        fadeRows = deviceRows;
        fadePreview = previewBox;
        page = next;
        scroll = 0;
        CloseDropdown();
        sliderDrag = kNone;
        pressed = kNone;
        hot = kNone;
        pageAnim.Start(1, Now(), Dur(kPageMs), 0);
        if (page != Page::Camera) ClosePreview();
        Invalidate();
    }

    void CloseDropdown() {
        if (openDrop == kNone) return;
        openDrop = kNone;
        dropHot = kNone;
        dropAnim.Set(0);
    }

    bool Transitioning(uint64_t now) const { return pageAnim.Running(now); }

    // --- Layout --------------------------------------------------------------------------------------

    Box curClip;
    float curAlpha = 1;
    bool curVisible = true;

    Item& Add(int id, Kind kind, const std::wstring& label, const Box& rect, int parent) {
        Item it;
        it.e.id = id;
        it.e.kind = kind;
        int key = -1;
        it.label = StripAccessKey(label, &key);
        it.e.name = it.label;
        it.e.accessKeyIndex = key;
        it.e.rect = rect;
        it.e.clip = curClip;
        it.e.parent = parent;
        it.e.visible = curVisible;
        it.e.focusable = kind == Kind::Button || kind == Kind::Checkbox || kind == Kind::Radio || kind == Kind::Link ||
                         kind == Kind::Nav || kind == Kind::Dropdown || kind == Kind::Toggle || kind == Kind::Slider;
        it.alpha = curAlpha;
        it.drawGroup = parent;
        items.push_back(std::move(it));
        return items.back();
    }

    void Deco_(const Box& b, const std::wstring& text, Font font, bool subtle, bool dim = false, int underline = -1) {
        decos.push_back({b, text, font, subtle, dim, underline});
    }

    Size ButtonSize(const std::wstring& label, float minW = 86.f) {
        int k;
        const Size t = paint.Measure(Font::BodyBold, StripAccessKey(label, &k));
        return {std::max(minW, t.w + 30), std::max(kRowH, t.h + 12)};
    }

    Item& AddButton(int id, const std::wstring& label, FlatStyle style, const Box& box, int parent, bool enabled = true) {
        Item& it = Add(id, Kind::Button, label, box, parent);
        it.style = style;
        it.e.enabled = enabled;
        return it;
    }

    // A label with a toggle switch at the right of the row.
    void AddToggle(int id, const std::wstring& label, bool on, int parent, float x, float* y, float w, bool enabled = true) {
        const float h = std::max(26.f, lineH + 12);
        Item& it = Add(id, Kind::Toggle, label, MakeBox(x, *y, w, h), parent);
        it.e.checked = on;
        it.e.enabled = enabled;
        const float tw = 36, th = 20;
        it.extra = MakeBox(x + w - tw, *y + (h - th) / 2, tw, th);
        Animate(id, on ? 1.f : 0.f, kToggleMs);
        *y += h;
    }

    // Label above, combo below; the entries are DropItems whose parent is the combo.
    void AddDropdown(int id, const std::wstring& label, const std::wstring& valueText,
                     const std::vector<std::pair<int, std::wstring>>& entries, int selected, int parent, float x, float* y,
                     float w, bool enabled) {
        int key = -1;
        const std::wstring plain = StripAccessKey(label, &key);
        Deco_(MakeBox(x, *y, w, smallH), plain, Font::Small, true, false, Cues() ? key : -1);
        *y += smallH + 4;
        Item& it = Add(id, Kind::Dropdown, label, MakeBox(x, *y, w, kRowH), parent);
        it.label = valueText;
        it.e.name = plain;
        it.e.enabled = enabled;
        it.e.expandable = true;
        it.e.expanded = openDrop == id;
        *y += kRowH;
        // The entries: visible (so UIA and the mouse can reach them) only while the list is open.
        const bool open = openDrop == id;
        const float rowH = std::max(26.f, lineH + 12);
        const float panelTop = it.e.rect.b + 4;
        const bool wasVisible = curVisible;
        const Box wasClip = curClip;
        curVisible = open;
        curClip = content;
        float ry = panelTop + 4;
        for (const auto& [entryId, text] : entries) {
            Item& row = Add(entryId, Kind::DropItem, text, MakeBox(x + 4, ry, w - 8, rowH), id);
            row.e.selected = entryId == selected;
            row.e.enabled = Enabled(entryId, Status());
            row.e.focusable = false;
            ry += rowH;
        }
        curVisible = wasVisible;
        curClip = wasClip;
        // Only the open list sets the panel (Layout clears it): a closed dropdown laid out after the open one
        // must not wipe it, or the list is clickable but never drawn.
        if (open) dropPanel = Box{x, panelTop, x + w, ry + 4};
    }
    Box dropPanel = {};

    void AddSlider(int id, const std::wstring& label, const std::wstring& valueText, float value, float lo, float hi,
                   float step, int parent, float x, float* y, float w, bool enabled) {
        int key = -1;
        const std::wstring plain = StripAccessKey(label, &key);
        const Size v = paint.Measure(Font::BodyBold, valueText);
        Deco_(MakeBox(x, *y, w - v.w - 6, smallH), plain, Font::Small, true, false, Cues() ? key : -1);
        Deco_(MakeBox(x, *y, w, smallH), valueText, Font::BodyBold, false, !enabled);
        decos.back().box = MakeBox(x + w - v.w, *y, v.w, smallH);
        *y += smallH + 2;
        const float h = 22;
        Item& it = Add(id, Kind::Slider, label, MakeBox(x, *y, w, h), parent);
        it.e.name = plain;
        it.label = valueText;
        it.e.enabled = enabled;
        it.e.value = value;
        it.e.rangeMin = lo;
        it.e.rangeMax = hi;
        it.e.rangeStep = step;
        it.extra = MakeBox(x + 8, *y + h / 2 - 2, w - 16, 4);
        Animate(id, SliderFraction(it.e), kStandardMs);
        *y += h;
    }

    // Keeps a tween per element id, retargeting it instead of snapping (§12). The first value is set
    // without animating, so nothing moves on the first paint.
    float Animate(int id, float target, float ms) {
        const uint64_t now = Now();
        auto it = valueAnim.find(id);
        if (it == valueAnim.end()) {
            Tween t;
            t.Set(target);
            valueAnim[id] = t;
            return target;
        }
        it->second.Retarget(target, now, Dur(ms), EaseOut);
        return it->second.Value(now, EaseOut);
    }
    float AnimValue(int id, float fallback) const {
        auto it = valueAnim.find(id);
        return it == valueAnim.end() ? fallback : it->second.Value(Now(), EaseOut);
    }

    // The "60 fps works at 720p on this camera" line (capability-driven, per quality).
    std::wstring FpsReason(const LinkStatus& s) const {
        const proto::CameraInfo& c = s.camera;
        if (!caps::Known(Connected(s), c)) return L"";
        static const wchar_t* names[3] = {L"720p", L"1080p", L"4K"};
        auto where = [&](uint8_t bit) {
            std::wstring out;
            for (uint8_t q = 0; q < 3; ++q) {
                if (q == c.quality || !(proto::FpsMask(c, q) & bit)) continue;
                if (q == proto::kQuality4K && !(c.flags & proto::kCamHas4K)) continue;
                out += (out.empty() ? L"" : L" and ") + std::wstring(names[q]);
            }
            return out;
        };
        const uint8_t mask = proto::FpsMask(c, c.quality);
        if (!(mask & proto::kFps60Bit)) {
            const std::wstring q = where(proto::kFps60Bit);
            if (!q.empty()) return L"60 fps works at " + q + L" on this camera.";
        }
        if (!(mask & proto::kFps120Bit)) {
            const std::wstring q = where(proto::kFps120Bit);
            if (!q.empty()) return L"120 fps works at " + q + L" on this camera.";
        }
        if (!(mask & (proto::kFps60Bit | proto::kFps120Bit)) && where(proto::kFps60Bit | proto::kFps120Bit).empty())
            return L"This camera does 30 fps.";
        return L"";
    }

    void Layout() {
        const uint64_t now = Now();
        const LinkStatus s = Status();
        items.clear();
        decos.clear();
        cards.clear();
        deviceRows.clear();
        dropPanel = {};
        lineH = paint.LineHeight(Font::Body);
        smallH = paint.LineHeight(Font::Small);

        const float border = xp::kBorder, top = xp::kTitleBarH;
        nav = {border, top, border + kNavW, kH - border};
        content = {nav.r, top, kW - border, kH - border};
        contentView = content;

        curClip = {0, 0, kW, kH};
        curAlpha = 1;
        curVisible = true;
        Add(kCaptionMin, Kind::CaptionButton, L"Minimize", xp::MinimizeButtonBox(kW), kNone);
        Add(kCaptionClose, Kind::CaptionButton, L"Close", xp::CloseButtonBox(kW), kNone);

        // --- Sidebar ------------------------------------------------------------------------------
        curClip = nav;
        float ny = nav.t + 12;
        const float itemH = std::max(kNavItemH, lineH + 16);
        for (const NavDef& d : kNav) {
            Item& it = Add(d.id, Kind::Nav, d.label, MakeBox(nav.l + 8, ny, kNavW - 16, itemH), kNone);
            it.e.radioSet = kSetNav;
            it.e.selected = page == d.page;
            it.glyph = d.glyph;
            it.hasGlyph = true;
            if (it.e.selected) {
                if (!navPillSet) {
                    navPill.Set(ny);
                    navPillSet = true;
                } else {
                    navPill.Retarget(ny, now, Dur(kStandardMs), EaseOut);
                }
            }
            ny += itemH + 2;
        }

        // --- Content ------------------------------------------------------------------------------
        curClip = contentView;
        const float x = content.l + kPad, w = content.W() - 2 * kPad;
        float y = content.t + kPad - scroll;
        switch (page) {
        case Page::Home: LayoutHome(s, now, x, &y, w); break;
        case Page::Devices: LayoutDevices(s, now, x, &y, w); break;
        case Page::Camera: LayoutCamera(s, now, x, &y, w); break;
        case Page::Settings: LayoutSettings(s, x, &y, w); break;
        case Page::About: LayoutAbout(x, &y, w); break;
        case Page::IPhone: LayoutIPhone(s, x, &y, w); break;
        default: LayoutPairing(s, now, x, &y, w); break;
        }
        contentH = y + scroll - content.t + kPad;

        // Default focus: the first tab stop.
        std::vector<Element> list = Elements();
        const Element* f = FindElement(list, focus);
        if (!f || !CanFocus(*f)) focus = NextTabStop(list, kNone, false);

        // Tell a UIA provider when anything changed.
        size_t sig = items.size();
        for (const auto& it : items) {
            sig = sig * 131 + size_t(it.e.id) * 7 + (it.e.enabled ? 1 : 0) + (it.e.checked ? 2 : 0) + (it.e.selected ? 4 : 0) +
                  (it.e.expanded ? 8 : 0) + (it.e.visible ? 16 : 0) + std::hash<std::wstring>()(it.e.name);
        }
        if (sig != lastSignature) {
            lastSignature = sig;
            if (owner->elementsChanged_) owner->elementsChanged_();
        }
    }

    void PageHeader(const std::wstring& title, const std::wstring& subtitle, float x, float* y, float w) {
        const Size t = paint.Measure(Font::Instruction, title, w);
        Item& h = Add(kPageTitle, Kind::Text, L"", MakeBox(x, *y, w, t.h), kNone);
        h.e.name = h.label = title;
        *y += t.h + 2;
        if (!subtitle.empty()) {
            const Size st = paint.Measure(Font::Small, subtitle, w);
            Item& sb = Add(kPageSubtitle, Kind::Text, L"", MakeBox(x, *y, w, st.h), kNone);
            sb.e.name = sb.label = subtitle;
            *y += st.h;
        }
        *y += kGap + 2;
    }

    Box BeginCard(int id, float x, float y, float w, float h, const std::wstring& name) {
        const Box box = MakeBox(x, y, w, h);
        cards.push_back({id, box});
        Item& c = Add(id, Kind::Card, L"", box, kNone);
        c.e.name = name;
        return box;
    }

    // --- Home ---------------------------------------------------------------------------------------

    void LayoutHome(const LinkStatus& s, uint64_t now, float x, float* y, float w) {
        PageHeader(L"Connect a Device", L"Find and connect to your phone", x, y, w);
        LayoutStatusStrip(s, now, x, y, w);
        *y += kGap + 2;

        // USB card.
        LayoutConnectCard(kCardUsb, Glyph::Usb, L"USB Connection", L"Connect your phone via USB for best performance.",
                          kScanUsb, L"Scan for &USB Devices", x, y, w);
        if (usbScanAt && now - usbScanAt < kUsbScanMs) {
            const std::wstring note = L"Looking for a phone on the cable…";
            const Size t = paint.Measure(Font::Small, note, w);
            Item& n = Add(kUsbNote, Kind::Text, L"", MakeBox(x + 2, *y + 4, w, t.h), kNone);
            n.e.name = n.label = note;
            n.e.liveRegion = true;
            *y += t.h + 4;
        }
        *y += kGap;
        // Wi-Fi card.
        LayoutConnectCard(kCardWifi, Glyph::Wifi, L"Wi-Fi Connection", L"Both devices must be on the same Wi-Fi network.",
                          kScan, L"&Scan for Devices", x, y, w);
        LayoutScanSentence(s, now, x, y, w);
        *y += kGap;
        // QR card: the code a phone scans to pair (Android in MyCam, iPhones through Safari; IMPROVEMENTS 12).
        LayoutConnectCard(kCardWebHome, Glyph::Phone, L"Generate QR",
                          L"Scan it with MyCam on Android, or an iPhone’s Camera app, on the same Wi-Fi.", kConnectIPhone,
                          L"Show &QR code", x, y, w);
    }

    void LayoutScanSentence(const LinkStatus& s, uint64_t now, float x, float* y, float w) {
        if (Scanning(s, now) || s.scansDone) {
            *y += 6;
            const std::wstring sentence = Scanning(s, now) ? L"Looking for phones on this Wi-Fi…"
                                                           : ScanResultSentence(s.nearbyPhones.size());
            const Size t = paint.Measure(Font::Small, sentence, w);
            Item& r = Add(kScanResult, Kind::Text, L"", MakeBox(x + 2, *y, w, t.h), kNone);
            r.e.name = r.label = sentence;
            r.e.liveRegion = true;
            *y += t.h;
            if (!s.nearbyPhones.empty() && !Scanning(s, now)) {
                *y += 4;
                const Size l = paint.Measure(Font::Body, L"See the devices that answered");
                Add(kNavDevicesLink, Kind::Link, L"See the devices that answered", MakeBox(x + 2, *y, l.w, l.h), kNone);
                *y += l.h;
            }
        }
    }

    // The status headline, its detail line, the progress bar and Pause: the mockup leaves them out and
    // that is wrong (docs/redesign-v2.md §10.1, §10.2), so Home keeps them.
    void LayoutStatusStrip(const LinkStatus& s, uint64_t now, float x, float* y, float w) {
        const float iconW = 32;
        const Size pause = ButtonSize(model->paused() ? L"&Resume the camera" : L"&Pause the camera", 120);
        const float tx = x + iconW + 10;
        const float tw = std::max(80.f, w - iconW - 10 - pause.w - 12);
        std::wstring shown = view.headline;
        const float maxH = paint.LineHeight(Font::Instruction) * 2 + 1;
        while (shown.size() > 4 && paint.Measure(Font::Instruction, shown, tw).h > maxH) {
            const bool dots = shown.back() == L'…';
            shown.erase(shown.size() - (dots ? 2 : 1));
            while (!shown.empty() && shown.back() == L' ') shown.pop_back();
            shown += L"…";
        }
        const Size head = paint.Measure(Font::Instruction, shown, tw);
        {
            Item& h = Add(kStatusHeadline, Kind::Text, L"", MakeBox(tx, *y, tw, head.h), kNone);
            h.e.name = view.headline;
            h.label = shown;
            h.e.liveRegion = true;
        }
        float ty = *y + head.h + 1;
        if (!view.facts.empty()) {
            const Size f = paint.Measure(Font::Small, view.facts, tw);
            Item& d = Add(kStatusDetail, Kind::Text, L"", MakeBox(tx, ty, tw, f.h), kNone);
            d.e.name = d.label = view.facts;
            ty += f.h;
        }
        if (!s.pairingCode.empty()) {
            ty += 3;
            const Size l = paint.Measure(Font::Body, L"Show the pairing code");
            Add(kShowPairing, Kind::Link, L"Show the pairing code", MakeBox(tx, ty, l.w, l.h), kNone);
            ty += l.h;
        }
        // Pause, right-aligned on the first line.
        {
            Item& p = Add(kPause, Kind::Button, model->paused() ? L"&Resume the camera" : L"&Pause the camera",
                          MakeBox(x + w - pause.w, *y, pause.w, pause.h), kNone);
            p.style = model->paused() ? FlatStyle::Primary : FlatStyle::Secondary;
            p.hasGlyph = true;
            p.glyph = model->paused() ? Glyph::Play : Glyph::Pause;
            p.e.enabled = PauseEnabled(s);
        }
        *y = std::max(*y + std::max(iconW, pause.h), ty);
        const bool applying = Applying(s, now);
        if (view.progress != StatusProgress::None || applying) {
            *y += 8;
            const float barH = 13;
            Item& bar = Add(kStatusProgress, Kind::ProgressBar, L"", MakeBox(x, *y, w, barH), kNone);
            bar.e.value = -1;
            bar.e.name = applying ? L"Starting the camera" : L"Working";
            *y += barH;
        }
    }

    void LayoutConnectCard(int cardId, Glyph glyph, const std::wstring& title, const std::wstring& body, int buttonId,
                           const std::wstring& buttonLabel, float x, float* y, float w) {
        const float iconW = 26, textX = x + kCardPad + iconW + 12, textW = w - kCardPad * 2 - iconW - 12;
        const Size t = paint.Measure(Font::BodyBold, title, textW);
        const Size b = paint.Measure(Font::Small, body, textW);
        const Size btn = ButtonSize(buttonLabel, 150);
        const float h = kCardPad + std::max(iconW, t.h) + 3 + b.h + 12 + btn.h + kCardPad;
        const Box card = BeginCard(cardId, x, *y, w, h, title);
        float cy = card.t + kCardPad;
        Deco_(MakeBox(x + kCardPad, cy, iconW, iconW), L"", Font::Body, false); // Placeholder: the glyph.
        decos.back().text.clear();
        Item& icon = Add(cardId + 1000, Kind::Text, L"", MakeBox(x + kCardPad, cy, iconW, iconW), cardId);
        icon.hasGlyph = true;
        icon.glyph = glyph;
        icon.e.visible = curVisible;
        Deco_(MakeBox(textX, cy, textW, t.h), title, Font::BodyBold, false);
        cy += t.h + 3;
        Deco_(MakeBox(textX, cy, textW, b.h), body, Font::Small, true);
        cy += b.h + 12;
        AddButton(buttonId, buttonLabel, FlatStyle::Primary, MakeBox(textX, cy, btn.w, btn.h), cardId);
        *y = card.b;
    }

    // --- Devices ------------------------------------------------------------------------------------

    // The rows: the phone on the cable (when there is one) first, then every phone that answered on the
    // Wi-Fi (§2.3: USB devices are listed in the same list).
    std::vector<DeviceRow> BuildRows(const LinkStatus& s) {
        std::vector<DeviceRow> rows;
        if (s.state >= LinkState::Waiting && !s.wireless) {
            DeviceRow r;
            r.usb = true;
            r.connected = s.state >= LinkState::Idle;
            r.name = s.phoneName.empty() ? L"Phone on USB" : s.phoneName;
            r.where = L"USB cable";
            r.status = r.connected ? L"Connected" : L"Waiting for the MyCam app";
            rows.push_back(std::move(r));
        }
        // iPhones (the web page): the one in use, and the remembered ones (they connect when their page opens).
        for (const LinkStatus::WebDeviceRow& d : s.webDevices) {
            if (int(rows.size()) >= kScanRowsMax) break;
            DeviceRow r;
            r.webId = d.id;
            r.name = d.name;
            r.connected = d.connected && s.state >= LinkState::Idle;
            r.usb = d.connected && s.webLink == L"USB hotspot";
            r.where = d.connected ? L"Safari · " + (s.webLink.empty() ? std::wstring(L"Wi-Fi") : s.webLink) : L"Safari";
            r.status = r.connected ? (s.webAway ? L"Connected · page closed or locked" : L"Connected")
                                   : L"Remembered: open MyCam on it to connect";
            rows.push_back(std::move(r));
        }
        for (const NearbyPhone& p : s.nearbyPhones) {
            if (int(rows.size()) >= kScanRowsMax) break;
            DeviceRow r;
            r.ipv4 = p.ip;
            r.name = Wide(p.name);
            r.where = Wide(p.ipText);
            r.connected = p.connected;
            r.status = p.connected ? L"Connected" : p.paired ? L"Paired" : L"New: it will ask for a code";
            rows.push_back(std::move(r));
        }
        return rows;
    }

    // One section of the Devices page: a small heading, then a card with `which` of the rows. Advances *y
    // past it, and does nothing when no row belongs to it.
    void LayoutDeviceSection(int titleId, int cardId, int groupId, int noteId, const std::wstring& heading,
                             const std::wstring& empty, bool connectedSection, float x, float* y, float w) {
        std::vector<size_t> mine;
        for (size_t i = 0; i < deviceRows.size(); ++i)
            if (deviceRows[i].connected == connectedSection) mine.push_back(i);
        if (mine.empty() && empty.empty()) return;

        const Size ht = paint.Measure(Font::BodyBold, heading, w);
        Item& h = Add(titleId, Kind::Text, L"", MakeBox(x, *y, w, ht.h), kNone);
        h.e.name = h.label = heading;
        *y += ht.h + 6;
        if (mine.empty()) {
            const Size et = paint.Measure(Font::Small, empty, w);
            Item& n = Add(noteId, Kind::Text, L"", MakeBox(x + 2, *y, w, et.h), kNone);
            n.e.name = n.label = empty;
            *y += et.h + kGap;
            return;
        }

        const float rowH = std::max(52.f, lineH + smallH + 26);
        const Box card = BeginCard(cardId, x, *y, w, rowH * float(mine.size()) + 2, heading);
        Add(groupId, Kind::Group, heading, card, cardId);
        const Size connect = ButtonSize(L"Connect", 84);
        for (size_t slot = 0; slot < mine.size(); ++slot) {
            const size_t i = mine[slot];
            DeviceRow& r = deviceRows[i];
            r.card = cardId;
            r.firstInCard = slot == 0;
            const float ry = card.t + 1 + rowH * float(slot);
            const bool canConnect = !r.connected && !r.usb && r.webId.empty() && model->connectTo;
            const bool canForget = !r.webId.empty() && model->forgetWebDevice;
            const Size forget = ButtonSize(L"Forget", 84);
            r.glyphBox = MakeBox(card.l + kCardPad, ry + (rowH - 24) / 2, 24, 24);
            const float nameL = r.glyphBox.r + 12;
            const float nameR = card.r - kCardPad - (canConnect ? connect.w + 12 : canForget ? forget.w + 12 : 0) - 26;
            r.nameBox = {nameL, ry + (rowH - lineH - smallH - 2) / 2, nameR, ry + (rowH - lineH - smallH - 2) / 2 + lineH};
            r.whereBox = {nameL, r.nameBox.b + 1, nameR, r.nameBox.b + 1 + smallH};
            r.transportBox = MakeBox(nameR + 4, ry + (rowH - 18) / 2, 18, 18);
            const Box rowBox = {card.l, ry, card.r, ry + rowH};
            {
                Item& row = Add(ScanRowId(int(i), 0), Kind::Text, L"", rowBox, groupId);
                row.e.name = r.name + L", " + r.where + L", " + r.status;
                row.drawGroup = cardId;
            }
            if (canConnect) {
                Item& c = AddButton(ScanRowId(int(i), 1), L"Connect", FlatStyle::Primary,
                                    MakeBox(card.r - kCardPad - connect.w, ry + (rowH - connect.h) / 2, connect.w, connect.h),
                                    groupId);
                c.e.name = L"Connect to " + r.name;
                c.drawGroup = cardId;
            }
            if (canForget) { // A remembered iPhone: Forget, right here (it scans the QR code again next time).
                Item& f = AddButton(ScanRowId(int(i), 2), L"Forget", FlatStyle::Secondary,
                                    MakeBox(card.r - kCardPad - forget.w, ry + (rowH - forget.h) / 2, forget.w, forget.h),
                                    groupId);
                f.e.name = L"Forget " + r.name + L" (it scans the code again next time)";
                f.drawGroup = cardId;
            }
        }
        *y = card.b + kGap;
    }

    void LayoutDevices(const LinkStatus& s, uint64_t now, float x, float* y, float w) {
        const Size refresh = ButtonSize(L"&Refresh", 96);
        const float titleW = w - refresh.w - 12;
        const Size t = paint.Measure(Font::Instruction, L"Devices", titleW);
        {
            Item& h = Add(kPageTitle, Kind::Text, L"", MakeBox(x, *y, titleW, t.h), kNone);
            h.e.name = h.label = L"Devices";
        }
        AddButton(kScan, L"&Refresh", FlatStyle::Secondary, MakeBox(x + w - refresh.w, *y - 3, refresh.w, refresh.h), kNone)
            .glyph = Glyph::Refresh;
        items.back().hasGlyph = true;
        float ty = *y + t.h + 2;
        const std::wstring sub = L"Phones on this Wi-Fi, the one on the cable, and your iPhones";
        const Size st = paint.Measure(Font::Small, sub, titleW);
        {
            Item& sb = Add(kPageSubtitle, Kind::Text, L"", MakeBox(x, ty, titleW, st.h), kNone);
            sb.e.name = sb.label = sub;
        }
        *y = std::max(ty + st.h, *y + refresh.h) + kGap;

        const bool scanning = Scanning(s, now);
        if (scanning) {
            const float barH = 13;
            Item& bar = Add(kScanProgress, Kind::ProgressBar, L"", MakeBox(x, *y, w, barH), kNone);
            bar.e.value = -1;
            bar.e.name = L"Looking for phones";
            *y += barH + 6;
        }
        const std::wstring sentence = scanning ? L"Looking for phones on this Wi-Fi…"
                                   : s.scansDone == 0 ? L"Choose Refresh to look for phones on this Wi-Fi."
                                                      : ScanResultSentence(s.nearbyPhones.size());
        {
            const Size sh = paint.Measure(Font::Small, sentence, w);
            Item& r = Add(kScanResult, Kind::Text, L"", MakeBox(x + 2, *y, w, sh.h), kNone);
            r.e.name = r.label = sentence;
            r.e.liveRegion = true;
            *y += sh.h + kGap;
        }

        // The phone in use is kept apart from the ones that are only there to pick (§2.1): the connected
        // section always shows, so "nothing is connected" is said rather than left to be inferred.
        deviceRows = BuildRows(s);
        LayoutDeviceSection(kConnectedTitle, kCardConnected, kConnectedList, kConnectedNote, L"Connected Device",
                            L"No device is connected yet.", true, x, y, w);
        LayoutDeviceSection(kAvailableTitle, kCardDevices, kScanList, kAvailableNote, L"Available Devices",
                            scanning ? L"" : L"Nothing else has answered yet.", false, x, y, w);
    }

    // --- Camera -------------------------------------------------------------------------------------

    void LayoutCamera(const LinkStatus& s, uint64_t now, float x, float* y, float w) {
        const proto::CameraInfo& c = s.camera;
        const bool known = caps::Known(Connected(s), c);
        PageHeader(L"Camera", L"Live controls for the connected phone", x, y, w);

        const float colGap = 14;
        float pvW = std::floor((w - colGap) * 0.57f);
        const float ctlW = w - colGap - pvW;
        float pvH = std::floor(pvW * 9 / 16);
        previewBox = MakeBox(x, *y, pvW, pvH);
        {
            Item& p = Add(kPreview, Kind::Image, L"", previewBox, kNone);
            p.e.name = L"Preview: the picture apps receive";
        }
        float py = previewBox.b + kGap;
        // Pause stays reachable on this page (§2.5 / §10.1): under the preview.
        {
            const Size pause = ButtonSize(model->paused() ? L"&Resume the camera" : L"&Pause the camera", 140);
            Item& p = Add(kPause, Kind::Button, model->paused() ? L"&Resume the camera" : L"&Pause the camera",
                          MakeBox(x, py, pause.w, pause.h), kNone);
            p.style = model->paused() ? FlatStyle::Primary : FlatStyle::Secondary;
            p.hasGlyph = true;
            p.glyph = model->paused() ? Glyph::Play : Glyph::Pause;
            p.e.enabled = PauseEnabled(s);
            py += pause.h;
        }
        {
            const Size hint = paint.Measure(Font::Small, view.hint, pvW);
            Item& t = Add(kStatusSentence, Kind::Text, L"", MakeBox(x, py + 8, pvW, hint.h), kNone);
            t.e.name = t.label = view.hint;
            t.e.liveRegion = true;
            py += 8 + hint.h;
        }

        // The controls column, in a card.
        const float cx = x + pvW + colGap, cw = ctlW;
        const float ix = cx + kCardPad, iw = cw - 2 * kCardPad;
        float cy = *y + kCardPad;
        const size_t cardIndex = cards.size();
        cards.push_back({kCardControls, MakeBox(cx, *y, cw, 10)});
        const size_t cardItem = items.size();
        {
            Item& card = Add(kCardControls, Kind::Card, L"", MakeBox(cx, *y, cw, 10), kNone);
            card.e.name = L"Camera controls";
        }
        const int facing = s.facing == proto::kFacingFront ? kFront : kBack;
        AddDropdown(kDdCamera, L"Ca&mera", facing == kFront ? L"Front Camera" : L"Back Camera",
                    {{kBack, L"Back Camera"}, {kFront, L"Front Camera"}}, facing, kCardControls, ix, &cy, iw, true);
        cy += kGap;
        const int q = !c.valid ? kNone : c.quality == proto::kQuality720p ? kQ720 : c.quality == proto::kQuality4K ? kQ4K : kQ1080;
        const wchar_t* qText = q == kQ720 ? L"1280 × 720" : q == kQ4K ? L"3840 × 2160" : q == kQ1080 ? L"1920 × 1080" : L"—";
        AddDropdown(kDdQuality, L"Res&olution", qText,
                    {{kQ720, L"1280 × 720"}, {kQ1080, L"1920 × 1080"}, {kQ4K, L"3840 × 2160 (4K)"}}, q, kCardControls, ix,
                    &cy, iw, Enabled(kDdQuality, s));
        cy += kGap;
        const int fps = caps::EffectiveFps(c);
        const int f = !c.valid ? kNone : fps == 120 ? kFps120 : fps == 60 ? kFps60 : kFps30;
        AddDropdown(kDdFps, L"&Frame rate", f == kFps120 ? L"120 FPS" : f == kFps60 ? L"60 FPS" : f == kFps30 ? L"30 FPS" : L"—",
                    {{kFps30, L"30 FPS"}, {kFps60, L"60 FPS"}, {kFps120, L"120 FPS"}}, f, kCardControls, ix, &cy, iw,
                    Enabled(kDdFps, s));
        // A dropdown hides capability, so the computed reason goes under the control (§10.3).
        const std::wstring reason = FpsReason(s);
        if (!reason.empty()) {
            cy += 4;
            const Size r = paint.Measure(Font::Small, reason, iw);
            Item& note = Add(kFpsReason, Kind::Text, L"", MakeBox(ix, cy, iw, r.h), kCardControls);
            note.e.name = note.label = reason;
            cy += r.h;
        }
        cy += kGap;
        // With no phone every entry above is greyed out, which on its own looks like a broken control.
        if (!Connected(s) || !c.valid) {
            const std::wstring note = L"Connect a phone to choose. Each phone reports which of these it can do.";
            const Size n = paint.Measure(Font::Small, note, iw);
            Item& it = Add(kCameraHint, Kind::Text, L"", MakeBox(ix, cy, iw, n.h), kCardControls);
            it.e.name = it.label = note;
            cy += n.h + kGap;
        }
        if (!known) {
            const std::wstring note = L"Zoom, brightness, focus and torch appear when an app uses the camera.";
            const Size n = paint.Measure(Font::Small, note, iw);
            Item& it = Add(kControlsNote, Kind::Text, L"", MakeBox(ix, cy, iw, n.h), kCardControls);
            it.e.name = it.label = note;
            cy += n.h;
        } else {
            wchar_t buf[32];
            swprintf_s(buf, L"%.1f×", c.zoomX100 / 100.0);
            AddSlider(kZoomSlider, L"&Zoom", buf, c.zoomX100 / 100.f, c.zoomMinX100 / 100.f, c.zoomMaxX100 / 100.f, 0.1f,
                      kCardControls, ix, &cy, iw, Enabled(kZoomSlider, s));
            cy += kGap - 2;
            const bool hasEv = c.evMax > c.evMin;
            const double ev = c.ev * c.evStepX100 / 100.0;
            if (hasEv) swprintf_s(buf, L"%s%.1f EV", ev > 0 ? L"+" : L"", ev);
            else wcscpy_s(buf, L"—");
            AddSlider(kEvValue, L"&Brightness", buf, float(c.ev), float(c.evMin), float(c.evMax), 1.f, kCardControls, ix, &cy,
                      iw, hasEv);
            cy += kGap - 4;
            AddToggle(kFocusAuto, L"A&uto focus", !(c.flags & proto::kCamFocusLocked), kCardControls, ix, &cy, iw,
                      Enabled(kFocusAuto, s));
            AddToggle(kTorch, L"&Torch", (c.flags & proto::kCamTorchOn) != 0, kCardControls, ix, &cy, iw, Enabled(kTorch, s));
            cy += 6;
            const Size reset = ButtonSize(L"Reset all", 90);
            AddButton(kAuto, L"Reset all", FlatStyle::Secondary, MakeBox(ix, cy, reset.w, reset.h), kCardControls,
                      Enabled(kAuto, s))
                .e.name = L"Reset zoom, brightness, focus and torch";
            cy += reset.h;
            if (c.width) {
                swprintf_s(buf, L"Now: %u×%u at %u fps", c.width, c.height, c.actualFps);
                cy += 8;
                Item& n = Add(kNowMode, Kind::Text, L"", MakeBox(ix, cy, iw, smallH), kCardControls);
                n.e.name = n.label = buf;
                cy += smallH;
            }
        }
        cy += kCardPad;
        const Box card = MakeBox(cx, *y, cw, cy - *y);
        cards[cardIndex].box = card;
        items[cardItem].e.rect = card;
        *y = std::max(py, card.b);
    }

    // --- Settings -----------------------------------------------------------------------------------

    void LayoutSettings(const LinkStatus& s, float x, float* y, float w) {
        (void)s;
        PageHeader(L"Settings", L"How MyCam behaves on this PC", x, y, w);
        const float ix = x + kCardPad, iw = w - 2 * kCardPad;
        {
            const size_t ci = cards.size(), ii = items.size();
            cards.push_back({kCardSettings, MakeBox(x, *y, w, 10)});
            Item& card = Add(kCardSettings, Kind::Card, L"", MakeBox(x, *y, w, 10), kNone);
            card.e.name = L"Picture and startup";
            float cy = *y + kCardPad - 2;
            AddToggle(kMirror, L"&Mirror the image", model->mirror(), kCardSettings, ix, &cy, iw);
            AddToggle(kFill, L"&Fill the frame (crop instead of black bars)", model->fill(), kCardSettings, ix, &cy, iw);
            AddToggle(kAutostart, L"Start MyCam with Windows", model->autostart(), kCardSettings, ix, &cy, iw);
            AddToggle(kWireless, L"Find phones on Wi-Fi (beta)", model->wireless(), kCardSettings, ix, &cy, iw);
            cy += kCardPad - 2;
            const Box box = MakeBox(x, *y, w, cy - *y);
            cards[ci].box = box;
            items[ii].e.rect = box;
            *y = box.b + kGap;
        }
        {
            const size_t ci = cards.size(), ii = items.size();
            cards.push_back({kCardAbout, MakeBox(x, *y, w, 10)});
            Item& card = Add(kCardAbout, Kind::Card, L"", MakeBox(x, *y, w, 10), kNone);
            card.e.name = L"Troubleshooting";
            float cy = *y + kCardPad;
            Deco_(MakeBox(ix, cy, iw, lineH), L"Troubleshooting", Font::BodyBold, false);
            cy += lineH + kGap;
            const Size a = ButtonSize(L"&Reconnect phone", 130), b = ButtonSize(L"Forget &Wi-Fi phones", 140),
                       c = ButtonSize(L"&Open log folder", 130);
            AddButton(kReconnect, L"&Reconnect phone", FlatStyle::Secondary, MakeBox(ix, cy, a.w, a.h), kCardAbout);
            AddButton(kForgetPhones, L"Forget &Wi-Fi phones", FlatStyle::Secondary, MakeBox(ix + a.w + 8, cy, b.w, b.h),
                      kCardAbout)
                .e.name = L"Forget Wi-Fi phones (they pair again with a code)";
            cy += a.h + 8;
            AddButton(kOpenLog, L"&Open log folder", FlatStyle::Secondary, MakeBox(ix, cy, c.w, c.h), kCardAbout);
            cy += c.h + kCardPad;
            const Box box = MakeBox(x, *y, w, cy - *y);
            cards[ci].box = box;
            items[ii].e.rect = box;
            *y = box.b;
        }
    }

    // --- About --------------------------------------------------------------------------------------

    void LayoutAbout(float x, float* y, float w) {
        PageHeader(L"About MyCam", L"Version, links and licences", x, y, w);
        const float ix = x + kCardPad, iw = w - 2 * kCardPad;
        const size_t ci = cards.size(), ii = items.size();
        cards.push_back({kCardAbout, MakeBox(x, *y, w, 10)});
        Item& card = Add(kCardAbout, Kind::Card, L"", MakeBox(x, *y, w, 10), kNone);
        card.e.name = L"About MyCam";
        float cy = *y + kCardPad;
        // The mark always comes from the generated icon (§11): never a second drawing of it.
        {
            Item& logo = Add(kAboutLogo, Kind::Image, L"", MakeBox(ix, cy, 48, 48), kCardAbout);
            logo.e.name = L"MyCam";
        }
        const float tx = ix + 48 + 14, tw = iw - 48 - 14;
        Deco_(MakeBox(tx, cy + 2, tw, paint.LineHeight(Font::Instruction)), L"MyCam", Font::Instruction, false);
        const std::wstring version = std::wstring(L"Version ") + kAppVersion;
        {
            Item& v = Add(kAboutVersion, Kind::Text, L"", MakeBox(tx, cy + paint.LineHeight(Font::Instruction) + 4, tw, lineH),
                          kCardAbout);
            v.e.name = v.label = version;
        }
        cy += 48 + kGap;
        const std::wstring body = L"MyCam turns an Android phone into a webcam for Windows, over USB or Wi-Fi.";
        const Size b = paint.Measure(Font::Body, body, iw);
        {
            Item& t = Add(kAboutText, Kind::Text, L"", MakeBox(ix, cy, iw, b.h), kCardAbout);
            t.e.name = t.label = body;
            cy += b.h + kGap;
        }
        const std::wstring licence =
            L"MyCam is open source under the MIT licence. Tahoma, Trebuchet MS and Segoe UI are Windows system fonts and "
            L"are never shipped with MyCam. Switching a phone into accessory mode uses UsbDk (GPL-3.0), which the "
            L"installer offers separately.";
        const Size l = paint.Measure(Font::Small, licence, iw);
        {
            Item& t = Add(kAboutLicence, Kind::Text, L"", MakeBox(ix, cy, iw, l.h), kCardAbout);
            t.e.name = t.label = licence;
            cy += l.h + kGap;
        }
        const Size link = paint.Measure(Font::Body, L"Open log folder");
        Add(kOpenLog, Kind::Link, L"&Open log folder", MakeBox(ix, cy, link.w, link.h), kCardAbout);
        cy += link.h + kCardPad;
        const Box box = MakeBox(x, *y, w, cy - *y);
        cards[ci].box = box;
        items[ii].e.rect = box;
        *y = box.b;
    }

    // --- Connect an iPhone (the web camera) ---------------------------------------------------------

    static std::wstring WideAscii(const std::string& s) { return std::wstring(s.begin(), s.end()); }

    std::wstring AddressLabel(const web::NetAddress& a) const {
        const wchar_t* kind = a.kind == web::NetKind::WiFi       ? L"Wi-Fi"
                            : a.kind == web::NetKind::Ethernet   ? L"Ethernet"
                            : a.kind == web::NetKind::IPhoneUsb ? L"iPhone over USB"
                                                                 : nullptr;
        std::wstring label = WideAscii(a.text);
        if (kind) label += std::wstring(L" · ") + kind;
        if (!a.adapter.empty() && (!kind || a.adapter != kind)) label += L" (" + a.adapter + L")";
        return label;
    }

    // The QR code's modules for the current address (it changes with each new one-time code).
    void UpdateQr(const std::wstring& url) {
        if (url == qrUrl) return;
        qrUrl = url;
        qrModules.clear();
        qrSize = 0;
        if (url.empty()) return;
        try {
            std::string text; // The address is ASCII.
            for (wchar_t c : url) text += char(c);
            const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
            qrSize = qr.getSize();
            qrModules.resize(size_t(qrSize) * size_t(qrSize));
            for (int yy = 0; yy < qrSize; ++yy)
                for (int xx = 0; xx < qrSize; ++xx)
                    qrModules[size_t(yy) * size_t(qrSize) + size_t(xx)] = qr.getModule(xx, yy) ? 1 : 0;
        } catch (...) {
            qrSize = 0;
        }
    }

    void LayoutIPhone(const LinkStatus& s, float x, float* y, float w) {
        PageHeader(L"Connect a phone", L"Scan this code: an iPhone or iPad with its Camera app, an Android phone with MyCam.", x, y, w);
        const float ix = x + kCardPad, iw = w - 2 * kCardPad;
        UpdateQr(s.webUrl);
        const web::NetAddress* current = nullptr;
        for (const web::NetAddress& a : s.webAddresses) if (a.ipv4 == s.webAddress) current = &a;
        {
            const size_t ci = cards.size(), ii = items.size();
            cards.push_back({kCardIPhone, MakeBox(x, *y, w, 10)});
            Item& card = Add(kCardIPhone, Kind::Card, L"", MakeBox(x, *y, w, 10), kNone);
            card.e.name = L"Scan this code with the phone";
            float cy = *y + kCardPad;
            if (!s.webReady || s.webAddresses.empty()) {
                const std::wstring why =
                    !s.webReady ? L"This isn’t available right now: MyCam couldn’t start its iPhone page on port 47900. "
                                  L"Restart MyCam; the log says more."
                                : L"This PC isn’t on a network a phone can reach. Connect it to Wi-Fi or Ethernet, "
                                  L"then come back here.";
                const Size t = paint.Measure(Font::Body, why, iw);
                Item& n = Add(kWebUnavailable, Kind::Text, L"", MakeBox(ix, cy, iw, t.h), kCardIPhone);
                n.e.name = n.label = why;
                n.e.liveRegion = true;
                cy += t.h;
            } else {
                const float side = std::floor(std::min(196.f, iw * 0.42f));
                {
                    Item& qr = Add(kWebQr, Kind::Image, L"", MakeBox(ix, cy, side, side), kCardIPhone);
                    qr.e.name = current ? L"QR code for https://" + WideAscii(current->text) + L":47900" : L"QR code";
                }
                const float tx = ix + side + 18, tw = iw - side - 18;
                float ty = cy;
                const std::wstring steps =
                    L"Android:  in MyCam, tap Scan QR Code and point it at this code. It connects by itself.\n\n"
                    L"iPhone:\n"
                    L"1.  Open the Camera app and point it at this code. Tap the link that appears.\n"
                    L"2.  The first time, Safari says the connection isn’t private. Tap Show Details, then “visit this "
                    L"website”.\n"
                    L"3.  Tap Start and allow the camera.\n\n"
                    L"Then choose “MyCam” as the camera in any app on this PC.";
                const Size st = paint.Measure(Font::Body, steps, tw);
                {
                    Item& t = Add(kWebSteps, Kind::Text, L"", MakeBox(tx, ty, tw, st.h), kCardIPhone);
                    t.e.name = t.label = steps;
                    ty += st.h + 10;
                }
                const std::wstring note = L"Each code works once, for 5 minutes; then a new one appears. The phone and "
                                          L"this PC must be on the same network.";
                const Size nt = paint.Measure(Font::Small, note, tw);
                {
                    Item& t = Add(kWebNote, Kind::Text, L"", MakeBox(tx, ty, tw, nt.h), kCardIPhone);
                    t.e.name = t.label = note;
                    ty += nt.h;
                }
                cy = std::max(cy + side, ty) + kGap;
                if (current) {
                    const std::wstring addr = L"Address: https://" + WideAscii(current->text) + L":47900";
                    const Size at = paint.Measure(Font::Body, addr, iw);
                    Item& a = Add(kWebAddress, Kind::Text, L"", MakeBox(ix, cy, iw, at.h), kCardIPhone);
                    a.e.name = a.label = addr;
                    cy += at.h;
                }
                // Several networks: let the user pick the one the iPhone is on.
                if (s.webAddresses.size() > 1) {
                    cy += kGap;
                    std::vector<std::pair<int, std::wstring>> entries;
                    int selected = kNone;
                    for (size_t i = 0; i < s.webAddresses.size() && i < size_t(kWebAddrMax); ++i) {
                        entries.push_back({kWebAddrBase + int(i), AddressLabel(s.webAddresses[i])});
                        if (s.webAddresses[i].ipv4 == s.webAddress) selected = kWebAddrBase + int(i);
                    }
                    AddDropdown(kDdWebAddress, L"&Network the iPhone is on", current ? AddressLabel(*current) : L"—",
                                entries, selected, kCardIPhone, ix, &cy, std::min(iw, 360.f), true);
                }
            }
            cy += kCardPad;
            const Box box = MakeBox(x, *y, w, cy - *y);
            cards[ci].box = box;
            items[ii].e.rect = box;
            *y = box.b + kGap + 4;
        }

        // The iPhones that can reconnect without scanning.
        const std::wstring heading = L"iPhones that can reconnect";
        const Size ht = paint.Measure(Font::BodyBold, heading, w);
        {
            Item& h = Add(kWebDevicesTitle, Kind::Text, L"", MakeBox(x, *y, w, ht.h), kNone);
            h.e.name = h.label = heading;
            *y += ht.h + 6;
        }
        const size_t n = std::min(s.webDevices.size(), size_t(kWebDevMax));
        if (n > 0) {
            const float rowH = std::max(48.f, lineH + smallH + 22);
            const Box card = BeginCard(kCardWebDevices, x, *y, w, rowH * float(n) + 2, heading);
            Add(kWebDevicesList, Kind::Group, heading, card, kCardWebDevices);
            const Size forget = ButtonSize(L"Forget", 84);
            for (size_t i = 0; i < n; ++i) {
                const LinkStatus::WebDeviceRow& d = s.webDevices[i];
                const float ry = card.t + 1 + rowH * float(i);
                wchar_t added[64] = L"";
                if (d.addedUnix) {
                    const __time64_t t = __time64_t(d.addedUnix);
                    tm local = {};
                    _localtime64_s(&local, &t);
                    wcsftime(added, std::size(added), L"Added %#d %b %Y", &local);
                }
                Item& row = Add(WebDeviceRowId(int(i), 0), Kind::Text, L"",
                                Box{card.l + kCardPad, ry, card.r - kCardPad - forget.w - 12, ry + rowH}, kWebDevicesList);
                row.label = d.name;
                row.sub = std::wstring(added) + (d.connected ? L" · Connected now" : L"");
                row.e.name = d.name + L", " + row.sub;
                row.drawGroup = kCardWebDevices;
                Item& b = AddButton(WebDeviceRowId(int(i), 1), L"Forget", FlatStyle::Secondary,
                                    MakeBox(card.r - kCardPad - forget.w, ry + (rowH - forget.h) / 2, forget.w, forget.h),
                                    kWebDevicesList);
                b.e.name = L"Forget " + d.name + L" (it scans the code again next time)";
                b.drawGroup = kCardWebDevices;
            }
            *y = card.b + 6;
        }
        const std::wstring note = n == 0 ? L"None yet. An iPhone is remembered after it scans the code."
                                         : L"They connect again when you open the MyCam page on them (or its Home Screen "
                                           L"icon) and tap Start. Forget one to make it scan the code again.";
        const Size nt = paint.Measure(Font::Small, note, w);
        Item& nn = Add(kWebDevicesNote, Kind::Text, L"", MakeBox(x + 2, *y, w, nt.h), kNone);
        nn.e.name = nn.label = note;
        *y += nt.h;
    }

    void DrawQr(const Box& r) {
        const xp::Palette& pal = paint.Colors();
        paint.RoundRect(r, 6, Rgb(0xFFFFFF));
        paint.RoundFrame(r, 6, pal.cardBorder);
        if (qrSize <= 0) {
            paint.Text(Font::Small, L"Making a code…", r, pal.subtle, Align::Center, -1, false, true);
            return;
        }
        // Four modules of white around the code: the quiet zone scanners need.
        const float module = std::floor(r.W() / float(qrSize + 8) * 4.f) / 4.f;
        const float size = module * float(qrSize);
        const float ox = std::round(r.l + (r.W() - size) / 2), oy = std::round(r.t + (r.H() - size) / 2);
        ID2D1DeviceContext* dc = paint.Dc();
        const D2D1_ANTIALIAS_MODE was = dc->GetAntialiasMode();
        dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        ID2D1SolidColorBrush* ink = paint.Brush(Rgb(0x000000));
        for (int yy = 0; yy < qrSize; ++yy) {
            for (int xx = 0; xx < qrSize;) { // One rectangle per run of dark modules.
                if (!qrModules[size_t(yy) * size_t(qrSize) + size_t(xx)]) { ++xx; continue; }
                int end = xx;
                while (end < qrSize && qrModules[size_t(yy) * size_t(qrSize) + size_t(end)]) ++end;
                dc->FillRectangle(D2D1::RectF(ox + module * float(xx), oy + module * float(yy), ox + module * float(end),
                                              oy + module * float(yy + 1)),
                                  ink);
                xx = end;
            }
        }
        dc->SetAntialiasMode(was);
    }

    // --- Pairing (a page while the window is open; the dialog covers the window being closed) --------

    void LayoutPairing(const LinkStatus& s, uint64_t now, float x, float* y, float w) {
        PageHeader(L"Pair with your phone", L"", x, y, w);
        const size_t ci = cards.size(), ii = items.size();
        cards.push_back({kCardPairing, MakeBox(x, *y, w, 10)});
        Item& card = Add(kCardPairing, Kind::Card, L"", MakeBox(x, *y, w, 10), kNone);
        card.e.name = L"Pairing";
        const float iw = w - 2 * kCardPad, ix = x + kCardPad;
        float cy = *y + kCardPad + 6;
        {
            Item& art = Add(kPairArt, Kind::Image, L"", MakeBox(x + (w - 48) / 2, cy, 48, 48), kCardPairing);
            art.e.name = L"MyCam";
            cy += 48 + kGap;
        }
        const std::wstring title = s.phoneName.empty() ? L"A pairing request has been sent to your phone."
                                                       : L"A pairing request has been sent to " + s.phoneName + L".";
        const Size t = paint.Measure(Font::Instruction, title, iw);
        {
            Item& h = Add(kPairTitle, Kind::Text, L"", MakeBox(ix, cy, iw, t.h), kCardPairing);
            h.e.name = h.label = title;
            h.e.liveRegion = true;
            cy += t.h + 4;
        }
        const std::wstring hint = s.pairingAllowed ? L"Allowed on this PC. Waiting for Allow on the phone…"
                                                   : L"Allow only if the phone shows the same code.";
        const Size hs = paint.Measure(Font::Body, hint, iw);
        {
            Item& h = Add(kPairHint, Kind::Text, L"", MakeBox(ix, cy, iw, hs.h), kCardPairing);
            h.e.name = h.label = hint;
            cy += hs.h + kGap + 2;
        }
        // The code in a large bordered box.
        const std::wstring code = s.pairingCode.empty() ? L"— — —" : s.pairingCode;
        const Size cs = paint.Measure(Font::Code, code);
        const float boxW = std::min(iw, cs.w + 56), boxH = cs.h + 22;
        {
            Item& b = Add(kPairCode, Kind::Text, L"", MakeBox(x + (w - boxW) / 2, cy, boxW, boxH), kCardPairing);
            b.e.name = L"Pairing code " + code;
            b.label = code;
            cy += boxH + kGap + 2;
        }
        // The countdown stays, as the green XP chunk bar (owner's decision, §4 Progress).
        {
            const uint64_t elapsed = pairingSince ? now - pairingSince : 0;
            const uint64_t left = elapsed >= kPairingAnswerMs ? 0 : kPairingAnswerMs - elapsed;
            wchar_t text[8];
            FormatCountdown(left, text, 8);
            const float tw = paint.Measure(Font::Body, L"0:00").w + 8;
            const float barW = std::min(iw, 260.f);
            Item& bar = Add(kPairProgress, Kind::ProgressBar, L"", MakeBox(x + (w - barW - tw) / 2, cy, barW, 13),
                            kCardPairing);
            bar.e.value = float(left) / float(kPairingAnswerMs);
            bar.label = text;
            bar.e.name = std::wstring(L"Time left to pair: ") + text;
            cy += 13 + kGap + 4;
        }
        {
            // Both users must allow a code pairing (PROTOCOL.md "The PC asks too"). Once allowed here, only
            // "Don't allow" stays, until the phone answers.
            const Size deny = ButtonSize(L"Don’t allow", 110);
            const Size allow = ButtonSize(L"Allow", 110);
            const float gap = 12, total = s.pairingAllowed ? deny.w : allow.w + gap + deny.w;
            float bx = x + (w - total) / 2;
            if (!s.pairingAllowed) {
                AddButton(kPairAllow, L"Allow", FlatStyle::Primary, MakeBox(bx, cy, allow.w, allow.h), kCardPairing)
                    .e.name = L"Allow pairing";
                bx += allow.w + gap;
            }
            AddButton(kPairCancel, L"Don’t allow", FlatStyle::Secondary, MakeBox(bx, cy, deny.w, deny.h), kCardPairing)
                .e.name = L"Don’t allow pairing";
            cy += deny.h + kCardPad;
        }
        const Box box = MakeBox(x, *y, w, cy - *y);
        cards[ci].box = box;
        items[ii].e.rect = box;
        *y = box.b;
    }

    // --- Scanning -----------------------------------------------------------------------------------

    // A scan is running, or was just asked for and the link hasn't picked it up yet.
    bool Scanning(const LinkStatus& s, uint64_t now) const {
        return s.scanning || (scanPressedAt && now - scanPressedAt < 1500 && s.scansDone == scansAtPress);
    }

    void StartScan() {
        if (!model->scanForPhones) return;
        const LinkStatus s = Status();
        scanPressedAt = Now();
        scansAtPress = s.scansDone;
        model->scanForPhones();
    }

    std::vector<Element> Elements() const {
        std::vector<Element> out;
        out.reserve(items.size());
        for (const auto& it : items) out.push_back(it.e);
        return out;
    }

    const Item* FindItem(int id) const {
        for (const auto& it : items) if (it.e.id == id) return &it;
        return nullptr;
    }

    float MaxScroll() const { return std::max(0.f, contentH - contentView.H()); }

    Box ThumbTrack() const { return {content.r - 9, content.t + 3, content.r - 3, content.b - 3}; }
    Box Thumb() const {
        const Box t = ThumbTrack();
        const float h = std::max(28.f, t.H() * contentView.H() / std::max(contentH, 1.f));
        const float max = MaxScroll();
        const float top = t.t + (max > 0 ? (t.H() - h) * scroll / max : 0);
        return {t.l, top, t.r, top + h};
    }

    void SetScroll(float value) {
        const float clamped = std::clamp(value, 0.f, MaxScroll());
        if (clamped == scroll) return;
        scroll = clamped;
        Invalidate();
    }

    void ScrollIntoView(int id) {
        const Item* it = FindItem(id);
        if (!it || it->e.rect.l < content.l) return;
        const Box& r = it->e.rect;
        if (r.t < contentView.t + 4) SetScroll(scroll - (contentView.t + 4 - r.t) - 8);
        else if (r.b > contentView.b - 4) SetScroll(scroll + (r.b - (contentView.b - 4)) + 8);
        Layout();
    }

    int HitTest(float x, float y) const {
        // An open dropdown's list is on top of everything else.
        if (openDrop != kNone) {
            for (const auto& it : items) {
                if (it.e.kind != Kind::DropItem || it.e.parent != openDrop || !it.e.visible || !it.e.enabled) continue;
                if (it.e.rect.Contains(x, y)) return it.e.id;
            }
        }
        for (const auto& it : items) {
            const Element& e = it.e;
            const bool clickable = e.kind == Kind::Button || e.kind == Kind::Checkbox || e.kind == Kind::Radio ||
                                   e.kind == Kind::Link || e.kind == Kind::CaptionButton || e.kind == Kind::Nav ||
                                   e.kind == Kind::Dropdown || e.kind == Kind::Toggle || e.kind == Kind::Slider;
            if (!clickable || !e.visible || !e.enabled) continue;
            if (e.rect.Contains(x, y) && e.clip.Contains(x, y)) return e.id;
        }
        return kNone;
    }

    // --- Behaviour -------------------------------------------------------------------------------

    void SetSliderFromX(int id, float x) {
        const Item* it = FindItem(id);
        if (!it || !it->e.enabled) return;
        const Box rail = it->extra;
        const float f = rail.W() > 0 ? (x - rail.l) / rail.W() : 0.f;
        const float v = SliderValueAt(it->e, f);
        const LinkStatus s = Status();
        if (id == kZoomSlider) {
            model->command(proto::kCmdSetZoom, uint8_t(std::clamp(int(v * 10 + 0.5f), 1, 255)));
        } else if (id == kEvValue) {
            const int step = int(std::lround(v));
            model->command(proto::kCmdSetExposure, uint8_t(std::clamp(step, int(s.camera.evMin), int(s.camera.evMax))));
        }
        Invalidate();
    }

    bool Activate(int id) {
        const LinkStatus s = Status();
        const Item* it = FindItem(id);
        if (it && (!it->e.enabled || !it->e.visible)) return false;
        // Picking an entry closes the list; the action below is the same one the radio used to do.
        if (it && it->e.kind == Kind::DropItem) CloseDropdown();
        switch (id) {
        case kNavHome: case kNavDevices: case kNavCamera: case kNavSettings: case kNavAbout:
            GoTo(PageOfNav(id));
            return true;
        case kNavDevicesLink:
            GoTo(Page::Devices);
            return true;
        case kConnectIPhone:
            GoTo(Page::IPhone);
            return true;
        case kDdCamera: case kDdQuality: case kDdFps: case kDdWebAddress:
            if (openDrop == id) CloseDropdown();
            else {
                openDrop = id;
                dropHot = kNone;
                dropAnim.Start(1, Now(), Dur(kShortMs), 0);
            }
            break;
        case kPause:
            if (!PauseEnabled(s)) return false;
            model->setPaused(!model->paused());
            break;
        case kBack: model->setFacing(proto::kFacingBack); StartApplying(3, proto::kFacingBack, s); break;
        case kFront: model->setFacing(proto::kFacingFront); StartApplying(3, proto::kFacingFront, s); break;
        case kMirror: model->setMirror(!model->mirror()); break;
        case kFill: model->setFill(!model->fill()); break;
        case kQ720: model->command(proto::kCmdSetQuality, proto::kQuality720p); StartApplying(1, proto::kQuality720p, s); break;
        case kQ1080: model->command(proto::kCmdSetQuality, proto::kQuality1080p); StartApplying(1, proto::kQuality1080p, s); break;
        case kQ4K: model->command(proto::kCmdSetQuality, proto::kQuality4K); StartApplying(1, proto::kQuality4K, s); break;
        case kFps30: model->command(proto::kCmdSetFps, 30); StartApplying(2, 30, s); break;
        case kFps60: model->command(proto::kCmdSetFps, 60); StartApplying(2, 60, s); break;
        case kFps120: model->command(proto::kCmdSetFps, 120); StartApplying(2, 120, s); break;
        case kFocusAuto: // Now a switch: on means auto focus.
            model->command(proto::kCmdSetFocus, (s.camera.flags & proto::kCamFocusLocked) ? 0 : 1);
            break;
        case kTorch: model->command(proto::kCmdSetTorch, (s.camera.flags & proto::kCamTorchOn) ? 0 : 1); break;
        case kAuto:
            model->command(proto::kCmdSetZoom, 10);
            model->command(proto::kCmdSetExposure, 0);
            model->command(proto::kCmdSetFocus, 0);
            model->command(proto::kCmdSetTorch, 0);
            break;
        case kAutostart: model->setAutostart(!model->autostart()); break;
        case kWireless: model->setWireless(!model->wireless()); break;
        case kReconnect: model->reconnect(); break;
        case kOpenLog: model->openLogFolder(); break;
        case kForgetPhones: if (model->forgetPhones) model->forgetPhones(); break; // They pair again (with a code).
        case kShowPairing: GoTo(Page::Pairing); break;
        // "Scan for Devices" shows its results, so it goes to Devices (Refresh on that page stays put).
        case kScan: StartScan(); GoTo(Page::Devices); break;
        case kScanNow: StartScan(); GoTo(Page::Devices); break;
        case kScanUsb: // No passive wait: drop the session and look at the bus again (§2.2).
            usbScanAt = Now();
            model->reconnect();
            break;
        case kPairAllow:
            if (model->allowPairing) model->allowPairing();
            break;
        case kPairCancel:
            if (model->cancelPairing) model->cancelPairing();
            break;
        case kCaptionClose: DestroyWindow(hwnd); return true;
        case kCaptionMin: ShowWindow(hwnd, SW_MINIMIZE); return true;
        default: {
            int part = 0;
            if (id >= kWebAddrBase && id < kWebAddrBase + kWebAddrMax) { // The network the QR code points at.
                const size_t i = size_t(id - kWebAddrBase);
                if (i < s.webAddresses.size() && model->startWebPairing) {
                    webIp = s.webAddresses[i].ipv4;
                    model->startWebPairing(webIp);
                }
                break;
            }
            const int webRow = WebDeviceRowIndex(id, &part);
            if (webRow >= 0 && part == 1 && webRow < int(s.webDevices.size()) && model->forgetWebDevice) {
                model->forgetWebDevice(s.webDevices[size_t(webRow)].id);
                break;
            }
            const int row = ScanRowIndex(id, &part);
            if (row >= 0 && part == 2 && row < int(deviceRows.size()) && model->forgetWebDevice &&
                !deviceRows[size_t(row)].webId.empty()) {
                model->forgetWebDevice(deviceRows[size_t(row)].webId);
                break;
            }
            if (row >= 0 && part == 1 && row < int(deviceRows.size()) && model->connectTo) {
                model->connectTo(deviceRows[size_t(row)].ipv4); // One Wi-Fi session (pairing as usual if new).
                break;
            }
            return false;
        }
        }
        Invalidate();
        return true;
    }

    void SetFocusTo(int id, bool cues) {
        focus = id;
        if (cues) keyboardCues = true;
        ScrollIntoView(id);
        Invalidate();
    }

    void Invalidate() {
        if (hwnd) InvalidateRect(hwnd, nullptr, FALSE);
    }

    // --- Timers (only while something moves: no idle CPU) -------------------------------------------

    bool MarqueeRunning() const {
        if (reducedMotion || paint.HighContrast()) return false;
        for (int id : {kStatusProgress, kScanProgress}) {
            const Item* bar = FindItem(id);
            if (bar && bar->e.value < 0 && bar->e.visible) return true;
        }
        return false;
    }

    bool PairingCountdown() const {
        const Item* bar = FindItem(kPairProgress);
        return bar && bar->e.value >= 0;
    }

    bool Animating(uint64_t now) const {
        bool any = statusFade.Running(now) || badgePop.Running(now) || liveGlow.Running(now) || previewFade.Running(now) ||
                   pageAnim.Running(now) || navPill.Running(now) || dropAnim.Running(now) || pressFade.Running(now);
        for (const auto& [id, t] : valueAnim) any = any || t.Running(now);
        if (usbScanAt && now - usbScanAt < kUsbScanMs + 100) any = true;
        return any;
    }

    void ScheduleTimers() {
        if (!hwnd) return;
        const bool visible = IsWindowVisible(hwnd) && !IsIconic(hwnd);
        const uint64_t now = Now();
        // While something is animating, repaint straight away rather than on a timer: Present already
        // waited for the vertical blank, so this runs at exactly the refresh rate. A 16 ms WM_TIMER beats
        // against the 16.67 ms blank and drops or doubles frames, which is what makes a slide look rough.
        if (visible && Animating(now) && !surface.Occluded()) {
            KillTimer(hwnd, kTimerAnim);
            Invalidate();
        }
        else if (visible && Animating(now)) SetTimer(hwnd, kTimerAnim, 16, nullptr);
        else if (visible && MarqueeRunning()) SetTimer(hwnd, kTimerAnim, 50, nullptr);
        else if (visible && PairingCountdown()) SetTimer(hwnd, kTimerAnim, 250, nullptr);
        else KillTimer(hwnd, kTimerAnim);
        // The preview polls the shared frame at <= 15 fps while frames are fresh, and checks twice a second
        // for an app starting the camera otherwise. Only the Camera page shows it.
        if (visible && page == Page::Camera) SetTimer(hwnd, kTimerPreview, previewFresh ? 66 : 500, nullptr);
        else {
            KillTimer(hwnd, kTimerPreview);
            if (!visible) ClosePreview();
        }
    }

    void ClosePreview() {
        if (!previewOpen && !previewFresh) return;
        preview.Close();
        previewOpen = false;
        previewFresh = false;
        previewFade.Set(0);
    }

    void PollPreview() {
        if (!pw || !ph) return;
        previewOpen = true;
        const PreviewSource::Result r = preview.Poll(pw, ph, previewPixels);
        const uint64_t now = Now();
        if (r == PreviewSource::Result::NewFrame) {
            previewPending = true;
            if (!previewFresh) {
                previewFresh = true;
                previewFade.Start(1, now, Dur(300), previewFade.Value(now));
            }
            Invalidate();
        } else if (r == PreviewSource::Result::NoFrame && previewFresh) {
            previewFresh = false;
            previewFade.Start(0, now, Dur(300), previewFade.Value(now));
            Invalidate();
        }
    }

    // --- Painting --------------------------------------------------------------------------------

    void EnsureBitmaps(ID2D1DeviceContext* dc) {
        if (iconGen != surface.Generation() || iconScale != scale) {
            appIcon16 = xp::LoadIconBitmap(dc, kIconApp, int(16 * scale + 0.5f));
            appIcon48 = xp::LoadIconBitmap(dc, kIconApp, int(48 * scale + 0.5f));
            statusIcon.Reset();
            statusIconId = 0;
            iconGen = surface.Generation();
            iconScale = scale;
            liveBitmap.Reset();
            artBitmap.Reset();
            artKind = -1;
            previewPending = !previewPixels.empty(); // Upload the last frame again (it may not change soon).
        }
    }

    ComPtr<ID2D1Bitmap>& IconFor(ID2D1DeviceContext* dc, int icon) {
        // The window draws the plain camera (or the grey one) and pops the state badge on top itself.
        const int base = icon == kIconDisconnected ? kIconDisconnected : kIconApp;
        if (statusIconId != base) {
            statusIcon = xp::LoadIconBitmap(dc, base, int(32 * scale + 0.5f));
            statusIconId = base;
        }
        return statusIcon;
    }

    // 0 paused, 1 waiting (no phone), 2 ready (connected, no app using the camera).
    int WantedArt(const LinkStatus& s) const {
        if (s.lockPaused || s.state == LinkState::Paused) return 0;
        if (s.state == LinkState::Idle || s.state == LinkState::Streaming) return 2;
        return 1;
    }

    void UploadPreview(ID2D1DeviceContext* dc) {
        const LinkStatus s = Status();
        const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.f * scale, 96.f * scale);
        if (previewPending && previewPixels.size() == size_t(pw) * ph * 4) {
            D2D1_SIZE_U have = liveBitmap ? liveBitmap->GetPixelSize() : D2D1::SizeU(0, 0);
            if (!liveBitmap || have.width != pw || have.height != ph) {
                liveBitmap.Reset();
                dc->CreateBitmap(D2D1::SizeU(pw, ph), previewPixels.data(), pw * 4, &props, &liveBitmap);
            } else {
                liveBitmap->CopyFromMemory(nullptr, previewPixels.data(), pw * 4);
            }
            previewPending = false;
        }
        const int want = WantedArt(s);
        if (want != artKind || artW != pw || artH != ph || !artBitmap) {
            if (!imagesLoaded) {
                pausedImage = LoadStatusImage(kImagePaused);
                waitingImage = LoadStatusImage(kImageWaiting);
                imagesLoaded = true;
            }
            std::vector<uint8_t> pixels;
            PreviewSource::Render(want == 0 ? pausedImage : waitingImage, pw, ph, pixels);
            artBitmap.Reset();
            if (pixels.size() == size_t(pw) * ph * 4) dc->CreateBitmap(D2D1::SizeU(pw, ph), pixels.data(), pw * 4, &props, &artBitmap);
            artKind = want;
            artW = pw;
            artH = ph;
        }
        if (effectGen != surface.Generation()) {
            artBlur.Reset();
            dc->CreateEffect(CLSID_D2D1GaussianBlur, &artBlur);
            if (artBlur) {
                artBlur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
                artBlur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED);
            }
            effectGen = surface.Generation();
        }
    }

    ID2D1Factory1* surfaceFactory() { return surface.Factory(); }

    void DrawPreview(ID2D1DeviceContext* dc, const Box& p, const LinkStatus& s, uint64_t now, bool chips) {
        const D2D1_RECT_F r = D2D1::RectF(p.l, p.t, p.r, p.b);
        ComPtr<ID2D1RoundedRectangleGeometry> shape;
        if (surfaceFactory()) surfaceFactory()->CreateRoundedRectangleGeometry({r, 8, 8}, &shape);
        if (shape) dc->PushLayer(D2D1::LayerParameters1(r, shape.Get()), nullptr);
        dc->FillRectangle(r, paint.Brush(Rgb(0x0B1016)));
        const float live = previewFade.Value(now, EaseOut);
        if (artBitmap && live < 1) {
            if (artKind == 2 && artBlur && !paint.HighContrast()) { // Connected, no app: the hills, softly blurred.
                artBlur->SetInput(0, artBitmap.Get());
                artBlur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, 6.f);
                const D2D1_POINT_2F at = D2D1::Point2F(p.l, p.t);
                dc->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
                dc->DrawImage(artBlur.Get(), &at);
                dc->PopAxisAlignedClip();
            } else {
                dc->DrawBitmap(artBitmap.Get(), r, 1.f, D2D1_INTERPOLATION_MODE_LINEAR, nullptr, nullptr);
            }
        }
        // The preview starts as a crossfade, never a pop (§12.3).
        if (liveBitmap && live > 0) dc->DrawBitmap(liveBitmap.Get(), r, live, D2D1_INTERPOLATION_MODE_LINEAR, nullptr, nullptr);

        const bool liveVideo = previewFresh && ((s.state == LinkState::Streaming && !s.lockPaused) || model->testPattern);
        if (chips && !liveVideo && artKind == 2) {
            const std::wstring text = L"The preview starts when an app uses the camera.";
            const Size t = paint.Measure(Font::BodyBold, text, p.W() - 40);
            const float h = t.h + 12;
            paint.DarkPill(MakeBox(p.l + (p.W() - t.w - 28) / 2, p.t + (p.H() - h) / 2, t.w + 28, h), text);
        }
        if (shape) dc->PopLayer();
        if (paint.HighContrast()) paint.RoundFrame(p, 8, xp::SysColor(COLOR_WINDOWTEXT));
    }

    ButtonState StateOf(const Item& it, uint64_t now) const {
        ButtonState st;
        st.disabled = !it.e.enabled;
        st.hot = hot == it.e.id && !st.disabled && (pressed == kNone || pressed == it.e.id);
        st.pressed = pressed == it.e.id && hot == it.e.id;
        if (!st.pressed && released == it.e.id) st.press = pressFade.Value(now, EaseOut);
        st.focused = focus == it.e.id;
        return st;
    }

    bool Cues() const { return keyboardCues || xp::KeyboardCuesAlways(); }

    void DrawItem(ID2D1DeviceContext* dc, const Item& it, const LinkStatus& s, uint64_t now) {
        const Element& e = it.e;
        const xp::Palette& pal = paint.Colors();
        const ButtonState st = StateOf(it, now);
        const int underline = Cues() ? e.accessKeyIndex : -1;
        switch (e.kind) {
        case Kind::CaptionButton:
            paint.CaptionButton(e.rect, e.id == kCaptionClose, st);
            break;
        case Kind::Nav: {
            const bool sel = e.selected;
            const D2D1_COLOR_F ink = sel ? pal.accentText : st.hot ? pal.text : pal.subtle;
            if (!sel && (st.hot || st.pressed)) paint.RoundRect(e.rect, 7, Rgb(0xEDF2F9));
            const float gs = std::min(18.f, e.rect.H() - 10);
            if (it.hasGlyph) paint.Icon(it.glyph, MakeBox(e.rect.l + 10, (e.rect.t + e.rect.b - gs) / 2, gs, gs), ink);
            paint.Text(sel ? Font::BodyBold : Font::Body, it.label,
                       Box{e.rect.l + 10 + gs + 10, e.rect.t, e.rect.r - 6, e.rect.b}, ink, Align::Leading, underline, true,
                       true);
            break;
        }
        case Kind::Button: {
            const float gs = 14;
            Box text = e.rect;
            if (it.hasGlyph) text = Box{e.rect.l + gs, e.rect.t, e.rect.r, e.rect.b};
            paint.FlatButton(e.rect, L"", -1, st, it.style, Font::BodyBold);
            const xp::Palette& p2 = paint.Colors();
            const D2D1_COLOR_F ink = paint.HighContrast() ? (st.hot ? xp::SysColor(COLOR_HIGHLIGHTTEXT) : xp::SysColor(COLOR_BTNTEXT))
                                     : st.disabled        ? (it.style == FlatStyle::Primary ? Rgb(0xF2F6FD) : p2.disabledText)
                                     : it.style == FlatStyle::Primary ? p2.accentText
                                                                      : p2.text;
            if (it.hasGlyph) {
                const Size t = paint.Measure(Font::BodyBold, it.label);
                const float total = gs + 6 + t.w;
                const float gx = (e.rect.l + e.rect.r - total) / 2;
                paint.Icon(it.glyph, MakeBox(gx, (e.rect.t + e.rect.b - gs) / 2, gs, gs), ink, 1.6f);
                paint.Text(Font::BodyBold, it.label, Box{gx + gs + 6, e.rect.t, e.rect.r, e.rect.b}, ink, Align::Leading,
                           underline, true, true);
            } else {
                paint.Text(Font::BodyBold, it.label, text, ink, Align::Center, underline, true, true);
            }
            break;
        }
        case Kind::Dropdown:
            paint.Combo(e.rect, it.label, st, openDrop == e.id);
            break;
        case Kind::Toggle: {
            const float on = AnimValue(e.id, e.checked ? 1.f : 0.f);
            paint.Switch(it.extra, on, st);
            paint.Text(Font::Body, it.label, Box{e.rect.l, e.rect.t, it.extra.l - 10, e.rect.b},
                       e.enabled ? pal.text : pal.disabledText, Align::Leading, underline, true, true);
            break;
        }
        case Kind::Slider:
            paint.SliderRail(it.extra, AnimValue(e.id, SliderFraction(e)),
                             [&] {
                                 ButtonState s2 = st;
                                 s2.pressed = sliderDrag == e.id;
                                 return s2;
                             }());
            break;
        case Kind::Checkbox: {
            const Box box = MakeBox(e.rect.l, e.rect.t + std::max(0.f, (std::min(e.rect.H(), lineH + 2) - 13) / 2), 13, 13);
            paint.Checkbox(box, e.checked, st);
            paint.Text(Font::Body, it.label, Box{e.rect.l + 18, e.rect.t, e.rect.r + 2, e.rect.b},
                       e.enabled ? pal.text : pal.disabledText, Align::Leading, underline);
            break;
        }
        case Kind::Radio: {
            const Box box = MakeBox(e.rect.l, e.rect.t + std::max(0.f, (std::min(e.rect.H(), lineH + 2) - 13) / 2), 13, 13);
            paint.Radio(box, e.selected, st);
            paint.Text(Font::Body, it.label, Box{e.rect.l + 18, e.rect.t, e.rect.r + 2, e.rect.b},
                       e.enabled ? pal.text : pal.disabledText, Align::Leading, underline);
            break;
        }
        case Kind::Link: {
            const bool isHot = hot == e.id;
            const D2D1_COLOR_F ink = isHot ? pal.linkHot : pal.link;
            paint.Text(Font::Body, it.label, e.rect, ink, Align::Leading, underline);
            if (isHot) {
                const float w = paint.Measure(Font::Body, it.label).w;
                paint.Line(e.rect.l, e.rect.t + lineH - 0.5f, e.rect.l + w, e.rect.t + lineH - 0.5f, ink);
            }
            break;
        }
        case Kind::ProgressBar:
            if (e.value >= 0) {
                paint.ProgressChunks(e.rect, DeterminateChunks(e.value, e.rect.W() - 6, xp::Painter::kChunk, xp::Painter::kChunkGap));
                paint.Text(Font::Body, it.label, Box{e.rect.r + 8, e.rect.t - 2, e.rect.r + 64, e.rect.b + 2}, pal.subtle,
                           Align::Leading, -1, false, true);
            } else if (reducedMotion || paint.HighContrast()) {
                if (e.id == kScanProgress) break; // The sentence under it already says "Looking for phones…".
                paint.Text(Font::Body, L"Working…", e.rect.Inset(0, -2), pal.subtle, Align::Leading, -1, false, true);
            } else {
                paint.ProgressMarquee(e.rect, MarqueeOffset(now, e.rect.W() - 6, xp::Painter::kChunk, xp::Painter::kChunkGap));
            }
            break;
        case Kind::Image:
            if (e.id == kPreview) DrawPreview(dc, e.rect, s, now, true);
            else if (e.id == kWebQr) DrawQr(e.rect);
            else if ((e.id == kAboutLogo || e.id == kPairArt) && appIcon48)
                dc->DrawBitmap(appIcon48.Get(), D2D1::RectF(e.rect.l, e.rect.t, e.rect.r, e.rect.b), 1.f,
                               D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            break;
        case Kind::Text:
            DrawTextItem(dc, it, s, now);
            break;
        default:
            break;
        }
    }

    void DrawTextItem(ID2D1DeviceContext* dc, const Item& it, const LinkStatus& s, uint64_t now) {
        const Element& e = it.e;
        const xp::Palette& pal = paint.Colors();
        (void)s;
        if (it.hasGlyph) { // A card's flat line icon.
            paint.Icon(it.glyph, e.rect, paint.HighContrast() ? pal.text : pal.accent, 1.6f);
            return;
        }
        switch (e.id) {
        case kStatusHeadline: case kStatusDetail: {
            // Crossfade from the previous status (200 ms).
            const float t = statusFade.Value(now, EaseOut);
            const Font font = e.id == kStatusHeadline ? Font::Instruction : Font::Small;
            D2D1_COLOR_F ink = e.id == kStatusHeadline ? pal.text : pal.subtle;
            if (t < 1) {
                const std::wstring& old = e.id == kStatusHeadline ? prevView.headline : prevView.facts;
                D2D1_COLOR_F faded = ink;
                faded.a = 1 - t;
                paint.Text(font, old, Box{e.rect.l, e.rect.t, e.rect.r, e.rect.t + 200}, faded);
                ink.a = t;
            }
            paint.Text(font, it.label, Box{e.rect.l, e.rect.t, e.rect.r, e.rect.t + 200}, ink);
            break;
        }
        case kPageTitle:
            paint.Text(Font::Instruction, it.label, e.rect, pal.text);
            break;
        case kPairTitle:
            paint.Text(Font::Instruction, it.label, e.rect, pal.text, Align::Center);
            break;
        case kPairHint:
            paint.Text(Font::Body, it.label, e.rect, pal.subtle, Align::Center);
            break;
        case kPairCode: {
            paint.RoundRect(e.rect, 8, paint.HighContrast() ? pal.card : Rgb(0xF7F9FC));
            paint.RoundFrame(e.rect, 8, paint.HighContrast() ? pal.text : pal.ctlBorder);
            paint.Text(Font::Code, it.label, e.rect, pal.text, Align::Center, -1, false, true);
            break;
        }
        case kAboutVersion: case kAboutText:
            paint.Text(e.id == kAboutText ? Font::Body : Font::Body, it.label, e.rect, pal.subtle);
            break;
        case kPageSubtitle: case kAboutLicence: case kFpsReason: case kScanResult: case kUsbNote: case kControlsNote:
        case kNowMode: case kStatusSentence: case kConnectedNote: case kAvailableNote: case kCameraHint:
        case kWebNote: case kWebDevicesNote:
            paint.Text(Font::Small, it.label, e.rect, pal.subtle);
            break;
        case kConnectedTitle: case kAvailableTitle: case kWebDevicesTitle:
            paint.Text(Font::BodyBold, it.label, e.rect, pal.text);
            break;
        default: {
            int part = 0;
            const int row = ScanRowIndex(e.id, &part);
            if (row >= 0 && part == 0 && row < int(deviceRows.size())) {
                DrawDeviceRow(deviceRows[size_t(row)], row);
                break;
            }
            if (WebDeviceRowIndex(e.id, &part) >= 0 && part == 0) { // A remembered iPhone: its name, then when added.
                const float lh = paint.LineHeight(Font::BodyBold), sh = paint.LineHeight(Font::Small);
                const float top = e.rect.t + (e.rect.H() - lh - sh - 2) / 2;
                paint.Text(Font::BodyBold, it.label, Box{e.rect.l, top, e.rect.r, top + lh}, pal.text, Align::Leading, -1, true);
                paint.Text(Font::Small, it.sub, Box{e.rect.l, top + lh + 2, e.rect.r, top + lh + 2 + sh}, pal.subtle,
                           Align::Leading, -1, true);
                break;
            }
            paint.Text(Font::Body, it.label, e.rect, pal.text);
            break;
        }
        }
        (void)dc;
    }

    void DrawDeviceRow(const DeviceRow& r, int index) {
        (void)index;
        const xp::Palette& pal = paint.Colors();
        if (!r.firstInCard) { // A hairline between the rows of the same card, never above its first row.
            const Box card = CardBox(r.card);
            paint.Line(card.l + kCardPad, r.glyphBox.t - (r.glyphBox.H() / 2) - 8, card.r - kCardPad,
                       r.glyphBox.t - (r.glyphBox.H() / 2) - 8, pal.cardBorder);
        }
        paint.Icon(Glyph::Phone, r.glyphBox, paint.HighContrast() ? pal.text : pal.subtle, 1.5f);
        paint.Text(Font::BodyBold, r.name, r.nameBox, pal.text, Align::Leading, -1, true, true);
        paint.Text(Font::Small, r.where + L" · " + r.status, r.whereBox, pal.subtle, Align::Leading, -1, true, true);
        paint.Icon(r.usb ? Glyph::Usb : Glyph::Wifi, r.transportBox,
                   paint.HighContrast() ? pal.text : r.connected ? pal.accent : pal.subtle, 1.5f);
    }

    Box CardBox(int id) const {
        for (const CardGeo& c : cards) if (c.id == id) return c.box;
        return {};
    }

    void DrawStatusIcon(ID2D1DeviceContext* dc, const Box& at, uint64_t now) {
        auto& icon = IconFor(dc, view.icon);
        const float t = statusFade.Value(now, EaseOut);
        if (icon) dc->DrawBitmap(icon.Get(), D2D1::RectF(at.l, at.t, at.r, at.b), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        // The state badge pops in (scale 0.6 -> 1 with a slight overshoot, 250 ms).
        const float pop = badgePop.Running(now) ? Lerp(0.6f, 1.f, BackOut(badgePop.Progress(now))) : 1.f;
        paint.Badge(at.r - 5, at.b - 5, 6.5f, BadgeFor(view.icon), pop * std::max(t, 0.6f));
    }

    void DrawSidebar(ID2D1DeviceContext* dc, const LinkStatus& s, uint64_t now) {
        const xp::Palette& pal = paint.Colors();
        paint.Fill(nav, pal.navBg);
        if (!paint.HighContrast()) paint.Line(nav.r - 0.5f, nav.t, nav.r - 0.5f, nav.b, pal.cardBorder);
        else paint.Line(nav.r - 0.5f, nav.t, nav.r - 0.5f, nav.b, pal.text);
        // The pill slides to the selected item (200 ms, §12).
        const Item* selected = nullptr;
        for (const auto& it : items) if (it.e.kind == Kind::Nav && it.e.selected) selected = &it;
        if (selected) paint.NavPill(MakeBox(selected->e.rect.l, navPill.Value(now, EaseOut), selected->e.rect.W(),
                                            selected->e.rect.H()));
        for (const auto& it : items) {
            if (it.e.kind == Kind::Nav) DrawItem(dc, it, s, now);
        }
    }

    // Draws one page's content: its cards, its static text and its items.
    void DrawPageContent(ID2D1DeviceContext* dc, const std::vector<Item>& list, const std::vector<Deco>& deco,
                         const std::vector<CardGeo>& cardList, const LinkStatus& s, uint64_t now) {
        for (const CardGeo& c : cardList) paint.Card(c.box);
        const xp::Palette& pal = paint.Colors();
        for (const Deco& d : deco) {
            if (d.text.empty()) continue;
            paint.Text(d.font, d.text, d.box, d.dim ? pal.disabledText : d.subtle ? pal.subtle : pal.text, Align::Leading,
                       d.underline, true, false);
        }
        for (const Item& it : list) {
            if (it.e.kind == Kind::Nav || it.e.kind == Kind::CaptionButton || it.e.kind == Kind::Card) continue;
            if (it.e.kind == Kind::DropItem) continue; // Drawn with the open panel, on top.
            DrawItem(dc, it, s, now);
        }
    }

    void DrawOutgoingPage(ID2D1DeviceContext* dc, const LinkStatus& s, uint64_t now, float dx, float alpha) {
        if (fadeItems.empty()) return;
        D2D1_LAYER_PARAMETERS1 lp = D2D1::LayerParameters1();
        lp.opacity = alpha;
        lp.contentBounds = D2D1::RectF(contentView.l, contentView.t, contentView.r, contentView.b);
        dc->PushLayer(lp, nullptr);
        dc->SetTransform(D2D1::Matrix3x2F::Translation(dx, 0));
        // The preview keeps its picture while it slides out.
        for (const Item& it : fadeItems) {
            if (it.e.id == kPreview && fadePreview.W() > 0) DrawPreview(dc, fadePreview, s, now, false);
        }
        std::vector<DeviceRow> keep = deviceRows;
        deviceRows = fadeRows;
        DrawPageContent(dc, fadeItems, fadeDecos, fadeCards, s, now);
        deviceRows = keep;
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        dc->PopLayer();
    }

    void DrawDropdown(ID2D1DeviceContext* dc, uint64_t now) {
        if (openDrop == kNone || dropPanel.W() <= 0) return;
        const float t = dropAnim.Value(now, EaseOut);
        const xp::Palette& pal = paint.Colors();
        D2D1_LAYER_PARAMETERS1 lp = D2D1::LayerParameters1();
        lp.opacity = t;
        dc->PushLayer(lp, nullptr);
        // Opens with a small scale from the control's edge, never a slide (§12.8).
        const float sc = Lerp(0.96f, 1.f, t);
        dc->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc, D2D1::Point2F((dropPanel.l + dropPanel.r) / 2, dropPanel.t)));
        paint.DropPanel(dropPanel);
        for (const Item& it : items) {
            if (it.e.kind != Kind::DropItem || it.e.parent != openDrop) continue;
            const bool isHot = dropHot == it.e.id && it.e.enabled;
            paint.DropRow(it.e.rect, it.e.selected, isHot);
            const D2D1_COLOR_F ink = !it.e.enabled ? pal.disabledText
                                     : isHot       ? pal.accentText
                                                   : pal.text;
            paint.Text(it.e.selected ? Font::BodyBold : Font::Body, it.label,
                       Box{it.e.rect.l + 10, it.e.rect.t, it.e.rect.r - 8, it.e.rect.b}, ink, Align::Leading, -1, true, true);
        }
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        dc->PopLayer();
    }

    void DrawFocus() {
        if (!Cues() || GetFocus() != hwnd) return;
        const Item* it = FindItem(focus);
        if (!it || !CanFocus(it->e)) return;
        const Element& e = it->e;
        const bool clip = e.rect.l >= content.l;
        if (clip) paint.Dc()->PushAxisAlignedClip(D2D1::RectF(contentView.l, contentView.t, contentView.r, contentView.b),
                                                  D2D1_ANTIALIAS_MODE_ALIASED);
        if (IsFlat(e.kind)) {
            paint.FocusRing(e.rect.Inset(-2, -2), e.kind == Kind::Nav ? 9.f : 8.f);
        } else if (e.kind == Kind::Checkbox || e.kind == Kind::Radio) {
            const Size t = paint.Measure(Font::Body, it->label, e.rect.W() - 18);
            paint.FocusRect(Box{e.rect.l + 16, e.rect.t - 1, e.rect.l + 18 + t.w + 3, e.rect.t + t.h + 1});
        } else {
            paint.FocusRect(e.rect.Inset(-2, -1));
        }
        if (clip) paint.Dc()->PopAxisAlignedClip();
    }

    void Paint() {
        const uint64_t now = Now();
        ID2D1DeviceContext* dc = surface.Begin(scale);
        if (!dc) return;
        paint.BeginFrame(dc, surface.Factory());
        Layout();
        const float maxScroll = MaxScroll();
        if (scroll > maxScroll) { // Content shrank: pull it back down.
            scroll = maxScroll;
            Layout();
        }
        EnsureBitmaps(dc);
        const Box pv = page == Page::Camera ? previewBox : fadePreview;
        pw = UINT(std::max(0.f, pv.W()) * scale) & ~1u;
        ph = UINT(std::max(0.f, pv.H()) * scale) & ~1u;
        if (pw && ph) UploadPreview(dc);
        const LinkStatus s = Status();

        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        xp::WindowChrome(paint, kW, kH, L"MyCam", active, appIcon16.Get());
        const xp::Palette& pal = paint.Colors();
        // Minimise and Close sit in the title bar, so they are drawn with the chrome and not with a page:
        // DrawPageContent skips them, and they must stay outside the content clip and the page animation.
        for (const Item& it : items) {
            if (it.e.kind == Kind::CaptionButton) DrawItem(dc, it, s, now);
        }
        paint.Fill(content, pal.pageBg);
        DrawSidebar(dc, s, now);

        dc->PushAxisAlignedClip(D2D1::RectF(contentView.l, contentView.t, contentView.r, contentView.b),
                                D2D1_ANTIALIAS_MODE_ALIASED);
        const bool moving = pageAnim.Running(now);
        const float t = pageAnim.Value(now, EaseOut);
        if (moving) {
            // Outgoing fades out and slides toward the nav; incoming fades in from the far side (§12).
            DrawOutgoingPage(dc, s, now, -kPageSlideDip * t, 1 - t);
            D2D1_LAYER_PARAMETERS1 lp = D2D1::LayerParameters1();
            lp.opacity = t;
            dc->PushLayer(lp, nullptr);
            dc->SetTransform(D2D1::Matrix3x2F::Translation(kPageSlideDip * (1 - t), 0));
        }
        DrawPageContent(dc, items, decos, cards, s, now);
        if (page == Page::Home) {
            if (const Item* head = FindItem(kStatusHeadline))
                DrawStatusIcon(dc, MakeBox(head->e.rect.l - 42, head->e.rect.t + 1, 32, 32), now);
        }
        if (moving) {
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
            dc->PopLayer();
        } else {
            fadeItems.clear();
            fadeDecos.clear();
            fadeCards.clear();
            fadeRows.clear();
        }
        DrawDropdown(dc, now);
        DrawFocus();

        // A thin flat scrollbar when a page overflows (large text, many devices).
        if (MaxScroll() > 0) {
            const Box track = ThumbTrack(), thumbBox = Thumb();
            if (paint.HighContrast()) {
                paint.Fill(track, xp::SysColor(COLOR_SCROLLBAR));
                paint.Fill(thumbBox, xp::SysColor(thumbHot || thumbDrag ? COLOR_HIGHLIGHT : COLOR_BTNTEXT));
            } else {
                paint.RoundRect(track, 3, Rgb(0xE6EBF2));
                paint.RoundRect(thumbBox, 3, thumbHot || thumbDrag ? Rgb(0x8E9CB2) : Rgb(0xB6C0CE));
            }
        }
        dc->PopAxisAlignedClip();

        if (!surface.End()) Invalidate(); // Device lost: rebuild and draw again, or the window stays empty.
        ScheduleTimers();
    }

    // --- Window procedure --------------------------------------------------------------------------

    void ToDips(LPARAM lp, float* x, float* y) const {
        *x = GET_X_LPARAM(lp) / scale;
        *y = GET_Y_LPARAM(lp) / scale;
    }

    void ReadSystemSettings() {
        reducedMotion = xp::ReducedMotion();
        paint.SetPalette(xp::HighContrastOn());
        paint.SetTextScale(xp::TextScaleFactor());
    }

    bool HandleKey(WPARAM key) {
        std::vector<Element> list = Elements();
        const Element* f = FindElement(list, focus);
        switch (key) {
        case VK_TAB:
            CloseDropdown();
            SetFocusTo(NextTabStop(list, focus, GetKeyState(VK_SHIFT) < 0), true);
            return true;
        case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN: {
            const int delta = key == VK_LEFT || key == VK_UP ? -1 : 1;
            keyboardCues = true;
            if (f && f->kind == Kind::Dropdown) { // Arrows pick the next entry that this phone can do.
                const int next = DropdownStep(list, f->id, delta);
                if (next != kNone) Activate(next);
                return true;
            }
            if (f && f->kind == Kind::Slider) {
                const float v = SliderStep(*f, delta);
                const Item* it = FindItem(f->id);
                if (it && it->extra.W() > 0) {
                    const float frac = (f->rangeMax > f->rangeMin) ? (v - f->rangeMin) / (f->rangeMax - f->rangeMin) : 0.f;
                    SetSliderFromX(f->id, it->extra.l + it->extra.W() * frac);
                }
                return true;
            }
            const int target = ArrowTarget(list, focus, delta);
            const Element* t = FindElement(list, target);
            SetFocusTo(target, true);
            if (t && (t->kind == Kind::Radio || t->kind == Kind::Nav) && !t->selected) Activate(target);
            return true;
        }
        case VK_SPACE: case VK_RETURN: {
            keyboardCues = true;
            if (f && f->kind == Kind::Dropdown) {
                Activate(f->id); // Opens or closes the list.
                return true;
            }
            if (f && (key == VK_SPACE || f->kind != Kind::Checkbox)) Activate(focus);
            return true;
        }
        case VK_PRIOR: SetScroll(scroll - contentView.H() * 0.8f); return true;
        case VK_NEXT: SetScroll(scroll + contentView.H() * 0.8f); return true;
        case VK_ESCAPE:
            if (openDrop != kNone) {
                CloseDropdown();
                Invalidate();
                return true;
            }
            DestroyWindow(hwnd);
            return true;
        }
        return false;
    }

    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) {
        if (uia) {
            LRESULT r = 0;
            if (uia->HandleMessage(msg, wp, lp, &r)) return r;
        }
        switch (msg) {
        case WM_NCCALCSIZE:
            if (wp) return 0; // The whole window is client area; we draw our own frame.
            break;
        case WM_NCHITTEST: {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            const float x = pt.x / scale, y = pt.y / scale;
            const int h = HitTest(x, y);
            if (y < xp::kTitleBarH && h != kCaptionClose && h != kCaptionMin) return HTCAPTION;
            return HTCLIENT;
        }
        case WM_NCACTIVATE:
            active = wp != FALSE;
            Invalidate();
            return TRUE; // Nothing non-client to draw.
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            Paint();
            EndPaint(hwnd, &ps);
            if (uia) uia->Update(); // After the layout: states, focus, rectangles and UIA events.
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (wp == SIZE_MINIMIZED) ScheduleTimers();
            else Invalidate();
            return 0;
        case WM_SHOWWINDOW:
            if (!wp) {
                KillTimer(hwnd, kTimerAnim);
                KillTimer(hwnd, kTimerPreview);
                ClosePreview();
            } else {
                Invalidate();
            }
            break;
        case WM_TIMER:
            if (wp == kTimerPreview) PollPreview();
            else Invalidate();
            if (wp == kTimerAnim && !Animating(Now()) && !MarqueeRunning() && !PairingCountdown()) KillTimer(hwnd, kTimerAnim);
            return 0;
        case WM_DPICHANGED: {
            if (forcedScale <= 0) scale = HIWORD(wp) / 96.f;
            const RECT* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Invalidate();
            return 0;
        }
        case WM_SETTINGCHANGE: case WM_SYSCOLORCHANGE: case WM_THEMECHANGED:
            ReadSystemSettings();
            Invalidate();
            break;
        case WM_SETFOCUS: case WM_KILLFOCUS:
            Invalidate();
            return 0;
        case WM_MOUSEMOVE: {
            if (!tracking) {
                TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                tracking = true;
            }
            float x, y;
            ToDips(lp, &x, &y);
            if (thumbDrag) {
                const Box track = ThumbTrack(), thumbBox = Thumb();
                const float span = track.H() - thumbBox.H();
                if (span > 0) SetScroll(dragStartScroll + (y - dragStartY) * MaxScroll() / span);
                return 0;
            }
            if (sliderDrag != kNone) {
                SetSliderFromX(sliderDrag, x);
                return 0;
            }
            const int h = HitTest(x, y);
            const bool overThumb = MaxScroll() > 0 && Thumb().Inset(-2, 0).Contains(x, y);
            const Item* over = FindItem(h);
            const int newDropHot = over && over->e.kind == Kind::DropItem ? h : kNone;
            if (h != hot || overThumb != thumbHot || newDropHot != dropHot) {
                hot = h;
                thumbHot = overThumb;
                dropHot = newDropHot;
                Invalidate();
            }
            SetCursor(LoadCursorW(nullptr, over && over->e.kind == Kind::Link ? IDC_HAND : IDC_ARROW));
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking = false;
            hot = kNone;
            thumbHot = false;
            dropHot = kNone;
            Invalidate();
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) return TRUE; // Set in WM_MOUSEMOVE.
            break;
        case WM_MOUSEWHEEL:
            if (MaxScroll() > 0) SetScroll(scroll - GET_WHEEL_DELTA_WPARAM(wp) / float(WHEEL_DELTA) * 48.f);
            return 0;
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
            float x, y;
            ToDips(lp, &x, &y);
            SetFocus(hwnd);
            keyboardCues = false;
            if (MaxScroll() > 0 && ThumbTrack().Inset(-2, 0).Contains(x, y)) {
                const Box thumbBox = Thumb();
                if (thumbBox.Inset(-2, 0).Contains(x, y)) {
                    thumbDrag = true;
                    dragStartY = y;
                    dragStartScroll = scroll;
                    SetCapture(hwnd);
                } else {
                    SetScroll(scroll + (y < thumbBox.t ? -1 : 1) * contentView.H() * 0.8f);
                }
                Invalidate();
                return 0;
            }
            const int target = HitTest(x, y);
            // A click outside an open list closes it and does nothing else.
            if (openDrop != kNone) {
                const Item* it = FindItem(target);
                const bool inList = it && (it->e.kind == Kind::DropItem || it->e.id == openDrop);
                if (!inList) {
                    CloseDropdown();
                    Invalidate();
                    return 0;
                }
            }
            pressed = target;
            const Item* it = FindItem(pressed);
            if (it && it->e.focusable) focus = pressed;
            if (it && it->e.kind == Kind::Slider && it->e.enabled) {
                sliderDrag = pressed;
                SetSliderFromX(pressed, x);
            }
            SetCapture(hwnd);
            Invalidate();
            return 0;
        }
        case WM_LBUTTONUP: {
            ReleaseCapture();
            if (thumbDrag) {
                thumbDrag = false;
                Invalidate();
                return 0;
            }
            if (sliderDrag != kNone) {
                sliderDrag = kNone;
                pressed = kNone;
                Invalidate();
                return 0;
            }
            float x, y;
            ToDips(lp, &x, &y);
            const int target = pressed;
            pressed = kNone;
            if (target != kNone) { // The press was immediate; the release eases back (§12.5).
                released = target;
                pressFade.Start(0, Now(), Dur(kShortMs), 1);
            }
            Invalidate();
            if (target != kNone && HitTest(x, y) == target) Activate(target);
            return 0;
        }
        case WM_CAPTURECHANGED:
            thumbDrag = false;
            sliderDrag = kNone;
            return 0;
        case WM_KEYDOWN:
            if (HandleKey(wp)) return 0;
            break;
        case WM_SYSKEYDOWN:
            if (wp == VK_MENU || wp == VK_F10) { // Alt shows the access-key underlines.
                if (!keyboardCues) {
                    keyboardCues = true;
                    Invalidate();
                }
                if (wp == VK_MENU) return 0;
            } else if (wp >= 'A' && wp <= 'Z' && (lp & (1 << 29))) {
                const int target = AccessKeyTarget(Elements(), wchar_t(wp));
                if (target != kNone) {
                    sysKeyHandled = true;
                    keyboardCues = true;
                    focus = target;
                    ScrollIntoView(target);
                    Activate(target);
                    Invalidate();
                    return 0;
                }
            }
            break;
        case WM_SYSCHAR:
            if (sysKeyHandled) { // Handled in WM_SYSKEYDOWN: no beep.
                sysKeyHandled = false;
                return 0;
            }
            break;
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS;
        case WM_DESTROY:
            if (page == Page::IPhone && model->stopWebPairing) model->stopWebPairing(); // No QR code without its page.
            if (uia) uia->Disconnect(); // Kept until the next Create: this may run inside its own action.
            KillTimer(hwnd, kTimerAnim);
            KillTimer(hwnd, kTimerPreview);
            ClosePreview();
            surface.Reset();
            liveBitmap.Reset();
            artBitmap.Reset();
            artBlur.Reset();
            appIcon16.Reset();
            appIcon48.Reset();
            statusIcon.Reset();
            hwnd = nullptr;
            if (owner->elementsChanged_) owner->elementsChanged_();
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        Impl* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
    }

    void Create() {
        static bool registered = false;
        HINSTANCE instance = GetModuleHandleW(nullptr);
        if (!registered) {
            WNDCLASSEXW wc = {sizeof(wc)};
            wc.lpfnWndProc = Proc;
            wc.hInstance = instance;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(kIconApp), IMAGE_ICON, 32, 32, 0));
            wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(kIconApp), IMAGE_ICON, 16, 16, 0));
            wc.lpszClassName = L"MyCamSettings";
            RegisterClassExW(&wc);
            registered = true;
        }
        if (!dwrite) {
            DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()));
            paint.Init(dwrite.Get(), xp::TextScaleFactor());
        }
        ReadSystemSettings();
        // Developer override for checking layouts at other scales without changing Windows' settings:
        // MYCAM_UI_SCALE=1.5 draws the window as at 150 %.
        wchar_t env[16] = {};
        if (GetEnvironmentVariableW(L"MYCAM_UI_SCALE", env, 16)) forcedScale = float(_wtof(env));

        // Centre on the work area of the monitor under the cursor, at that monitor's DPI.
        POINT cursor;
        GetCursorPos(&cursor);
        HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(monitor, &mi);
        UINT dpi = 96;
        if (HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
            using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
            if (auto fn = reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor"))) {
                UINT dx = 96, dy = 96;
                if (SUCCEEDED(fn(monitor, 0, &dx, &dy))) dpi = dx;
            }
            FreeLibrary(shcore);
        }
        scale = forcedScale > 0 ? forcedScale : dpi / 96.f;
        const int w = int(kW * scale + 0.5f), h = int(kH * scale + 0.5f);
        const RECT& wa = mi.rcWork;
        const int x = wa.left + std::max(0, int(wa.right - wa.left - w) / 2);
        const int y = wa.top + std::max(0, int(wa.bottom - wa.top - h) / 2);

        CreateWindowExW(WS_EX_APPWINDOW | WS_EX_NOREDIRECTIONBITMAP, L"MyCamSettings", L"MyCam",
                        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, x, y, w, h, nullptr, nullptr, instance, this);
        if (!hwnd) return;
        scale = forcedScale > 0 ? forcedScale : GetDpiForWindow(hwnd) / 96.f;
        surface.Attach(hwnd);
        uia = std::make_unique<UiaHost>(hwnd, UiaSource{
            [this] { return owner->Elements(); },
            [this](int id) { return owner->Invoke(id); },
            [this](int id) { return owner->Focus(id); },
            [this] { return owner->FocusedId(); },
            [this](const Box& b) { return owner->ClientDipsToScreen(b); },
            [this](int id, double value) { return owner->SetValue(id, value); },
        });

        // XP silhouette: square bottom corners from DWM (the rounded top ones are drawn, transparent
        // outside), no Windows 11 accent border, and keep the DWM drop shadow.
        MARGINS margins = {0, 0, 0, 1};
        DwmExtendFrameIntoClientArea(hwnd, &margins);
        const DWORD corners = 1; // DWMWCP_DONOTROUND
        DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corners, sizeof(corners));
        const COLORREF none = 0xFFFFFFFE; // DWMWA_COLOR_NONE
        DwmSetWindowAttribute(hwnd, 34 /* DWMWA_BORDER_COLOR */, &none, sizeof(none));
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

        viewShown = false;
        navPillSet = false;
        valueAnim.clear();
        page = Status().pairingCode.empty() ? Page::Home : Page::Pairing;
        returnPage = Page::Home;
        pageAnim.Set(1);
        UpdateStatus();
        statusFade.Set(1);
        badgePop.Set(1);
        // Draw the first frame before the window is shown. WS_EX_NOREDIRECTIONBITMAP means it has no
        // surface of its own, so showing it before the swap chain holds a frame flashes a see-through
        // window for one or two frames. Nothing animates on this first paint (§12.2).
        Paint();
        ValidateRect(hwnd, nullptr);
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
    }
};

SettingsWindow::~SettingsWindow() {
    if (impl_ && impl_->hwnd) DestroyWindow(impl_->hwnd);
    delete impl_;
}

void SettingsWindow::Show() {
    if (!impl_) {
        impl_ = new Impl();
        impl_->owner = this;
        impl_->model = &model_;
    }
    if (impl_->hwnd) {
        ShowWindow(impl_->hwnd, IsIconic(impl_->hwnd) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(impl_->hwnd);
        return;
    }
    impl_->Create();
}

void SettingsWindow::Refresh() {
    if (!impl_ || !impl_->hwnd) return;
    impl_->UpdateStatus();
    impl_->Invalidate();
}

void SettingsWindow::StartScan() {
    Show();
    if (!impl_ || !impl_->hwnd) return;
    impl_->GoTo(ui::Page::Devices);
    impl_->StartScan();
    impl_->Invalidate();
}

void SettingsWindow::ShowPairing() {
    Show();
    if (!impl_ || !impl_->hwnd) return;
    impl_->GoTo(ui::Page::Pairing);
    impl_->Invalidate();
}

void SettingsWindow::ShowCamera() {
    if (!impl_ || !impl_->hwnd) return; // Only when open: a pairing started from the phone doesn't open it.
    impl_->GoTo(ui::Page::Camera);
    impl_->Invalidate();
}

HWND SettingsWindow::Hwnd() const { return impl_ ? impl_->hwnd : nullptr; }

std::vector<ui::Element> SettingsWindow::Elements() const {
    if (!impl_ || !impl_->hwnd) return {};
    return impl_->Elements();
}

bool SettingsWindow::Invoke(int id) { return impl_ && impl_->hwnd && impl_->Activate(id); }

bool SettingsWindow::Focus(int id) {
    if (!impl_ || !impl_->hwnd) return false;
    const auto list = impl_->Elements();
    const ui::Element* e = ui::FindElement(list, id);
    if (!e || !ui::CanFocus(*e)) return false;
    impl_->SetFocusTo(id, true);
    return true;
}

bool SettingsWindow::SetValue(int id, double value) {
    if (!impl_ || !impl_->hwnd) return false;
    const auto list = impl_->Elements();
    const ui::Element* e = ui::FindElement(list, id);
    if (!e || e->kind != ui::Kind::Slider || !e->enabled) return false;
    const Item* it = impl_->FindItem(id);
    if (!it || it->extra.W() <= 0) return false;
    const float span = e->rangeMax - e->rangeMin;
    const float frac = span > 0 ? float((value - e->rangeMin) / span) : 0.f;
    impl_->SetSliderFromX(id, it->extra.l + it->extra.W() * std::clamp(frac, 0.f, 1.f));
    return true;
}

int SettingsWindow::FocusedId() const { return impl_ && impl_->hwnd ? impl_->focus : ui::kNone; }

RECT SettingsWindow::ClientDipsToScreen(const ui::Box& box) const {
    RECT r = {};
    if (!impl_ || !impl_->hwnd) return r;
    const float s = impl_->scale;
    POINT a = {LONG(std::floor(box.l * s)), LONG(std::floor(box.t * s))};
    POINT b = {LONG(std::ceil(box.r * s)), LONG(std::ceil(box.b * s))};
    ClientToScreen(impl_->hwnd, &a);
    ClientToScreen(impl_->hwnd, &b);
    return {a.x, a.y, b.x, b.y};
}

void SettingsWindow::SetElementsChanged(std::function<void()> callback) { elementsChanged_ = std::move(callback); }

} // namespace mycam
