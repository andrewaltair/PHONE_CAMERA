#include "xp_draw.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace mycam::xp {

namespace {
Box MakeBoxCentered(float cx, float cy, float w, float h) { return Box{cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2}; }
} // namespace

// --- Surface ----------------------------------------------------------------------------------------

ID2D1Factory1* Surface::Factory() {
    if (!factory_) {
        D2D1_FACTORY_OPTIONS options = {};
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
                          reinterpret_cast<void**>(factory_.GetAddressOf()));
    }
    return factory_.Get();
}

bool Surface::CreateDevice() {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_1};
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, UINT(std::size(levels)),
                                   D3D11_SDK_VERSION, &d3d_, nullptr, nullptr);
    if (FAILED(hr)) { // No usable GPU (or a remote session without one): software rendering.
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, UINT(std::size(levels)),
                               D3D11_SDK_VERSION, &d3d_, nullptr, nullptr);
    }
    if (FAILED(hr) || !Factory()) return false;
    ComPtr<IDXGIDevice> dxgi;
    if (FAILED(d3d_.As(&dxgi)) || FAILED(factory_->CreateDevice(dxgi.Get(), &d2dDevice_)) ||
        FAILED(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_)) ||
        FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&dcomp_))) ||
        FAILED(dcomp_->CreateTargetForHwnd(hwnd_, TRUE, &dcompTarget_)) || FAILED(dcomp_->CreateVisual(&visual_)) ||
        FAILED(dcompTarget_->SetRoot(visual_.Get()))) {
        Reset();
        return false;
    }
    dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE); // ClearType needs an opaque target.
    ++generation_;
    return true;
}

bool Surface::EnsureSize(UINT w, UINT h, float dpiScale) {
    if (swapChain_ && (w != width_ || h != height_)) {
        dc_->SetTarget(nullptr);
        target_.Reset();
        if (FAILED(swapChain_->ResizeBuffers(2, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, 0))) {
            Reset();
            return false;
        }
        width_ = w;
        height_ = h;
    }
    if (!swapChain_) {
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> dxgiFactory;
        if (FAILED(d3d_.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter)) ||
            FAILED(adapter->GetParent(IID_PPV_ARGS(&dxgiFactory)))) {
            return false;
        }
        DXGI_SWAP_CHAIN_DESC1 desc = {};
        desc.Width = w;
        desc.Height = h;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED; // Transparent rounded top corners.
        desc.Scaling = DXGI_SCALING_STRETCH;
        if (FAILED(dxgiFactory->CreateSwapChainForComposition(d3d_.Get(), &desc, nullptr, &swapChain_))) return false;
        visual_->SetContent(swapChain_.Get());
        dcomp_->Commit();
        width_ = w;
        height_ = h;
    }
    if (!target_ || dpiScale != dpiScale_) {
        dc_->SetTarget(nullptr);
        target_.Reset();
        ComPtr<IDXGISurface> surface;
        if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&surface)))) return false;
        const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f * dpiScale, 96.f * dpiScale);
        if (FAILED(dc_->CreateBitmapFromDxgiSurface(surface.Get(), &props, &target_))) return false;
        dc_->SetTarget(target_.Get());
        dc_->SetDpi(96.f * dpiScale, 96.f * dpiScale);
        dpiScale_ = dpiScale;
    }
    return true;
}

ID2D1DeviceContext* Surface::Begin(float dpiScale) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const UINT w = UINT(std::max<LONG>(rc.right, 1)), h = UINT(std::max<LONG>(rc.bottom, 1));
    if (!dc_ && !CreateDevice()) return nullptr;
    if (!EnsureSize(w, h, dpiScale)) {
        Reset();
        return nullptr;
    }
    dc_->BeginDraw();
    dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    return dc_.Get();
}

bool Surface::End() {
    if (!dc_) return false;
    HRESULT hr = dc_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        Reset();
        return false;
    }
    hr = swapChain_->Present(1, 0);
    // Present(1, 0) normally waits for one vertical blank, which is what paces the animations. Fully
    // covered, it returns immediately instead, so the caller must fall back to a timer.
    occluded_ = hr == DXGI_STATUS_OCCLUDED;
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        Reset();
        return false;
    }
    return true;
}

void Surface::Reset() {
    if (dc_) dc_->SetTarget(nullptr);
    target_.Reset();
    if (visual_) visual_->SetContent(nullptr);
    swapChain_.Reset();
    visual_.Reset();
    dcompTarget_.Reset();
    dcomp_.Reset();
    dc_.Reset();
    d2dDevice_.Reset();
    d3d_.Reset();
    width_ = height_ = 0;
}

// --- Settings -----------------------------------------------------------------------------------------

D2D1_COLOR_F SysColor(int index, float a) {
    const COLORREF c = GetSysColor(index);
    return D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f, a);
}

bool ReducedMotion() {
    BOOL on = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0);
    return !on;
}

bool HighContrastOn() {
    HIGHCONTRASTW hc = {sizeof(hc)};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
}

bool KeyboardCuesAlways() {
    BOOL on = FALSE;
    SystemParametersInfoW(SPI_GETKEYBOARDCUES, 0, &on, 0);
    return on != FALSE;
}

float TextScaleFactor() {
    wchar_t env[16] = {}; // Developer override: PHONE_CAMERA_UI_TEXT_SCALE=1.5
    if (GetEnvironmentVariableW(L"PHONE_CAMERA_UI_TEXT_SCALE", env, 16)) return std::clamp(float(_wtof(env)), 1.f, 2.25f);
    DWORD value = 100, size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Accessibility", L"TextScaleFactor", RRF_RT_REG_DWORD, nullptr,
                 &value, &size);
    return std::clamp(value, 100ul, 225ul) / 100.f;
}

ComPtr<ID2D1Bitmap> LoadIconBitmap(ID2D1DeviceContext* dc, int resourceId, int px) {
    ComPtr<ID2D1Bitmap> out;
    HICON icon = static_cast<HICON>(
        LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(resourceId), IMAGE_ICON, px, px, LR_DEFAULTCOLOR));
    if (!icon || !dc) return out;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmap> wicBitmap;
    ComPtr<IWICFormatConverter> converter;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
        SUCCEEDED(wic->CreateBitmapFromHICON(icon, &wicBitmap)) && SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(wicBitmap.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                        0, WICBitmapPaletteTypeCustom))) {
        D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties();
        dc->CreateBitmapFromWicBitmap(converter.Get(), &props, &out);
    }
    DestroyIcon(icon);
    return out;
}

// --- Painter: text --------------------------------------------------------------------------------------

void Painter::Init(IDWriteFactory* dwrite, float textScale) {
    dwrite_ = dwrite;
    textScale_ = textScale;
    CreateFormats();
    SetPalette(HighContrastOn());
}

void Painter::SetTextScale(float textScale) {
    if (textScale == textScale_) return;
    textScale_ = textScale;
    CreateFormats();
}

