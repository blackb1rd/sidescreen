# Sanitizers and fuzzing, applied to spanly_core and everything linking it (presets: <os>-asan,
# <os>-tsan, linux-fuzz).
set(SPANLY_SANITIZE "" CACHE STRING "address (AddressSanitizer + UBSan), thread (ThreadSanitizer), or empty")
set_property(CACHE SPANLY_SANITIZE PROPERTY STRINGS "" address thread)
option(SPANLY_FUZZ "Build the libFuzzer targets in fuzz/ (clang; implies address)" OFF)

set(sanitizers "")
if(SPANLY_FUZZ OR SPANLY_SANITIZE STREQUAL "address" OR SPANLY_SANITIZE STREQUAL "ON")
  set(sanitizers address,undefined)
elseif(SPANLY_SANITIZE STREQUAL "thread")
  set(sanitizers thread)
elseif(SPANLY_SANITIZE)
  message(FATAL_ERROR "SPANLY_SANITIZE must be address, thread or empty, not \"${SPANLY_SANITIZE}\"")
endif()

if(sanitizers)
  set(instrument ${sanitizers})
  if(SPANLY_FUZZ)
    string(APPEND instrument ",fuzzer-no-link") # coverage for the fuzzers
  endif()
  target_compile_options(spanly_core PUBLIC -fsanitize=${instrument} -fno-omit-frame-pointer)
  if(sanitizers MATCHES "undefined")
    target_compile_options(spanly_core PUBLIC -fno-sanitize-recover=undefined) # UB fails the run
  endif()
  target_link_options(spanly_core PUBLIC -fsanitize=${sanitizers})
endif()
