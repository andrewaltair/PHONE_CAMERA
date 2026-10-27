# Linux build (docs/LINUX-SUPPORT.md). Included from CMakeLists.txt when not on Windows: the PHONE_CAMERA companion (the
# headless daemon), the unit tests, and developer tools. The platform split is per file, never #ifdef in
# shared code: *_posix.cpp, *_openssl.cpp, *_ffmpeg.cpp and *_v4l2.cpp stand in for the Windows files.

find_package(OpenSSL 3 REQUIRED)
find_package(PkgConfig REQUIRED)
# Linked dynamically against the distro's FFmpeg: never vendored or static (PHONE_CAMERA is MIT, FFmpeg LGPL/GPL).
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)
# The distro's libusb (the vendored copy in third_party/ has only the Windows backends).
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)

# The video path: decoded phone frames into a v4l2loopback camera.
add_library(phone_camera_video STATIC companion/decoder_ffmpeg.cpp companion/frame_writer_v4l2.cpp vcam/frame_transform.cpp
    companion/log_posix.cpp companion/platform_posix.cpp companion/linux_device.cpp)
target_link_libraries(phone_camera_video PUBLIC PkgConfig::FFMPEG)
target_compile_options(phone_camera_video PRIVATE -Wall -Wextra)

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
add_executable(phone-camera-companion
    companion/main_linux.cpp
    companion/linux_control.cpp
    companion/linux_identity.cpp
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
    companion/linux_status_frame.cpp
    companion/status_images_ffmpeg.cpp
    third_party/qrcodegen/qrcodegen.cpp
    ${WEB_ASSETS})