void Painter::CreateFormats() {
    // Segoe UI for the body, Trebuchet MS Bold for the window caption only (docs/redesign-v2.md §4). Both ship
    // with Windows and are never shipped with MyCam; Tahoma stands in if Segoe UI is missing.
    static const FontSpec specs[int(Font::Count)] = {
        {L"Segoe UI", L"Tahoma", DWRITE_FONT_WEIGHT_NORMAL, 9.f},
        {L"Segoe UI", L"Tahoma", DWRITE_FONT_WEIGHT_SEMI_BOLD, 9.f},
        {L"Trebuchet MS", L"Segoe UI", DWRITE_FONT_WEIGHT_BOLD, 10.f},
        {L"Segoe UI", L"Tahoma", DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.f},
        {L"Segoe UI", L"Tahoma", DWRITE_FONT_WEIGHT_SEMI_BOLD, 24.f},
        {L"Segoe UI", L"Tahoma", DWRITE_FONT_WEIGHT_NORMAL, 8.f},
    };
    ComPtr<IDWriteFontCollection> fonts;
    dwrite_->GetSystemFontCollection(&fonts);
    for (int i = 0; i < int(Font::Count); ++i) {
        const FontSpec& s = specs[i];
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (fonts) fonts->FindFamilyName(s.family, &index, &exists);
        const wchar_t* family = exists ? s.family : s.fallback;
        // Tahoma 8 pt renders as 11 px at 96 dpi; keep that size for the Segoe UI fallback too.
        const float px = std::round(s.points * 96.f / 72.f) * textScale_;
        for (ComPtr<IDWriteTextFormat>* out : {std::addressof(formats_[i]), std::addressof(ellipsisFormat_[i])}) {
            out->Reset();
            dwrite_->CreateTextFormat(family, nullptr, s.weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px,
                                      L"en-us", out->GetAddressOf());
        }
        formats_[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        ellipsisFormat_[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING trimming = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> sign;
        dwrite_->CreateEllipsisTrimmingSign(ellipsisFormat_[i].Get(), &sign);
        ellipsisFormat_[i]->SetTrimming(&trimming, sign.Get());
    }
    measureCache_.clear();
    layoutCache_.clear(); // The formats changed: every cached layout refers to an old one.
}

Size Painter::Measure(Font font, const std::wstring& text, float maxWidth) {
    const auto key = std::make_tuple(int(font), int(maxWidth * 4), text);
    auto it = measureCache_.find(key);
    if (it != measureCache_.end()) return it->second;
    ComPtr<IDWriteTextLayout> layout;
    Size size;
    if (SUCCEEDED(dwrite_->CreateTextLayout(text.c_str(), UINT32(text.size()), formats_[int(font)].Get(), maxWidth, 10000.f,
                                            &layout))) {
        DWRITE_TEXT_METRICS m = {};
        layout->GetMetrics(&m);
        size = {std::ceil(m.widthIncludingTrailingWhitespace), std::ceil(m.height)};
    }
    if (measureCache_.size() > 2000) measureCache_.clear();
    measureCache_[key] = size;
    return size;
}

float Painter::LineHeight(Font font) { return Measure(font, L"Ag").h; }

void Painter::Text(Font font, const std::wstring& text, const Box& box, D2D1_COLOR_F color, Align align, int underline,
                   bool ellipsis, bool vcenter) {
    if (text.empty() || !dc_) return;
    // The layout only depends on these, never on the colour or the position, so it is reused between
    // frames: building one per string per frame is what made page transitions stutter.
    const float w = std::max(box.W(), 1.f), h = std::max(box.H(), 1.f);
    const int flags = (ellipsis ? 1 : 0) | (int(align) << 1) | (vcenter ? 1 << 4 : 0);
    const auto key = std::make_tuple(int(font), flags, underline, int(w * 4), int(h * 4), text);
    auto it = layoutCache_.find(key);
    if (it == layoutCache_.end()) {
        IDWriteTextFormat* format = (ellipsis ? ellipsisFormat_ : formats_)[int(font)].Get();
        ComPtr<IDWriteTextLayout> made;
        if (FAILED(dwrite_->CreateTextLayout(text.c_str(), UINT32(text.size()), format, w, h, &made))) return;
        made->SetTextAlignment(align == Align::Center     ? DWRITE_TEXT_ALIGNMENT_CENTER
                               : align == Align::Trailing ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                          : DWRITE_TEXT_ALIGNMENT_LEADING);
        made->SetParagraphAlignment(vcenter ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        if (underline >= 0 && underline < int(text.size())) made->SetUnderline(TRUE, {UINT32(underline), 1});
        if (layoutCache_.size() > 600) layoutCache_.clear(); // Values change (addresses, counts): don't grow.
        it = layoutCache_.emplace(key, std::move(made)).first;
    }
    // The ellipsis formats are the ones that carry text from a phone (a device name in a title), and they are
    // laid out as a single line. A line separator inside such a name would still break the line and paint
    // over whatever is below, so those layouts are clipped to their own box. Names are cleaned as well
    // (web::CleanName); this is the second line of defence, and the only one the drawing code can give.
    dc_->DrawTextLayout(D2D1::Point2F(box.l, box.t), it->second.Get(), Brush(color),
                        ellipsis ? D2D1_DRAW_TEXT_OPTIONS_CLIP : D2D1_DRAW_TEXT_OPTIONS_NONE);
}

// --- Painter: primitives ------------------------------------------------------------------------------

ID2D1SolidColorBrush* Painter::Brush(D2D1_COLOR_F c) {
    if (!brush_ || brushOwner_.Get() != dc_) {
        brush_.Reset();
        dc_->CreateSolidColorBrush(c, &brush_);
        brushOwner_ = dc_;
    }
    brush_->SetColor(c);
    return brush_.Get();
}

void Painter::Fill(const Box& b, D2D1_COLOR_F c) { dc_->FillRectangle(D2D1::RectF(b.l, b.t, b.r, b.b), Brush(c)); }

void Painter::Line(float x0, float y0, float x1, float y1, D2D1_COLOR_F c, float width) {
    dc_->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), Brush(c), width);
}

void Painter::Frame(const Box& b, D2D1_COLOR_F c, float width) {
    const float h = width / 2;
    dc_->DrawRectangle(D2D1::RectF(b.l + h, b.t + h, b.r - h, b.b - h), Brush(c), width);
}

ComPtr<ID2D1LinearGradientBrush> Painter::Gradient(float x0, float y0, float x1, float y1, std::initializer_list<Stop> stops) {
    std::vector<D2D1_GRADIENT_STOP> s;
    for (const Stop& st : stops) s.push_back({st.pos, Rgb(st.rgb, st.alpha)});
    ComPtr<ID2D1GradientStopCollection> collection;
    ComPtr<ID2D1LinearGradientBrush> brush;
    dc_->CreateGradientStopCollection(s.data(), UINT32(s.size()), &collection);
    if (collection) {
        dc_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1)),
                                       collection.Get(), &brush);
    }
    return brush;
}

void Painter::VGradient(const Box& b, std::initializer_list<Stop> stops, float radius) {
    auto brush = Gradient(0, b.t, 0, b.b, stops);
    if (!brush) return;
    if (radius > 0) dc_->FillRoundedRectangle({D2D1::RectF(b.l, b.t, b.r, b.b), radius, radius}, brush.Get());
    else dc_->FillRectangle(D2D1::RectF(b.l, b.t, b.r, b.b), brush.Get());
}

void Painter::HGradient(const Box& b, std::initializer_list<Stop> stops) {
    auto brush = Gradient(b.l, 0, b.r, 0, stops);
    if (brush) dc_->FillRectangle(D2D1::RectF(b.l, b.t, b.r, b.b), brush.Get());
}

ComPtr<ID2D1PathGeometry> Painter::TopRounded(const Box& b, float r) {
    ComPtr<ID2D1PathGeometry> path;
    factory_->CreatePathGeometry(&path);
    ComPtr<ID2D1GeometrySink> sink;
    if (!path || FAILED(path->Open(&sink))) return path;
    sink->BeginFigure(D2D1::Point2F(b.l, b.b), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(b.l, b.t + r));
    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(b.l + r, b.t), D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                  D2D1_ARC_SIZE_SMALL));
    sink->AddLine(D2D1::Point2F(b.r - r, b.t));
    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(b.r, b.t + r), D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                  D2D1_ARC_SIZE_SMALL));
    sink->AddLine(D2D1::Point2F(b.r, b.b));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    return path;
}

