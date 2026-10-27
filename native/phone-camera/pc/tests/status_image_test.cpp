#include <windows.h>
#include <objbase.h>

#include <string>
#include "../companion/marquee.h"
#include "../companion/status_images.h"

int wmain(int argc, wchar_t** argv) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const mycam::Nv12Image image = mycam::LoadStatusImage(mycam::kImageWaiting);
    int failures = 0;
    if (image.width != 1280 || image.height != 720) ++failures;
    if (image.data.size() != size_t(1280) * 720 * 3 / 2) ++failures;
    const mycam::MarqueeGeometry g = mycam::MarqueeGeometryFor(image.width, image.height);
    if (!g.valid() || g.x != 490 || g.y != 447 || g.w != 300 || g.h != 18) ++failures;
    if (!image.empty() && g.valid()) {
        const uint8_t* y = image.data.data();
        if (y[size_t(450) * image.width + 600] <= 200) ++failures;
        if (y[size_t(430) * image.width + 600] >= 180) ++failures;
        mycam::Nv12Image paused = image;
        mycam::DrawMarquee(paused, image, 2.2);
        if (paused.data != image.data) ++failures;

        std::wstring rawPath;
        if (argc == 3 && wcscmp(argv[1], L"--output") == 0) rawPath = argv[2];
        if (!rawPath.empty()) {
            HANDLE file = CreateFileW(rawPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) {
                ++failures;
            } else {
                DWORD written = 0;
                const BOOL wrote = WriteFile(file, image.data.data(), DWORD(image.data.size()), &written, nullptr);
                CloseHandle(file);
                if (!wrote || written != image.data.size()) ++failures;
            }
        } else if (argc != 1) {
            ++failures;
        }
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return failures ? 1 : 0;
}
