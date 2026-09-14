# Windows runtime dependencies

This portable directory includes dynamically linked libraries from the MSYS2
mingw64 distribution. `dependency-manifest.json` records each included package's
exact version, archive SHA-256, upstream website, official archive URL, package
license labels, and the runtime files selected from it. `runtime-manifest.json`
records the SHA-256 and PE imports of every included executable and DLL.

Package-specific copyright and license notices are included under `licenses/`.
Shared GNU, Apache, BSD, and Mozilla license texts are under `licenses/common/`.
Some MSYS2 packages rely on distribution-wide license texts and do not include a
package-specific license directory; these packages are listed explicitly in the
dependency manifest. Package license labels describe the upstream package and
are not a declaration that every alternative license applies to every file.

This distribution uses GPL-enabled FFmpeg with the libx264 encoder. The complete
MSYS2 FFmpeg package is GPL-3.0-or-later. When a smaller custom FFmpeg build is
selected, its exact source, configuration, and license are recorded in the
dependency manifest's `local_overlay` entry. It is not an LGPL-only FFmpeg configuration.
Qt libraries remain separate DLLs and can be replaced with compatible rebuilt
versions. The included notices and source links do not waive the requirements of
the respective licenses when redistributing or modifying these dependencies.

Upstream source and build recipes:

- Qt: https://download.qt.io/archive/qt/ and https://invent.kde.org/qt/qt/qtbase
- FFmpeg: https://ffmpeg.org/download.html
- x264: https://code.videolan.org/videolan/x264
- OpenSSL: https://openssl-library.org/source/
- MSYS2 package recipes and patches: https://github.com/msys2/MINGW-packages

The selected package archive URLs and checksums are the authoritative record of
the exact binaries in this directory. Upstream URLs and package recipes are
source references; this directory is not a complete archive of the corresponding
source of every third-party library. Keep the LanDesk application source with
the runtime release, and retain this dependency manifest and the notices.

The runtime package does not include Windows system DLLs. Windows 10 x64 supplies
those libraries. The packager verifies non-system PE imports and explicitly adds
OpenSSL, which Qt loads dynamically. This static verification does not substitute
for testing the application on the destination Windows installation.
