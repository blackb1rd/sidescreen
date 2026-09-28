# Third-party code, pinned and built from source as static libraries (nothing to install).
include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# mbedTLS 3.6 LTS: HKDF-SHA256 and AES-256-GCM for the encrypted Wi-Fi link.
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_Declare(mbedtls
  URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
  URL_HASH SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
  EXCLUDE_FROM_ALL)
set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE) # mbedTLS 3.6 asks for an old CMake policy
FetchContent_MakeAvailable(mbedtls)
add_library(spanly::mbedcrypto ALIAS mbedcrypto)

# libusb 1.0.30 (CMake build): the raw USB accessory link. Linked statically, so the app
# needs no separate library. Device lists are polled, so udev isn't needed on Linux.
set(LIBUSB_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(LIBUSB_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)
set(LIBUSB_ENABLE_UDEV OFF CACHE BOOL "" FORCE)
set(LIBUSB_ENABLE_LOGGING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libusb
  GIT_REPOSITORY https://github.com/libusb/libusb-cmake.git
  GIT_TAG bd098071279b954acd491a4da34fd999b66a5afb # v1.0.30-0
  GIT_SHALLOW FALSE
  EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(libusb)
add_library(spanly::libusb ALIAS usb-1.0)

# Every dependency, built on its own (CodeQL builds these before it starts watching the compiler).
add_custom_target(spanly_dependencies)
add_dependencies(spanly_dependencies mbedcrypto usb-1.0)
