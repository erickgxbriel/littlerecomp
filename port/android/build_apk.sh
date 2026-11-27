#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${1:-$REPO_ROOT/out/android-arm64-phase1}"
BUILD_TYPE="${BUILD_TYPE:-debug}"
TOOLS_DIR="$REPO_ROOT/out/android-sdk-tools"
JDK_DIR="$TOOLS_DIR/jdk17"
SDK_DIR="$TOOLS_DIR/sdk"
BUILD_TOOLS="$SDK_DIR/build-tools/35.0.0"
PLATFORM_JAR="$SDK_DIR/platforms/android-35/android.jar"
APK_SRC="$SCRIPT_DIR/apk"
SDL_JAVA="$REPO_ROOT/out/deps/sdl3-src/android-project/app/src/main/java"
NDK_BIN="/opt/android-ndk/toolchains/llvm/prebuilt/linux-x86_64/bin"
NDK_LIBCPP="/opt/android-ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"

if [ ! -f "$PLATFORM_JAR" ] || [ ! -x "$JDK_DIR/bin/javac" ]; then
    "$SCRIPT_DIR/setup_android_sdk.sh"
fi

export JAVA_HOME="$JDK_DIR"
export PATH="$JAVA_HOME/bin:$PATH"

SHADERC_CACHE="$BUILD_DIR/shaderc"
SHADERC_LIB="$SHADERC_CACHE/libs/c++_shared/arm64-v8a/libshaderc.a"
if [ ! -f "$SHADERC_LIB" ]; then
    echo "=== Building offline NDK libshaderc_combined (arm64-v8a, cached) ==="
    mkdir -p "$SHADERC_CACHE"
    /opt/android-ndk/ndk-build \
        NDK_PROJECT_PATH=/opt/android-ndk/sources/third_party/shaderc \
        APP_BUILD_SCRIPT=/opt/android-ndk/sources/third_party/shaderc/Android.mk \
        APP_STL=c++_shared \
        APP_ABI=arm64-v8a \
        APP_PLATFORM=android-26 \
        NDK_OUT="$SHADERC_CACHE/obj" \
        NDK_LIBS_OUT="$SHADERC_CACHE/libs" \
        libshaderc_combined -j"$(nproc)"
fi

WANT_OPT="OFF"
if [ "$BUILD_TYPE" = "release" ]; then
    WANT_OPT="ON"
fi
if ! grep -q "SCARFACE_RELEASE_OPT:BOOL=$WANT_OPT" "$BUILD_DIR/CMakeCache.txt" 2>/dev/null; then
    echo "=== Reconfiguring CMake (SCARFACE_RELEASE_OPT=$WANT_OPT) ==="
    cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -DSCARFACE_BUILD_ANDROID_APP=ON -DSCARFACE_RELEASE_OPT="$WANT_OPT"
fi

echo "=== Building native ARM64 libraries ($BUILD_TYPE: Scarface_code, SDL3-shared, main) ==="
cmake --build "$BUILD_DIR" --target Scarface_code SDL3-shared main -j"$(nproc)"

WORK="$BUILD_DIR/apk_work"
rm -rf "$WORK"

LIB_SCARFACE="$BUILD_DIR/libScarface.so"
LIB_MAIN="$BUILD_DIR/libmain.so"
LIB_SDL3="$(find "$BUILD_DIR" "$REPO_ROOT/out/deps/sdl3-build" -maxdepth 2 -name 'libSDL3.so' 2>/dev/null | head -n1)"

for f in "$LIB_SCARFACE" "$LIB_MAIN" "$LIB_SDL3" "$NDK_LIBCPP"; do
    if [ ! -f "$f" ]; then
        echo "error: missing library $f" >&2
        exit 1
    fi
done

mkdir -p "$WORK/lib/arm64-v8a" "$WORK/classes" "$WORK/dex"

echo "=== Staging arm64-v8a libraries ($BUILD_TYPE) ==="
cp "$LIB_SDL3" "$WORK/lib/arm64-v8a/libSDL3.so"
cp "$NDK_LIBCPP" "$WORK/lib/arm64-v8a/libc++_shared.so"
if [ "$BUILD_TYPE" = "release" ]; then
    "$NDK_BIN/llvm-strip" --strip-all -o "$WORK/lib/arm64-v8a/libmain.so" "$LIB_MAIN"
    "$NDK_BIN/llvm-strip" --strip-debug -o "$WORK/lib/arm64-v8a/libScarface.so" "$LIB_SCARFACE"
