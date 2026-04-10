# Android NDK cross-compile toolchain stub
# Usage: cmake -S . -B build-android -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/android.cmake
#              -DANDROID_NDK=/path/to/ndk
#              -DANDROID_ABI=arm64-v8a
#              -DANDROID_PLATFORM=android-26
#              -DENGINE_TARGET_PLATFORM=Android
#
# Full cross-compilation requires the Android NDK installed.
# This stub sets minimum required variables so that cmake configure
# succeeds and validates the toolchain path early.

# ABI and API level.
if(NOT DEFINED ANDROID_ABI)
    set(ANDROID_ABI "arm64-v8a" CACHE STRING "Android ABI target")
endif()
if(NOT DEFINED ANDROID_PLATFORM)
    set(ANDROID_PLATFORM "android-26" CACHE STRING "Android API platform")
endif()

# Force ENGINE_TARGET_PLATFORM for this toolchain.
set(ENGINE_TARGET_PLATFORM "Android" CACHE STRING
    "Target platform (forced by android toolchain)" FORCE)

# Resolve NDK path.
if(DEFINED ANDROID_NDK)
    set(_ENGINE_NDK_ROOT "${ANDROID_NDK}")
elseif(DEFINED ENV{ANDROID_NDK_HOME})
    set(_ENGINE_NDK_ROOT "$ENV{ANDROID_NDK_HOME}")
elseif(DEFINED ENV{ANDROID_NDK})
    set(_ENGINE_NDK_ROOT "$ENV{ANDROID_NDK}")
else()
    set(_ENGINE_NDK_ROOT "")
endif()

if(_ENGINE_NDK_ROOT)
    # Delegate to the NDK's canonical toolchain file.
    include("${_ENGINE_NDK_ROOT}/build/cmake/android.toolchain.cmake"
        OPTIONAL RESULT_VARIABLE _NDK_TOOLCHAIN_LOADED)
    if(_NDK_TOOLCHAIN_LOADED)
        message(STATUS "Android NDK toolchain loaded from ${_ENGINE_NDK_ROOT}")
    else()
        message(WARNING "android.toolchain.cmake not found in NDK at ${_ENGINE_NDK_ROOT}. "
            "Continuing as stub-only configure.")
        set(CMAKE_SYSTEM_NAME Android)
        set(CMAKE_SYSTEM_VERSION 26)
        set(CMAKE_ANDROID_ARCH_ABI ${ANDROID_ABI})
        set(CMAKE_ANDROID_STL_TYPE "c++_static")
    endif()
else()
    message(WARNING
        "ANDROID_NDK not found — running as stub. Full NDK cross-compilation "
        "requires: -DANDROID_NDK=/path/to/ndk or ANDROID_NDK_HOME env var. "
        "Platform-specific sources gated behind ENGINE_PLATFORM_ANDROID will "
        "still be selectable; only native compilation is skipped.")
    # Stub mode: set ENGINE_TARGET_PLATFORM only, skip cross-compile setup.
    # cmake configure succeeds so CI can validate toolchain file loading.
endif()

message(STATUS "Android toolchain stub: ABI=${ANDROID_ABI}, NDK=${_ENGINE_NDK_ROOT}")
