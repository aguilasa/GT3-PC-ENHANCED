# Build environment for the 32-bit (i686) Linux build: Ubuntu 24.04 with the
# compilers, the build tools and the -dev:i386 libraries that SDL3,
# DXVK-native, FFmpeg and the game are built against (scripts/build-linux.sh).
#
# The -dev:i386 packages belong in this container, not on a desktop host:
# on Ubuntu 24.04 installing them there makes apt remove the desktop
# (gnome-shell, gdm3).  The host only needs the :i386 runtime libraries and
# a 32-bit Vulkan driver to run the game.
#
#   docker build -t gt3pc-linux-build -f scripts/linux-build.Dockerfile scripts
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN dpkg --add-architecture i386 && apt-get update && apt-get install -y --no-install-recommends \
      build-essential gcc-multilib g++-multilib clang lld llvm cmake ninja-build meson \
      glslang-tools pkg-config git ca-certificates python3 file nasm \
      libvulkan-dev:i386 libx11-dev:i386 libxext-dev:i386 libwayland-dev:i386 \
      libxkbcommon-dev:i386 libdecor-0-dev:i386 libpipewire-0.3-dev:i386 \
      libasound2-dev:i386 libpulse-dev:i386 \
      libxcursor-dev:i386 libxfixes-dev:i386 libxi-dev:i386 libxrandr-dev:i386 libxss-dev:i386 \
      libegl-dev:i386 libudev-dev:i386 libdbus-1-dev:i386 libusb-1.0-0-dev:i386 \
      libwayland-bin wayland-protocols \
    && rm -rf /var/lib/apt/lists/*
