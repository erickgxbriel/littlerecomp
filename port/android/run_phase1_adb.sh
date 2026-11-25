#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${1:-$REPO_ROOT/out/android-arm64-phase1}"
NDK_LIBCPP="/opt/android-ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"
REMOTE_DIR="/data/local/tmp/scarface_test"

if [ ! -f "$BUILD_DIR/scarface_headless_arm64" ] || [ ! -f "$BUILD_DIR/libScarface.so" ]; then
    echo "error: build artifacts missing in $BUILD_DIR" >&2
    exit 1
fi

adb wait-for-device
adb shell "mkdir -p $REMOTE_DIR/data"
adb push "$BUILD_DIR/scarface_headless_arm64" "$REMOTE_DIR/"
adb push "$BUILD_DIR/libScarface.so" "$REMOTE_DIR/"
adb push "$NDK_LIBCPP" "$REMOTE_DIR/"
adb shell "chmod +x $REMOTE_DIR/scarface_headless_arm64"

echo "=== Running ARM64 Fiber + Exception + Symbol Interposition Self-Test ==="
adb shell "LD_LIBRARY_PATH=$REMOTE_DIR $REMOTE_DIR/scarface_headless_arm64 --test-fiber-throw"

if [ -n "${2:-}" ]; then
    GAME_ARG="$2"
    echo "=== Running ARM64 Headless Boot against $GAME_ARG ==="
    adb shell "LD_LIBRARY_PATH=$REMOTE_DIR XBE_VERBOSE=1 $REMOTE_DIR/scarface_headless_arm64 '$GAME_ARG' --data-dir $REMOTE_DIR/data --max-frames 30 --timeout 30"
fi
