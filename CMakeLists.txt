# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
cmake_minimum_required(VERSION 3.15)

# The version is stated in one place, the public header.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/xisfconv.h" _version_lines
     REGEX "^#define XISFCONV_VERSION_(MAJOR|MINOR|PATCH) +[0-9]+")
set(_version "")
foreach(_part MAJOR MINOR PATCH)
  foreach(_line IN LISTS _version_lines)
    if(_line MATCHES "^#define XISFCONV_VERSION_${_part} +([0-9]+)")
      list(APPEND _version "${CMAKE_MATCH_1}")
    endif()
  endforeach()
endforeach()
list(LENGTH _version _version_parts)
if(NOT _version_parts EQUAL 3)
  message(FATAL_ERROR "cannot read XISFCONV_VERSION_MAJOR, _MINOR and _PATCH from include/xisfconv.h")
endif()
list(JOIN _version "." _version)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS include/xisfconv.h)   # configure again when it changes

project(xisfconv VERSION ${_version} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(XISFCONV_WITH_ZSTD "Support Zstandard-compressed XISF files (needs libzstd)" ON)
option(BUILD_SHARED_LIBS "Build libxisfconv as a shared library (default: static, linked into the tool)" OFF)
option(XISFCONV_BUILD_TESTS "Build the C test program of the library (needs a C compiler)" OFF)
option(XISFCONV_PORTABLE "Build a self-contained binary for distribution (static zstd/zlib/C++ runtime)" OFF)

# The Python wheel (pyproject.toml, built by scikit-build-core) holds the shared library and
# nothing else of this build: no command line tool, no header.
if(SKBUILD)
  set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
endif()

if(XISFCONV_PORTABLE)
  # MSVC: static C runtime (pair with the vcpkg x64-windows-static triplet).
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  # Prefer static zlib where the platform ships one (CMake >= 3.24 honors this). Not for a shared
  # libxisfconv outside MSVC: the distributions' libz.a is not position-independent code.
  if(NOT APPLE AND (MSVC OR NOT BUILD_SHARED_LIBS))
    set(ZLIB_USE_STATIC_LIBS ON)
  endif()
endif()

find_package(ZLIB REQUIRED)

# libxisfconv: everything but the command line, behind the C API of include/xisfconv.h.
# Static by default; -DBUILD_SHARED_LIBS=ON builds the shared library.
add_library(xisfconv_core
  src/asdf.cpp
  src/capi.cpp
  src/common.cpp
  src/xml.cpp
  src/codecs.cpp
  src/xisf.cpp
  src/xisfrewrite.cpp
  src/xisfwrite.cpp
  src/convert.cpp
  src/fits.cpp
  src/fitsread.cpp
  src/fitstile.cpp
  src/pipeline.cpp
  src/property.cpp
  src/tiff.cpp
  src/png.cpp
  src/wcs.cpp
  src/yaml.cpp
)
target_include_directories(xisfconv_core
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
  PRIVATE src)
target_compile_definitions(xisfconv_core PRIVATE XISFCONV_BUILDING)
if(NOT BUILD_SHARED_LIBS)
  target_compile_definitions(xisfconv_core PUBLIC XISFCONV_STATIC)
endif()
target_link_libraries(xisfconv_core PRIVATE ZLIB::ZLIB)
# The code is compiled with hidden visibility: of its own functions, only those of xisfconv.h are
# visible from outside. The version of the shared library changes with every 0.x release: there
# is no ABI promise yet.
set_target_properties(xisfconv_core PROPERTIES
  EXPORT_NAME xisfconv
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  POSITION_INDEPENDENT_CODE ON)
if(NOT SKBUILD)
  # (a wheel holds one file without a version in its name: wheels cannot hold symbolic links)
  set_target_properties(xisfconv_core PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})
