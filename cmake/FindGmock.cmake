# Read-Only variables:
#  GMOCK_FOUND - system has the GMOCK library
#  GMOCK_INCLUDE_DIR - the GMOCK include directory
#  GMOCK_LIBRARIES - The libraries needed to use GMOCK
#  GMOCK_VERSION - This is set to $major.$minor.$revision$path (eg. 0.4.1)

if (UNIX)
  find_package(PkgConfig QUIET)
  pkg_check_modules(_gmock QUIET gmock)
endif ()

find_path(GMOCK_INCLUDE_DIR
  NAMES
    gmock/gmock.h
  HINTS
    ${GMOCK_ROOT_DIR}
    $ENV{GMOCK_ROOT_DIR}
    ${_GMOCK_INCLUDEDIR}
    $ENV{GMOCK_INCLUDEDIR}
  PATH_SUFFIXES
    include
)

set(GMOCK_INCLUDE_DIR "${GMOCK_INCLUDE_DIR}")

if(WIN32 AND NOT CYGWIN)
  if(MSVC)
    find_library(gmock
      NAMES
        "gmock"
        gmock.lib
      HINTS
        ${GMOCK_ROOT_DIR}
        $ENV{GMOCK_ROOT_DIR}
        $ENV{GMOCK_LIBDIR}
      PATH_SUFFIXES
        bin
        lib
    )

    mark_as_advanced(gmock)
    set(GMOCK_LIBRARIES ${gmock} ws2_32)
  else()
      # bother supporting this?
  endif()
else()

  find_library(GMOCK_LIBRARY
    NAMES
      libgmock.a
      libgmockd.a
      gmock
    HINTS
      ${GMOCK_ROOT_DIR}
      $ENV{GMOCK_ROOT_DIR}
      ${_GMOCK_LIBDIR}
      $ENV{GMOCK_LIBDIR}
    PATH_SUFFIXES
      lib
  )

  mark_as_advanced(GMOCK_LIBRARY)

  find_package (Threads REQUIRED)

  set(GMOCK_LIBRARIES ${GMOCK_LIBRARY} ${CMAKE_THREAD_LIBS_INIT})

endif()

if (GMOCK_INCLUDE_DIR)
  if (_GMOCK_VERSION)
     set(GMOCK_VERSION "${_GMOCK_VERSION}")
  elseif(GMOCK_INCLUDE_DIR AND EXISTS "${GMOCK_INCLUDE_DIR}/gmock.h")
     file(STRINGS "${GMOCK_INCLUDE_DIR}/gmock.h" GMOCK_version_str
        REGEX "^#define[\t ]+GMOCK_VERSION[\t ]+\"[0-9.]+\"[\t ]+$")

     string(REGEX REPLACE "^.*GMOCK_VERSION[\t ]+\"[0-9.]+\"[\t ]+$"
        "\\1" GMOCK_VERSION "${GMOCK_version_str}")
  endif ()
endif ()

if (GMOCK_VERSION)
message(${GMOCK_VERSION})
endif()

include(FindPackageHandleStandardArgs)

if (GMOCK_VERSION)
   find_package_handle_standard_args(gmock
    REQUIRED_VARS
      GMOCK_LIBRARIES
      GMOCK_INCLUDE_DIR
    VERSION_VAR
      GMOCK_VERSION
    FAIL_MESSAGE
      "Could NOT find gmock version"
  )
else ()
   find_package_handle_standard_args(gmock "Could NOT find gmock uuuurh"
      GMOCK_LIBRARIES
      GMOCK_INCLUDE_DIR
  )
endif ()

mark_as_advanced(GMOCK_INCLUDE_DIR GMOCK_LIBRARIES)
