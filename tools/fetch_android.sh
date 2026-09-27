#!/bin/sh
# Versioned downloads and all large Android caches stay off the root filesystem.
set -eu
export JAVA_HOME=${JAVA_HOME:-/usr/lib/jvm/java-21-openjdk-amd64}
export ANDROID_HOME=/mnt/build/android-sdk
export ANDROID_SDK_ROOT=$ANDROID_HOME
export GRADLE_USER_HOME=/mnt/build/gradle
deps=/mnt/build/android-deps
mkdir -p "$ANDROID_HOME/cmdline-tools" "$deps" "$GRADLE_USER_HOME"
fetch() {
    if [ ! -f "$2" ]; then
        curl --fail --location --retry 3 --output "$2.part" "$1"
        mv "$2.part" "$2"
    fi
}
if [ ! -x "$ANDROID_HOME/cmdline-tools/11076708/bin/sdkmanager" ]; then
    fetch https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip "$deps/commandlinetools-linux-11076708.zip"
    unzip -q "$deps/commandlinetools-linux-11076708.zip" -d "$ANDROID_HOME/cmdline-tools"
    mv "$ANDROID_HOME/cmdline-tools/cmdline-tools" "$ANDROID_HOME/cmdline-tools/11076708"
fi
sdkmanager="$ANDROID_HOME/cmdline-tools/11076708/bin/sdkmanager"
# sdkmanager reads the license stream; yes can exit on the closed pipe.
yes | "$sdkmanager" --sdk_root="$ANDROID_HOME" --licenses >/dev/null
"$sdkmanager" --sdk_root="$ANDROID_HOME" 'platform-tools' 'platforms;android-34' 'build-tools;34.0.0' 'ndk;26.3.11579264' 'cmake;3.22.1'
if [ ! -f "$deps/SDL2-2.32.10/CMakeLists.txt" ]; then
    fetch https://www.libsdl.org/release/SDL2-2.32.10.tar.gz "$deps/SDL2-2.32.10.tar.gz"
    tar -xzf "$deps/SDL2-2.32.10.tar.gz" -C "$deps"
fi
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
"$root/tools/extract_installer.sh"
printf 'Android SDK: %s\nSDL2: %s\n' "$ANDROID_HOME" "$deps/SDL2-2.32.10"
