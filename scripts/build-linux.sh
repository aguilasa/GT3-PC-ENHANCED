#!/usr/bin/env bash
# Builds GT3 PC for 32-bit x86 Linux (i686) into build-linux/: first SDL3 and
# DXVK-native from their pinned upstream sources into build-linux/deps (the
# prefix: lib/, include/, lib/cmake/SDL3, lib/pkgconfig), then the game
# (gt3pc) and the command line installer (gt3launcher) into build-linux/bin,
# with FFmpeg built by the project itself (cmake/ffmpeg.cmake, also in
# build-linux/deps).  libSDL3 and libdxvk_d3d11 / libdxvk_dxgi are copied next
# to the programs, which find them there by their run path ($ORIGIN): the
# folder build-linux/bin runs as it is, with no LD_LIBRARY_PATH.  Extra
# arguments go to "cmake --build", e.g.  scripts/build-linux.sh --target gt3pc
#
# It needs the 32-bit (-dev:i386) development libraries, which on a desktop
# Ubuntu 24.04 cannot be installed without apt removing the desktop: it runs
# inside the container of scripts/linux-build.Dockerfile, with the clone
# mounted at the same path and as the user who owns it:
#
#   docker build -t gt3pc-linux-build -f scripts/linux-build.Dockerfile scripts
#   docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
#       -v "$PWD:$PWD" -w "$PWD" gt3pc-linux-build scripts/build-linux.sh
#
# (podman: the same commands, with "podman" for "docker" and, rootless,
# --userns=keep-id for --user.)  The network is needed the first time: the
# sources of SDL3 and DXVK-native (git, checked against their pinned commits)
# and FFmpeg's release tarball (checked against its SHA-256).
#
# To run the game the host needs only the :i386 runtime libraries (libc6,
# libstdc++6, libvulkan1, libx11-6, the sound libraries) and a 32-bit Vulkan
# driver.
#
# Environment: GT3PC_BUILD_DIR (default build-linux in the clone),
# CMAKE_BUILD_TYPE (default RelWithDebInfo, as CMakeLists.txt chooses).
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD=${GT3PC_BUILD_DIR:-$ROOT/build-linux}
DEPS=$BUILD/deps
SRC=$DEPS/src
TOOLCHAIN=$ROOT/cmake/toolchains/linux-i686.cmake
JOBS=$(nproc 2>/dev/null || echo 4)

# The pinned dependencies: a release tag and the commit it must resolve to.
SDL3_VERSION=3.2.26
SDL3_TAG=release-$SDL3_VERSION
SDL3_COMMIT=badbf8da4ee72b3ef599c721ffc9899e8d7c8d90
SDL3_URL=https://github.com/libsdl-org/SDL.git
DXVK_VERSION=3.1.1
DXVK_TAG=v$DXVK_VERSION
DXVK_COMMIT=b1a1c99ab52b687cf950d62c88bc2fa316b41663
DXVK_URL=https://github.com/doitsujin/dxvk.git

die() {
    printf 'build-linux.sh: %s\n' "$@" >&2
    exit 1
}

step() {
    printf '\n==> %s\n' "$*"
}

# ---- the build environment -------------------------------------------------
CONTAINER_HINT=(
    "build in the container of scripts/linux-build.Dockerfile, not on the host"
    "(on a desktop Ubuntu installing the -dev:i386 packages removes the desktop):"
    "  docker build -t gt3pc-linux-build -f scripts/linux-build.Dockerfile scripts"
    "  docker run --rm --user \"\$(id -u):\$(id -g)\" -e HOME=/tmp -v \"\$PWD:\$PWD\" -w \"\$PWD\" gt3pc-linux-build scripts/build-linux.sh"
)