void Painter::SetPalette(bool hc) {
    pal_.highContrast = hc;
    if (hc) {
        pal_.surface = SysColor(COLOR_WINDOW);
        pal_.text = SysColor(COLOR_WINDOWTEXT);
        pal_.subtle = SysColor(COLOR_WINDOWTEXT);
        pal_.disabledText = SysColor(COLOR_GRAYTEXT);
        pal_.link = SysColor(COLOR_HOTLIGHT);
        pal_.linkHot = SysColor(COLOR_HOTLIGHT);
        pal_.selection = SysColor(COLOR_HIGHLIGHT);
        pal_.selectionText = SysColor(COLOR_HIGHLIGHTTEXT);
        pal_.paneTop = pal_.paneBottom = SysColor(COLOR_WINDOW);
        pal_.groupBody = SysColor(COLOR_WINDOW);
        pal_.groupBorder = SysColor(COLOR_WINDOWTEXT);
        pal_.groupTitle = SysColor(COLOR_BTNTEXT);
        pal_.heroTitle = SysColor(COLOR_HIGHLIGHTTEXT);
        pal_.frame = SysColor(COLOR_ACTIVEBORDER);
        pal_.focus = SysColor(COLOR_WINDOWTEXT);
        pal_.card = SysColor(COLOR_WINDOW);
        // High contrast: flat system colours, no gloss and no tints (docs/redesign-v2.md §12).
        pal_.accent = SysColor(COLOR_HIGHLIGHT);
        pal_.accentHot = SysColor(COLOR_HIGHLIGHT);
        pal_.accentDown = SysColor(COLOR_HIGHLIGHT);
        pal_.accentText = SysColor(COLOR_HIGHLIGHTTEXT);
        pal_.pageBg = SysColor(COLOR_WINDOW);
        pal_.navBg = SysColor(COLOR_WINDOW);
        pal_.cardBorder = SysColor(COLOR_WINDOWTEXT);
        pal_.ctlFill = SysColor(COLOR_WINDOW);
        pal_.ctlBorder = SysColor(COLOR_WINDOWTEXT);
        pal_.ctlHotBorder = SysColor(COLOR_HIGHLIGHT);
        pal_.trackOff = SysColor(COLOR_WINDOW);
        pal_.trackOffBorder = SysColor(COLOR_WINDOWTEXT);
        return;
    }
    // docs/redesign.md §3.2 (Luna Blue).
    pal_.surface = Rgb(0xECE9D8);
    pal_.text = Rgb(0x000000);
    pal_.subtle = Rgb(0x4D4D4D);
    pal_.disabledText = Rgb(0xACA899);
    pal_.link = Rgb(0x215DC6);
    pal_.linkHot = Rgb(0x428EFF);
    pal_.selection = Rgb(0x316AC5);
    pal_.selectionText = Rgb(0xFFFFFF);
    pal_.paneTop = Rgb(0x7BA2E7);
    pal_.paneBottom = Rgb(0x6375D6);
    pal_.groupBody = Rgb(0xD6DFF7);
    pal_.groupBorder = Rgb(0xFFFFFF);
    pal_.groupTitle = Rgb(0x215DC6);
    pal_.heroTitle = Rgb(0xFFFFFF);
    pal_.frame = Rgb(0x0831D9);
    pal_.focus = Rgb(0x000000);
    pal_.card = Rgb(0xFFFFFF);
    // docs/redesign-v2.md §4: one blue accent, white cards, near-white page. White on #1E6FE8 is 4.6 : 1.
    pal_.accent = Rgb(0x1E6FE8);
    pal_.accentHot = Rgb(0x3281F0);
    pal_.accentDown = Rgb(0x1760D0);
    pal_.accentText = Rgb(0xFFFFFF);
    pal_.pageBg = Rgb(0xF5F7FA);
    pal_.navBg = Rgb(0xFFFFFF);
    pal_.cardBorder = Rgb(0xE3E8EF);
    pal_.ctlFill = Rgb(0xFFFFFF);
    pal_.ctlBorder = Rgb(0xD0D7E2);
    pal_.ctlHotBorder = Rgb(0xA8B7CC);
    pal_.trackOff = Rgb(0xD7DEE8);
    pal_.trackOffBorder = Rgb(0xB4C0D0);
    // The body is white now, not beige: text and secondary text get a touch more contrast.
    pal_.text = Rgb(0x1B2330);
    pal_.subtle = Rgb(0x5C6675);
    pal_.disabledText = Rgb(0xA3ACBA);
    pal_.link = Rgb(0x1760D0);
    pal_.linkHot = Rgb(0x3281F0);
}

// --- Painter: XP widgets ---------------------------------------------------------------------------

void Painter::TitleBar(const Box& b, bool active, float radius) {
    auto shape = TopRounded(b, radius);
    if (pal_.highContrast) {
        dc_->FillGeometry(shape.Get(), Brush(SysColor(active ? COLOR_ACTIVECAPTION : COLOR_INACTIVECAPTION)));
        return;
    }
    // §3.2 TitleBar: #3D8AF7 → #0A5DEB (8 %) → #0053E1 (50 %) → #0047D0, highlight #7FB6FF, edge #0831D9.
    auto fill = active ? Gradient(0, b.t, 0, b.b, {{0.f, 0x3D8AF7}, {0.08f, 0x0A5DEB}, {0.5f, 0x0053E1}, {0.92f, 0x0047D0}, {1.f, 0x0041C0}})
                       : Gradient(0, b.t, 0, b.b, {{0.f, 0xA9C0F2}, {0.08f, 0x8FAAEC}, {0.5f, 0x7E9BE6}, {1.f, 0x7792E0}});
    if (fill) dc_->FillGeometry(shape.Get(), fill.Get());
    // The XP gloss: a lighter top band and a 1 px highlight line under the top edge.
    dc_->PushAxisAlignedClip(D2D1::RectF(b.l, b.t, b.r, b.t + b.H() * 0.42f), D2D1_ANTIALIAS_MODE_ALIASED);
    auto gloss = Gradient(0, b.t, 0, b.t + b.H() * 0.42f, {{0.f, 0xFFFFFF, active ? 0.22f : 0.18f}, {1.f, 0xFFFFFF, 0.f}});
    if (gloss) dc_->FillGeometry(shape.Get(), gloss.Get());
    dc_->PopAxisAlignedClip();
    Line(b.l + radius * 0.6f, b.t + 1.5f, b.r - radius * 0.6f, b.t + 1.5f, Rgb(active ? 0x7FB6FF : 0xD3DEF8, 0.9f));
    dc_->DrawGeometry(shape.Get(), Brush(Rgb(active ? 0x0831D9 : 0x7A93DF)), 1.f);
}

void Painter::CaptionButton(const Box& b, bool isClose, const ButtonState& s) {
    const D2D1_ROUNDED_RECT rr = {D2D1::RectF(b.l + 0.5f, b.t + 0.5f, b.r - 0.5f, b.b - 0.5f), 3, 3};
    if (pal_.highContrast) {
        dc_->FillRoundedRectangle(rr, Brush(SysColor(s.hot ? COLOR_HIGHLIGHT : COLOR_BTNFACE)));
        dc_->DrawRoundedRectangle(rr, Brush(SysColor(COLOR_BTNTEXT)));
    } else {
        ComPtr<ID2D1LinearGradientBrush> fill;
        if (isClose) { // §3.2 ErrorRed #FF7B6B → #C81E0F.
            fill = s.pressed ? Gradient(0, b.t, 0, b.b, {{0.f, 0xC8462F}, {0.5f, 0xB52A15}, {1.f, 0xD45A3E}})
                 : s.hot     ? Gradient(0, b.t, 0, b.b, {{0.f, 0xFFA898}, {0.45f, 0xF26A50}, {1.f, 0xD8331C}})
                             : Gradient(0, b.t, 0, b.b, {{0.f, 0xFF8E7E}, {0.45f, 0xE9563E}, {1.f, 0xC81E0F}});
        } else {
            fill = s.pressed ? Gradient(0, b.t, 0, b.b, {{0.f, 0x1F4FC0}, {0.5f, 0x2A5FD4}, {1.f, 0x3F78E6}})
                 : s.hot     ? Gradient(0, b.t, 0, b.b, {{0.f, 0x8FB8FF}, {0.45f, 0x5590F8}, {1.f, 0x2D68E2}})
                             : Gradient(0, b.t, 0, b.b, {{0.f, 0x6EA1F8}, {0.45f, 0x3B78EE}, {1.f, 0x1E56D8}});
        }
        if (fill) dc_->FillRoundedRectangle(rr, fill.Get());
        // Soft inner light at the top-left, the white XP rim.
        auto shine = Gradient(b.l, b.t, b.r, b.b, {{0.f, 0xFFFFFF, 0.35f}, {0.5f, 0xFFFFFF, 0.f}});
        if (shine && !s.pressed) dc_->FillRoundedRectangle({D2D1::RectF(b.l + 1.5f, b.t + 1.5f, b.r - 1.5f, b.b - 1.5f), 2, 2}, shine.Get());
        dc_->DrawRoundedRectangle(rr, Brush(Rgb(0xFFFFFF, 0.95f)), 1.f);
    }
    const float shift = s.pressed ? 1.f : 0.f;
    const float cx = (b.l + b.r) / 2 + shift, cy = (b.t + b.b) / 2 + shift;
    const D2D1_COLOR_F ink = pal_.highContrast ? SysColor(s.hot ? COLOR_HIGHLIGHTTEXT : COLOR_BTNTEXT) : Rgb(0xFFFFFF);
    if (isClose) {
        const float d = b.W() * 0.2f;
        auto cross = [&](D2D1_COLOR_F c, float w, float o) {
            Line(cx - d + o, cy - d + o, cx + d + o, cy + d + o, c, w);
            Line(cx + d + o, cy - d + o, cx - d + o, cy + d + o, c, w);
        };
        if (!pal_.highContrast) cross(Rgb(0x7A1405, 0.45f), 2.2f, 0.7f);
        cross(ink, 2.2f, 0);
    } else {
        const float w = b.W() * 0.36f;
        const Box bar = MakeBoxCentered(cx - b.W() * 0.08f, b.b - b.H() * 0.3f + shift, w, 2.5f);
        if (!pal_.highContrast) Fill(bar.Offset(0.6f, 0.6f), Rgb(0x0A2C8C, 0.45f));
        Fill(bar, ink);
    }
}

