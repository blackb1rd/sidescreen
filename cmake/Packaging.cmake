# CPack: the Linux .tar.gz and Windows .zip on the releases page (cpack --preset <os>-release).
# The Mac app is assembled, signed and notarized by scripts/build-mac.sh instead.
if(APPLE OR NOT TARGET spanly)
  return()
endif()

install(TARGETS spanly RUNTIME DESTINATION .)
install(FILES README.md LICENSE DESTINATION .)
if(UNIX)
  install(FILES packaging/linux/70-spanly.rules DESTINATION .)
endif()

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" arch)
if(arch STREQUAL "amd64")
  set(arch x86_64)
endif()
string(TOLOWER "${CMAKE_SYSTEM_NAME}" os)
set(CPACK_PACKAGE_NAME spanly)
set(CPACK_PACKAGE_VENDOR caigenix)
set(CPACK_PACKAGE_VERSION ${SPANLY_VERSION})
set(CPACK_PACKAGE_FILE_NAME "spanly-${os}-${arch}")
set(CPACK_STRIP_FILES ${UNIX})
include(CPack)
