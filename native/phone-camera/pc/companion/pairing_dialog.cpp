#include "pairing_dialog.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

#include "status_text.h"
#include "uia_provider.h"
#include "ui_layout.h"
#include "ui_motion.h"
#include "xp_draw.h"

namespace mycam {

using namespace ui;
using xp::Align;
using xp::Font;
using xp::Rgb;
using Microsoft::WRL::ComPtr;

namespace {
constexpr float kW = 400;
constexpr float kPad = 16;
constexpr UINT_PTR kTimerTick = 1;
constexpr uint64_t kPairedShowMs = 8000; // How long "Paired with ..." stays without OK.
} // namespace

struct PairingDialog::Impl {
    PairingModel* model = nullptr;
    HWND hwnd = nullptr, owner = nullptr;
    float scale = 1.f, height = 240.f;
    bool active = true, keyboardCues = false, tracking = false, reducedMotion = false;
    xp::Surface surface;
    xp::Painter paint;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<ID2D1Bitmap> icon16, icon32;
    int iconGen = -1;
    float iconScale = 0;

    std::wstring code, phone;
    bool allowed = false;     // This PC's user allowed it; waiting for the phone (Allow is gone then).
    int focus = kPairCancel;  // The focused button. "Don't allow" first, so a stray Enter never allows.
    std::wstring pairedName;  // Non-empty: the "Paired with ..." confirmation instead of a code (ShowPaired).
    uint64_t pairedAt = 0;    // ...shown since (it closes by itself after kPairedShowMs).
    uint64_t since = 0;
    Tween openFade;
    int hot = kNone, pressed = kNone;
    std::vector<Element> elements;
    std::unique_ptr<UiaHost> uia; // Screen readers (uia_provider.h); disconnected on destroy.

    static uint64_t Now() { return xp::NowMs(); }

    void Layout() {
        elements.clear();
        const Box all = {0, 0, kW, height};
        auto add = [&](int id, Kind kind, const std::wstring& name, const Box& r, bool focusable = false) {
            Element e;
            e.id = id;
            e.kind = kind;
            e.name = name;
            e.rect = r;
            e.clip = all;
            e.focusable = focusable;
            elements.push_back(e);
            return &elements.back();
        };
        add(kCaptionClose, Kind::CaptionButton, L"Close", xp::CloseButtonBox(kW));
        const float x = kPad + 40, w = kW - x - kPad;
        float y = xp::kTitleBarH + 14;
        const float titleH = paint.LineHeight(Font::Instruction);
        const float lineH = paint.LineHeight(Font::Body);
        const float bh = std::max(23.f, lineH + 10);
        if (!pairedName.empty()) { // The confirmation after any pairing (QR code, 6-digit code or iPhone).
            add(kPairTitle, Kind::Text, L"Paired with " + pairedName, MakeBox(x, y, w, titleH));
            y += titleH + 8;
            const std::wstring text = L"It’s connected. Choose “PHONE_CAMERA” as the camera in any app on this PC.";
            const float th = paint.Measure(Font::Body, text, w).h;
            add(kPairHint, Kind::Text, text, MakeBox(x, y, w, th));
            y += th + 22;
            const float ow = std::max(96.f, paint.Measure(Font::Body, L"OK").w + 30);
            add(kPairOk, Kind::Button, L"OK", MakeBox(kW - kPad - ow, y, ow, bh), true);
            y += bh + 14;
            height = std::ceil(y + xp::kBorder);
            return;
        }
        add(kPairTitle, Kind::Text, L"Pair with " + phone + L"?", MakeBox(x, y, w, titleH));
        y += titleH + 4;
        y += lineH + 6; // "Check that the phone shows:"
        const Size codeSize = paint.Measure(Font::Code, code.empty() ? L"000 000" : code);
        const float cardH = codeSize.h + 12;
        add(kPairCode, Kind::Text, L"Code " + code, MakeBox(x, y, w, cardH));
        y += cardH + 8;
        const std::wstring hint = allowed ? L"Allowed on this PC. Waiting for Allow on the phone…"
                                          : L"Allow only if the phone shows the same code, then tap Allow on the phone too.";
        const float hintH = paint.Measure(Font::Body, hint, w).h;
        add(kPairHint, Kind::Text, hint, MakeBox(x, y, w, hintH));
        y += hintH + 12;
        const float countW = paint.Measure(Font::Body, L"0:00").w + 8;
        Element* bar = add(kPairProgress, Kind::ProgressBar, L"Time left", MakeBox(x, y, w - countW, 13));
        const uint64_t elapsed = since ? Now() - since : 0;
        bar->value = elapsed >= kPairingAnswerMs ? 0.f : float(kPairingAnswerMs - elapsed) / float(kPairingAnswerMs);
        y += 13 + 18;
        // Both users must allow a code pairing (PROTOCOL.md "The PC asks too"): Allow, and "Don't allow".
        const float dw = std::max(96.f, paint.Measure(Font::Body, L"Don’t allow").w + 30);
        if (!allowed) { // Left to right, in reading order for screen readers.
            const float aw = std::max(96.f, paint.Measure(Font::Body, L"Allow").w + 30);
            add(kPairAllow, Kind::Button, L"Allow", MakeBox(kW - kPad - dw - 10 - aw, y, aw, bh), true);
        }
        add(kPairCancel, Kind::Button, L"Don’t allow", MakeBox(kW - kPad - dw, y, dw, bh), true);
        y += bh + 14;
        height = std::ceil(y + xp::kBorder);
    }

