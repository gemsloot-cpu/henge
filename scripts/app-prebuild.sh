#!/bin/sh
# Xcode pre-build phase for the henge app (see app/project.yml): converts
# samples/libadd.so for the platform being built via `make app-dylib`.
set -e
case "$PLATFORM_NAME" in
  iphonesimulator) P=iossim ;;
  iphoneos) P=ios ;;
  *) echo "error: unsupported PLATFORM_NAME $PLATFORM_NAME"; exit 1 ;;
esac
# Clean env so the host-side tools (henge-convert, NDK clang) aren't built
# against Xcode's iOS SDKROOT / arch settings. NDK_ROOT is forwarded when set.
exec /usr/bin/env -i HOME="$HOME" PATH="/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin" \
  /usr/bin/make -C "$SRCROOT/.." app-dylib PLATFORM="$P" ${NDK_ROOT:+NDK_ROOT="$NDK_ROOT"}
