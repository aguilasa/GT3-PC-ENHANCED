#!/usr/bin/env bash
# Packages the 32-bit x86 Linux build of scripts/build-linux.sh (build-linux/bin)
# as an i686 AppImage: build-linux/appimage/GT3PC-i686.AppImage.  It holds
# gt3pc, gt3launcher, libSDL3, libdxvk_d3d11 / libdxvk_dxgi, the licences of
# SDL3, DXVK and FFmpeg, packaging/linux/gt3pc.desktop and its PNG icon, and
# packaging/linux/AppRun as the entry point:
#
#   ./GT3PC-i686.AppImage --install <image.iso>   installs the game files
#   ./GT3PC-i686.AppImage                         plays
#   ./GT3PC-i686.AppImage launcher [arguments]    gt3launcher as it is
#
# The game writes beside its executable (gt3pc.ini, the log, the save
# folders, the caches), so AppRun copies the programs once per AppImage into
# GT3PC_HOME (default ~/.local/share/gt3pc) and runs them from there; the
# game files are installed there too (packaging/linux/AppRun).
#
# What the machine that runs it needs (no development packages): the :i386
# runtime libraries the AppImage does not carry - libc6, libstdc++6,
# libgcc-s1, libvulkan1, libx11-6, libxext6 and, for sound, libasound2t64 or
# libpulse0 - and a 32-bit Vulkan driver: mesa-vulkan-drivers:i386, or the
# NVIDIA driver's 32-bit libraries (libnvidia-gl-<version>:i386).  The
# AppImage is mounted with FUSE (libfuse2 is not needed: the runtime carries
# it, but the fusermount program is); without FUSE (a container) it runs with
# --appimage-extract-and-run, or APPIMAGE_EXTRACT_AND_RUN=1 in the
# environment.
#
# It runs on the host, after scripts/build-linux.sh: it needs bash, curl,
# sha256sum and file, and fetches the first time (into the output folder's
# tools/, checked against their pinned SHA-256) appimagetool and the i686
# AppImage runtime from their upstream GitHub releases.
#
# Environment: GT3PC_BUILD_DIR (default build-linux in the clone, whose bin/
# is packaged), GT3PC_APPIMAGE_DIR (default <build dir>/appimage: the AppDir,
# the tools and the AppImage).
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD=${GT3PC_BUILD_DIR:-$ROOT/build-linux}
BIN=$BUILD/bin
OUT=${GT3PC_APPIMAGE_DIR:-$BUILD/appimage}
PACKAGING=$ROOT/packaging/linux
APPDIR=$OUT/AppDir
TOOLS=$OUT/tools
IMAGE=$OUT/GT3PC-i686.AppImage

# The pinned tools: appimagetool for the machine that packages, and the
# runtime of an i686 AppImage (the part that mounts it), with their SHA-256.
APPIMAGETOOL_VERSION=1.9.1
APPIMAGETOOL_URL=https://github.com/AppImage/appimagetool/releases/download/$APPIMAGETOOL_VERSION
APPIMAGETOOL_SHA256_x86_64=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
APPIMAGETOOL_SHA256_i686=7ad9ff47c203aae0149b18f6df9e3018b2e2f470ea644a0413e3ded39e9e3bdb
RUNTIME_VERSION=20251108
RUNTIME_URL=https://github.com/AppImage/type2-runtime/releases/download/$RUNTIME_VERSION/runtime-i686
RUNTIME_SHA256=e72ea0b140a0a16e680713238a6f30aad278b62c4ca17919c554864124515498

PROGRAMS=(gt3pc gt3launcher libSDL3.so.0 libdxvk_d3d11.so.0 libdxvk_dxgi.so.0)
LICENCES=(SDL3-LICENSE.txt dxvk-LICENSE ffmpeg-LICENSE.md ffmpeg-COPYING.LGPLv2.1)

die() {
    printf 'package-appimage.sh: %s\n' "$@" >&2
    exit 1
}

step() {
    printf '\n==> %s\n' "$*"
}

