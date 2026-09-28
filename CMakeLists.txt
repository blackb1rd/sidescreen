cmake_minimum_required(VERSION 3.24)
include(cmake/Version.cmake)
project(spanly VERSION ${SPANLY_VERSION_NUMBER} LANGUAGES C CXX)

# Spanly for Mac, Windows and Linux: one C++ codebase. The shared core (protocol, crypto,
# links, sessions) is in src/core; each OS has its backend in src/platform/<os>.

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # for clang-tidy
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(APPLE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0 CACHE STRING "")
endif()

option(SPANLY_SANITIZE "Build with AddressSanitizer and UBSan" OFF)

# sccache, when installed, caches compiled objects across build folders (and in CI across runs).
# Visual Studio projects can't use a compiler launcher, so Windows builds without it.
option(SPANLY_SCCACHE "Compile through sccache if it is installed" ON)
find_program(SPANLY_SCCACHE_PROGRAM sccache)
if(SPANLY_SCCACHE AND SPANLY_SCCACHE_PROGRAM AND NOT CMAKE_GENERATOR MATCHES "Visual Studio")
  set(CMAKE_C_COMPILER_LAUNCHER "${SPANLY_SCCACHE_PROGRAM}")
  set(CMAKE_CXX_COMPILER_LAUNCHER "${SPANLY_SCCACHE_PROGRAM}")
  message(STATUS "Compiling through ${SPANLY_SCCACHE_PROGRAM}")
endif()
include(cmake/Dependencies.cmake)

add_library(spanly_core STATIC
  src/core/log.cpp
  src/core/protocol.cpp
  src/core/crypto.cpp
  src/core/flow.cpp
  src/core/link.cpp
  src/core/net.cpp
  src/core/tcp_link.cpp
  src/core/listeners.cpp
  src/core/beacon.cpp
  src/core/usb.cpp
  src/core/usb_io.cpp
  src/core/udp_video.cpp
)
target_include_directories(spanly_core PUBLIC src "${PROJECT_BINARY_DIR}/generated")
configure_file(cmake/version.hpp.in generated/spanly_version.hpp @ONLY)
file(WRITE "${PROJECT_BINARY_DIR}/version.txt" "${SPANLY_VERSION}\n") # read by scripts/build-mac.sh
target_link_libraries(spanly_core PUBLIC spanly::mbedcrypto spanly::libusb)
if(WIN32)
  target_link_libraries(spanly_core PUBLIC ws2_32 iphlpapi bcrypt)
  target_compile_definitions(spanly_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 UNICODE _UNICODE)
endif()
find_package(Threads REQUIRED)
target_link_libraries(spanly_core PUBLIC Threads::Threads)

if(MSVC)
  target_compile_options(spanly_core PUBLIC /W4 /permissive- /utf-8)
else()
  target_compile_options(spanly_core PUBLIC -Wall -Wextra -Wpedantic -Wno-gnu-zero-variadic-macro-arguments)
endif()
if(SPANLY_SANITIZE)
  target_compile_options(spanly_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(spanly_core PUBLIC -fsanitize=address,undefined)
endif()

# The app: the shared session logic (src/app) on top of a platform backend.
if(APPLE)
  add_library(spanly_platform STATIC
    src/platform/mac/audio.cpp
    src/platform/mac/capture.cpp
    src/platform/mac/display.cpp
    src/platform/mac/encoder.cpp
    src/platform/mac/events.cpp
    src/platform/mac/pointer.cpp
    src/platform/mac/system.cpp
    src/platform/mac/tray.cpp
    src/platform/mac/video_window.cpp
    src/platform/posix/process.cpp
  )
  target_link_libraries(spanly_platform PUBLIC spanly_core objc
    "-framework AppKit" "-framework ApplicationServices" "-framework AudioToolbox" "-framework AVFoundation" "-framework QuartzCore" "-framework CoreAudio" "-framework CoreFoundation" "-framework CoreGraphics"
    "-framework CoreMedia" "-framework CoreVideo" "-framework Foundation" "-framework IOKit" "-framework ScreenCaptureKit" "-framework ServiceManagement"
    "-framework SystemConfiguration" "-framework VideoToolbox")
  target_compile_options(spanly_platform PRIVATE -fblocks)
endif()

if(WIN32)
  add_library(spanly_platform STATIC
    src/platform/common/file_store.cpp
    src/platform/common/unsupported.cpp
    src/platform/windows/capture.cpp
    src/platform/windows/display.cpp
    src/platform/windows/encoder.cpp
    src/platform/windows/encoder_hw.cpp
    src/platform/windows/gpu_convert.cpp
    src/platform/windows/pointer.cpp
    src/platform/windows/system.cpp
    src/platform/windows/tray.cpp
  )
  target_link_libraries(spanly_platform PUBLIC spanly_core d3d11 dxgi mf mfplat mfuuid ole32 oleaut32 shell32 user32 advapi32)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(PkgConfig)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(SPANLY_LINUX IMPORTED_TARGET gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 gio-2.0 gio-unix-2.0)
  endif()
  if(SPANLY_LINUX_FOUND)
    add_library(spanly_platform STATIC
      src/platform/common/file_store.cpp
      src/platform/common/unsupported.cpp
      src/platform/linux/capture.cpp
      src/platform/linux/display.cpp
      src/platform/linux/encoder.cpp
      src/platform/linux/pointer.cpp
      src/platform/linux/portal.cpp
      src/platform/linux/system.cpp
      src/platform/linux/va.cpp
      src/platform/posix/process.cpp
    )
    target_link_libraries(spanly_platform PUBLIC spanly_core PkgConfig::SPANLY_LINUX)
  else()
    message(STATUS "GStreamer/GLib development files not found: building the core and tests only")
  endif()
endif()

if(TARGET spanly_platform)
  add_executable(spanly
    src/app/main.cpp
    src/app/adb.cpp
    src/app/controller.cpp
    src/app/controller_share.cpp
    src/app/menu.cpp
    src/app/options.cpp
    src/app/session.cpp
    src/app/session_hello.cpp
    src/app/settings.cpp
    src/app/stats.cpp
  )
  target_link_libraries(spanly PRIVATE spanly_platform)
  if(WIN32)
    set_target_properties(spanly PROPERTIES WIN32_EXECUTABLE ON) # no console window
    configure_file(packaging/windows/spanly.rc.in generated/spanly.rc @ONLY)
    target_sources(spanly PRIVATE "${PROJECT_BINARY_DIR}/generated/spanly.rc")
    if(MSVC)
      target_link_options(spanly PRIVATE /ENTRY:mainCRTStartup)
    endif()
  endif()
endif()

# Tests: the shared protocol, crypto and flow-control rules (same vectors as the Android app).
enable_testing()
add_executable(spanly_tests tests/main.cpp tests/protocol_test.cpp tests/crypto_test.cpp tests/flow_test.cpp)
target_link_libraries(spanly_tests PRIVATE spanly_core)
add_test(NAME spanly_tests COMMAND spanly_tests)

include(cmake/Packaging.cmake)
