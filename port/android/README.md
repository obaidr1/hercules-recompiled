# Android app

The Android build of Hercules Recompiled: SDL3's `android-project` (its Java
part and Gradle wrapper are taken from the SDL3 source tree at build time),
plus `HerculesActivity` and this manifest. The game itself is the normal
runtime built with the NDK as a shared library and packaged as `libmain.so`.

    scripts/build_android.sh [arm64-v8a] [x86_64]   # on a machine with the SDK/NDK

Needs `ANDROID_HOME` (SDK with NDK r30), `JAVA_HOME` (JDK 17) and `SDL3_SRC`
(SDL3 source, e.g. from `psxrecomp/tools/ci/prefetch_sdl3.sh`).