else
    cp "$LIB_MAIN" "$WORK/lib/arm64-v8a/libmain.so"
    "$NDK_BIN/llvm-strip" --strip-debug -o "$WORK/lib/arm64-v8a/libScarface.so" "$LIB_SCARFACE"
fi
ls -lh "$WORK/lib/arm64-v8a/"

MANIFEST="$WORK/AndroidManifest.xml"
cp "$APK_SRC/AndroidManifest.xml" "$MANIFEST"
if [ "$BUILD_TYPE" = "release" ]; then
    sed -i 's/android:debuggable="true"/android:debuggable="false"/' "$MANIFEST"
fi

echo "=== Generating Miami Noir icon & compiling resources ==="
python3 "$APK_SRC/make_icon.py"
"$BUILD_TOOLS/aapt2" compile --dir "$APK_SRC/res" -o "$WORK/res.zip"
"$BUILD_TOOLS/aapt2" link \
    -I "$PLATFORM_JAR" \
    --manifest "$MANIFEST" \
    -o "$WORK/unaligned.apk" \
    "$WORK/res.zip"

echo "=== Compiling Java + D8 DEX ==="
mapfile -t JAVA_FILES < <(find "$SDL_JAVA/org/libsdl/app" "$APK_SRC/src" -name '*.java' | sort)
"$JDK_DIR/bin/javac" \
    --release 11 \
    -Xlint:none \
    -cp "$PLATFORM_JAR" \
    -d "$WORK/classes" \
    "${JAVA_FILES[@]}"

mapfile -t CLASS_FILES < <(find "$WORK/classes" -name '*.class' | sort)
"$BUILD_TOOLS/d8" \
    --min-api 30 \
    --lib "$PLATFORM_JAR" \
    --output "$WORK/dex" \
    "${CLASS_FILES[@]}"

echo "=== Packaging uncompressed native libs (16 KB aligned) ==="
python3 - "$WORK/unaligned.apk" "$WORK/dex/classes.dex" "$WORK/lib/arm64-v8a" <<'PY'
import sys
import zipfile
from pathlib import Path

apk_path, dex_path, lib_dir = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
with zipfile.ZipFile(apk_path, "a") as zf:
    zf.write(dex_path, "classes.dex", compress_type=zipfile.ZIP_DEFLATED)
    for so in sorted(lib_dir.glob("*.so")):
        zf.write(so, f"lib/arm64-v8a/{so.name}", compress_type=zipfile.ZIP_STORED)
PY

"$BUILD_TOOLS/zipalign" -P 16 -f 4 "$WORK/unaligned.apk" "$WORK/aligned.apk"
"$BUILD_TOOLS/zipalign" -c -P 16 4 "$WORK/aligned.apk"

if [ "$BUILD_TYPE" = "release" ]; then
    KEYSTORE="${SCARFACE_KEYSTORE:-$TOOLS_DIR/release.keystore}"
    KEY_ALIAS="${SCARFACE_KEY_ALIAS:-scarfacedroid}"
    KEY_PASS="${SCARFACE_KEY_PASS:-scarfacedroid_beta}"
    if [ ! -f "$KEYSTORE" ]; then
        "$JDK_DIR/bin/keytool" -genkeypair -v \
            -keystore "$KEYSTORE" \
            -storepass "$KEY_PASS" -keypass "$KEY_PASS" \
            -alias "$KEY_ALIAS" \
            -keyalg RSA -keysize 2048 -validity 10000 \
            -dname "CN=LittleRecomp,O=OpenSource,C=BR"
    fi
    OUT_APK="$BUILD_DIR/LittleRecomp-v0.4.0-beta-release.apk"
else
    KEYSTORE="$TOOLS_DIR/debug.keystore"
    KEY_ALIAS="androiddebugkey"
    KEY_PASS="android"
    if [ ! -f "$KEYSTORE" ]; then
        "$JDK_DIR/bin/keytool" -genkeypair -v \
            -keystore "$KEYSTORE" \
            -storepass android -keypass android \
            -alias androiddebugkey \
            -keyalg RSA -keysize 2048 -validity 10000 \
            -dname "CN=Android Debug,O=Android,C=US"
    fi
    OUT_APK="$BUILD_DIR/ScarfaceDroid-debug.apk"
fi

"$BUILD_TOOLS/apksigner" sign \
    --ks "$KEYSTORE" \
    --ks-key-alias "$KEY_ALIAS" \
    --ks-pass "pass:$KEY_PASS" \
    --key-pass "pass:$KEY_PASS" \
    --out "$OUT_APK" \
    "$WORK/aligned.apk"

"$BUILD_TOOLS/apksigner" verify --verbose "$OUT_APK"
ls -lh "$OUT_APK"
