# iOS (Xcode/Apple Silicon) cross-compile toolchain stub
# Usage: cmake -S . -B build-ios -G Xcode
#              -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/ios.cmake
#              -DENGINE_TARGET_PLATFORM=iOS
#
# Requires Xcode installed on macOS with the iOS SDK.
# Minimum deployment target: iOS 14.0

set(CMAKE_SYSTEM_NAME iOS)

# Deployment target — iOS 14+ required for arm64 devices and Metal API.
set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "iOS minimum deployment target")

# Target device architectures.
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "iOS target architectures")

# SDK selection — iphoneos for device, iphonesimulator for sim.
if(NOT DEFINED IOS_PLATFORM)
    set(IOS_PLATFORM "OS64" CACHE STRING "iOS platform: OS64 or SIMULATOR64")
endif()

if(IOS_PLATFORM STREQUAL "SIMULATOR64")
    set(CMAKE_OSX_SYSROOT "iphonesimulator")
else()
    set(CMAKE_OSX_SYSROOT "iphoneos")
endif()

# Force ENGINE_TARGET_PLATFORM for this toolchain.
set(ENGINE_TARGET_PLATFORM "iOS" CACHE STRING
    "Target platform (forced by ios toolchain)" FORCE)

# Bitcode — disabled (deprecated in Xcode 14+).
set(CMAKE_XCODE_ATTRIBUTE_ENABLE_BITCODE "NO")

message(STATUS "iOS toolchain: sysroot=${CMAKE_OSX_SYSROOT}, "
    "archs=${CMAKE_OSX_ARCHITECTURES}, "
    "min_os=${CMAKE_OSX_DEPLOYMENT_TARGET}")
