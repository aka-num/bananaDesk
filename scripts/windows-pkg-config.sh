#!/bin/sh
set -eu
: "${LANDESK_WINDOWS_SDK:?Set LANDESK_WINDOWS_SDK to the absolute sdk/mingw64 path}"
export PKG_CONFIG_SYSROOT_DIR="$(dirname "$LANDESK_WINDOWS_SDK")"
export PKG_CONFIG_LIBDIR="$LANDESK_WINDOWS_SDK/lib/pkgconfig"
unset PKG_CONFIG_PATH
exec pkg-config "$@"