    const Element* Find(int id) const { return FindElement(elements, id); }

    int HitTest(float x, float y) const {
        for (const auto& e : elements) {
            if ((e.kind == Kind::Button || e.kind == Kind::CaptionButton) && e.rect.Contains(x, y)) return e.id;
        }
        return kNone;
    }

    void Paint() {
        const uint64_t now = Now();
        ID2D1DeviceContext* dc = surface.Begin(scale);
        if (!dc) return;
        paint.BeginFrame(dc, surface.Factory());
        Layout();
        if (iconGen != surface.Generation() || iconScale != scale) {
            icon16 = xp::LoadIconBitmap(dc, kIconApp, int(16 * scale + 0.5f));
            icon32 = xp::LoadIconBitmap(dc, kIconApp, int(32 * scale + 0.5f));
            iconGen = surface.Generation();
            iconScale = scale;
        }
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        // Fades in and rises a little (docs/redesign.md §6.3), 220 ms.
        const bool opening = openFade.Running(now);
        if (opening) {
            const float t = openFade.Value(now, EaseOut);
            dc->SetTransform(D2D1::Matrix3x2F::Translation(0, (1 - t) * 8));
            D2D1_LAYER_PARAMETERS1 lp = D2D1::LayerParameters1();
            lp.opacity = t;
            dc->PushLayer(lp, nullptr);
        }
        const bool paired = !pairedName.empty();
        xp::WindowChrome(paint, kW, height, paired ? L"Paired" : L"Pair with a phone", active, icon16.Get());
        const xp::Palette& pal = paint.Colors();
        const Box body = {xp::kBorder, xp::kTitleBarH, kW - xp::kBorder, height - xp::kBorder};
        // The v2 flat palette, the same as the window's own pairing page: page grey, ink text and the code
        // in a pale bordered box. The old XP beige body, navy type and steel borders are gone (§4).
        paint.Fill(body, pal.highContrast ? pal.surface : pal.pageBg);
        if (icon32) dc->DrawBitmap(icon32.Get(), D2D1::RectF(kPad, xp::kTitleBarH + 14, kPad + 32, xp::kTitleBarH + 46));

        const Element* title = Find(kPairTitle);
        // The phone name is what gets the ellipsis; the code never does.
        paint.Text(Font::Instruction, title->name, title->rect, pal.text, Align::Leading, -1, true);
        if (paired) {
            const Element* text = Find(kPairHint);
            paint.Text(Font::Body, text->name, text->rect, pal.subtle);
            const Element* ok = Find(kPairOk);
            if (!pal.highContrast) paint.Line(body.l + kPad, ok->rect.t - 9.5f, body.r - kPad, ok->rect.t - 9.5f, pal.cardBorder);
        } else {
            const Element* card = Find(kPairCode);
            paint.Text(Font::Body, L"Check that the phone shows:", MakeBox(card->rect.l, card->rect.t - paint.LineHeight(Font::Body) - 6,
                                                                          card->rect.W(), paint.LineHeight(Font::Body)), pal.subtle);
            paint.RoundRect(card->rect, 8, paint.HighContrast() ? pal.card : Rgb(0xF7F9FC));
            paint.RoundFrame(card->rect, 8, paint.HighContrast() ? pal.text : pal.ctlBorder);
            paint.Text(Font::Code, code, card->rect, pal.text, Align::Center, -1, false, true);
            const Element* hint = Find(kPairHint);
            paint.Text(Font::Body, hint->name, hint->rect, pal.subtle);

            const Element* bar = Find(kPairProgress);
            paint.ProgressChunks(bar->rect, DeterminateChunks(bar->value, bar->rect.W() - 6, xp::Painter::kChunk, xp::Painter::kChunkGap));
            wchar_t left[8];
            FormatCountdown(uint64_t(bar->value * kPairingAnswerMs), left, 8);
            paint.Text(Font::Body, bar->value > 0 ? left : L"0:00", Box{bar->rect.r + 8, bar->rect.t - 3, kW - kPad, bar->rect.b + 3},
                       pal.subtle, Align::Leading, -1, false, true);

            // A single hairline above the button row (the XP etched groove was beige over white), then the
            // buttons, drawn with the v2 flat kit rather than the old 3-D push button.
            const Element* cancel = Find(kPairCancel);
            if (!pal.highContrast) {
                paint.Line(body.l + kPad, cancel->rect.t - 9.5f, body.r - kPad, cancel->rect.t - 9.5f, pal.cardBorder);
            }
        }
        for (const auto& e : elements) {
            xp::ButtonState st;
            st.hot = hot == e.id && (pressed == kNone || pressed == e.id);
            st.pressed = pressed == e.id && hot == e.id;
            if (e.kind == Kind::CaptionButton) paint.CaptionButton(e.rect, true, st);
            if (e.kind == Kind::Button) {
                paint.FlatButton(e.rect, e.name, -1, st,
                                 e.id == kPairAllow || e.id == kPairOk ? xp::FlatStyle::Primary : xp::FlatStyle::Secondary,
                                 Font::Body);
                if (keyboardCues && GetFocus() == hwnd && e.id == focus) paint.FocusRing(e.rect.Inset(-2, -2), 8.f);
            }
        }
        if (opening) dc->PopLayer();
        const bool presented = surface.End();
        if (opening || !presented) InvalidateRect(hwnd, nullptr, FALSE); // Device lost: draw again.
    }

