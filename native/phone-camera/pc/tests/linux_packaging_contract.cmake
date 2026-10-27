if(NOT DEFINED ROOT)
    message(FATAL_ERROR "ROOT must point at native/phone-camera/pc")
endif()

set(NOTICES
    "${ROOT}/../../../LICENSE"
    "${ROOT}/../LICENSE"
    "${ROOT}/third_party/qrcodegen/LICENSE"
    "${ROOT}/../app/src/main/assets/licenses/DejaVu-LICENSE.txt")
foreach(path ${NOTICES})
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "missing package notice: ${path}")
    endif()
endforeach()

file(READ "${ROOT}/linux.cmake" LINUX_CMAKE)
foreach(token
        "share/doc/phone-camera"
        "PHONE_CAMERA-MIT-LICENSE.txt"
        "MyCam-MIT-LICENSE.txt"
        "qrcodegen-MIT-LICENSE.txt"
        "DejaVu-LICENSE.txt")
    string(FIND "${LINUX_CMAKE}" "${token}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "linux.cmake has no install rule token: ${token}")
    endif()
endforeach()

message(STATUS "PHONE_CAMERA Linux packaging notices: PASS")
