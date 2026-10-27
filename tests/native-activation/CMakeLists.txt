cmake_minimum_required(VERSION 3.20)

project(phone_camera_native_activation LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "native activation tests require Windows")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(phone_camera_native_activation_test
    native_activation_test.cpp
)

target_compile_definitions(phone_camera_native_activation_test PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
)

if(MSVC)
    target_compile_options(phone_camera_native_activation_test PRIVATE /W4 /EHsc /utf-8)
endif()

target_link_libraries(phone_camera_native_activation_test PRIVATE
    mfplat
    mfuuid
    ole32
    strmiids
)

enable_testing()

# The paths are intentionally explicit: this harness never discovers or loads a
# registered COM class.  Configure either or both optional CTest entries with
# -DPHONE_CAMERA_*_DLL=...; direct command-line use remains supported.
set(PHONE_CAMERA_MF_DLL "" CACHE FILEPATH "MF virtual-camera DLL to exercise")
set(PHONE_CAMERA_DSHOW_DLL "" CACHE FILEPATH "DirectShow virtual-camera DLL to exercise")
set(PHONE_CAMERA_BASELINE_MF_DLL "" CACHE FILEPATH "Baseline MF DLL for the expected-red test")
set(PHONE_CAMERA_BASELINE_DSHOW_DLL "" CACHE FILEPATH "Baseline DirectShow DLL for the expected-red test")

if(PHONE_CAMERA_MF_DLL AND PHONE_CAMERA_DSHOW_DLL)
    add_test(NAME phone_camera_native_activation_pass
        COMMAND phone_camera_native_activation_test
            --mode pass
            --mf "${PHONE_CAMERA_MF_DLL}"
            --dshow "${PHONE_CAMERA_DSHOW_DLL}"
    )
endif()

if(PHONE_CAMERA_BASELINE_MF_DLL AND PHONE_CAMERA_BASELINE_DSHOW_DLL)
    add_test(NAME phone_camera_native_activation_expected_red
        COMMAND phone_camera_native_activation_test
            --mode red
            --mf "${PHONE_CAMERA_BASELINE_MF_DLL}"
            --dshow "${PHONE_CAMERA_BASELINE_DSHOW_DLL}"
    )
endif()