    void Cancel() {
        if (pairedName.empty() && model->cancel) model->cancel(); // The confirmation just closes.
        Destroy();
    }

    // Allowed here: the dialog stays until the phone answers (the status then clears the code and closes it).
    void Allow() {
        if (allowed) return;
        if (model->allow) model->allow();
        allowed = true;
        focus = kPairCancel;
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    void Activate(int id) {
        if (id == kPairOk) Destroy();
        else if (id == kPairAllow) Allow();
        else if (id == kPairCancel || id == kCaptionClose) Cancel();
    }

    void Destroy() {
        // Re-enable the owner first (as modal dialogs do), so it gets the activation back instead of
        // another application.
        if (hwnd && owner && IsWindow(owner)) EnableWindow(owner, TRUE);
        if (hwnd) DestroyWindow(hwnd);
    }

    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) {
        if (uia) {
            LRESULT r = 0;
            if (uia->HandleMessage(msg, wp, lp, &r)) return r;
        }
        switch (msg) {
        case WM_NCCALCSIZE:
            if (wp) return 0;
            break;
        case WM_NCHITTEST: {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            const float x = pt.x / scale, y = pt.y / scale;
            if (y < xp::kTitleBarH && HitTest(x, y) != kCaptionClose) return HTCAPTION;
            return HTCLIENT;
        }
        case WM_NCACTIVATE:
            active = wp != FALSE;
            InvalidateRect(hwnd, nullptr, FALSE);
            return TRUE;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            Paint();
            EndPaint(hwnd, &ps);
            if (uia) uia->Update();
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_TIMER:
            if (!pairedName.empty() && Now() - pairedAt > kPairedShowMs) {
                Destroy(); // The confirmation goes away by itself.
                return 0;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED: {
            scale = HIWORD(wp) / 96.f;
            const RECT* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_SETTINGCHANGE: case WM_SYSCOLORCHANGE: case WM_THEMECHANGED:
            paint.SetPalette(xp::HighContrastOn());
            reducedMotion = xp::ReducedMotion();
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        case WM_MOUSEMOVE: {
            if (!tracking) {
                TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                tracking = true;
            }
            const int h = HitTest(GET_X_LPARAM(lp) / scale, GET_Y_LPARAM(lp) / scale);
            if (h != hot) {
                hot = h;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking = false;
            hot = kNone;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            pressed = HitTest(GET_X_LPARAM(lp) / scale, GET_Y_LPARAM(lp) / scale);
            keyboardCues = false;
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            ReleaseCapture();
            const int target = pressed;
            pressed = kNone;
            InvalidateRect(hwnd, nullptr, FALSE);
            if (target != kNone && HitTest(GET_X_LPARAM(lp) / scale, GET_Y_LPARAM(lp) / scale) == target) Activate(target);
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                Cancel();
                return 0;
            }
            if (wp == VK_RETURN || wp == VK_SPACE) {
                Activate(focus);
                return 0;
            }
            if (wp == VK_TAB || wp == VK_LEFT || wp == VK_RIGHT) { // Two buttons at most: any of these switches.
                keyboardCues = true;
                if (!allowed && pairedName.empty()) focus = focus == kPairCancel ? kPairAllow : kPairCancel;
                if (uia) uia->Update();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_SYSKEYDOWN:
            if (wp == VK_MENU && !keyboardCues) {
                keyboardCues = true;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case WM_CLOSE: // Alt+F4 or the close button in the taskbar: cancel, like the dialog's own close button.
            Cancel();
            return 0;
        case WM_DESTROY:
            if (uia) uia->Disconnect(); // Kept until the next Create: this may run inside its own action.
            KillTimer(hwnd, kTimerTick);
            surface.Reset();
            icon16.Reset();
            icon32.Reset();
            if (owner && IsWindow(owner)) EnableWindow(owner, TRUE);
            hwnd = nullptr;
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

    void Create(HWND ownerWindow) {
        static bool registered = false;
        HINSTANCE instance = GetModuleHandleW(nullptr);
        if (!registered) {
            WNDCLASSEXW wc = {sizeof(wc)};
            wc.lpfnWndProc = Proc;
            wc.hInstance = instance;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(kIconApp), IMAGE_ICON, 32, 32, 0));
            wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(kIconApp), IMAGE_ICON, 16, 16, 0));
            wc.lpszClassName = L"PHONE_CAMERAPairing";
            RegisterClassExW(&wc);
            registered = true;
        }
        if (!dwrite) {
            DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()));
            paint.Init(dwrite.Get(), xp::TextScaleFactor());
        }
        paint.SetPalette(xp::HighContrastOn());
        paint.SetTextScale(xp::TextScaleFactor());
        reducedMotion = xp::ReducedMotion();
        Layout(); // Measures the height.

        // Centre over the owner if it's open, else on the monitor under the cursor.
        RECT anchor;
        HMONITOR monitor;
        // Only an owner that is on screen: Windows hides the windows a minimised owner owns, and this
        // time-limited dialog must be seen.
        if (ownerWindow && (!IsWindowVisible(ownerWindow) || IsIconic(ownerWindow))) ownerWindow = nullptr;
        if (ownerWindow) {
            GetWindowRect(ownerWindow, &anchor);
            monitor = MonitorFromWindow(ownerWindow, MONITOR_DEFAULTTONEAREST);
        } else {
            POINT cursor;
            GetCursorPos(&cursor);
            monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = {sizeof(mi)};
            GetMonitorInfoW(monitor, &mi);
            anchor = mi.rcWork;
        }
        UINT dpi = 96;
        if (HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
            using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
            if (auto fn = reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor"))) {
                UINT dx = 96, dy = 96;
                if (SUCCEEDED(fn(monitor, 0, &dx, &dy))) dpi = dx;
            }
            FreeLibrary(shcore);
        }
        scale = dpi / 96.f;
        const int w = int(kW * scale + 0.5f), h = int(height * scale + 0.5f);
        const int x = anchor.left + (anchor.right - anchor.left - w) / 2, y = anchor.top + (anchor.bottom - anchor.top - h) / 2;
        owner = ownerWindow && IsWindow(ownerWindow) ? ownerWindow : nullptr;
        // Topmost: it's a time-limited security check the user started on the phone.
        CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | (owner ? 0 : WS_EX_APPWINDOW), L"PHONE_CAMERAPairing", L"Pair with a phone",
                        WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w, h, owner, nullptr, instance, this);
        if (!hwnd) return;
        scale = GetDpiForWindow(hwnd) / 96.f;
        surface.Attach(hwnd);
        uia = std::make_unique<UiaHost>(hwnd, UiaSource{
            [this] { return hwnd ? elements : std::vector<Element>{}; },
            [this](int id) {
                if (!pairedName.empty() ? id != kPairOk && id != kCaptionClose
                                        : id != kPairCancel && id != kCaptionClose && !(id == kPairAllow && !allowed))
                    return false;
                Activate(id); // "Don't allow", OK and Close close the dialog; Allow keeps it until the phone answers.
                return true;
            },
            [this](int id) {
                return !pairedName.empty() ? id == kPairOk : id == kPairCancel || (id == kPairAllow && !allowed);
            },
            [this] { return hwnd ? focus : int(kNone); },
            [this](const Box& b) {
                POINT a = {LONG(std::floor(b.l * scale)), LONG(std::floor(b.t * scale))};
                POINT c = {LONG(std::ceil(b.r * scale)), LONG(std::ceil(b.b * scale))};
                ClientToScreen(hwnd, &a);
                ClientToScreen(hwnd, &c);
                return RECT{a.x, a.y, c.x, c.y};
            },
        });
        MARGINS margins = {0, 0, 0, 1};
        DwmExtendFrameIntoClientArea(hwnd, &margins);
        const DWORD corners = 1; // DWMWCP_DONOTROUND
        DwmSetWindowAttribute(hwnd, 33, &corners, sizeof(corners));
        const COLORREF none = 0xFFFFFFFE;
        DwmSetWindowAttribute(hwnd, 34, &none, sizeof(none));
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
        if (owner) EnableWindow(owner, FALSE);
        const bool still = reducedMotion || xp::HighContrastOn();
        openFade.Start(1, Now(), still ? 0.f : 220.f, still ? 1.f : 0.f);
        SetTimer(hwnd, kTimerTick, 250, nullptr); // The countdown moves in whole chunks.
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
    }

