#!/bin/sh
# check-macos-binary.sh — assert a macOS build has the expected architecture
# and minimum macOS version.
#
#   scripts/check-macos-binary.sh <binary> <arm64|x64> <minos>
#
# Both can drift without any build error:
# - a runner label can be remapped to other hardware;
# - the minimum version defaults to the build machine's SDK, so moving to a
#   newer runner silently drops users of older macOS releases.
# The published binaries have targeted macOS 14.0 since the macos-14 runner.
set -eu

bin=$1
want_arch=$2
want_minos=$3

case "$want_arch" in
  x64) want_arch=x86_64 ;;
esac

arch=$(lipo -archs "$bin")
minos=$(otool -l "$bin" | awk '/LC_BUILD_VERSION/ { f = 1 } f && $1 == "minos" { print $2; exit }')
echo "$bin: arch=$arch minos=$minos"

if [ "$arch" != "$want_arch" ]; then
  echo "::error::$bin is $arch, expected $want_arch" >&2
  exit 1
fi
if [ "$minos" != "$want_minos" ]; then
  echo "::error::$bin requires macOS $minos, expected $want_minos" >&2
  exit 1
fi
