# Read-Only variables:
#  GTEST_FOUND - system has the GTEST library
#  GTEST_INCLUDE_DIR - the GTEST include directory
#  GTEST_LIBRARIES - The libraries needed to use GTEST
#  GTEST_VERSION - This is set to $major.$minor.$revision$path (eg. 0.4.1)

if (UNIX)
  find_package(PkgConfig QUIET)
  pkg_check_modules(_gtest QUIET gtest)
endif ()

find_path(GTEST_INCLUDE_DIR
  NAMES
    gtest/gtest.h
  HINTS
    ${GTEST_ROOT_DIR}
    $ENV{GTEST_ROOT_DIR}
    ${_GTEST_INCLUDEDIR}
    $ENV{GTEST_INCLUDEDIR}
    /usr/include
  PATH_SUFFIXES
    include
)

set(GTEST_INCLUDE_DIR "${GTEST_INCLUDE_DIR}")

if(WIN32 AND NOT CYGWIN)
  if(MSVC)
    find_library(gtest
      NAMES
        "gtest"
        gtest.lib
      HINTS
        ${GTEST_ROOT_DIR}
        $ENV{GTEST_ROOT_DIR}
        $ENV{GTEST_LIBDIR}
      PATH_SUFFIXES
        bin
        lib
    )

    mark_as_advanced(gtest)
    set(GTEST_LIBRARIES ${gtest} ws2_32)
  else()
      # bother supporting this?
  endif()
else()

  find_library(GTEST_LIBRARY
    NAMES
      libgtest.a
      libgtestd.a
      gtest
    HINTS
      ${GTEST_ROOT_DIR}
      $ENV{GTEST_ROOT_DIR}
      ${_GTEST_LIBDIR}
      $ENV{GTEST_LIBDIR}
      /usr/src/gtest
    PATH_SUFFIXES
      lib
  )

  mark_as_advanced(GTEST_LIBRARY)

  find_package (Threads REQUIRED)

#  set(GTEST_LIBRARIES ${GTEST_LIBRARY} ${CMAKE_THREAD_LIBS_INIT})
  set(GTEST_LIBRARIES ${GTEST_LIBRARY})

endif()

if (GTEST_INCLUDE_DIR)
  if (_GTEST_VERSION)
     set(GTEST_VERSION "${_GTEST_VERSION}")
  elseif(GTEST_INCLUDE_DIR AND EXISTS "${GTEST_INCLUDE_DIR}/gtest.h")
     file(STRINGS "${GTEST_INCLUDE_DIR}/gtest.h" GTEST_version_str
        REGEX "^#define[\t ]+GTEST_VERSION[\t ]+\"[0-9.]+\"[\t ]+$")

     string(REGEX REPLACE "^.*GTEST_VERSION[\t ]+\"[0-9.]+\"[\t ]+$"
        "\\1" GTEST_VERSION "${GTEST_version_str}")
  endif ()
endif ()

if (GTEST_VERSION)
message(${GTEST_VERSION})
endif()

include(FindPackageHandleStandardArgs)

if (GTEST_VERSION)
   find_package_handle_standard_args(gtest
    REQUIRED_VARS
      GTEST_LIBRARIES
      GTEST_INCLUDE_DIR
    VERSION_VAR
      GTEST_VERSION
    FAIL_MESSAGE
      "Could NOT find gtest version"
  )
else ()
   find_package_handle_standard_args(gtest "Could NOT find gtest"
      GTEST_LIBRARIES
      GTEST_INCLUDE_DIR
  )
endif ()

mark_as_advanced(GTEST_INCLUDE_DIR GTEST_LIBRARIES)
