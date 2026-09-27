cmake_minimum_required(VERSION 3.24)
project(spanly VERSION 0.1.0 LANGUAGES C CXX)

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
)
target_include_directories(spanly_core PUBLIC src)
target_link_libraries(spanly_core PUBLIC spanly::mbedcrypto spanly::libusb)
if(WIN32)
  target_link_libraries(spanly_core PUBLIC ws2_32 iphlpapi bcrypt)
  target_compile_definitions(spanly_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
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
    src/platform/mac/capture.cpp
    src/platform/mac/display.cpp
    src/platform/mac/encoder.cpp
    src/platform/mac/pointer.cpp
    src/platform/mac/system.cpp
  )
  target_link_libraries(spanly_platform PUBLIC spanly_core objc
    "-framework AppKit" "-framework ApplicationServices" "-framework CoreFoundation" "-framework CoreGraphics"
    "-framework CoreMedia" "-framework CoreVideo" "-framework Foundation" "-framework ScreenCaptureKit"
    "-framework SystemConfiguration" "-framework VideoToolbox")
  target_compile_options(spanly_platform PRIVATE -fblocks)
endif()

if(TARGET spanly_platform)
  add_executable(spanly
    src/app/main.cpp
    src/app/controller.cpp
    src/app/options.cpp
    src/app/session.cpp
    src/app/session_hello.cpp
    src/app/settings.cpp
    src/app/stats.cpp
  )
  target_link_libraries(spanly PRIVATE spanly_platform)
endif()

# Tests: the shared protocol, crypto and flow-control rules (same vectors as the Android app).
enable_testing()
add_executable(spanly_tests tests/main.cpp tests/protocol_test.cpp tests/crypto_test.cpp tests/flow_test.cpp)
target_link_libraries(spanly_tests PRIVATE spanly_core)
add_test(NAME spanly_tests COMMAND spanly_tests)
