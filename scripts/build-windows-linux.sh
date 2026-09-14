#!/usr/bin/env bash
set -euo pipefail
source_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:?Usage: build-windows-linux.sh /absolute/build-directory}"
: "${LANDESK_MINGW_ROOT:?Set to the portable toolchain/usr directory}"
: "${LANDESK_WINDOWS_SDK:?Set to the SDK mingw64 directory}"
cmake -S "$source_dir" -B "$build_dir" \
  -DCMAKE_TOOLCHAIN_FILE="$source_dir/cmake/mingw-portable.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DLANDESK_BUILD_TESTS=ON \
  -DLANDESK_HOST_MOC="${LANDESK_HOST_MOC:-/usr/lib/qt5/bin/moc}" \
  -DLANDESK_HOST_RCC="${LANDESK_HOST_RCC:-/usr/lib/qt5/bin/rcc}"
cmake --build "$build_dir" --parallel "${LANDESK_BUILD_JOBS:-4}"
