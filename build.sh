#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build xr-tweak for aarch64 (the Steam Frame) and x86_64 (x86 apps run through FEX; and the tests)
# inside a pinned Debian bookworm image. Output: out/{aarch64,x86_64}/libxr_tweak.so
#   ./build.sh           build both
#   ./build.sh test      build both + run the off-headset tests (real loader, stub runtime, lavapipe)
#   ./build.sh shell     a shell in the build image
set -euo pipefail
here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
img=xr-tweak-build:1
if ! docker image inspect "$img" >/dev/null 2>&1; then
    docker build -q -t "$img" "$here/docker" >/dev/null
fi
run() { docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$here:/src" -w /src "$img" "$@"; }
case "${1:-build}" in
    build) run sh -c 'make -s ARCH=aarch64 && make -s ARCH=x86_64' ;;
    test)  run sh -c 'make -s ARCH=aarch64 && make -s ARCH=x86_64 && make -s ARCH=x86_64 tests && python3 test/run_tests.py && python3 test/test_cli.py' ;;
    shell) docker run --rm -it -v "$here:/src" -w /src "$img" bash ;;
    *) echo "usage: build.sh [build|test|shell]" >&2; exit 2 ;;
esac
