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

# The runtime's own files (the same for every ABI) as APK assets under runtime/:
# files.txt lists them, stamp.txt hashes them; the app writes them into its
# files folder when the stamp changes (main.cpp android_install_runtime_files).
# Never the disc or anything the player owns.
R=android/app/src/main/assets/runtime
rm -rf "$R" && mkdir -p "$R"
(cd "build-android-${ABIS[0]}" && cp -r --parents assets bios game.toml game_options.toml \
   psx_game_version.txt mods/README.md mods/bundled "$ROOT/$R/")
(cd "$R" && find . -type f ! -name files.txt ! -name stamp.txt | sed 's#^\./##' | LC_ALL=C sort > files.txt &&
   xargs -d '\n' sha256sum < files.txt | sha256sum | cut -c1-16 > stamp.txt)
echo "runtime files: $(wc -l < "$R/files.txt"), stamp $(cat "$R/stamp.txt")"

# Gradle wrapper and placeholder icons/theme from SDL3's android-project.
T="$SDL3_SRC/android-project"
cp -r "$T/gradle" "$T/gradlew" android/
for d in "$T"/app/src/main/res/mipmap-*; do cp -r "$d" android/app/src/main/res/; done
cp "$T/app/src/main/res/values/colors.xml" android/app/src/main/res/values/
echo "sdk.dir=$ANDROID_HOME" > android/local.properties
(cd android && ./gradlew --no-daemon -q ${HERC_GRADLE_ARGS:-} assembleDebug $( [ -n "${HERC_KEYSTORE:-}" ] && echo assembleRelease ))
ls -la android/app/build/outputs/apk/*/*.apk
