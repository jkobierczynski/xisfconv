cmake_minimum_required(VERSION 3.15)
project(xisfconv VERSION 0.9.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(XISFCONV_WITH_ZSTD "Support Zstandard-compressed XISF files (needs libzstd)" ON)
option(XISFCONV_PORTABLE "Build a self-contained binary for distribution (static zstd/zlib/C++ runtime)" OFF)

if(XISFCONV_PORTABLE)
  # MSVC: static C runtime (pair with the vcpkg x64-windows-static triplet).
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  # Prefer static zlib where the platform ships one (CMake >= 3.24 honors this).
  if(NOT APPLE)
    set(ZLIB_USE_STATIC_LIBS ON)
  endif()
endif()

find_package(ZLIB REQUIRED)

# Everything but the command line: the formats and the conversions between them.
add_library(xisfconv_core STATIC
  src/asdf.cpp
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
  src/tiff.cpp
  src/png.cpp
  src/wcs.cpp
  src/yaml.cpp
)
target_include_directories(xisfconv_core PUBLIC src)
target_link_libraries(xisfconv_core PUBLIC ZLIB::ZLIB)

add_executable(xisfconv src/main.cpp)
target_link_libraries(xisfconv PRIVATE xisfconv_core)

foreach(_target xisfconv_core xisfconv)
  if(MSVC)
    target_compile_options(${_target} PRIVATE /W4)
    target_compile_definitions(${_target} PRIVATE _CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(${_target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endforeach()

# Zstandard: try the CMake package (vcpkg, recent distros), then pkg-config, then a plain search.
if(XISFCONV_WITH_ZSTD)
  set(_zstd_target "")
  if(XISFCONV_PORTABLE AND NOT MSVC)
    # Link the static archive so the binary doesn't need libzstd at run time.
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
    target_link_libraries(xisfconv_core PUBLIC ${_zstd_target})
    target_compile_definitions(xisfconv_core PRIVATE XISFCONV_HAVE_ZSTD)
    message(STATUS "xisfconv: Zstandard support enabled (${_zstd_target})")
  else()
    message(WARNING "libzstd not found: zstd-compressed XISF files will not be readable. "
                    "Install libzstd-dev (Debian/Ubuntu), libzstd-devel (Fedora) or zstd (Homebrew/vcpkg).")
  endif()
endif()

if(XISFCONV_PORTABLE AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT APPLE)
  # Avoid depending on the build machine's libstdc++ version.
  target_link_options(xisfconv PRIVATE -static-libgcc -static-libstdc++)
endif()

include(GNUInstallDirs)
install(TARGETS xisfconv RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