void Painter::PushButton(const Box& b, const std::wstring& label, int underline, const ButtonState& s, ButtonStyle style,
                         Font font) {
    const D2D1_ROUNDED_RECT rr = {D2D1::RectF(b.l + 0.5f, b.t + 0.5f, b.r - 0.5f, b.b - 0.5f), 3, 3};
    const float shift = s.pressed && !s.disabled ? 1.f : 0.f; // XP pressed state: contents move 1 px.
    D2D1_COLOR_F ink;
    if (pal_.highContrast) {
        dc_->FillRoundedRectangle(rr, Brush(SysColor(s.hot && !s.disabled ? COLOR_HIGHLIGHT : COLOR_BTNFACE)));
        dc_->DrawRoundedRectangle(rr, Brush(SysColor(s.disabled ? COLOR_GRAYTEXT : COLOR_BTNTEXT)), s.isDefault ? 2.f : 1.f);
        ink = SysColor(s.disabled ? COLOR_GRAYTEXT : s.hot ? COLOR_HIGHLIGHTTEXT : COLOR_BTNTEXT);
    } else if (s.disabled) {
        dc_->FillRoundedRectangle(rr, Brush(Rgb(0xF5F4EA)));
        dc_->DrawRoundedRectangle(rr, Brush(Rgb(0xC9C7BA)));
        ink = Rgb(0xACA899);
    } else {
        const bool green = style == ButtonStyle::Green;
        ComPtr<ID2D1LinearGradientBrush> fill;
        if (green) { // §3.2 GoGreen #3FAA3F → #2A7F2A, border #1D5E1D.
            fill = s.pressed ? Gradient(0, b.t, 0, b.b, {{0.f, 0x2A7F2A}, {1.f, 0x3A9E3A}})
                 : s.hot     ? Gradient(0, b.t, 0, b.b, {{0.f, 0x5CC45C}, {0.5f, 0x43AE43}, {1.f, 0x2E872E}})
                             : Gradient(0, b.t, 0, b.b, {{0.f, 0x4DB84D}, {0.5f, 0x3FAA3F}, {1.f, 0x2A7F2A}});
        } else { // Button normal: #FFFFFF → #ECEBE6 (85 %) → #D6D0C5.
            fill = s.pressed ? Gradient(0, b.t, 0, b.b, {{0.f, 0xE3E2DA}, {0.8f, 0xE6E5DE}, {1.f, 0xF1F0EB}})
                             : Gradient(0, b.t, 0, b.b, {{0.f, 0xFFFFFF}, {0.85f, 0xECEBE6}, {1.f, 0xD6D0C5}});
        }
        if (fill) dc_->FillRoundedRectangle(rr, fill.Get());
        // Hot track (XP orange) or default ring (blue): a 2 px inner ring.
        if ((s.hot && !s.pressed) || s.isDefault) {
            auto ring = s.hot ? Gradient(0, b.t, 0, b.b, {{0.f, 0xFFF0CF}, {0.3f, 0xFFCF6B}, {1.f, 0xE5A01A}})
                              : Gradient(0, b.t, 0, b.b, {{0.f, 0xCEE7FF}, {0.4f, 0xA9C9FC}, {1.f, 0x6982EE}});
            if (ring) dc_->DrawRoundedRectangle({D2D1::RectF(b.l + 2, b.t + 2, b.r - 2, b.b - 2), 2, 2}, ring.Get(), 2.f);
        }
        dc_->DrawRoundedRectangle(rr, Brush(Rgb(green ? 0x1D5E1D : 0x003C74)));
        ink = green ? Rgb(0xFFFFFF) : Rgb(0x000000);
    }
    Text(font, label, b.Offset(shift, shift), ink, Align::Center, underline, false, true);
}

void Painter::Checkbox(const Box& box, bool checked, const ButtonState& s) {
    const D2D1_RECT_F r = D2D1::RectF(box.l + 0.5f, box.t + 0.5f, box.r - 0.5f, box.b - 0.5f);
    D2D1_COLOR_F checkInk;
    if (pal_.highContrast) {
        Fill(box, SysColor(COLOR_WINDOW));
        dc_->DrawRectangle(r, Brush(SysColor(s.disabled ? COLOR_GRAYTEXT : s.hot ? COLOR_HIGHLIGHT : COLOR_WINDOWTEXT)));
        checkInk = SysColor(s.disabled ? COLOR_GRAYTEXT : COLOR_WINDOWTEXT);
    } else {
        if (s.disabled) Fill(box, Rgb(0xFFFFFF));
        else if (s.pressed) {
            auto g = Gradient(box.l, box.t, box.r, box.b, {{0.f, 0xB0B0A7}, {1.f, 0xE3E1D4}});
            if (g) dc_->FillRectangle(r, g.Get());
        } else {
            auto g = Gradient(box.l, box.t, box.r, box.b, {{0.f, 0xDCDCD7}, {1.f, 0xFFFFFF}});
            if (g) dc_->FillRectangle(r, g.Get());
        }
        if (s.hot && !s.disabled && !s.pressed) {
            auto ring = Gradient(0, box.t, 0, box.b, {{0.f, 0xFFF0CF}, {1.f, 0xF8B330}});
            if (ring) dc_->DrawRectangle(D2D1::RectF(box.l + 2, box.t + 2, box.r - 2, box.b - 2), ring.Get(), 2.f);
        }
        dc_->DrawRectangle(r, Brush(Rgb(s.disabled ? 0xCAC8BB : 0x1C5180)));
        checkInk = s.disabled ? Rgb(0xCAC8BB) : Rgb(0x21A121); // XP checks are green.
    }
    if (!checked) return;
    ComPtr<ID2D1PathGeometry> path;
    factory_->CreatePathGeometry(&path);
    ComPtr<ID2D1GeometrySink> sink;
    if (!path || FAILED(path->Open(&sink))) return;
    const float u = box.W() / 13.f;
    sink->BeginFigure(D2D1::Point2F(box.l + 3.2f * u, box.t + 6.2f * u), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine(D2D1::Point2F(box.l + 5.4f * u, box.t + 8.6f * u));
    sink->AddLine(D2D1::Point2F(box.l + 9.9f * u, box.t + 3.9f * u));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    ComPtr<ID2D1StrokeStyle> round;
    factory_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                            D2D1_LINE_JOIN_ROUND),
                                nullptr, 0, &round);
    dc_->DrawGeometry(path.Get(), Brush(checkInk), 2.2f * u, round.Get());
}