endif()
if(BUILD_SHARED_LIBS AND UNIX AND NOT APPLE)
  # Only the functions of xisfconv.h are offered to the rest of the process: not the standard
  # library templates the code uses, and not a static library linked into this one (Zstandard in
  # the Python wheels), of which the process may hold another copy.
  target_link_options(xisfconv_core PRIVATE
    "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/libxisfconv.map")
  set_property(TARGET xisfconv_core APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/libxisfconv.map")
endif()
if(MSVC)
  # libxisfconv.dll / libxisfconv.lib: the plain name is taken by xisfconv.exe and its .pdb
  set_target_properties(xisfconv_core PROPERTIES OUTPUT_NAME libxisfconv)
else()
  set_target_properties(xisfconv_core PROPERTIES OUTPUT_NAME xisfconv)
endif()

# The command line tool uses the library through its C API only.
set(_targets xisfconv_core)
if(NOT SKBUILD)
  add_executable(xisfconv src/main.cpp)
  target_link_libraries(xisfconv PRIVATE xisfconv_core)
  if(MINGW)
    target_link_options(xisfconv PRIVATE -municode)   # wmain: the arguments arrive as UTF-16
  endif()
  list(APPEND _targets xisfconv)
endif()

foreach(_target IN LISTS _targets)
  if(MSVC)
    target_compile_options(${_target} PRIVATE /W4)
    target_compile_definitions(${_target} PRIVATE _CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(${_target} PRIVATE -Wall -Wextra -Wpedantic)
    # Every multiplication and addition is rounded by itself, on every processor. Left to
    # themselves, GCC and Clang fuse a*b + c into one instruction where the processor has one
    # (arm64 always has): the last bit of a result then depends on the machine, and a file would
    # not convert to the same bytes everywhere.
    target_compile_options(${_target} PRIVATE -ffp-contract=off)
  endif()
endforeach()

# Zstandard: try the CMake package (vcpkg, recent distros), then pkg-config, then a plain search.
if(XISFCONV_WITH_ZSTD)
  set(_zstd_target "")
  if(XISFCONV_PORTABLE AND NOT MSVC AND NOT BUILD_SHARED_LIBS)
    # Link the static archive so the binary doesn't need libzstd at run time. (A shared library
    # needs position-independent code, which the distributions' libzstd.a is not built as.)
    find_path(ZSTD_INCLUDE_DIR zstd.h)
    find_library(ZSTD_STATIC_LIBRARY NAMES libzstd.a)
    if(ZSTD_INCLUDE_DIR AND ZSTD_STATIC_LIBRARY)
      add_library(xisfconv_zstd_static STATIC IMPORTED)
      set_target_properties(xisfconv_zstd_static PROPERTIES
        IMPORTED_LOCATION "${ZSTD_STATIC_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${ZSTD_INCLUDE_DIR}")
      set(_zstd_target xisfconv_zstd_static)
    else()
      message(WARNING "XISFCONV_PORTABLE: static libzstd.a not found; linking it dynamically")
    endif()
  endif()
  if(NOT _zstd_target)
    find_package(zstd CONFIG QUIET)
  endif()
  if(_zstd_target)
    # already chosen (portable static build)
  elseif(TARGET zstd::libzstd_shared)
    set(_zstd_target zstd::libzstd_shared)
  elseif(TARGET zstd::libzstd_static)
    set(_zstd_target zstd::libzstd_static)
  elseif(TARGET zstd::libzstd)
    set(_zstd_target zstd::libzstd)
  endif()
  if(NOT _zstd_target)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(ZSTD QUIET IMPORTED_TARGET libzstd)
      if(ZSTD_FOUND)
        set(_zstd_target PkgConfig::ZSTD)
      endif()
    endif()
  endif()
  if(NOT _zstd_target)
    find_path(ZSTD_INCLUDE_DIR zstd.h)
    find_library(ZSTD_LIBRARY NAMES zstd zstd_static libzstd)
    if(ZSTD_INCLUDE_DIR AND ZSTD_LIBRARY)
      add_library(xisfconv_zstd UNKNOWN IMPORTED)
      set_target_properties(xisfconv_zstd PROPERTIES
        IMPORTED_LOCATION "${ZSTD_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${ZSTD_INCLUDE_DIR}")
      set(_zstd_target xisfconv_zstd)
    endif()
  endif()
  if(_zstd_target)
    target_link_libraries(xisfconv_core PRIVATE ${_zstd_target})
    target_compile_definitions(xisfconv_core PRIVATE XISFCONV_HAVE_ZSTD)
    message(STATUS "xisfconv: Zstandard support enabled (${_zstd_target})")
  else()
    message(WARNING "libzstd not found: zstd-compressed XISF files will not be readable. "
                    "Install libzstd-dev (Debian/Ubuntu), libzstd-devel (Fedora) or zstd (Homebrew/vcpkg).")
  endif()
endif()

if(XISFCONV_PORTABLE AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT APPLE AND TARGET xisfconv)
  # Avoid depending on the build machine's libstdc++ version.
  target_link_options(xisfconv PRIVATE -static-libgcc -static-libstdc++)
endif()

if(SKBUILD)
  # The wheel: the library next to the Python modules, where python/xisfconv/_lib.py looks for it.
  install(TARGETS xisfconv_core
    RUNTIME DESTINATION . COMPONENT python
    LIBRARY DESTINATION . COMPONENT python
    ARCHIVE DESTINATION . COMPONENT python-import-library)
  return()
endif()

# The C test program: the API seen from a C compiler.
if(XISFCONV_BUILD_TESTS)
  enable_language(C)
  # capi_test: the checks; capi_readall: reads all there is of any file (the target for fuzzing)
  foreach(_test capi_test capi_readall)
    add_executable(xisfconv_${_test} tests/${_test}.c)
    set_target_properties(xisfconv_${_test} PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF LINKER_LANGUAGE CXX)
    target_link_libraries(xisfconv_${_test} PRIVATE xisfconv_core)
    if(MSVC)
      target_compile_options(xisfconv_${_test} PRIVATE /W4)
      target_compile_definitions(xisfconv_${_test} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
      target_compile_options(xisfconv_${_test} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
  endforeach()
endif()

include(GNUInstallDirs)
if(BUILD_SHARED_LIBS AND NOT WIN32)
  # the installed tool finds the library next to it
  if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
    set_target_properties(xisfconv PROPERTIES INSTALL_RPATH "${CMAKE_INSTALL_LIBDIR}")
  elseif(APPLE)
    set_target_properties(xisfconv PROPERTIES INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
  else()
    set_target_properties(xisfconv PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
  endif()
endif()
install(TARGETS xisfconv RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# The library, its header and a pkg-config file. The CMake package comes with the shared library.
set(_export "")
if(BUILD_SHARED_LIBS)
  set(_export EXPORT xisfconvTargets)
endif()
install(TARGETS xisfconv_core ${_export}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES include/xisfconv.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE COPYING.LESSER DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/xisfconv)

# The .pc file finds its prefix from where it is installed, so that it stays right when the
# installation is moved or `cmake --install --prefix` names another place.
if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
  set(XISFCONV_PC_PREFIX "${CMAKE_INSTALL_PREFIX}")
  set(XISFCONV_PC_LIBDIR "${CMAKE_INSTALL_LIBDIR}")
else()
  file(RELATIVE_PATH _to_prefix "/prefix/${CMAKE_INSTALL_LIBDIR}/pkgconfig" "/prefix")
  set(XISFCONV_PC_PREFIX "\${pcfiledir}/${_to_prefix}")
  set(XISFCONV_PC_LIBDIR "\${exec_prefix}/${CMAKE_INSTALL_LIBDIR}")
endif()
if(IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
  set(XISFCONV_PC_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}")
else()
  set(XISFCONV_PC_INCLUDEDIR "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
endif()
set(XISFCONV_PC_LIBS_PRIVATE "-lz")
if(_zstd_target)
  string(APPEND XISFCONV_PC_LIBS_PRIVATE " -lzstd")
endif()
if(APPLE)
  string(APPEND XISFCONV_PC_LIBS_PRIVATE " -lc++")
elseif(NOT MSVC)
  string(APPEND XISFCONV_PC_LIBS_PRIVATE " -lstdc++ -lm")
endif()
set(XISFCONV_PC_CFLAGS "")
if(NOT BUILD_SHARED_LIBS)
  set(XISFCONV_PC_CFLAGS " -DXISFCONV_STATIC")
endif()
configure_file(cmake/xisfconv.pc.in xisfconv.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/xisfconv.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

if(BUILD_SHARED_LIBS)
  include(CMakePackageConfigHelpers)
  configure_file(cmake/xisfconvConfig.cmake.in xisfconvConfig.cmake COPYONLY)
  write_basic_package_version_file(xisfconvConfigVersion.cmake VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
  install(EXPORT xisfconvTargets NAMESPACE xisfconv:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/xisfconv)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/xisfconvConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/xisfconvConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/xisfconv)
endif()
