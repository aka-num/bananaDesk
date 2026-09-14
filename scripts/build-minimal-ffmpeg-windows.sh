#!/usr/bin/env bash
# Rebuild the three FFmpeg DLLs shipped in the Windows portable package.
set -euo pipefail
ffmpeg_source="${1:?Usage: build-minimal-ffmpeg-windows.sh /ffmpeg-source /build-dir /stage-dir}"
ffmpeg_build="${2:?Provide an absolute build directory}"
ffmpeg_stage="${3:?Provide an absolute installation directory}"
: "${LANDESK_MINGW_ROOT:?Set to the portable toolchain/usr directory}"
: "${LANDESK_WINDOWS_SDK:?Set to the SDK mingw64 directory containing x264 headers/import library}"
ffmpeg_nasm="${LANDESK_NASM:-$(command -v nasm || true)}"
test -x "$ffmpeg_nasm" || { echo 'Set LANDESK_NASM to a native Linux NASM executable.' >&2; exit 1; }
test -x "$ffmpeg_source/configure"
test "$(cat "$ffmpeg_source/RELEASE")" = '9.0.1'
mkdir -p "$ffmpeg_build/host-tools" "$ffmpeg_stage"
ffmpeg_source="$(realpath "$ffmpeg_source")"
ffmpeg_build="$(realpath "$ffmpeg_build")"
ffmpeg_stage="$(realpath "$ffmpeg_stage")"
cross_bin="$(realpath "$LANDESK_MINGW_ROOT/bin")"
export LANDESK_WINDOWS_SDK="$(realpath "$LANDESK_WINDOWS_SDK")"
ffmpeg_cc="$cross_bin/x86_64-w64-mingw32-gcc-posix"
ffmpeg_cxx="$cross_bin/x86_64-w64-mingw32-g++-posix"
test -x "$ffmpeg_cc" || ffmpeg_cc="$cross_bin/x86_64-w64-mingw32-gcc"
test -x "$ffmpeg_cxx" || ffmpeg_cxx="$cross_bin/x86_64-w64-mingw32-g++"
# windres invokes the unsuffixed GCC name even with Ubuntu's -posix compiler.
ln -sf "$ffmpeg_cc" "$ffmpeg_build/host-tools/x86_64-w64-mingw32-gcc"
cat > "$ffmpeg_build/host-tools/pkg-config" <<'EOF'
#!/bin/sh
export PKG_CONFIG_SYSROOT_DIR="$(dirname -- "$LANDESK_WINDOWS_SDK")"
export PKG_CONFIG_LIBDIR="$LANDESK_WINDOWS_SDK/lib/pkgconfig"
unset PKG_CONFIG_PATH
exec /usr/bin/pkg-config "$@"
EOF
chmod +x "$ffmpeg_build/host-tools/pkg-config"
export PATH="$cross_bin:$ffmpeg_build/host-tools:$PATH"
cd "$ffmpeg_build"
"$ffmpeg_source/configure" \
  --prefix="$ffmpeg_stage" \
  --target-os=mingw32 --arch=x86_64 --enable-cross-compile \
  --cc="$ffmpeg_cc" --cxx="$ffmpeg_cxx" --cross-prefix=x86_64-w64-mingw32- \
  --pkg-config="$ffmpeg_build/host-tools/pkg-config" --x86asmexe="$ffmpeg_nasm" \
  --extra-cflags="-I$LANDESK_WINDOWS_SDK/include" --extra-ldflags="-L$LANDESK_WINDOWS_SDK/lib" \
  --disable-autodetect --disable-everything \
  --enable-shared --disable-static --disable-debug \
  --disable-programs --disable-doc --disable-network \
  --disable-avformat --disable-avdevice --disable-avfilter --disable-swresample \
  --enable-avcodec --enable-avutil --enable-swscale \
  --enable-gpl --enable-libx264 --enable-encoder=libx264 \
  --enable-decoder=h264 --enable-parser=h264 \
  --enable-runtime-cpudetect --enable-x86asm 2>&1 | tee configure.log
make -j"${LANDESK_BUILD_JOBS:-4}" 2>&1 | tee build.log
make install 2>&1 | tee install.log
mkdir -p "$ffmpeg_stage/licenses/FFmpeg"
cp "$ffmpeg_source/COPYING.GPLv2" "$ffmpeg_source/COPYING.LGPLv2.1" \
   "$ffmpeg_source/LICENSE.md" "$ffmpeg_stage/licenses/FFmpeg/"
cp config.h ffbuild/config.mak configure.log build.log install.log "$ffmpeg_stage/licenses/FFmpeg/"
