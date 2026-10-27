// capture_test.exe [out.bmp]: opens the "MyCam" camera like any app would, grabs a frame after
// ~1 second and saves it as a BMP. Used to verify the virtual camera without a video-call app.

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <stdio.h>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

static int Fail(const char* what, HRESULT hr) {
    fprintf(stderr, "%s failed: 0x%08X\n", what, unsigned(hr));
    return 1;
}

int wmain(int argc, wchar_t** argv) {
    const wchar_t* out = argc > 1 ? argv[1] : L"mycam_capture.bmp";
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 1);
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFEnumDeviceSources(attrs.Get(), &devices, &count);
    if (FAILED(hr)) return Fail("MFEnumDeviceSources", hr);

    ComPtr<IMFActivate> chosen;
    printf("Cameras:\n");
    for (UINT32 i = 0; i < count; ++i) {
        wchar_t* name = nullptr;
        UINT32 len = 0;
        devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len);
        printf("  %ls\n", name ? name : L"?");
        if (name && wcsncmp(name, L"MyCam", 5) == 0) chosen = devices[i];
        CoTaskMemFree(name);
    }
    for (UINT32 i = 0; i < count; ++i) devices[i]->Release();
    CoTaskMemFree(devices);
    if (!chosen) {
        fprintf(stderr, "MyCam camera not found (is MyCamCompanion running?)\n");
        return 2;
    }

    ComPtr<IMFMediaSource> source;
    hr = chosen->ActivateObject(IID_PPV_ARGS(&source));
    if (FAILED(hr)) return Fail("ActivateObject", hr);

    ComPtr<IMFAttributes> readerAttrs;
    MFCreateAttributes(&readerAttrs, 1);
    readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    ComPtr<IMFSourceReader> reader;
    hr = MFCreateSourceReaderFromMediaSource(source.Get(), readerAttrs.Get(), &reader);
    if (FAILED(hr)) return Fail("MFCreateSourceReaderFromMediaSource", hr);

    ComPtr<IMFMediaType> rgb;
    MFCreateMediaType(&rgb);
    rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, rgb.Get());
    if (FAILED(hr)) return Fail("SetCurrentMediaType(RGB32)", hr);
    ComPtr<IMFMediaType> actual;
    reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &actual);
    UINT32 w = 0, h = 0;
    MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &w, &h);

    ComPtr<IMFSample> last;
    int frames = 0;
    ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < 1500) {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, nullptr, &sample);
        if (FAILED(hr)) return Fail("ReadSample", hr);
        if (sample) { last = sample; ++frames; }
    }
    double fps = frames / ((GetTickCount64() - start) / 1000.0);
    printf("Got %d frames (%.1f fps) at %ux%u\n", frames, fps, w, h);
    if (!last) return 3;

    ComPtr<IMFMediaBuffer> buffer;
    last->ConvertToContiguousBuffer(&buffer);
    BYTE* data = nullptr;
    DWORD length = 0;
    buffer->Lock(&data, nullptr, &length);

    BITMAPFILEHEADER bf = {};
    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof(bi);
    bi.biWidth = LONG(w);
    bi.biHeight = -LONG(h); // Top-down.
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;
    bf.bfType = 0x4D42;
    bf.bfOffBits = sizeof(bf) + sizeof(bi);
    bf.bfSize = bf.bfOffBits + w * h * 4;
    FILE* f = _wfopen(out, L"wb");
    if (!f) return Fail("open output", E_FAIL);
    fwrite(&bf, sizeof(bf), 1, f);
    fwrite(&bi, sizeof(bi), 1, f);
    fwrite(data, 1, std::min<DWORD>(length, w * h * 4), f);
    fclose(f);
    buffer->Unlock();
    printf("Saved %ls\n", out);

    source->Shutdown();
    MFShutdown();
    CoUninitialize();
    return 0;
}
