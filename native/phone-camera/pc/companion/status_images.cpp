#include "status_images.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "../vcam/frame_transform.h"

using Microsoft::WRL::ComPtr;

namespace mycam {

Nv12Image LoadStatusImage(int resourceId) {
    Nv12Image image;
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
