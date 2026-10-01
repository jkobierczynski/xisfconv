cmake_minimum_required(VERSION 3.14)
project(xisfconv VERSION 0.3.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(XISFCONV_WITH_ZSTD "Support Zstandard-compressed XISF files (needs libzstd)" ON)

find_package(ZLIB REQUIRED)

add_executable(xisfconv
  src/main.cpp
  src/common.cpp
  src/xml.cpp
  src/codecs.cpp
  src/xisf.cpp
  src/convert.cpp
  src/fits.cpp
  src/tiff.cpp
  src/png.cpp
  src/wcs.cpp
)
target_link_libraries(xisfconv PRIVATE ZLIB::ZLIB)

if(MSVC)
  target_compile_options(xisfconv PRIVATE /W4)
  target_compile_definitions(xisfconv PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(xisfconv PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Zstandard: try the CMake package (vcpkg, recent distros), then pkg-config, then a plain search.
if(XISFCONV_WITH_ZSTD)
  set(_zstd_target "")
  find_package(zstd CONFIG QUIET)
  if(TARGET zstd::libzstd_shared)
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
    target_link_libraries(xisfconv PRIVATE ${_zstd_target})
    target_compile_definitions(xisfconv PRIVATE XISFCONV_HAVE_ZSTD)
    message(STATUS "xisfconv: Zstandard support enabled (${_zstd_target})")
  else()
    message(WARNING "libzstd not found: zstd-compressed XISF files will not be readable. "
                    "Install libzstd-dev (Debian/Ubuntu), libzstd-devel (Fedora) or zstd (Homebrew/vcpkg).")
  endif()
endif()

include(GNUInstallDirs)
install(TARGETS xisfconv RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