check_environment() {
    local missing=() tool
    for tool in git cmake ninja meson glslangValidator pkg-config gcc g++ clang clang++ make; do
        command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
    done
    if ((${#missing[@]})); then
        die "missing tools: ${missing[*]}" "${CONTAINER_HINT[@]}"
    fi

    local pcdir=/usr/lib/i386-linux-gnu/pkgconfig:/usr/share/pkgconfig
    local modules=(vulkan x11 xext xkbcommon wayland-client alsa libpulse dbus-1 libudev)
    local absent=() m
    for m in "${modules[@]}"; do
        PKG_CONFIG_LIBDIR=$pcdir PKG_CONFIG_PATH='' pkg-config --exists "$m" || absent+=("$m")
    done
    local probe
    probe=$(mktemp -d)
    printf 'int main(void) { return 0; }\n' >"$probe/m32.c"
    if ! gcc -m32 "$probe/m32.c" -o "$probe/m32" >/dev/null 2>&1; then
        absent+=("gcc -m32 (gcc-multilib)")
    fi
    rm -rf "$probe"
    if ((${#absent[@]})); then
        die "the 32-bit (i386) development libraries are missing: ${absent[*]}" "${CONTAINER_HINT[@]}"
    fi
}

# ---- the sources -------------------------------------------------------------
# fetch <dir> <url> <tag> <commit> [git clone options]: a shallow clone of the
# tag, kept when it is already there at the pinned commit.
fetch() {
    local dir=$1 url=$2 tag=$3 commit=$4
    shift 4
    if [[ -d $dir/.git ]] && [[ $(git -C "$dir" rev-parse HEAD) == "$commit" ]]; then
        return 0
    fi
    [[ -e $dir ]] && die "$dir exists but is not $tag ($commit): remove it and run again"
    git clone --quiet --depth 1 --branch "$tag" -c advice.detachedHead=false "$@" "$url" "$dir.part"
    local head
    head=$(git -C "$dir.part" rev-parse HEAD)
    [[ $head == "$commit" ]] || die "$url $tag is $head, expected $commit"
    mv "$dir.part" "$dir"
}

# ---- SDL3 ----------------------------------------------------------------------
build_sdl3() {
    step "SDL3 $SDL3_VERSION"
    fetch "$SRC/SDL" "$SDL3_URL" "$SDL3_TAG" "$SDL3_COMMIT"
    cmake -S "$SRC/SDL" -B "$DEPS/build-sdl3" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
        -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
        -DCMAKE_C_FLAGS="-m32 -msse2 -mfpmath=sse" -DCMAKE_CXX_FLAGS="-m32 -msse2 -mfpmath=sse" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$DEPS" -DCMAKE_INSTALL_LIBDIR=lib \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
    cmake --build "$DEPS/build-sdl3" -j "$JOBS"
    cmake --install "$DEPS/build-sdl3"
}

# ---- DXVK-native -----------------------------------------------------------------
build_dxvk() {
    step "DXVK-native $DXVK_VERSION"
    fetch "$SRC/dxvk" "$DXVK_URL" "$DXVK_TAG" "$DXVK_COMMIT" \
        --recurse-submodules --shallow-submodules
    # meson's cross file: gcc -m32 with SSE2 floating point, the 32-bit
    # pkg-config files and the prefix's (SDL3).
    local cross=$DEPS/i686-linux.meson
    cat >"$cross" <<MESON
[binaries]
c = 'gcc'
cpp = 'g++'
ar = 'ar'
strip = 'strip'
pkg-config = 'pkg-config'
[built-in options]
c_args = ['-m32', '-msse2', '-mfpmath=sse']
cpp_args = ['-m32', '-msse2', '-mfpmath=sse']
c_link_args = ['-m32']
cpp_link_args = ['-m32']
[properties]
pkg_config_libdir = ['$DEPS/lib/pkgconfig', '/usr/lib/i386-linux-gnu/pkgconfig', '/usr/share/pkgconfig']
[host_machine]
system = 'linux'
cpu_family = 'x86'
cpu = 'i686'
endian = 'little'
MESON
    local reconfigure=()
    [[ -f $DEPS/build-dxvk/build.ninja ]] && reconfigure=(--reconfigure)
    meson setup "${reconfigure[@]}" "$DEPS/build-dxvk" "$SRC/dxvk" --cross-file "$cross" \
        --prefix "$DEPS" --libdir lib --buildtype release \
        -Dnative_sdl3=enabled -Dnative_sdl2=disabled -Dnative_glfw=disabled
    ninja -C "$DEPS/build-dxvk" -j "$JOBS"
    meson install -C "$DEPS/build-dxvk" --quiet
}

# ---- the game and the installer ----------------------------------------------------
build_game() {
    step "gt3pc and gt3launcher"
    local type=()
    [[ -n ${CMAKE_BUILD_TYPE:-} ]] && type=(-DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE")
    # SDL3 and DXVK-native from the prefix; the run path is only $ORIGIN,
    # which finds the copies next to the programs.  (The build tree's run
    # path would also hold the prefix's folder, an absolute path of this
    # machine, since DXVK is linked by its path: the install run path is
    # used instead, and it never takes the link folders.)
    cmake -S "$ROOT" -B "$BUILD" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" "${type[@]}" \
        -DSDL3_DIR="$DEPS/lib/cmake/SDL3" \
        -DGT3PC_DXVK_ROOT="$DEPS" \
        -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON \
        -DCMAKE_INSTALL_RPATH="\$ORIGIN"
    cmake --build "$BUILD" -j "$JOBS" "$@"

    local bin=$BUILD/bin lib
    mkdir -p "$bin"
    for lib in libSDL3.so.0 libdxvk_d3d11.so.0 libdxvk_dxgi.so.0; do
        cp -L "$DEPS/lib/$lib" "$bin/$lib"
    done
    cp "$SRC/SDL/LICENSE.txt" "$bin/SDL3-LICENSE.txt"
    cp "$SRC/dxvk/LICENSE" "$bin/dxvk-LICENSE"

    if [[ -f $bin/gt3pc ]]; then
        local unresolved
        unresolved=$(env -u LD_LIBRARY_PATH ldd "$bin/gt3pc" | grep 'not found' || true)
        [[ -z $unresolved ]] || die "gt3pc has unresolved libraries:" "$unresolved"
    fi
}

check_environment
mkdir -p "$SRC"
build_sdl3
build_dxvk
build_game "$@"
step "Done: $BUILD/bin"
ls -l "$BUILD/bin"
