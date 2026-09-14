#!/usr/bin/env bash
set -euo pipefail
LANDESK_WINE_ROOT="${LANDESK_WINDOWS_TEST_DIR:-$(cd -- "$(dirname -- "$0")" && pwd)}"
export LD_LIBRARY_PATH="$LANDESK_WINE_ROOT/root/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export WINEDLLPATH="$LANDESK_WINE_ROOT/root11/opt/wine-stable/lib/wine"
export WINESERVER="$LANDESK_WINE_ROOT/root11/opt/wine-stable/bin/wineserver"
export WINELOADER="$LANDESK_WINE_ROOT/root11/opt/wine-stable/bin/wine"
export WINEPREFIX="$LANDESK_WINE_ROOT/prefix11-full"
export WINEARCH=win64
export WINEDLLOVERRIDES="mscoree,mshtml="
export WINEDEBUG="${LANDESK_WINEDEBUG:--all}"
export XDG_RUNTIME_DIR="$LANDESK_WINE_ROOT/runtime11"
export TMPDIR="$LANDESK_WINE_ROOT/tmp11"
mkdir -p "$TMPDIR/.X11-unix"
export XDG_CACHE_HOME="$LANDESK_WINE_ROOT/cache11"
export XDG_CONFIG_HOME="$LANDESK_WINE_ROOT/config11"
export XDG_DATA_HOME="$LANDESK_WINE_ROOT/data11"
export DISPLAY="${LANDESK_WINE_DISPLAY:-:$(cat "$LANDESK_WINE_ROOT/display.txt")}" QT_QPA_PLATFORM=windows
unset QT_PLUGIN_PATH QT_QPA_PLATFORM_PLUGIN_PATH WAYLAND_DISPLAY
mkdir -p "$XDG_RUNTIME_DIR";chmod 700 "$XDG_RUNTIME_DIR"
if [[ "${1:-}" == --server ]]; then shift; WINELOADER="$WINESERVER"; fi
exec bwrap --ro-bind / / --bind "$LANDESK_WINE_ROOT" "$LANDESK_WINE_ROOT" --bind "$XDG_RUNTIME_DIR" "/run/user/$(id -u)" --bind "$TMPDIR" /tmp --ro-bind /tmp/.X11-unix /tmp/.X11-unix --dev-bind /dev /dev --proc /proc -- "$WINELOADER" "$@"
