cmake_minimum_required(VERSION 3.20)
project(PadDisplayHost VERSION 0.2.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(PadDisplay WIN32
    src/main.cpp
    src/log.cpp
    src/settings.cpp
    src/display.cpp
    src/capture.cpp
    src/encoder_nvenc.cpp
    src/encoder_mf.cpp
    src/net.cpp
    src/input.cpp
    src/session.cpp
    src/vdd.cpp
    src/gpu_profile.cpp
    src/audio.cpp
    src/app.rc
)

target_include_directories(PadDisplay PRIVATE src third_party/nv-codec-headers/include third_party/nvapi)
target_compile_definitions(PadDisplay PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000006
    PD_VERSION_MAJOR=${PROJECT_VERSION_MAJOR} PD_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    PD_VERSION_PATCH=${PROJECT_VERSION_PATCH} PD_VERSION_STR="${PROJECT_VERSION}")
target_compile_options(PadDisplay PRIVATE /W4 /permissive- /utf-8 /wd4100 /wd4201)
target_link_libraries(PadDisplay PRIVATE
    d3d11 dxgi d3dcompiler ws2_32 iphlpapi shell32 user32 gdi32 advapi32
    mfplat mfuuid mf mfreadwrite qwave setupapi newdev cfgmgr32 avrt ole32
    ${CMAKE_SOURCE_DIR}/third_party/nvapi/amd64/nvapi64.lib)

# The virtual monitor driver package ships next to the exe (driver\vdd).
add_custom_command(TARGET PadDisplay POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/driver $<TARGET_FILE_DIR:PadDisplay>/driver)