    // Takes the current status; false if there is nothing to pair any more.
    bool Sync(const LinkStatus& s) {
        if (!pairedName.empty()) return true; // The confirmation stays until OK or its time is up.
        if (s.pairingCode.empty()) return false;
        if (s.pairingCode != code) { // A new pairing: nothing allowed yet.
            code = s.pairingCode;
            since = Now();
            allowed = false;
            focus = kPairCancel;
        }
        if (s.pairingAllowed) allowed = true;
        phone = s.phoneName.empty() ? std::wstring(L"the phone") : s.phoneName;
        return true;
    }
};

PairingDialog::~PairingDialog() {
    if (impl_ && impl_->hwnd) DestroyWindow(impl_->hwnd);
    delete impl_;
}

void PairingDialog::Show(HWND owner) {
    if (!impl_) {
        impl_ = new Impl();
        impl_->model = &model_;
    }
    if (!impl_->pairedName.empty()) { // A new code pairing replaces a confirmation still on screen.
        impl_->pairedName.clear();
        if (impl_->hwnd) impl_->Destroy();
    }
    if (!impl_->Sync(model_.status())) return;
    if (impl_->hwnd) {
        ShowWindow(impl_->hwnd, SW_SHOW);
        SetForegroundWindow(impl_->hwnd);
        InvalidateRect(impl_->hwnd, nullptr, FALSE);
        return;
    }
    impl_->Create(owner);
}

void PairingDialog::ShowPaired(const std::wstring& phoneName, HWND owner) {
    if (!impl_) {
        impl_ = new Impl();
        impl_->model = &model_;
    }
    if (impl_->hwnd) impl_->Destroy(); // A pairing dialog becomes the confirmation (it has another size).
    impl_->pairedName = phoneName.empty() ? std::wstring(L"your phone") : phoneName;
    impl_->pairedAt = Impl::Now();
    impl_->focus = kPairOk;
    impl_->Create(owner);
}

void PairingDialog::Close() {
    if (impl_) impl_->Destroy();
}

void PairingDialog::Refresh() {
    if (!impl_ || !impl_->hwnd) return;
    if (!impl_->Sync(model_.status())) {
        impl_->Destroy(); // Resolved: allowed, refused, timed out or cancelled on the phone.
        return;
    }
    InvalidateRect(impl_->hwnd, nullptr, FALSE);
}

bool PairingDialog::IsOpen() const { return impl_ && impl_->hwnd; }
HWND PairingDialog::Hwnd() const { return impl_ ? impl_->hwnd : nullptr; }

std::vector<ui::Element> PairingDialog::Elements() const {
    if (!impl_ || !impl_->hwnd) return {};
    return impl_->elements;
}

} // namespace mycam
