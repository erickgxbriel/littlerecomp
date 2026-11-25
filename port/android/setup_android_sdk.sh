#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TOOLS_DIR="$REPO_ROOT/out/android-sdk-tools"
JDK_DIR="$TOOLS_DIR/jdk17"
SDK_DIR="$TOOLS_DIR/sdk"
CMDLINE_DIR="$SDK_DIR/cmdline-tools/latest"

mkdir -p "$TOOLS_DIR" "$SDK_DIR/cmdline-tools"

if [ ! -x "$JDK_DIR/bin/javac" ]; then
    echo "=== Installing portable Eclipse Temurin JDK 17 ==="
    TMP_JDK="$(mktemp -d)"
    curl -fsSL -o "$TMP_JDK/jdk17.tar.gz" \
        "https://api.adoptium.net/v3/binary/latest/17/ga/linux/x64/jdk/hotspot/normal/eclipse"
    tar -xzf "$TMP_JDK/jdk17.tar.gz" -C "$TMP_JDK"
    EXTRACTED="$(find "$TMP_JDK" -mindepth 1 -maxdepth 1 -type d | head -n1)"
    rm -rf "$JDK_DIR"
    mv "$EXTRACTED" "$JDK_DIR"
    rm -rf "$TMP_JDK"
fi

export JAVA_HOME="$JDK_DIR"
export PATH="$JAVA_HOME/bin:$PATH"
java -version

if [ ! -x "$CMDLINE_DIR/bin/sdkmanager" ]; then
    echo "=== Installing Android cmdline-tools ==="
    TMP_CMD="$(mktemp -d)"
    curl -fsSL -o "$TMP_CMD/cmdline-tools.zip" \
        "https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip"
    unzip -q "$TMP_CMD/cmdline-tools.zip" -d "$TMP_CMD"
    rm -rf "$CMDLINE_DIR"
    mv "$TMP_CMD/cmdline-tools" "$CMDLINE_DIR"
    rm -rf "$TMP_CMD"
fi

export ANDROID_SDK_ROOT="$SDK_DIR"
export ANDROID_HOME="$SDK_DIR"
export PATH="$CMDLINE_DIR/bin:$PATH"

if [ ! -f "$SDK_DIR/platforms/android-35/android.jar" ] || [ ! -x "$SDK_DIR/build-tools/35.0.0/apksigner" ]; then
    echo "=== Installing Android SDK platforms;android-35 and build-tools;35.0.0 ==="
    set +o pipefail
    yes | sdkmanager --sdk_root="$SDK_DIR" --licenses >/dev/null || true
    yes | sdkmanager --sdk_root="$SDK_DIR" "platforms;android-35" "build-tools;35.0.0"
    set -o pipefail
fi

echo "=== Verifying toolchain ==="
"$JDK_DIR/bin/javac" -version
"$JDK_DIR/bin/keytool" -help >/dev/null
"$SDK_DIR/build-tools/35.0.0/aapt2" version
"$SDK_DIR/build-tools/35.0.0/d8" --version
test -x "$SDK_DIR/build-tools/35.0.0/zipalign"
test -x "$SDK_DIR/build-tools/35.0.0/apksigner"
test -f "$SDK_DIR/platforms/android-35/android.jar"
echo "SDK setup OK at $TOOLS_DIR"
