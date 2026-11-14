# SPDX-License-Identifier: MIT
# Build + test image for xr-tweak. Debian bookworm: glibc 2.36, gcc 12,
# lavapipe (software Vulkan) for the off-headset tests, and an aarch64 cross compiler.
# The OpenXR headers and loader come from a pinned Khronos OpenXR-SDK release, built here,
# so the tests run against the same loader generation apps ship today.
FROM debian:bookworm-slim
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build pkg-config python3 curl ca-certificates \
      gcc-aarch64-linux-gnu g++-aarch64-linux-gnu binutils-aarch64-linux-gnu \
      glslang-tools libvulkan-dev mesa-vulkan-drivers vulkan-tools file \
      libx11-dev libxxf86vm-dev libxrandr-dev libgl-dev \
    && rm -rf /var/lib/apt/lists/*
ARG OPENXR_SDK=release-1.1.63
RUN mkdir -p /opt/src && cd /opt/src \
    && curl -fsSL https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/${OPENXR_SDK}.tar.gz | tar xz \
    && cmake -S OpenXR-SDK-${OPENXR_SDK} -B /opt/build/openxr -G Ninja -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_INSTALL_PREFIX=/opt/openxr -DDYNAMIC_LOADER=ON -DBUILD_TESTS=OFF -DBUILD_API_LAYERS=OFF -DPRESENTATION_BACKEND=xlib \
    && cmake --build /opt/build/openxr && cmake --install /opt/build/openxr \
    && rm -rf /opt/build
ENV OPENXR_SDK=${OPENXR_SDK}
