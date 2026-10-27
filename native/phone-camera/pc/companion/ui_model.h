#pragma once
// The element model of the custom-drawn companion windows. Every interactive (and every informative)
// thing on screen is one Element with a stable id, a kind, an accessible name, a rectangle, its state and
// its parent group. The windows rebuild this list on every layout; keyboard navigation (W9) works on it,
// and a UI Automation provider (W2, docs/redesign.md §8.4) can be layered on top of it without touching the
// drawing code: one UIA fragment per Element, Groups as ExpandCollapse, Radios as SelectionItem (their
// radioSet is the Selection container), Checkboxes as Toggle, Buttons/Links as Invoke, the status
// headline as a live region.
//
// Pure C++ (no Windows types), unit-tested in pc/tests.

#include <string>
#include <vector>

#include "ui_layout.h"

namespace mycam::ui {

// Shown on the About page. Keep in step with app/build.gradle.kts, installer/mycam.iss and
// installer/build.ps1 when releasing (see CLAUDE.md "Release steps").
constexpr wchar_t kAppVersion[] = L"1.4.0";

enum class Kind {
    Group,          // Collapsible task-pane group (or the fixed "Now" group). UIA: Group + ExpandCollapse.
    Button,         // Push button. UIA: Button + Invoke.
    Checkbox,       // UIA: CheckBox + Toggle.
    Radio,          // One option of a radio set (radioSet). UIA: RadioButton + SelectionItem.
    Link,           // Task link. UIA: Hyperlink + Invoke.
    CaptionButton,  // Title-bar minimise / close. UIA: Button + Invoke.
    Text,           // Static text (status headline, sentence, values). UIA: Text.
    Image,          // The live preview. UIA: Image.
    ProgressBar,    // The green XP bar. UIA: ProgressBar (+ RangeValue when determinate).
    // docs/redesign-v2.md §4: the flat component kit of the v2 shell.
    Nav,            // Sidebar page item. UIA: TabItem + SelectionItem (radioSet kSetNav is its container).
    Dropdown,       // Combo box. UIA: ComboBox + ExpandCollapse + Selection; its DropItems are its children.
    DropItem,       // One entry of a Dropdown (its parent). UIA: ListItem + SelectionItem.
    Toggle,         // On/off switch. UIA: CheckBox + Toggle (`checked`).
    Slider,         // Continuous value. UIA: Slider + RangeValue (`value`, rangeMin/rangeMax/rangeStep).
    Card,           // White card / section container. UIA: Group.
};

// Stable ids. Never renumber: a UIA provider and the tests use them. Add new ones at the end of a block.
enum Id : int {
    kNone = 0,
    // Window chrome.
    kCaptionMin = 1, kCaptionClose = 2,
    // Sidebar navigation: one page each (docs/redesign-v2.md §2.1).
    kNavHome = 10, kNavDevices = 11, kNavCamera = 12, kNavSettings = 13, kNavAbout = 14,
    // Task pane groups.
    kGroupNow = 100, kGroupCamera = 101, kGroupVideo = 102, kGroupPicture = 103, kGroupWifi = 104, kGroupTasks = 105,
    // Cards (v2 pages).
    kCardUsb = 110, kCardWifi = 111, kCardDevices = 112, kCardControls = 113, kCardSettings = 114,
    kCardAbout = 115, kCardStatus = 116, kCardPairing = 117, kCardConnected = 118,
    kCardIPhone = 119, kCardWebDevices = 120, kCardWebHome = 121, // "Connect an iPhone" (web camera).
    // "Now" group / the Home page's status area.
    kStatusHeadline = 200, kStatusDetail = 201, kPause = 202, kLivePill = 203, kStatusProgress = 204,
    kShowPairing = 205, kScanNow = 206, // "Scan for phones" in Now (only while no phone is connected).
    kScanUsb = 207,                     // "Scan for USB Devices" (re-enumerate + reconnect).
    kPageTitle = 208, kPageSubtitle = 209, kUsbNote = 210, kNavDevicesLink = 211,
    kConnectIPhone = 212, // Home: "Connect an iPhone" (opens the iPhone page).
    // Camera group.
    kBack = 300, kFront = 301, kCameraNote = 302,
    kDdCamera = 303, // Camera dropdown; its entries are kBack / kFront.
    // Video group.
    kQ720 = 400, kQ1080 = 401, kQ4K = 402, kFps30 = 403, kFps60 = 404, kFps120 = 405,
    kDdQuality = 406, kDdFps = 407, // Resolution / Frame rate dropdowns (entries: the ids above).
    kFpsReason = 408,               // "120 fps works at 720p on this camera." under the control.
    // Picture group.
    kMirror = 500, kFill = 501,
    // Wi-Fi and startup group.
    kAutostart = 600, kWireless = 601, kForgetPhones = 602,
    // "Scan for phones": the button, its marquee, the result sentence (live region) and the list of phones
    // (a Group whose children are the rows, see ScanRowId). Shown in Now while no phone is connected, else
    // in the Wi-Fi group.
    kScan = 603, kScanProgress = 604, kScanResult = 605, kScanList = 606,
    // The Devices page keeps the phone in use apart from the ones that are only available (§2.1): a heading
    // and a list of its own for each.
    kConnectedList = 607, kConnectedTitle = 608, kAvailableTitle = 609,
    kConnectedNote = 610, kAvailableNote = 611,
    // "Connect a phone to choose." under the Camera page's controls while nothing is connected.
    kCameraHint = 612,
    // Task links.
    kReconnect = 700, kOpenLog = 701,
    // Right side: preview and live camera controls.
    kPreview = 800, kStatusSentence = 801, kZoomOut = 802, kZoomValue = 803, kZoomIn = 804, kZoomReset = 805,
    kEvDown = 806, kEvValue = 807, kEvUp = 808, kFocusAuto = 809, kFocusLock = 810, kTorch = 811, kAuto = 812,
    kNowMode = 813, kControlsNote = 814,
    kZoomSlider = 815,                                  // Zoom slider (replaces the −/+/1× row).
    kChipRes = 816, kChipFps = 817, kChipCam = 818,      // The three dark pills under the preview.
    // Pairing (the dialog, and the pairing page in the window).
    kPairTitle = 900, kPairCode = 901, kPairHint = 902, kPairProgress = 903, kPairCancel = 904, kPairArt = 905, kPairAllow = 906, kPairOk = 907,
    // "Connect an iPhone" page: the QR code, the steps, the address (and the network to use), and the
    // remembered iPhones (rows: WebDeviceRowId).
    kWebQr = 920, kWebSteps = 921, kWebAddress = 922, kDdWebAddress = 923, kWebNote = 924, kWebDevicesTitle = 925,
    kWebDevicesNote = 926, kWebDevicesList = 927, kWebUnavailable = 928,
    // About page.
    kAboutLogo = 950, kAboutName = 951, kAboutVersion = 952, kAboutText = 953, kAboutLicence = 954,
    // Scan results: one block of ids per row (ScanRowId).
    kScanRowBase = 1000, kScanRowStride = 10, kScanRowsMax = 16,
    // "Connect an iPhone": the network dropdown's entries, and the remembered iPhones (part 0 the row, part 1
    // its Forget button).
    kWebAddrBase = 3000, kWebAddrMax = 8,
    kWebDevBase = 3100, kWebDevStride = 10, kWebDevMax = 16,
};

// The pages of the v2 shell (docs/redesign-v2.md §2). Pairing has no nav item: the window shows it while a
// phone is pairing and returns to the page the user was on when the code is gone.
enum class Page : int { Home = 0, Devices = 1, Camera = 2, Settings = 3, About = 4, Pairing = 5, IPhone = 6, Count = 7 };

inline int NavIdOf(Page p) {
    switch (p) {
    case Page::Home: return kNavHome;
    case Page::Devices: return kNavDevices;
    case Page::Camera: return kNavCamera;
    case Page::Settings: return kNavSettings;
    case Page::About: return kNavAbout;
    default: return kNone;
    }
}

inline Page PageOfNav(int id) {
    switch (id) {
    case kNavDevices: return Page::Devices;
    case kNavCamera: return Page::Camera;
    case kNavSettings: return Page::Settings;
    case kNavAbout: return Page::About;
    default: return Page::Home;
    }
}

// Scan result row `index`: part 0 is the row's Text (name, address and status), part 1 its Connect link.
inline int ScanRowId(int index, int part) { return kScanRowBase + index * kScanRowStride + part; }
// The row index of a scan-row id, or -1; *part receives 0 (row) or 1 (Connect).
inline int ScanRowIndex(int id, int* part) {
    if (id < kScanRowBase || id >= kScanRowBase + kScanRowsMax * kScanRowStride) return -1;
    *part = (id - kScanRowBase) % kScanRowStride;
    return (id - kScanRowBase) / kScanRowStride;
}

// Remembered iPhone `index`: part 0 is its row (Text), part 1 its Forget button.
inline int WebDeviceRowId(int index, int part) { return kWebDevBase + index * kWebDevStride + part; }
inline int WebDeviceRowIndex(int id, int* part) {
    if (id < kWebDevBase || id >= kWebDevBase + kWebDevMax * kWebDevStride) return -1;
    *part = (id - kWebDevBase) % kWebDevStride;
    return (id - kWebDevBase) / kWebDevStride;
}

// Radio sets (Element::radioSet). UIA: the Selection container for their Radios (and Navs).
enum RadioSet : int { kSetNone = 0, kSetFacing = 1, kSetQuality = 2, kSetFps = 3, kSetFocus = 4, kSetNav = 5 };

struct Element {
    int id = kNone;
    Kind kind = Kind::Text;
    std::wstring name;         // Accessible name (no '&'); for Text, the text itself.
    int accessKeyIndex = -1;   // Index in `name` of the underlined access-key letter (Alt+letter), or -1.
    Box rect;                  // Client area, DIPs, after scrolling.
    Box clip;                  // The part of the client area it may be seen in (the pane's viewport, ...).
    int parent = kNone;        // Group id, or kNone for the window itself.
    int radioSet = kSetNone;
    bool enabled = true;
    bool visible = true;       // False inside a collapsed group.
    bool checked = false;      // Checkbox.
    bool selected = false;     // Radio.
    bool expanded = false;     // Group.
    bool expandable = false;   // Group that can be collapsed.
    bool focusable = false;    // Can take keyboard focus.
    bool liveRegion = false;   // Status text Narrator should announce when it changes.
    float value = -1;          // ProgressBar: 0..1, or -1 while indeterminate. Slider: the value itself.
    float rangeMin = 0, rangeMax = 0, rangeStep = 0; // Slider (UIA RangeValue).

