# The version comes from the git tag: v1.2.3 -> 1.2.3, and between tags e.g. 1.2.3-4-gabc1234
# (4 commits after v1.2.3). Without git or tags it is 0.0.0-dev. The release workflow sets the
# SPANLY_VERSION environment variable to the tag it builds; -DSPANLY_VERSION=... also works.
# Included before project(), which takes SPANLY_VERSION_NUMBER (MAJOR.MINOR.PATCH).
if(NOT SPANLY_VERSION AND DEFINED ENV{SPANLY_VERSION})
  set(SPANLY_VERSION "$ENV{SPANLY_VERSION}")
endif()
if(NOT SPANLY_VERSION)
  set(SPANLY_VERSION 0.0.0-dev)
  find_program(SPANLY_GIT git)
  if(SPANLY_GIT)
    execute_process(COMMAND "${SPANLY_GIT}" describe --tags --match "v[0-9]*" --dirty
                    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" RESULT_VARIABLE result
                    OUTPUT_VARIABLE described OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(result EQUAL 0)
      set(SPANLY_VERSION "${described}")
    endif()
  endif()
endif()
string(REGEX REPLACE "^v" "" SPANLY_VERSION "${SPANLY_VERSION}")
if(NOT SPANLY_VERSION MATCHES "^(([0-9]+)\\.([0-9]+)\\.([0-9]+))")
  message(FATAL_ERROR "The version must look like 1.2.3 or v1.2.3 (a git tag), not \"${SPANLY_VERSION}\"")
endif()
set(SPANLY_VERSION_NUMBER ${CMAKE_MATCH_1})
set(SPANLY_VERSION_MAJOR ${CMAKE_MATCH_2})
set(SPANLY_VERSION_MINOR ${CMAKE_MATCH_3})
set(SPANLY_VERSION_PATCH ${CMAKE_MATCH_4})
message(STATUS "Spanly version ${SPANLY_VERSION}")