void Painter::Radio(const Box& box, bool selected, const ButtonState& s, float dotScale) {
    const float cx = (box.l + box.r) / 2, cy = (box.t + box.b) / 2, rad = box.W() / 2 - 0.5f;
    const D2D1_ELLIPSE outer = D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad);
    if (pal_.highContrast) {
        dc_->FillEllipse(outer, Brush(SysColor(COLOR_WINDOW)));
        dc_->DrawEllipse(outer, Brush(SysColor(s.disabled ? COLOR_GRAYTEXT : s.hot ? COLOR_HIGHLIGHT : COLOR_WINDOWTEXT)));
        if (selected) {
            const float d = rad * 0.45f * dotScale;
            dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), d, d), Brush(SysColor(s.disabled ? COLOR_GRAYTEXT : COLOR_WINDOWTEXT)));
        }
        return;
    }
    if (s.disabled) dc_->FillEllipse(outer, Brush(Rgb(0xFFFFFF)));
    else {
        auto g = s.pressed ? Gradient(box.l, box.t, box.r, box.b, {{0.f, 0xB0B0A7}, {1.f, 0xE3E1D4}})
                           : Gradient(box.l, box.t, box.r, box.b, {{0.f, 0xDCDCD7}, {1.f, 0xFFFFFF}});
        if (g) dc_->FillEllipse(outer, g.Get());
    }
    if (s.hot && !s.disabled && !s.pressed) {
        auto ring = Gradient(0, box.t, 0, box.b, {{0.f, 0xFFF0CF}, {1.f, 0xF8B330}});
        if (ring) dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad - 1.5f, rad - 1.5f), ring.Get(), 2.f);
    }
    dc_->DrawEllipse(outer, Brush(Rgb(s.disabled ? 0xCAC8BB : 0x1C5180)));
    if (selected && dotScale > 0) { // Green dot #7BD87B → #21A121.
        const float d = rad * 0.46f * dotScale;
        if (s.disabled) {
            dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), d, d), Brush(Rgb(0xCAC8BB)));
        } else {
            auto dot = Gradient(cx - d, cy - d, cx + d, cy + d, {{0.f, 0x7BD87B}, {1.f, 0x21A121}});
            if (dot) dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), d, d), dot.Get());
        }
    }
}

void Painter::GroupHeader(const Box& b, const std::wstring& title, bool hero, Chevron chevron, float turn, bool hot) {
    auto shape = TopRounded(b, 4);
    if (pal_.highContrast) {
        dc_->FillGeometry(shape.Get(), Brush(SysColor(hero ? COLOR_HIGHLIGHT : COLOR_BTNFACE)));
        dc_->DrawGeometry(shape.Get(), Brush(SysColor(COLOR_WINDOWTEXT)));
    } else {
        // §3.2 GroupHeader #FFFFFF → #C6D3F7 (title #215DC6), GroupHeaderHero #0055E5 → #2463D6 (white title).
        auto fill = hero ? Gradient(b.l, 0, b.r, 0, {{0.f, 0x0055E5}, {1.f, 0x2463D6}})
                         : Gradient(b.l, 0, b.r, 0, {{0.f, 0xFFFFFF}, {1.f, 0xC6D3F7}});
        if (fill) dc_->FillGeometry(shape.Get(), fill.Get());
    }
    const D2D1_COLOR_F titleInk = pal_.highContrast ? SysColor(hero ? COLOR_HIGHLIGHTTEXT : COLOR_BTNTEXT)
                                  : hero            ? Rgb(0xFFFFFF)
                                  : hot             ? Rgb(0x428EFF)
                                                    : Rgb(0x215DC6);
    const float chevronSpace = chevron == Chevron::None ? 0.f : 24.f;
    Text(Font::BodyBold, title, Box{b.l + 12, b.t, b.r - chevronSpace - 4, b.b}, titleInk, Align::Leading, -1, true, true);
    if (chevron == Chevron::None) return;

    // The round XP chevron button with a double chevron, rotating as the group opens and closes.
    const float cx = b.r - 14, cy = (b.t + b.b) / 2, rad = 8.5f;
    const D2D1_ELLIPSE circle = D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad);
    D2D1_COLOR_F ink;
    if (pal_.highContrast) {
        dc_->DrawEllipse(circle, Brush(titleInk));
        ink = titleInk;
    } else if (hero) {
        dc_->FillEllipse(circle, Brush(Rgb(0xFFFFFF, hot ? 0.3f : 0.15f)));
        dc_->DrawEllipse(circle, Brush(Rgb(0xFFFFFF, 0.85f)));
        ink = Rgb(0xFFFFFF);
    } else {
        auto g = Gradient(0, cy - rad, 0, cy + rad, {{0.f, 0xFFFFFF}, {1.f, hot ? 0xFFE7B0u : 0xD9E2F9u}});
        if (g) dc_->FillEllipse(circle, g.Get());
        dc_->DrawEllipse(circle, Brush(Rgb(hot ? 0xE5A01A : 0xA3B4E6)));
        ink = Rgb(hot ? 0x428EFF : 0x215DC6);
    }
    D2D1_MATRIX_3X2_F saved;
    dc_->GetTransform(&saved);
    dc_->SetTransform(D2D1::Matrix3x2F::Rotation(180.f * turn, D2D1::Point2F(cx, cy)) * saved);
    for (float dy : {-2.5f, 1.5f}) { // Two stacked "^" chevrons.
        Line(cx - 3.2f, cy + dy + 2.f, cx, cy + dy - 1.2f, ink, 1.6f);
        Line(cx, cy + dy - 1.2f, cx + 3.2f, cy + dy + 2.f, ink, 1.6f);
    }
    dc_->SetTransform(saved);
}

void Painter::GroupBody(const Box& b, bool hero) {
    if (pal_.highContrast) {
        Fill(b, SysColor(COLOR_WINDOW));
        Frame(b, SysColor(COLOR_WINDOWTEXT));
        return;
    }
    Fill(b, hero ? Rgb(0xEEF3FF) : Rgb(0xD6DFF7));
    // White 1 px border on the sides and bottom (the header covers the top).
    Line(b.l + 0.5f, b.t, b.l + 0.5f, b.b, Rgb(0xFFFFFF));
    Line(b.r - 0.5f, b.t, b.r - 0.5f, b.b, Rgb(0xFFFFFF));
    Line(b.l, b.b - 0.5f, b.r, b.b - 0.5f, Rgb(0xFFFFFF));
}

void Painter::ProgressTrack(const Box& b) {
    const D2D1_ROUNDED_RECT rr = {D2D1::RectF(b.l + 0.5f, b.t + 0.5f, b.r - 0.5f, b.b - 0.5f), 3, 3};
    dc_->FillRoundedRectangle(rr, Brush(pal_.highContrast ? SysColor(COLOR_WINDOW) : Rgb(0xFFFFFF)));
    dc_->DrawRoundedRectangle(rr, Brush(pal_.highContrast ? SysColor(COLOR_WINDOWTEXT) : Rgb(0xACA899)));
}

namespace {
Box Interior(const Box& b) { return Box{b.l + 3, b.t + 2.5f, b.r - 3, b.b - 2.5f}; }
} // namespace

void Painter::ProgressMarquee(const Box& b, float offset) {
    ProgressTrack(b);
    const Box in = Interior(b);
    dc_->PushAxisAlignedClip(D2D1::RectF(in.l, in.t, in.r, in.b), D2D1_ANTIALIAS_MODE_ALIASED);
    for (int i = 0; i < 3; ++i) {
        const float x = in.l + offset + i * (kChunk + kChunkGap);
        const Box chunk = {x, in.t, x + kChunk, in.b};
        if (pal_.highContrast) Fill(chunk, SysColor(COLOR_HIGHLIGHT));
        else VGradient(chunk, {{0.f, 0xE2F8E2}, {0.45f, 0x6FD86F}, {0.55f, 0x2DB52D}, {1.f, 0x5ACD5A}});
    }
    dc_->PopAxisAlignedClip();
}

void Painter::ProgressChunks(const Box& b, int chunks) {
    ProgressTrack(b);
    const Box in = Interior(b);
    dc_->PushAxisAlignedClip(D2D1::RectF(in.l, in.t, in.r, in.b), D2D1_ANTIALIAS_MODE_ALIASED);
    for (int i = 0; i < chunks; ++i) {
        const float x = in.l + i * (kChunk + kChunkGap);
        const Box chunk = {x, in.t, std::min(x + kChunk, in.r), in.b};
        if (pal_.highContrast) Fill(chunk, SysColor(COLOR_HIGHLIGHT));
        else VGradient(chunk, {{0.f, 0xE2F8E2}, {0.45f, 0x6FD86F}, {0.55f, 0x2DB52D}, {1.f, 0x5ACD5A}});
    }
    dc_->PopAxisAlignedClip();
}

