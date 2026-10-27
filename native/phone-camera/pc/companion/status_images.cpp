#include "status_images.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <string.h>
#include <vector>

#include "../vcam/frame_transform.h"

using Microsoft::WRL::ComPtr;

namespace mycam {

namespace {

void Fill(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    if (!brush) return;
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void RoundPanel(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 2, border);
    if (!brush || !pen) {
        if (brush) DeleteObject(brush);
        if (pen) DeleteObject(pen);
        return;
    }
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void Text(HDC dc, const wchar_t* text, const RECT& rect, int height, int weight, COLORREF color, UINT format) {
    HFONT font = CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    if (!font) return;
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    RECT copy = rect;
    DrawTextW(dc, text, -1, &copy, format);
    SelectObject(dc, oldFont);
    DeleteObject(font);
}

Nv12Image BuildWaitingFrame() {
    constexpr int kWidth = 1280, kHeight = 720;
    Nv12Image image;
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = kWidth;
    info.bmiHeader.biHeight = -kHeight; // top-down BGRA, matching BgraToNV12's row order.
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC dc = CreateCompatibleDC(nullptr);
    if (!bitmap || !dc || !bits) {
        if (dc) DeleteDC(dc);
        if (bitmap) DeleteObject(bitmap);
        return image;
    }
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    Fill(dc, {0, 0, kWidth, kHeight}, RGB(24, 49, 80));
    RoundPanel(dc, {120, 72, 1160, 648}, 24, RGB(43, 77, 116), RGB(103, 153, 202));
    RoundPanel(dc, {170, 106, 1110, 252}, 18, RGB(57, 95, 139), RGB(117, 167, 211));

    // A simple camera mark, deliberately drawn without the inherited MyCam artwork.
    HBRUSH logoBrush = CreateSolidBrush(RGB(226, 243, 255));
    HPEN logoPen = CreatePen(PS_SOLID, 3, RGB(226, 243, 255));
    if (logoBrush && logoPen) {
        HGDIOBJ oldBrush = SelectObject(dc, logoBrush), oldPen = SelectObject(dc, logoPen);
        Ellipse(dc, 205, 135, 275, 205);
        Rectangle(dc, 250, 153, 286, 187);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
    }
    if (logoPen) DeleteObject(logoPen);
    if (logoBrush) DeleteObject(logoBrush);
    Text(dc, L"PHONE_CAMERA", {325, 120, 1080, 190}, 50, FW_BOLD, RGB(245, 250, 255), DT_LEFT | DT_SINGLELINE);
    Text(dc, L"Waiting for your phone", {325, 182, 1080, 232}, 25, FW_NORMAL, RGB(215, 231, 246), DT_LEFT | DT_SINGLELINE);

    RoundPanel(dc, {260, 286, 1020, 620}, 18, RGB(31, 61, 96), RGB(89, 133, 177));
    Text(dc, L"Connect your phone to this PC", {315, 326, 965, 365}, 27, FW_SEMIBOLD, RGB(245, 250, 255),
         DT_CENTER | DT_SINGLELINE);
    Text(dc, L"Android: USB or Wi-Fi  |  iPhone: scan the QR code", {315, 372, 965, 415}, 20, FW_NORMAL,
         RGB(190, 216, 238), DT_CENTER | DT_SINGLELINE);

    // Keep this empty track exactly aligned with marquee.h's locked 1280x720 geometry.
    const RECT track = {490, 447, 790, 465};
    Fill(dc, track, RGB(248, 250, 252));
    HBRUSH trackBorder = CreateSolidBrush(RGB(172, 184, 198));
    if (trackBorder) {
        FrameRect(dc, &track, trackBorder);
        DeleteObject(trackBorder);
    }
    Text(dc, L"Waiting for connection...", {315, 505, 965, 550}, 18, FW_NORMAL,
         RGB(190, 216, 238), DT_CENTER | DT_SINGLELINE);

    std::vector<uint8_t> bgra(size_t(kWidth) * kHeight * 4);
    memcpy(bgra.data(), bits, bgra.size());
    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    image.width = kWidth;
    image.height = kHeight;
    image.data.resize(size_t(kWidth) * kHeight * 3 / 2);
    BgraToNV12(bgra.data(), kWidth * 4, kWidth, kHeight, image.data.data());
    return image;
}

} // namespace

Nv12Image LoadStatusImage(int resourceId) {
    Nv12Image image;
    if (resourceId == kImageWaiting) return BuildWaitingFrame();
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    HGLOBAL handle = res ? LoadResource(module, res) : nullptr;
    const void* bytes = handle ? LockResource(handle) : nullptr;
    const DWORD size = res ? SizeofResource(module, res) : 0;
    if (!bytes || !size) return image;

    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(static_cast<BYTE*>(const_cast<void*>(bytes)), size)) ||
        FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(wic->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom))) {
        return image;
    }
    UINT w = 0, h = 0;
    converter->GetSize(&w, &h);
    w &= ~1u;
    h &= ~1u;
    if (!w || !h) return image;
    std::vector<uint8_t> bgra(size_t(w) * h * 4);
    if (FAILED(converter->CopyPixels(nullptr, w * 4, UINT(bgra.size()), bgra.data()))) return image;

    image.width = w;
    image.height = h;
    image.data.resize(size_t(w) * h * 3 / 2);
    BgraToNV12(bgra.data(), w * 4, w, h, image.data.data());
    return image;
}

} // namespace mycam
