# libFuzzer targets for the code that reads bytes from other devices. Each one also runs as a
# test for SPANLY_FUZZ_SECONDS, starting from the seeds in fuzz/seeds/<name> and keeping what it
# finds in <build>/corpus/<name>. A crashing input is saved in <build>/fuzz-crashes/.
set(SPANLY_FUZZ_SECONDS 30 CACHE STRING "How long each fuzzer runs as a test")

foreach(name reader records annexb)
  add_executable(fuzz_${name} ${name}.cpp)
  target_link_libraries(fuzz_${name} PRIVATE spanly_core)
  target_link_options(fuzz_${name} PRIVATE -fsanitize=fuzzer)
  set(corpus "${PROJECT_BINARY_DIR}/corpus/${name}")
  file(MAKE_DIRECTORY "${corpus}" "${PROJECT_BINARY_DIR}/fuzz-crashes")
  add_test(NAME fuzz_${name}
           COMMAND fuzz_${name} -max_total_time=${SPANLY_FUZZ_SECONDS} -print_final_stats=1
                   "-artifact_prefix=${PROJECT_BINARY_DIR}/fuzz-crashes/${name}-"
                   "${corpus}" "${CMAKE_CURRENT_SOURCE_DIR}/seeds/${name}")
endforeach()