void Painter::LivePill(const Box& b, float glow, float opacity) {
    const float rad = b.H() / 2;
    const D2D1_ROUNDED_RECT rr = {D2D1::RectF(b.l + 0.5f, b.t + 0.5f, b.r - 0.5f, b.b - 0.5f), rad, rad};
    if (pal_.highContrast) {
        dc_->FillRoundedRectangle(rr, Brush(SysColor(COLOR_HIGHLIGHT)));
        Text(Font::BodyBold, L"LIVE", b, SysColor(COLOR_HIGHLIGHTTEXT), Align::Center, -1, false, true);
        return;
    }
    auto fill = Gradient(0, b.t, 0, b.b, {{0.f, 0x4DB84D, opacity}, {0.5f, 0x3FAA3F, opacity}, {1.f, 0x2A7F2A, opacity}});
    if (fill) dc_->FillRoundedRectangle(rr, fill.Get());
    auto top = Gradient(0, b.t, 0, (b.t + b.b) / 2, {{0.f, 0xFFFFFF, 0.35f * opacity}, {1.f, 0xFFFFFF, 0.f}});
    if (top) dc_->FillRoundedRectangle({D2D1::RectF(b.l + 1.5f, b.t + 1.5f, b.r - 1.5f, (b.t + b.b) / 2), rad - 1, rad - 1}, top.Get());
    if (glow >= 0 && glow <= 1) { // The one-shot gloss sweep when streaming starts (§6.3).
        const float x = b.l - b.W() * 0.6f + (b.W() * 2.2f) * glow;
        auto band = Gradient(x - 14, 0, x + 14, 0, {{0.f, 0xFFFFFF, 0.f}, {0.5f, 0xFFFFFF, 0.75f * opacity}, {1.f, 0xFFFFFF, 0.f}});
        if (band) dc_->FillRoundedRectangle(rr, band.Get());
    }
    dc_->DrawRoundedRectangle(rr, Brush(Rgb(0x1D5E1D, opacity)));
    Text(Font::BodyBold, L"LIVE", b, Rgb(0xFFFFFF, opacity), Align::Center, -1, false, true);
}

void Painter::Badge(float cx, float cy, float radius, BadgeKind kind, float scale) {
    if (kind == BadgeKind::None || scale <= 0) return;
    const float r = radius * scale;
    const D2D1_ELLIPSE e = D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r);
    if (pal_.highContrast) {
        dc_->FillEllipse(e, Brush(SysColor(COLOR_WINDOWTEXT)));
    } else {
        dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r + 2 * scale, r + 2 * scale), Brush(Rgb(0xFFFFFF)));
        auto fill = kind == BadgeKind::Play  ? Gradient(cx - r, cy - r, cx + r, cy + r, {{0.f, 0x7BD87B}, {1.f, 0x1E8E1E}})
                  : kind == BadgeKind::Pause ? Gradient(cx - r, cy - r, cx + r, cy + r, {{0.f, 0xFFD86A}, {1.f, 0xE08A00}})
                                             : Gradient(cx - r, cy - r, cx + r, cy + r, {{0.f, 0xFF7B6B}, {1.f, 0xC81E0F}});
        if (fill) dc_->FillEllipse(e, fill.Get());
        auto gloss = Gradient(0, cy - r, 0, cy, {{0.f, 0xFFFFFF, 0.55f}, {1.f, 0xFFFFFF, 0.f}});
        if (gloss) dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy - r * 0.35f), r * 0.75f, r * 0.55f), gloss.Get());
    }
    const D2D1_COLOR_F ink = pal_.highContrast ? SysColor(COLOR_WINDOW) : Rgb(0xFFFFFF);
    if (kind == BadgeKind::Play) {
        ComPtr<ID2D1PathGeometry> tri;
        factory_->CreatePathGeometry(&tri);
        ComPtr<ID2D1GeometrySink> sink;
        if (tri && SUCCEEDED(tri->Open(&sink))) {
            sink->BeginFigure(D2D1::Point2F(cx - r * 0.3f, cy - r * 0.45f), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(D2D1::Point2F(cx + r * 0.5f, cy));
            sink->AddLine(D2D1::Point2F(cx - r * 0.3f, cy + r * 0.45f));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            dc_->FillGeometry(tri.Get(), Brush(ink));
        }
    } else if (kind == BadgeKind::Pause) {
        Fill(Box{cx - r * 0.42f, cy - r * 0.45f, cx - r * 0.12f, cy + r * 0.45f}, ink);
        Fill(Box{cx + r * 0.12f, cy - r * 0.45f, cx + r * 0.42f, cy + r * 0.45f}, ink);
    } else {
        const float d = r * 0.38f;
        Line(cx - d, cy - d, cx + d, cy + d, ink, r * 0.28f);
        Line(cx + d, cy - d, cx - d, cy + d, ink, r * 0.28f);
    }
}

void Painter::FocusRect(const Box& b) {
    ComPtr<ID2D1StrokeStyle> dots;
    factory_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                            D2D1_LINE_JOIN_MITER, 10.f, D2D1_DASH_STYLE_DOT),
                                nullptr, 0, &dots);
    dc_->DrawRectangle(D2D1::RectF(b.l + 0.5f, b.t + 0.5f, b.r - 0.5f, b.b - 0.5f), Brush(pal_.focus), 1.f, dots.Get());
}

Box CloseButtonBox(float width) { return ui::MakeBox(width - 5 - 21, 5, 21, 21); }
Box MinimizeButtonBox(float width) { return ui::MakeBox(width - 5 - 21 - 2 - 21, 5, 21, 21); }

