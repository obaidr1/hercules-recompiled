#!/usr/bin/env bash
# Android APK: the runtime + game as libmain.so per ABI (NDK, CMake), packaged
# with SDL3's Java glue by Gradle (android/).
#
#   scripts/build_android.sh [ABI ...]     # default: arm64-v8a x86_64
#       -> android/app/build/outputs/apk/{debug,release}/*.apk
#
# Env: ANDROID_HOME (SDK with NDK r30), JAVA_HOME (JDK 17), SDL3_SRC (SDL3
# source tree, psxrecomp/tools/ci/prefetch_sdl3.sh), HERC_KEYSTORE +
# HERC_KEYSTORE_PASS to also sign a release APK (only where the key is).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
: "${ANDROID_HOME:?set ANDROID_HOME}" "${SDL3_SRC:?set SDL3_SRC}"
NDK="${ANDROID_NDK:-$ANDROID_HOME/ndk/30.0.16248370}"
ABIS=("$@"); [ ${#ABIS[@]} -gt 0 ] || ABIS=(arm64-v8a x86_64)
STRIP="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"

for abi in "${ABIS[@]}"; do
  b="build-android-$abi"
  cmake -S . -B "$b" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$abi" -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static \
    -DPSX_ENABLE_VULKAN=OFF -DPSX_RECOMP_UI=OFF -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE \
    -DFETCHCONTENT_SOURCE_DIR_SDL3="$SDL3_SRC" ${HERC_ANDROID_CMAKE_ARGS:-}
  cmake --build "$b" --target psx-runtime
  mkdir -p "android/app/libs/$abi"
  "$STRIP" --strip-unneeded -o "android/app/libs/$abi/libmain.so" "$b/libHercules_Recompiled.so"
done

# Gradle wrapper and placeholder icons/theme from SDL3's android-project.
T="$SDL3_SRC/android-project"
cp -r "$T/gradle" "$T/gradlew" android/
for d in "$T"/app/src/main/res/mipmap-*; do cp -r "$d" android/app/src/main/res/; done
cp "$T/app/src/main/res/values/colors.xml" "$T/app/src/main/res/values/styles.xml" android/app/src/main/res/values/
echo "sdk.dir=$ANDROID_HOME" > android/local.properties
(cd android && ./gradlew --no-daemon -q assembleDebug $( [ -n "${HERC_KEYSTORE:-}" ] && echo assembleRelease ))
ls -la android/app/build/outputs/apk/*/*.apk
