cmake_minimum_required(VERSION 3.20)
project(MyCam LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Linux (docs/LINUX-SUPPORT.md) builds its own targets; everything below this is the Windows build.
if(NOT WIN32)
    include(linux.cmake)
    return()
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # Static CRT: no VC++ redist needed.
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
add_compile_options(/utf-8) # Sources are UTF-8 (status text uses · and ×).

# --- libusb (vendored, static) -------------------------------------------------------------------
set(LIBUSB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libusb)
add_library(usb STATIC
    ${LIBUSB_DIR}/libusb/core.c
    ${LIBUSB_DIR}/libusb/descriptor.c
    ${LIBUSB_DIR}/libusb/hotplug.c
    ${LIBUSB_DIR}/libusb/io.c
    ${LIBUSB_DIR}/libusb/strerror.c
    ${LIBUSB_DIR}/libusb/sync.c
    ${LIBUSB_DIR}/libusb/os/events_windows.c
    ${LIBUSB_DIR}/libusb/os/threads_windows.c
    ${LIBUSB_DIR}/libusb/os/windows_common.c
    ${LIBUSB_DIR}/libusb/os/windows_usbdk.c
    ${LIBUSB_DIR}/libusb/os/windows_winusb.c
)
target_include_directories(usb PUBLIC ${LIBUSB_DIR}/libusb PRIVATE ${LIBUSB_DIR}/msvc)
target_compile_options(usb PRIVATE /W0)

# --- Virtual camera media source (loaded by Windows Frame Server) --------------------------------
add_library(MyCamVCam SHARED
    vcam/dllmain.cpp
    vcam/media_source.cpp
    vcam/frame_reader.cpp
    vcam/frame_transform.cpp
    vcam/MyCamVCam.def
)
target_link_libraries(MyCamVCam PRIVATE mfplat mfuuid advapi32 runtimeobject)

# --- DirectShow virtual camera (Windows 10 fallback; see docs/WINDOWS10-SUPPORT.md) ---------------
add_library(MyCamVCamDShow SHARED
    vcam-dshow/dllmain.cpp
    vcam-dshow/filter.cpp
    vcam-dshow/output_pin.cpp
    vcam-dshow/dshow_media_types.cpp
    vcam-dshow/dshow_log.cpp
    vcam/frame_reader.cpp
    vcam/frame_transform.cpp
    vcam-dshow/MyCamVCamDShow.def
)
target_link_libraries(MyCamVCamDShow PRIVATE strmiids ole32 oleaut32 advapi32 runtimeobject)

# --- Companion tray app ---------------------------------------------------------------------------
add_executable(MyCamCompanion WIN32
    companion/main.cpp
    companion/phone_link.cpp
    companion/phone_link_usb_win.cpp
    companion/phone_link_wifi.cpp
    companion/platform_win.cpp
    companion/wifi_crypto.cpp
    companion/wifi_crypto_common.cpp
    companion/web_proto.cpp
    companion/web_server.cpp
    companion/tls_server.cpp
    companion/phone_link_web.cpp
    third_party/qrcodegen/qrcodegen.cpp
    companion/decoder.cpp
    companion/frame_writer.cpp
    companion/packet_parser.cpp
    companion/winusb_bind.cpp
    companion/driver_task.cpp
    companion/log.cpp
    companion/app_settings.cpp
    companion/status_text.cpp
    companion/status_images.cpp
    companion/marquee.cpp
    vcam/frame_transform.cpp
    companion/settings_window.cpp
    companion/pairing_dialog.cpp
    companion/uia_provider.cpp
    companion/xp_draw.cpp
    companion/preview_source.cpp
    companion/preview_convert.cpp
    companion/companion.rc
    companion/companion.manifest
)
target_link_libraries(MyCamCompanion PRIVATE usb mfplat mfuuid ole32 shell32 advapi32 setupapi newdev cfgmgr32
    comctl32 d2d1 dwrite windowscodecs dwmapi wtsapi32 taskschd ws2_32 iphlpapi bcrypt crypt32 ncrypt secur32 d3d11 dxgi dcomp dxguid uiautomationcore)

# --- Developer tools (not shipped; install.ps1 copies only the two binaries above) -------------------
option(MYCAM_BUILD_TOOLS "Build developer tools (capture_test, usb_probe, uia_dump, web_probe)" OFF)
if(MYCAM_BUILD_TOOLS)
    add_executable(capture_test tools/capture_test.cpp)
    target_link_libraries(capture_test PRIVATE mfplat mf mfreadwrite mfuuid ole32)
    add_executable(usb_probe tools/usb_probe.cpp)
    target_link_libraries(usb_probe PRIVATE usb)
    add_executable(web_probe tools/web_probe.cpp companion/companion.rc companion/web_server.cpp companion/web_proto.cpp companion/tls_server.cpp
        companion/wifi_crypto.cpp companion/wifi_crypto_common.cpp companion/packet_parser.cpp companion/platform_win.cpp)
    target_link_libraries(web_probe PRIVATE ws2_32 iphlpapi bcrypt crypt32 ncrypt secur32 ole32)
    add_executable(uia_dump tools/uia_dump.cpp)
    target_link_libraries(uia_dump PRIVATE ole32 oleaut32 uiautomationcore)
endif()

# --- Unit tests ------------------------------------------------------------------------------------
option(MYCAM_BUILD_TESTS "Build unit tests" ON)
if(MYCAM_BUILD_TESTS)
    enable_testing()
    add_executable(mycam_tests tests/tests.cpp companion/packet_parser.cpp companion/wifi_crypto.cpp companion/wifi_crypto_common.cpp
        companion/marquee.cpp
        companion/preview_convert.cpp companion/status_text.cpp companion/control_proto.cpp companion/web_proto.cpp vcam/frame_transform.cpp
        vcam-dshow/dshow_media_types.cpp companion/winusb_bind.cpp)
    target_link_libraries(mycam_tests PRIVATE bcrypt strmiids ole32 setupapi newdev cfgmgr32)
    target_compile_definitions(mycam_tests PRIVATE MYCAM_GOLDEN_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../protocol/golden.txt")
    add_test(NAME mycam_tests COMMAND mycam_tests)
    # The iPhone page's protocol code against the same golden vectors (needs Node.js; skipped without it).
    find_program(NODE_EXE node)
    if(NODE_EXE)
        add_test(NAME web_golden COMMAND ${NODE_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/webtest/golden_check.js
                 ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/golden.txt)
    endif()
endif()