    // Scrolled out of its viewport (UIA IsOffscreen).
    bool Offscreen() const { return !visible || !rect.Intersects(clip); }
};

inline const Element* FindElement(const std::vector<Element>& list, int id) {
    for (const auto& e : list) if (e.id == id) return &e;
    return nullptr;
}

inline bool CanFocus(const Element& e) { return e.focusable && e.enabled && e.visible; }

// Kinds UIA exposes with SelectionItem (selected / Select()).
inline bool IsSelectionItem(Kind k) { return k == Kind::Radio || k == Kind::Nav || k == Kind::DropItem; }
// Kinds whose Selection container is the synthetic radioSet node (a Dropdown is its items' container).
inline bool IsSetItem(Kind k) { return k == Kind::Radio || k == Kind::Nav; }

// --- Dropdowns (combo boxes) ---------------------------------------------------------------------
// A Dropdown's entries are the DropItems whose parent is the dropdown, in layout order. Entries the
// phone cannot do stay in the list but are disabled (docs/redesign-v2.md §10.3), so stepping skips them.

inline std::vector<int> DropdownItems(const std::vector<Element>& list, int dropdown) {
    std::vector<int> out;
    for (const auto& e : list)
        if (e.kind == Kind::DropItem && e.parent == dropdown) out.push_back(e.id);
    return out;
}

inline int DropdownSelected(const std::vector<Element>& list, int dropdown) {
    for (const auto& e : list)
        if (e.kind == Kind::DropItem && e.parent == dropdown && e.selected) return e.id;
    return kNone;
}

// The enabled entry `delta` steps from the selected one, skipping disabled entries and stopping at the
// ends (as a Win32 combo box does). Returns kNone when there is nowhere to go.
inline int DropdownStep(const std::vector<Element>& list, int dropdown, int delta) {
    const std::vector<int> items = DropdownItems(list, dropdown);
    if (items.empty()) return kNone;
    const int selected = DropdownSelected(list, dropdown);
    int at = -1;
    for (int i = 0; i < int(items.size()); ++i) if (items[size_t(i)] == selected) at = i;
    for (int i = at + delta; i >= 0 && i < int(items.size()); i += delta) {
        const Element* e = FindElement(list, items[size_t(i)]);
        if (e && e->enabled && e->visible) return e->id;
    }
    return kNone;
}

// --- Sliders -------------------------------------------------------------------------------------

inline float ClampRange(const Element& e, float value) {
    const float lo = e.rangeMin, hi = e.rangeMax < e.rangeMin ? e.rangeMin : e.rangeMax;
    return value < lo ? lo : value > hi ? hi : value;
}
// Arrow / wheel step on a slider.
inline float SliderStep(const Element& e, int delta) {
    const float step = e.rangeStep > 0 ? e.rangeStep : (e.rangeMax - e.rangeMin) / 10.f;
    return ClampRange(e, e.value + float(delta) * step);
}
// Where the thumb sits (0..1) and the value a position maps to.
inline float SliderFraction(const Element& e) {
    const float span = e.rangeMax - e.rangeMin;
    return span > 0 ? (ClampRange(e, e.value) - e.rangeMin) / span : 0.f;
}
inline float SliderValueAt(const Element& e, float fraction) {
    const float f = fraction < 0 ? 0 : fraction > 1 ? 1 : fraction;
    return ClampRange(e, e.rangeMin + f * (e.rangeMax - e.rangeMin));
}

// Tab stops in visual order: every focusable control, except that a radio set is a single stop (its
// selected option, or its first enabled one).
inline std::vector<int> TabStops(const std::vector<Element>& list) {
    std::vector<int> stops;
    std::vector<int> setsDone;
    for (const auto& e : list) {
        if (!CanFocus(e)) continue;
        if (e.radioSet == kSetNone) {
            stops.push_back(e.id);
            continue;
        }
        bool done = false;
        for (int s : setsDone) done |= s == e.radioSet;
        if (done) continue;
        setsDone.push_back(e.radioSet);
        int pick = e.id;
        for (const auto& o : list) {
            if (o.radioSet == e.radioSet && CanFocus(o) && o.selected) { pick = o.id; break; }
        }
        stops.push_back(pick);
    }
    return stops;
}

// Tab / Shift+Tab: the next stop after `current` (wrapping). A focused radio counts as its set's stop.
inline int NextTabStop(const std::vector<Element>& list, int current, bool back) {
    const std::vector<int> stops = TabStops(list);
    if (stops.empty()) return kNone;
    int at = -1;
    const Element* cur = FindElement(list, current);
    for (int i = 0; i < int(stops.size()); ++i) {
        const Element* s = FindElement(list, stops[i]);
        if (stops[i] == current || (cur && cur->radioSet != kSetNone && s && s->radioSet == cur->radioSet)) at = i;
    }
    const int n = int(stops.size());
    if (at < 0) return back ? stops[n - 1] : stops[0];
    return stops[(at + (back ? n - 1 : 1)) % n];
}

// Arrow keys: inside a radio set, the next enabled option of the same set (wrapping; the caller selects
// it). Elsewhere, the next focusable control of the same parent group (wrapping), so arrows never leave
// the group. Returns `current` if there is nowhere to go.
inline int ArrowTarget(const std::vector<Element>& list, int current, int delta) {
    const Element* cur = FindElement(list, current);
    if (!cur) return current;
    std::vector<int> ring;
    for (const auto& e : list) {
        if (!CanFocus(e)) continue;
        if (cur->radioSet != kSetNone ? e.radioSet == cur->radioSet : (e.parent == cur->parent && e.radioSet == kSetNone))
            ring.push_back(e.id);
    }
    const int n = int(ring.size());
    for (int i = 0; i < n; ++i) {
        if (ring[i] == current) return ring[((i + delta) % n + n) % n];
    }
    return current;
}

// Alt+letter: the enabled, visible element whose access key is `letter` (case-insensitive), or kNone.
inline int AccessKeyTarget(const std::vector<Element>& list, wchar_t letter) {
    auto lower = [](wchar_t c) { return c >= L'A' && c <= L'Z' ? wchar_t(c - L'A' + L'a') : c; };
    for (const auto& e : list) {
        if (e.accessKeyIndex < 0 || e.accessKeyIndex >= int(e.name.size()) || !e.enabled || !e.visible) continue;
        if (lower(e.name[size_t(e.accessKeyIndex)]) == lower(letter)) return e.id;
    }
    return kNone;
}

// "&Pause the camera" -> name "Pause the camera", accessKeyIndex 0.
inline std::wstring StripAccessKey(const std::wstring& label, int* index) {
    std::wstring out;
    *index = -1;
    for (size_t i = 0; i < label.size(); ++i) {
        if (label[i] == L'&' && i + 1 < label.size()) {
            if (label[i + 1] != L'&') *index = int(out.size());
            ++i;
        }
        out.push_back(label[i]);
    }
    return out;
}

} // namespace mycam::ui
