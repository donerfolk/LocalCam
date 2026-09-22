cmake_minimum_required(VERSION 3.21)
project(LocalCam CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT: no VC++ redistributable
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
add_compile_options(/utf-8 /W3)

add_subdirectory(softcam)  # before the defines below: softcam needs the full windows.h
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 UNICODE _UNICODE)

# A 32-bit configure (-A Win32) only builds the camera DLL, for 32-bit video apps.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_executable(LocalCam WIN32
        src/main.cpp src/server.cpp src/decoder.cpp src/convert.cpp
        third_party/qrcodegen/qrcodegen.cpp
        src/app.rc)
    target_include_directories(LocalCam PRIVATE third_party/qrcodegen)
    set_source_files_properties(src/app.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../web/index.html)
    target_link_libraries(LocalCam PRIVATE softcam
        ws2_32 secur32 crypt32 ncrypt bcrypt iphlpapi mfplat mfuuid wmcodecdspuuid ole32 shell32 user32 gdi32 advapi32 dwmapi)

    enable_testing()
    add_executable(localcam_tests src/tests.cpp src/convert.cpp src/decoder.cpp src/server.cpp)
    target_link_libraries(localcam_tests PRIVATE ws2_32 secur32 crypt32 ncrypt bcrypt mfplat mfuuid wmcodecdspuuid ole32)
    add_test(NAME localcam_tests COMMAND localcam_tests)
endif()
