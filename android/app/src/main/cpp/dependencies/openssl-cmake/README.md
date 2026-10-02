# OpenSSL-CMake
CMake script supplying `OpenSSL` libraries conveniently, encapsulating the
`OpenSSL` build system on various platforms.

## Features
* Allows usage of system OpenSSL
* Allows trivial and complex building of OpenSSL
* Allows cross compilation, especially for Android
* Defaults to verified source builds on Android, preserving all four NDK ABIs

## System OpenSSL
To use the system OpenSSL, simply set `SYSTEM_OPENSSL=ON`.

## Prebuilt OpenSSL
Non-Android builds default to a prebuilt binary for debugging convenience.
An independently verified `OPENSSL_PREBUILT_HASH` SHA256 is required.
Android uses source builds because the 3.3.2 prebuilts are incomplete (x86 is
missing) and do not supply checksums. Do not remove an ABI to work around this.

## Build OpenSSL
Set `BUILD_OPENSSL=ON`. The pinned default is OpenSSL 3.3.2 from
`https://www.openssl.org/source/openssl-3.3.2.tar.gz`, with official SHA256
`2e8a40b01979afe8be0bbfb3de5dc1c6709fedb46d6c89c10da114ab5fc3d281`
([official checksum](https://www.openssl.org/source/openssl-3.3.2.tar.gz.sha256)).
Every source download requires `OPENSSL_BUILD_HASH`; changing versions requires
an independently verified matching hash and `OPENSSL_BUILD_URL`.
TLS certificate verification remains enabled. `OPENSSL_MODULES` defaults to
empty so Sunshine's existing deprecated RSA/crypto APIs remain available.

### General Cross Compile
Cross compilation is enabled using `CROSS=ON` and the target is specified using
`CROSS_TARGET=mingw` along with the optional `CROSS_PREFIX=mingw32-`. 

### Android Cross Compile
Android defaults to `BUILD_OPENSSL=ON` and `CROSS_ANDROID=ON`. The NDK CMake
toolchain supplies the ABI, API level, NDK root and LLVM compiler location.
The source build uses the NDK-resolved `ANDROID_PLATFORM_LEVEL` (or explicit
`ANDROID_PLATFORM=android-N`), not a shadowed system version or highest NDK API.
Targets map to android-arm, android-arm64, android-x86 and android-x86_64.
The environment uses the modern LLVM bin directory, not removed
GCC directories; it is intended for NDK 28 and requires Python 3, Perl and GNU
make on the build host. Build verification is performed by the parent project.
For armeabi-v7a, an idempotent version-guarded configure patch marks the internal
`OPENSSL_armcap_P` definition hidden. OpenSSL 3.3.2 ARM assembly references it
with R_ARM_REL32; NDK r28 lld rejects that relocation against a preemptible
definition when static libcrypto is linked into a JNI shared library. This
retains ARM assembly acceleration instead of disabling crypto assembly or
weakening linker checks. The patch is a configure dependency and fails visibly
if the expected source declaration changes.

Before `add_subdirectory(dependencies/openssl-cmake)`, the Android application
should set these cache options explicitly (use `FORCE` to replace stale cache
values from previous prebuilt configurations):
```cmake
set(SYSTEM_OPENSSL OFF CACHE BOOL "Use bundled OpenSSL" FORCE)
set(BUILD_OPENSSL ON CACHE BOOL "Build verified OpenSSL source" FORCE)
set(CROSS_ANDROID ON CACHE BOOL "Build OpenSSL for Android" FORCE)
set(CROSS OFF CACHE BOOL "Use Android-specific cross compilation" FORCE)
set(OPENSSL_BUILD_VERSION "3.3.2" CACHE STRING "OpenSSL version" FORCE)
set(OPENSSL_BUILD_URL "https://www.openssl.org/source/openssl-3.3.2.tar.gz" CACHE STRING "OpenSSL source" FORCE)
set(OPENSSL_BUILD_HASH "2e8a40b01979afe8be0bbfb3de5dc1c6709fedb46d6c89c10da114ab5fc3d281" CACHE STRING "OpenSSL SHA256" FORCE)
set(OPENSSL_USE_STATIC_LIBS ON CACHE BOOL "Link OpenSSL into Sunshine" FORCE)
set(OPENSSL_MODULES "" CACHE STRING "Retain Sunshine crypto compatibility" FORCE)
set(OPENSSL_ENABLE_TESTS OFF CACHE BOOL "Do not run cross-compiled tests" FORCE)
set(OPENSSL_INSTALL_MAN OFF CACHE BOOL "Do not install manual pages" FORCE)
```
ExternalProject stamps manage incomplete/repeated builds; an existing partial
install directory no longer skips target creation.

## Usage
1. Add `OpenSSL-CMake` as a submodule to your Git project using `git submodule 
add <URL> external/openssl-cmake`
2. Initialize the submodule using `git submodule update --init`
3. In your `CMakeLists.txt` include the directory using 
`add_subdirectory(external/openssl-cmake)`
4. Link against `ssl` and `crypto` targets, which will also include the headers

## Licensing
These scripts, unless otherwise stated, are subject to the MIT license.
