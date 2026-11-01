cmake_minimum_required(VERSION 3.20)
project(PadDisplayHost VERSION 0.4.2 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(PadDisplay WIN32
    src/main.cpp
    src/exepath.cpp
    src/tray/tray.cpp
    src/tray/traymenu.cpp
    src/tray/autostart.cpp
    src/tray/instance.cpp
    src/tray/gpu_preference.cpp
    src/tray/micdriver.cpp
    src/log.cpp
    src/realtime.cpp
    src/text.cpp
    src/crashdump.cpp
    src/settings.cpp
    src/display/display.cpp
    src/display/realscreens.cpp
    src/display/securedesktop.cpp
    src/capture/adapters.cpp
    src/capture/capture.cpp
    src/capture/cursor_overlay.cpp
    src/capture/capture_wgc.cpp
    src/capture/lockcapture.cpp
    src/encoder/encoder_nvenc.cpp
    src/encoder/encoder_mf.cpp
    src/net/net.cpp
    src/net/tls.cpp
    src/net/discovery.cpp
    src/net/adb.cpp
    src/childprocess.cpp
    src/input/input.cpp
    src/input/gamepad.cpp
    src/session/session.cpp
    src/session/session_stream.cpp
    src/session/session_input.cpp
    src/session/server.cpp
    src/session/monitor_off.cpp
    src/session/parameter_sets.cpp
    src/setup/vdd.cpp
    src/encoder/gpu_profile.cpp
    src/encoder/gpu_select.cpp
    src/audio/audio.cpp
    src/audio/audiomix.cpp
    src/audio/cablesink.cpp
    src/audio/jitterbuffer.cpp
    src/mic/micsink.cpp
    src/mic/micuse.cpp
    src/mic/micbackend.cpp
    src/setup/setup.cpp
    src/setup/setup_commands.cpp
    src/camera/camdecoder.cpp
    src/camera/cammailbox.cpp
    src/camera/camsink.cpp
    src/camera/vcam.cpp
    src/secure/uacrelay.cpp
    src/secure/service.cpp
    src/app.rc
)

# ViGEmClient talks to the ViGEmBus driver (virtual Xbox 360 controllers). Built as its own target:
# it is third-party code that does not follow this project's warning level or definitions.
add_library(ViGEmClient STATIC third_party/ViGEmClient/src/ViGEmClient.cpp)
target_include_directories(ViGEmClient PRIVATE third_party/ViGEmClient/src)
target_include_directories(ViGEmClient SYSTEM PUBLIC third_party/ViGEmClient/include)
target_compile_options(ViGEmClient PRIVATE /W0)
target_link_libraries(ViGEmClient PUBLIC setupapi)

target_include_directories(PadDisplay PRIVATE src third_party/nv-codec-headers/include third_party/nvapi)
target_compile_definitions(PadDisplay PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000006
    PD_VERSION_MAJOR=${PROJECT_VERSION_MAJOR} PD_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    PD_VERSION_PATCH=${PROJECT_VERSION_PATCH} PD_VERSION_STR="${PROJECT_VERSION}")
target_compile_options(PadDisplay PRIVATE /W4 /permissive- /utf-8 /wd4100 /wd4201)
target_link_libraries(PadDisplay PRIVATE
    ViGEmClient d3d11 dxgi d3dcompiler ws2_32 iphlpapi shell32 user32 gdi32 advapi32
    mfplat mfuuid mf mfreadwrite qwave setupapi newdev cfgmgr32 avrt winmm windowsapp ole32 secur32 crypt32 ncrypt bcrypt wmcodecdspuuid wintrust taskschd oleaut32 wtsapi32 userenv dbghelp
    ${CMAKE_SOURCE_DIR}/third_party/nvapi/amd64/nvapi64.lib)

# The virtual monitor driver package ships next to the exe (driver\vdd).
add_custom_command(TARGET PadDisplay POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/driver $<TARGET_FILE_DIR:PadDisplay>/driver)

# The virtual camera's media source. The Windows 11 Frame Server (a service) loads it; PadDisplay.exe registers it
# with MFCreateVirtualCamera. No driver: it is an ordinary COM DLL.
add_library(PadDisplayCamera SHARED camera/camera.cpp camera/frame_reader.cpp camera/camera.def)
target_include_directories(PadDisplayCamera PRIVATE src)
target_compile_definitions(PadDisplayCamera PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A00000B) # NTDDI_WIN10_CO: the headers hide the virtual camera APIs below Windows 11
target_compile_options(PadDisplayCamera PRIVATE /W4 /permissive- /utf-8 /wd4100)
add_custom_command(TARGET PadDisplayCamera POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:PadDisplay>/driver/camera
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:PadDisplayCamera> $<TARGET_FILE_DIR:PadDisplay>/driver/camera/)
add_dependencies(PadDisplay PadDisplayCamera)

# Tests of the code that needs no device and no network: cmake --build build --target check
add_executable(audiomix_test EXCLUDE_FROM_ALL tests/audiomix_test.cpp src/audio/audiomix.cpp)
add_executable(jitterbuffer_test EXCLUDE_FROM_ALL tests/jitterbuffer_test.cpp src/audio/jitterbuffer.cpp)
add_executable(cammailbox_test EXCLUDE_FROM_ALL tests/cammailbox_test.cpp src/camera/cammailbox.cpp)
add_executable(settings_test EXCLUDE_FROM_ALL tests/settings_test.cpp src/settings.cpp)
add_executable(gpu_select_test EXCLUDE_FROM_ALL tests/gpu_select_test.cpp src/encoder/gpu_select.cpp)
target_link_libraries(gpu_select_test PRIVATE dxgi)
add_executable(realtime_test EXCLUDE_FROM_ALL tests/realtime_test.cpp src/realtime.cpp src/log.cpp)
target_link_libraries(realtime_test PRIVATE avrt)
add_executable(childprocess_test EXCLUDE_FROM_ALL tests/childprocess_test.cpp src/childprocess.cpp)
add_executable(cursorshape_test EXCLUDE_FROM_ALL tests/cursorshape_test.cpp src/capture/cursor_overlay.cpp src/log.cpp)
target_link_libraries(cursorshape_test PRIVATE d3dcompiler)
add_executable(parameter_sets_test EXCLUDE_FROM_ALL tests/parameter_sets_test.cpp src/session/parameter_sets.cpp)
add_executable(camframe_test EXCLUDE_FROM_ALL tests/camframe_test.cpp camera/frame_reader.cpp)
target_include_directories(camframe_test PRIVATE camera)
set(UNIT_TESTS audiomix_test jitterbuffer_test cammailbox_test settings_test gpu_select_test realtime_test childprocess_test cursorshape_test parameter_sets_test camframe_test)
foreach(test ${UNIT_TESTS})
    target_include_directories(${test} PRIVATE src)
    target_compile_definitions(${test} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
    target_compile_options(${test} PRIVATE /W4 /permissive- /utf-8 /wd4100)
    list(APPEND RUN_UNIT_TESTS COMMAND ${test})
endforeach()
add_custom_target(check ${RUN_UNIT_TESTS})