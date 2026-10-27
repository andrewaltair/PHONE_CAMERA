# Linux build (docs/LINUX-SUPPORT.md). Included from CMakeLists.txt when not on Windows: mycam-companion (the
# headless daemon), the unit tests, and developer tools. The platform split is per file, never #ifdef in
# shared code: *_posix.cpp, *_openssl.cpp, *_ffmpeg.cpp and *_v4l2.cpp stand in for the Windows files.

find_package(OpenSSL 3 REQUIRED)
find_package(PkgConfig REQUIRED)
# Linked dynamically against the distro's FFmpeg: never vendored or static (MyCam is MIT, FFmpeg LGPL/GPL).
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)
# The distro's libusb (the vendored copy in third_party/ has only the Windows backends).
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)

# The video path: decoded phone frames into a v4l2loopback camera.
add_library(mycam_video STATIC companion/decoder_ffmpeg.cpp companion/frame_writer_v4l2.cpp vcam/frame_transform.cpp
    companion/log_posix.cpp companion/platform_posix.cpp)
target_link_libraries(mycam_video PUBLIC PkgConfig::FFMPEG)
target_compile_options(mycam_video PRIVATE -Wall -Wextra)

# The page files and status pictures, compiled in (companion.rc's RCDATA on Windows).
set(WEB_ASSETS ${CMAKE_CURRENT_BINARY_DIR}/web_assets.cpp)
file(GLOB WEB_FILES ${CMAKE_CURRENT_SOURCE_DIR}/companion/web/* ${CMAKE_CURRENT_SOURCE_DIR}/companion/res/frame_*.png
    ${CMAKE_CURRENT_SOURCE_DIR}/companion/res/tray_*.ico)
add_custom_command(OUTPUT ${WEB_ASSETS}
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${WEB_ASSETS}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_assets.cmake
    DEPENDS ${WEB_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/../design/assets/app-256.png
            ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_assets.cmake
    COMMENT "Embedding the iPhone page")

# The daemon: the same PhoneLink as the Windows tray app.
add_executable(mycam-companion
    companion/main_linux.cpp
    companion/control_posix.cpp
    companion/control_proto.cpp
    companion/status_text.cpp
    companion/phone_link.cpp
    companion/phone_link_usb_posix.cpp
    companion/phone_link_wifi.cpp
    companion/phone_link_web.cpp
    companion/web_server.cpp
    companion/web_proto.cpp
    companion/tls_server_openssl.cpp
    companion/wifi_crypto_openssl.cpp
    companion/wifi_crypto_common.cpp
    companion/packet_parser.cpp
    companion/marquee.cpp
    companion/status_images_ffmpeg.cpp
    third_party/qrcodegen/qrcodegen.cpp
    ${WEB_ASSETS})
target_include_directories(mycam-companion PRIVATE companion)
find_package(Threads REQUIRED)
target_link_libraries(mycam-companion PRIVATE mycam_video PkgConfig::LIBUSB OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
target_compile_options(mycam-companion PRIVATE -Wall)
install(TARGETS mycam-companion RUNTIME DESTINATION bin)

# The tray icon (phase 4a): its own program in the desktop session, talking to the daemon over its socket.
# Built when GTK 3 and AppIndicator are there (libgtk-3-dev libayatana-appindicator3-dev); CI and the .deb
# always have them.
pkg_check_modules(TRAY IMPORTED_TARGET gtk+-3.0 ayatana-appindicator3-0.1)
if(TRAY_FOUND)
    add_executable(mycam-tray companion/tray_linux.cpp companion/control_proto.cpp third_party/qrcodegen/qrcodegen.cpp
        ${WEB_ASSETS})
    target_include_directories(mycam-tray PRIVATE companion)
    target_link_libraries(mycam-tray PRIVATE PkgConfig::TRAY)
    target_compile_options(mycam-tray PRIVATE -Wall)
    install(TARGETS mycam-tray RUNTIME DESTINATION bin)
    install(FILES linux/mycam-tray.desktop DESTINATION /etc/xdg/autostart)
    install(FILES linux/mycam-tray.desktop DESTINATION share/applications RENAME mycam.desktop)
    install(FILES ../design/assets/app-256.png DESTINATION share/icons/hicolor/256x256/apps RENAME mycam.png)
else()
    message(STATUS "No GTK 3 / AppIndicator: building without mycam-tray")
endif()

option(MYCAM_BUILD_TOOLS "Build developer tools (v4l2_play)" ON)
if(MYCAM_BUILD_TOOLS)
    add_executable(v4l2_play tools/v4l2_play.cpp)
    target_link_libraries(v4l2_play PRIVATE mycam_video)
endif()

option(MYCAM_BUILD_TESTS "Build unit tests" ON)
if(MYCAM_BUILD_TESTS)
    enable_testing()
    add_executable(mycam_tests tests/tests.cpp companion/packet_parser.cpp companion/wifi_crypto_openssl.cpp
        companion/wifi_crypto_common.cpp companion/marquee.cpp companion/preview_convert.cpp companion/status_text.cpp
        companion/control_proto.cpp
        companion/web_proto.cpp vcam/frame_transform.cpp)
    target_link_libraries(mycam_tests PRIVATE OpenSSL::Crypto)
    target_compile_options(mycam_tests PRIVATE -Wall)
    target_compile_definitions(mycam_tests PRIVATE MYCAM_GOLDEN_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../protocol/golden.txt")
    add_test(NAME mycam_tests COMMAND mycam_tests)
    # The OpenSSL TLS server against an OpenSSL client, in memory.
    add_executable(tls_tests tests/tls_test.cpp companion/tls_server_openssl.cpp companion/log_posix.cpp)
    target_link_libraries(tls_tests PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    target_compile_options(tls_tests PRIVATE -Wall)
    add_test(NAME tls_tests COMMAND tls_tests)
    # The iPhone page's protocol code against the same golden vectors (needs Node.js; skipped without it).
    find_program(NODE_EXE node)
    if(NODE_EXE)
        add_test(NAME web_golden COMMAND ${NODE_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/webtest/golden_check.js
                 ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/golden.txt)
    endif()
endif()

# --- Packaging: cpack -G DEB (docs/LINUX-SUPPORT.md "Definition of done") ----------------------------------
# One version for Android and PC (CLAUDE.md): read it from the app, not a fourth copy.
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/../app/build.gradle.kts MYCAM_VERSION_LINE REGEX "versionName = \"[0-9.]+\"")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" MYCAM_VERSION "${MYCAM_VERSION_LINE}")

# USB access for the logged-in user, before the switch to accessory mode (the phone maker's vendor id, from
# kAndroidVendors[] in phone_link.cpp, the list the companion asks) and after it (Google, 18d1:2d00-2d05).
# Without the first kind, libusb can't open the phone to ask it to switch at all.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/companion/phone_link.cpp PHONE_LINK_SOURCE)
string(REGEX MATCH "kAndroidVendors\\[\\] = {[^}]*}" VENDOR_ARRAY "${PHONE_LINK_SOURCE}")
string(REGEX MATCHALL "0x[0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f]" VENDOR_IDS "${VENDOR_ARRAY}")
list(LENGTH VENDOR_IDS VENDOR_COUNT)
if(VENDOR_COUNT LESS 10)
    message(FATAL_ERROR "Could not read kAndroidVendors[] from phone_link.cpp (found ${VENDOR_COUNT})")
endif()
set(UDEV_RULES "# MyCam: lets the logged-in user's mycam-companion open Android phones over USB. Generated from\n# kAndroidVendors[] in pc/companion/phone_link.cpp by pc/linux.cmake.\n")
string(APPEND UDEV_RULES "# After the switch to accessory mode (IsAccessoryPid: 0x2D00-0x2D05).\nSUBSYSTEM==\"usb\", ATTR{idVendor}==\"18d1\", ATTR{idProduct}==\"2d0[0-5]\", TAG+=\"uaccess\"\n# Before it: the phone makers MyCam asks to switch.\n")
foreach(vid ${VENDOR_IDS})
    string(SUBSTRING "${vid}" 2 4 hex)
    string(TOLOWER "${hex}" hex)
    string(APPEND UDEV_RULES "SUBSYSTEM==\"usb\", ATTR{idVendor}==\"${hex}\", TAG+=\"uaccess\"\n")
endforeach()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/69-mycam.rules "${UDEV_RULES}")

set(CMAKE_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
    WORLD_READ WORLD_EXECUTE) # 755, whatever the builder's umask.
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/69-mycam.rules DESTINATION lib/udev/rules.d)
install(FILES linux/mycam.service DESTINATION lib/systemd/user)
install(FILES linux/modprobe.conf DESTINATION lib/modprobe.d RENAME mycam.conf)
install(FILES linux/modules-load.conf DESTINATION lib/modules-load.d RENAME mycam.conf)

set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME mycam)
set(CPACK_PACKAGE_VERSION ${MYCAM_VERSION})
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Use your phone as a webcam")
set(CPACK_PACKAGE_DESCRIPTION "MyCam turns an Android phone (USB or Wi-Fi) or an iPhone (Safari, Wi-Fi) into a webcam\nfor Zoom, OBS, Discord and browsers, through a v4l2loopback camera.")
set(CPACK_PACKAGE_CONTACT "MyCam <https://github.com/whoissaaif/mycam/issues>")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/whoissaaif/mycam")
set(CPACK_DEBIAN_PACKAGE_SECTION video)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON) # libavcodec, libusb, libssl...: the distro's, linked dynamically.
# v4l2loopback ships with some kernels (linux-modules-extra) and as DKMS elsewhere: recommended, and postinst
# says what to do when it's missing, rather than forcing a DKMS build that may not suit the kernel.
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "v4l2loopback-dkms")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA ${CMAKE_CURRENT_SOURCE_DIR}/linux/postinst ${CMAKE_CURRENT_SOURCE_DIR}/linux/prerm)
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)
include(CPack)