void WindowChrome(Painter& p, float w, float h, const std::wstring& caption, bool active, ID2D1Bitmap* icon) {
    ID2D1DeviceContext* dc = p.Dc();
    const bool hc = p.HighContrast();
    // Borders: everything below the rounded corners, then the title bar over the top.
    const Box frame = {0, kCornerRadius, w, h};
    if (hc) {
        p.Fill(frame, SysColor(active ? COLOR_ACTIVEBORDER : COLOR_INACTIVEBORDER));
        p.Frame(frame, SysColor(COLOR_WINDOWFRAME));
    } else {
        p.Fill(frame, Rgb(active ? 0x0B4BD9 : 0x7C98E2));
        p.Frame(frame, Rgb(active ? 0x002EA8 : 0x6A86D6));
        p.Frame(frame.Inset(1, 1), Rgb(active ? 0x2A6DF0 : 0x93ACEA));
    }
    p.TitleBar(Box{0, 0, w, kTitleBarH}, active, kCornerRadius);
    float x = 8;
    if (icon) {
        dc->DrawBitmap(icon, D2D1::RectF(x, 7, x + 16, 23), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        x += 16 + 6;
    }
    const Box text = {x, 0, w - 60, kTitleBarH};
    if (!hc) p.Text(Font::Caption, caption, text.Offset(1, 1), Rgb(0x0A1E78, active ? 0.9f : 0.4f), Align::Leading, -1, true, true);
    p.Text(Font::Caption, caption, text, hc ? SysColor(active ? COLOR_CAPTIONTEXT : COLOR_INACTIVECAPTIONTEXT) : Rgb(0xFFFFFF),
           Align::Leading, -1, true, true);
}

void Painter::SunkenFrame(const Box& b) {
    // XP's list/preview border (#7F9DB9), with a hairline shadow inside.
    Frame(b, pal_.highContrast ? SysColor(COLOR_WINDOWTEXT) : Rgb(0x7F9DB9));
}

// --- v2 flat components (docs/redesign-v2.md §4) -------------------------------------------------------

namespace {
float Clamp01f(float t) { return t < 0 ? 0.f : t > 1 ? 1.f : t; }
float Mix1(float a, float b, float t) { return a + (b - a) * t; }
D2D1_COLOR_F Mix(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
    t = Clamp01f(t);
    return D2D1::ColorF(Mix1(a.r, b.r, t), Mix1(a.g, b.g, t), Mix1(a.b, b.b, t), Mix1(a.a, b.a, t));
}
} // namespace

void Painter::RoundRect(const Box& b, float r, D2D1_COLOR_F fill) {
    dc_->FillRoundedRectangle({D2D1::RectF(b.l, b.t, b.r, b.b), r, r}, Brush(fill));
}

void Painter::RoundFrame(const Box& b, float r, D2D1_COLOR_F c, float w) {
    const float h = w / 2;
    dc_->DrawRoundedRectangle({D2D1::RectF(b.l + h, b.t + h, b.r - h, b.b - h), r, r}, Brush(c), w);
}

void Painter::Card(const Box& b, float radius, float shadow) {
    if (!pal_.highContrast && shadow > 0) {
        // Soft shadow: a few stacked translucent rounded rectangles, slightly below the card.
        for (int i = 4; i >= 1; --i) {
            const float sp = float(i);
            RoundRect({b.l + sp * 0.4f, b.t + sp * 0.7f, b.r - sp * 0.4f, b.b + sp * 0.5f}, radius + sp,
                      Rgb(0x0A1A33, 0.030f * shadow));
        }
    }
    RoundRect(b, radius, pal_.card);
    RoundFrame(b, radius, pal_.cardBorder);
}

void Painter::FlatButton(const Box& b, const std::wstring& label, int underline, const ButtonState& s, FlatStyle style,
                         Font font) {
    constexpr float r = 6;
    const float p = s.pressed ? 1.f : Clamp01f(s.press);
    D2D1_COLOR_F ink;
    if (pal_.highContrast) {
        RoundRect(b, r, SysColor(s.hot && !s.disabled ? COLOR_HIGHLIGHT : COLOR_BTNFACE));
        RoundFrame(b, r, SysColor(s.disabled ? COLOR_GRAYTEXT : COLOR_BTNTEXT), s.isDefault ? 2.f : 1.f);
        ink = SysColor(s.disabled ? COLOR_GRAYTEXT : s.hot ? COLOR_HIGHLIGHTTEXT : COLOR_BTNTEXT);
    } else if (style == FlatStyle::Primary) {
        const D2D1_COLOR_F rest = s.hot ? pal_.accentHot : pal_.accent;
        RoundRect(b, r, s.disabled ? Rgb(0xBED3F3) : Mix(rest, pal_.accentDown, p));
        ink = s.disabled ? Rgb(0xF2F6FD) : pal_.accentText;
    } else {
        const bool ghost = style == FlatStyle::Ghost;
        const D2D1_COLOR_F rest = ghost ? Rgb(0xFFFFFF, 0.f) : pal_.ctlFill;
        RoundRect(b, r, Mix(s.hot && !s.disabled ? Rgb(0xF2F6FC) : rest, Rgb(0xE7EDF6), p));
        if (!ghost) RoundFrame(b, r, s.disabled ? pal_.cardBorder : s.hot ? pal_.ctlHotBorder : pal_.ctlBorder);
        ink = s.disabled ? pal_.disabledText : pal_.text;
    }
    Text(font, label, b, ink, Align::Center, underline, true, true);
}

void Painter::NavPill(const Box& b, float opacity) {
    const D2D1_COLOR_F c = pal_.highContrast ? SysColor(COLOR_HIGHLIGHT) : pal_.accent;
    RoundRect(b, 7, D2D1::ColorF(c.r, c.g, c.b, c.a * Clamp01f(opacity)));
}

void Painter::Combo(const Box& b, const std::wstring& text, const ButtonState& s, bool open) {
    constexpr float r = 6;
    RoundRect(b, r, s.disabled && !pal_.highContrast ? Rgb(0xF5F7FA) : pal_.ctlFill);
    RoundFrame(b, r,
               s.disabled ? pal_.cardBorder : open ? pal_.accent : s.hot ? pal_.ctlHotBorder : pal_.ctlBorder,
               open ? 1.6f : 1.f);
    const D2D1_COLOR_F ink = s.disabled ? pal_.disabledText : pal_.text;
    Text(Font::Body, text, {b.l + 10, b.t, b.r - 26, b.b}, ink, Align::Leading, -1, true, true);
    // Chevron: down when closed, up when open.
    const float cx = b.r - 15, cy = (b.t + b.b) / 2, d = 3.6f, dir = open ? -1.f : 1.f;
    const D2D1_COLOR_F chev = s.disabled ? pal_.disabledText : pal_.subtle;
    Line(cx - d, cy - d * 0.55f * dir, cx, cy + d * 0.55f * dir, chev, 1.6f);
    Line(cx + d, cy - d * 0.55f * dir, cx, cy + d * 0.55f * dir, chev, 1.6f);
}

void Painter::DropPanel(const Box& b) {
    if (!pal_.highContrast) {
        for (int i = 5; i >= 1; --i) {
            const float sp = float(i);
            RoundRect({b.l + sp * 0.3f, b.t + sp * 0.6f, b.r - sp * 0.3f, b.b + sp * 0.8f}, 7 + sp, Rgb(0x0A1A33, 0.045f));
        }
    }
    RoundRect(b, 7, pal_.ctlFill);
    RoundFrame(b, 7, pal_.highContrast ? pal_.ctlBorder : Rgb(0xCFD8E5));
}

void Painter::DropRow(const Box& b, bool selected, bool hot) {
    if (pal_.highContrast) {
        if (hot) RoundRect(b, 4, SysColor(COLOR_HIGHLIGHT));
        else if (selected) RoundFrame(b, 4, SysColor(COLOR_WINDOWTEXT));
        return;
    }
    if (hot) RoundRect(b, 4, pal_.accent);
    else if (selected) RoundRect(b, 4, Rgb(0xEAF1FD));
}

void Painter::Switch(const Box& t, float on, const ButtonState& s) {
    on = Clamp01f(on);
    const float r = t.H() / 2;
    if (pal_.highContrast) {
        RoundRect(t, r, on > 0.5f ? SysColor(COLOR_HIGHLIGHT) : SysColor(COLOR_WINDOW));
        RoundFrame(t, r, SysColor(s.disabled ? COLOR_GRAYTEXT : COLOR_WINDOWTEXT));
    } else {
        const D2D1_COLOR_F onColor = s.disabled ? Rgb(0xBED3F3) : s.hot ? pal_.accentHot : pal_.accent;
        const D2D1_COLOR_F offColor = s.disabled ? Rgb(0xEAEEF4) : pal_.trackOff;
        RoundRect(t, r, Mix(offColor, onColor, on));
        // The off state needs a visible border, not just colour (§4, accessibility).
        RoundFrame(t, r, Mix(s.disabled ? pal_.cardBorder : pal_.trackOffBorder, onColor, on));
    }
    const float d = t.H() - 6;
    const float cx = Mix1(t.l + 3 + d / 2, t.r - 3 - d / 2, on), cy = (t.t + t.b) / 2;
    if (!pal_.highContrast) {
        dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy + 0.7f), d / 2, d / 2), Brush(Rgb(0x0A1A33, 0.20f)));
        dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), d / 2, d / 2), Brush(Rgb(0xFFFFFF)));
    } else {
        dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), d / 2, d / 2), Brush(SysColor(COLOR_BTNFACE)));
        dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), d / 2, d / 2), Brush(SysColor(COLOR_WINDOWTEXT)));
    }
}