target_include_directories(phone-camera-companion PRIVATE companion)
find_package(Threads REQUIRED)
target_link_libraries(phone_camera_video PUBLIC Threads::Threads)
target_link_libraries(phone-camera-companion PRIVATE phone_camera_video PkgConfig::LIBUSB OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
target_compile_options(phone-camera-companion PRIVATE -Wall)
install(TARGETS phone-camera-companion RUNTIME DESTINATION bin)

# The tray icon (phase 4a): its own program in the desktop session, talking to the daemon over its socket.
# Built when GTK 3 and AppIndicator are there (libgtk-3-dev libayatana-appindicator3-dev); CI and the .deb
# always have them.
pkg_check_modules(TRAY IMPORTED_TARGET gtk+-3.0 ayatana-appindicator3-0.1)
if(TRAY_FOUND)
    add_executable(phone-camera-tray companion/tray_linux.cpp companion/control_proto.cpp companion/linux_identity.cpp
        third_party/qrcodegen/qrcodegen.cpp
        ${WEB_ASSETS})
    target_include_directories(phone-camera-tray PRIVATE companion)
    target_link_libraries(phone-camera-tray PRIVATE PkgConfig::TRAY)
    target_compile_options(phone-camera-tray PRIVATE -Wall)
    install(TARGETS phone-camera-tray RUNTIME DESTINATION bin)
    install(FILES linux/phone-camera-tray.desktop DESTINATION share/applications RENAME phone-camera.desktop)
    install(FILES ../design/assets/app-256.png DESTINATION share/icons/hicolor/256x256/apps RENAME phone-camera.png)
else()
    message(STATUS "No GTK 3 / AppIndicator: building without phone-camera-tray")
endif()

option(MYCAM_BUILD_TOOLS "Build developer tools (v4l2_play)" ON)
if(MYCAM_BUILD_TOOLS)
    add_executable(phone_camera_v4l2_play tools/v4l2_play.cpp)
    target_link_libraries(phone_camera_v4l2_play PRIVATE phone_camera_video)
endif()

option(MYCAM_BUILD_TESTS "Build unit tests" ON)
if(MYCAM_BUILD_TESTS)
    enable_testing()
    add_executable(phone_camera_tests tests/tests.cpp companion/packet_parser.cpp companion/wifi_crypto_openssl.cpp
        companion/wifi_crypto_common.cpp companion/marquee.cpp companion/preview_convert.cpp companion/status_text.cpp
        companion/control_proto.cpp
        companion/web_proto.cpp vcam/frame_transform.cpp)
    target_link_libraries(phone_camera_tests PRIVATE OpenSSL::Crypto)
    target_compile_options(phone_camera_tests PRIVATE -Wall)
    target_compile_definitions(phone_camera_tests PRIVATE MYCAM_GOLDEN_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../protocol/golden.txt")
    add_test(NAME phone_camera_tests COMMAND phone_camera_tests)
    add_executable(phone_camera_linux_identity_test tests/linux_identity_test.cpp companion/linux_identity.cpp)
    target_compile_options(phone_camera_linux_identity_test PRIVATE -Wall -Wextra)
    add_test(NAME phone_camera_linux_identity COMMAND phone_camera_linux_identity_test)
    add_executable(phone_camera_linux_device_test tests/linux_device_test.cpp companion/linux_device.cpp)
    target_link_libraries(phone_camera_linux_device_test PRIVATE Threads::Threads)
    target_compile_options(phone_camera_linux_device_test PRIVATE -Wall -Wextra)
    add_test(NAME phone_camera_linux_device COMMAND phone_camera_linux_device_test)
    add_executable(phone_camera_linux_status_image_test tests/linux_status_image_test.cpp companion/linux_status_frame.cpp
        companion/marquee.cpp)
    target_compile_options(phone_camera_linux_status_image_test PRIVATE -Wall -Wextra)
    add_test(NAME phone_camera_linux_status_image COMMAND phone_camera_linux_status_image_test)
    add_test(NAME phone_camera_linux_packaging_contract
             COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_CURRENT_SOURCE_DIR}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/linux_packaging_contract.cmake)
    # The OpenSSL TLS server against an OpenSSL client, in memory.
    add_executable(phone_camera_tls_tests tests/tls_test.cpp companion/tls_server_openssl.cpp companion/log_posix.cpp)
    target_link_libraries(phone_camera_tls_tests PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    target_compile_options(phone_camera_tls_tests PRIVATE -Wall)
    add_test(NAME phone_camera_tls_tests COMMAND phone_camera_tls_tests)
    # The iPhone page's protocol code against the same golden vectors (needs Node.js; skipped without it).
    find_program(NODE_EXE node)
    if(NODE_EXE)
        add_test(NAME phone_camera_web_golden COMMAND ${NODE_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/webtest/golden_check.js
                 ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/golden.txt)
    endif()
endif()

# --- Packaging: cpack -G DEB (docs/LINUX-SUPPORT.md "Definition of done") ----------------------------------
# One version for Android and PC (CLAUDE.md): read it from the app, not a fourth copy.
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/../app/build.gradle.kts MYCAM_VERSION_LINE REGEX "versionName = \"[0-9.]+\"")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" MYCAM_VERSION "${MYCAM_VERSION_LINE}")

# The DEB must carry every notice for code/assets it redistributes. Check the
# source paths while configuring so a moved notice cannot silently disappear
# from data.tar.
set(PHONE_CAMERA_NOTICE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../LICENSE)
set(MYCAM_NOTICE ${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE)
set(QRCODEGEN_NOTICE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/qrcodegen/LICENSE)
set(DEJAVU_NOTICE ${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/assets/licenses/DejaVu-LICENSE.txt)
foreach(notice ${PHONE_CAMERA_NOTICE_DIR} ${MYCAM_NOTICE} ${QRCODEGEN_NOTICE} ${DEJAVU_NOTICE})
    if(NOT EXISTS ${notice})
        message(FATAL_ERROR "PHONE_CAMERA Linux package notice is missing: ${notice}")
    endif()
endforeach()

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
set(UDEV_RULES "# PHONE_CAMERA: lets the logged-in user's phone-camera-companion open Android phones over USB. Generated from\n# kAndroidVendors[] in pc/companion/phone_link.cpp by pc/linux.cmake.\n")
string(APPEND UDEV_RULES "# After the switch to accessory mode (IsAccessoryPid: 0x2D00-0x2D05).\nSUBSYSTEM==\"usb\", ATTR{idVendor}==\"18d1\", ATTR{idProduct}==\"2d0[0-5]\", TAG+=\"uaccess\"\n# Before it: the phone makers PHONE_CAMERA asks to switch.\n")
foreach(vid ${VENDOR_IDS})
    string(SUBSTRING "${vid}" 2 4 hex)
    string(TOLOWER "${hex}" hex)
    string(APPEND UDEV_RULES "SUBSYSTEM==\"usb\", ATTR{idVendor}==\"${hex}\", TAG+=\"uaccess\"\n")
endforeach()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/69-phone-camera.rules "${UDEV_RULES}")

set(CMAKE_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
    WORLD_READ WORLD_EXECUTE) # 755, whatever the builder's umask.
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/69-phone-camera.rules DESTINATION lib/udev/rules.d)
install(FILES linux/phone-camera.service DESTINATION lib/systemd/user)
install(FILES ${PHONE_CAMERA_NOTICE_DIR} DESTINATION share/doc/phone-camera RENAME PHONE_CAMERA-MIT-LICENSE.txt)
install(FILES ${MYCAM_NOTICE} DESTINATION share/doc/phone-camera RENAME MyCam-MIT-LICENSE.txt)
install(FILES ${QRCODEGEN_NOTICE} DESTINATION share/doc/phone-camera RENAME qrcodegen-MIT-LICENSE.txt)
install(FILES ${DEJAVU_NOTICE} DESTINATION share/doc/phone-camera RENAME DejaVu-LICENSE.txt)
# Deliberately do not install modprobe.d/modules-load.d drop-ins. v4l2loopback
# is system-wide and may be shared with OBS or another virtual-camera product;
# the owner must provide or approve its module options. The daemon auto-selects
# only a node labelled PHONE_CAMERA, or accepts an explicitly assigned --device.

set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME phone-camera)
set(CPACK_PACKAGE_VERSION ${MYCAM_VERSION})
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "PHONE_CAMERA: use your phone as a webcam")
set(CPACK_PACKAGE_DESCRIPTION "PHONE_CAMERA turns an Android phone (USB or Wi-Fi) or an iPhone (Safari, Wi-Fi) into a webcam\nfor Zoom, OBS, Discord and browsers, through a v4l2loopback camera. Based on the MIT-licensed MyCam project.")
set(CPACK_PACKAGE_CONTACT "PHONE_CAMERA <https://github.com/andrewaltair/PHONE_CAMERA/issues>")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/andrewaltair/PHONE_CAMERA")
set(CPACK_DEBIAN_PACKAGE_SECTION video)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON) # libavcodec, libusb, libssl...: the distro's, linked dynamically.
# v4l2loopback ships with some kernels (linux-modules-extra) and as DKMS elsewhere. Keep DKMS as a suggestion,
# never an install-time recommendation: the package must not silently add a kernel driver or change module options.
set(CPACK_DEBIAN_PACKAGE_SUGGESTS "v4l2loopback-dkms")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA ${CMAKE_CURRENT_SOURCE_DIR}/linux/postinst ${CMAKE_CURRENT_SOURCE_DIR}/linux/prerm)
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)
include(CPack)
