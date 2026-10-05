#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Jurgen Kobierczynski
#
# Builds Zstandard as a static, position-independent library for the Python wheels, so that the
# libxisfconv inside a wheel needs no libzstd on the machine it is installed on.
#
#   python/tools/build-zstd.sh <install prefix>
#
# then build with CMAKE_PREFIX_PATH=<install prefix>. Needs curl (or wget), cmake and a C
# compiler. The archive is checked against its SHA-256 before anything of it is used.
set -eu

version=1.5.7
sha256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3

prefix=${1:?usage: build-zstd.sh <install prefix>}
if [ -f "$prefix/lib/libzstd.a" ] || [ -f "$prefix/lib64/libzstd.a" ]; then
  echo "build-zstd: $prefix already holds libzstd.a"
  exit 0
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
archive="$work/zstd.tar.gz"
url="https://github.com/facebook/zstd/releases/download/v$version/zstd-$version.tar.gz"
if command -v curl > /dev/null 2>&1; then
  curl -fsSL -o "$archive" "$url"
else
  wget -q -O "$archive" "$url"
fi
if command -v sha256sum > /dev/null 2>&1; then
  have=$(sha256sum "$archive" | cut -d' ' -f1)
else
  have=$(shasum -a 256 "$archive" | cut -d' ' -f1)
fi
if [ "$have" != "$sha256" ]; then
  echo "build-zstd: the archive of zstd $version has SHA-256 $have, expected $sha256" >&2
  exit 1
fi

tar -xzf "$archive" -C "$work"
cmake -S "$work/zstd-$version/build/cmake" -B "$work/build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DCMAKE_INSTALL_LIBDIR=lib \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DZSTD_BUILD_STATIC=ON -DZSTD_BUILD_SHARED=OFF \
  -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF -DZSTD_BUILD_CONTRIB=OFF \
  -DZSTD_MULTITHREAD_SUPPORT=OFF -DZSTD_LEGACY_SUPPORT=OFF
cmake --build "$work/build" --parallel
cmake --install "$work/build"
echo "build-zstd: zstd $version installed in $prefix"