void Painter::SliderRail(const Box& rail, float fraction, const ButtonState& s) {
    const float f = Clamp01f(fraction), r = rail.H() / 2;
    const D2D1_COLOR_F fill = s.disabled ? Rgb(0xBED3F3) : s.hot || s.pressed ? pal_.accentHot : pal_.accent;
    if (pal_.highContrast) {
        RoundRect(rail, r, SysColor(COLOR_WINDOW));
        RoundFrame(rail, r, SysColor(COLOR_WINDOWTEXT));
        if (f > 0) RoundRect({rail.l, rail.t, rail.l + rail.W() * f, rail.b}, r, SysColor(COLOR_HIGHLIGHT));
    } else {
        RoundRect(rail, r, s.disabled ? Rgb(0xEAEEF4) : pal_.trackOff);
        if (f > 0) RoundRect({rail.l, rail.t, rail.l + rail.W() * f, rail.b}, r, fill);
    }
    const float cx = rail.l + rail.W() * f, cy = (rail.t + rail.b) / 2, tr = s.pressed ? 7.5f : 7.f;
    if (pal_.highContrast) {
        dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), tr, tr), Brush(SysColor(COLOR_BTNFACE)));
        dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), tr, tr), Brush(SysColor(COLOR_WINDOWTEXT)));
        return;
    }
    dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy + 0.8f), tr, tr), Brush(Rgb(0x0A1A33, 0.18f)));
    dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), tr, tr), Brush(Rgb(0xFFFFFF)));
    dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), tr - 0.9f, tr - 0.9f), Brush(fill), 1.8f);
}

void Painter::DarkPill(const Box& b, const std::wstring& text, Font font) {
    const float r = std::min(b.H() / 2, 11.f);
    if (pal_.highContrast) {
        RoundRect(b, r, SysColor(COLOR_WINDOW));
        RoundFrame(b, r, SysColor(COLOR_WINDOWTEXT));
        Text(font, text, b, SysColor(COLOR_WINDOWTEXT), Align::Center, -1, true, true);
        return;
    }
    RoundRect(b, r, Rgb(0x000000, 0.70f));
    Text(font, text, b, Rgb(0xFFFFFF), Align::Center, -1, true, true);
}

void Painter::FocusRing(const Box& b, float radius) {
    const D2D1_COLOR_F c = pal_.highContrast ? SysColor(COLOR_WINDOWTEXT) : pal_.accent;
    RoundFrame(b, radius, D2D1::ColorF(c.r, c.g, c.b, pal_.highContrast ? 1.f : 0.85f), 2.f);
}

void Painter::Icon(xp::Glyph g, const Box& b, D2D1_COLOR_F ink, float w) {
    const float cx = (b.l + b.r) / 2, cy = (b.t + b.b) / 2, u = std::min(b.W(), b.H()) / 24.f;
    auto P = [&](float x, float y) { return D2D1::Point2F(cx + (x - 12) * u, cy + (y - 12) * u); };
    auto line = [&](float x0, float y0, float x1, float y1) { dc_->DrawLine(P(x0, y0), P(x1, y1), Brush(ink), w); };
    auto box = [&](float x0, float y0, float x1, float y1, float r) {
        const D2D1_POINT_2F a = P(x0, y0), c = P(x1, y1);
        dc_->DrawRoundedRectangle({D2D1::RectF(a.x, a.y, c.x, c.y), r * u, r * u}, Brush(ink), w);
    };
    auto solidBox = [&](float x0, float y0, float x1, float y1) {
        const D2D1_POINT_2F a = P(x0, y0), c = P(x1, y1);
        dc_->FillRectangle(D2D1::RectF(a.x, a.y, c.x, c.y), Brush(ink));
    };
    auto circle = [&](float x, float y, float r, bool fill) {
        const D2D1_ELLIPSE e = D2D1::Ellipse(P(x, y), r * u, r * u);
        if (fill) dc_->FillEllipse(e, Brush(ink));
        else dc_->DrawEllipse(e, Brush(ink), w);
    };
    auto poly = [&](std::initializer_list<D2D1_POINT_2F> pts, bool fill, bool close) {
        ComPtr<ID2D1PathGeometry> path;
        if (!factory_) return;
        factory_->CreatePathGeometry(&path);
        ComPtr<ID2D1GeometrySink> sink;
        if (!path || FAILED(path->Open(&sink))) return;
        auto it = pts.begin();
        sink->BeginFigure(*it++, fill ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
        for (; it != pts.end(); ++it) sink->AddLine(*it);
        sink->EndFigure(close ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
        sink->Close();
        if (fill) dc_->FillGeometry(path.Get(), Brush(ink));
        else dc_->DrawGeometry(path.Get(), Brush(ink), w);
    };
    auto arc = [&](float x0, float y0, float x1, float y1, float r, bool large) {
        ComPtr<ID2D1PathGeometry> path;
        if (!factory_) return;
        factory_->CreatePathGeometry(&path);
        ComPtr<ID2D1GeometrySink> sink;
        if (!path || FAILED(path->Open(&sink))) return;
        sink->BeginFigure(P(x0, y0), D2D1_FIGURE_BEGIN_HOLLOW);
        const D2D1_ARC_SEGMENT seg = {P(x1, y1), D2D1::SizeF(r * u, r * u), 0.f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                      large ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL};
        sink->AddArc(seg);
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        dc_->DrawGeometry(path.Get(), Brush(ink), w);
    };
    switch (g) {
    case xp::Glyph::Home:
        poly({P(3, 11.5f), P(12, 4), P(21, 11.5f)}, false, false);
        poly({P(5.8f, 10), P(5.8f, 20.2f), P(18.2f, 20.2f), P(18.2f, 10)}, false, false);
        break;
    case xp::Glyph::Devices:
        box(2.5f, 6, 11.5f, 15.5f, 1.5f);  // A small monitor …
        line(7, 15.5f, 7, 18.5f);
        line(4.5f, 18.5f, 9.5f, 18.5f);
        box(14, 4.5f, 21.5f, 19.5f, 2.f);  // … and a phone.
        line(16.5f, 17.2f, 19, 17.2f);
        break;
    case xp::Glyph::Camera:
        poly({P(8.5f, 7), P(10, 4.2f), P(14, 4.2f), P(15.5f, 7)}, false, false);
        box(3, 7, 21, 19.5f, 2.5f);
        circle(12, 13.2f, 3.6f, false);
        break;
    case xp::Glyph::Settings: {
        circle(12, 12, 3.2f, false);
        for (int i = 0; i < 8; ++i) {
            const float a = float(i) * 3.14159265f / 4;
            line(12 + std::cos(a) * 5.f, 12 + std::sin(a) * 5.f, 12 + std::cos(a) * 8.f, 12 + std::sin(a) * 8.f);
        }
        break;
    }
    case xp::Glyph::About:
        circle(12, 12, 8.6f, false);
        circle(12, 7.8f, 1.f, true);
        line(12, 11, 12, 16.6f);
        break;
    case xp::Glyph::Usb: // The USB trident.
        poly({P(12, 2.5f), P(14.6f, 6.8f), P(9.4f, 6.8f)}, true, true);
        line(12, 6.8f, 12, 19.5f);
        circle(12, 21, 1.7f, true);
        line(12, 13.5f, 7.5f, 10.4f);
        solidBox(5.6f, 7.2f, 9.2f, 10.8f);
        line(12, 16, 16.5f, 13);
        circle(17.4f, 11.6f, 1.7f, true);
        break;
    case xp::Glyph::Wifi: // Arcs over a dot.
        circle(12, 18.6f, 1.5f, true);
        for (float r : {4.6f, 8.4f, 12.2f}) arc(12 - r * 0.72f, 18.6f - r * 0.69f, 12 + r * 0.72f, 18.6f - r * 0.69f, r, false);
        break;
    case xp::Glyph::Phone:
        box(6.5f, 2.8f, 17.5f, 21.2f, 2.5f);
        line(10.2f, 18.6f, 13.8f, 18.6f);
        break;
    case xp::Glyph::Refresh:
        arc(12, 4.6f, 4.6f, 12, 7.4f, true);
        poly({P(11.6f, 1.6f), P(11.6f, 7.6f), P(15.8f, 4.6f)}, true, true);
        break;
    case xp::Glyph::Monitor:
        box(2.5f, 4.5f, 21.5f, 17, 2.f);
        line(12, 17, 12, 20);
        line(8, 20.2f, 16, 20.2f);
        break;
    case xp::Glyph::Pause:
        solidBox(8, 5, 10.8f, 19);
        solidBox(13.2f, 5, 16, 19);
        break;
    case xp::Glyph::Play:
        poly({P(8, 4.6f), P(19, 12), P(8, 19.4f)}, true, true);
        break;
    }
}

} // namespace mycam::xp