# fetch <url> <sha256> <file>: downloads once, and checks it every time
fetch() {
    local url=$1 sum=$2 file=$3
    if [[ ! -f $file ]]; then
        curl -fL --retry 3 -o "$file.part" "$url" || die "could not download $url"
        mv "$file.part" "$file"
    fi
    if ! printf '%s  %s\n' "$sum" "$file" | sha256sum -c --quiet -; then
        rm -f "$file"
        die "$url: not the SHA-256 expected ($sum); removed, run again"
    fi
}

# ---- what is packaged --------------------------------------------------------
for tool in curl sha256sum file; do
    command -v "$tool" >/dev/null 2>&1 || die "missing tool: $tool"
done
for f in "${PROGRAMS[@]}" "${LICENCES[@]}"; do
    [[ -f $BIN/$f ]] || die "$BIN/$f is missing: build first with scripts/build-linux.sh"
done
for f in gt3pc gt3launcher; do
    file -L "$BIN/$f" | grep -q 'ELF 32-bit.*Intel 80386' ||
        die "$BIN/$f is not a 32-bit x86 program (scripts/build-linux.sh builds one)"
done
for f in AppRun gt3pc.desktop gt3pc.png; do
    [[ -f $PACKAGING/$f ]] || die "$PACKAGING/$f is missing"
done

host=$(uname -m)
case $host in
x86_64) tool_sum=$APPIMAGETOOL_SHA256_x86_64 ;;
i?86) host=i686 tool_sum=$APPIMAGETOOL_SHA256_i686 ;;
*) die "appimagetool is fetched for x86_64 and i686 machines, not $host" ;;
esac

# ---- the tools -------------------------------------------------------------
step "appimagetool $APPIMAGETOOL_VERSION ($host) and the i686 runtime $RUNTIME_VERSION"
mkdir -p "$TOOLS"
APPIMAGETOOL=$TOOLS/appimagetool-$APPIMAGETOOL_VERSION-$host.AppImage
RUNTIME=$TOOLS/runtime-i686-$RUNTIME_VERSION
fetch "$APPIMAGETOOL_URL/appimagetool-$host.AppImage" "$tool_sum" "$APPIMAGETOOL"
fetch "$RUNTIME_URL" "$RUNTIME_SHA256" "$RUNTIME"
chmod +x "$APPIMAGETOOL"

# ---- the AppDir ------------------------------------------------------------
step "AppDir $APPDIR"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/lib/gt3pc" "$APPDIR/usr/share/licenses/gt3pc" "$APPDIR/usr/share/gt3pc" \
    "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons/hicolor/256x256/apps"
for f in "${PROGRAMS[@]}"; do
    cp "$BIN/$f" "$APPDIR/usr/lib/gt3pc/$f"
done
for f in "${LICENCES[@]}"; do
    cp "$BIN/$f" "$APPDIR/usr/share/licenses/gt3pc/$f"
done
# (AppRun copies the programs out again when this changes: their checksums)
(cd "$APPDIR/usr/lib/gt3pc" && sha256sum "${PROGRAMS[@]}") | sha256sum | cut -d' ' -f1 \
    > "$APPDIR/usr/share/gt3pc/build-id"
install -m 755 "$PACKAGING/AppRun" "$APPDIR/AppRun"
cp "$PACKAGING/gt3pc.desktop" "$APPDIR/gt3pc.desktop"
cp "$PACKAGING/gt3pc.desktop" "$APPDIR/usr/share/applications/gt3pc.desktop"
cp "$PACKAGING/gt3pc.png" "$APPDIR/gt3pc.png"
cp "$PACKAGING/gt3pc.png" "$APPDIR/usr/share/icons/hicolor/256x256/apps/gt3pc.png"
ln -s gt3pc.png "$APPDIR/.DirIcon"

# ---- the AppImage ----------------------------------------------------------
step "AppImage $IMAGE"
rm -f "$IMAGE"
# (appimagetool is itself an AppImage: extracted and run, with no FUSE needed)
ARCH=i686 APPIMAGE_EXTRACT_AND_RUN=1 "$APPIMAGETOOL" --no-appstream --runtime-file "$RUNTIME" \
    "$APPDIR" "$IMAGE"
file "$IMAGE"
