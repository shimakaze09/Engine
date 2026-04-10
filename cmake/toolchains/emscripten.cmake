# Emscripten (WebAssembly) cross-compile toolchain stub
# Usage: cmake -S . -B build-web
#              -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/emscripten.cmake
#              -DENGINE_TARGET_PLATFORM=Web
#
# Requires Emscripten SDK (emsdk) installed and activated.
# Set EMSDK environment variable or pass -DEMSDK=/path/to/emsdk.

set(CMAKE_SYSTEM_NAME Emscripten)

# Locate Emscripten toolchain file from emsdk.
if(DEFINED EMSDK)
    set(_EMSDK_ROOT "${EMSDK}")
elseif(DEFINED ENV{EMSDK})
    set(_EMSDK_ROOT "$ENV{EMSDK}")
else()
    message(WARNING
        "EMSDK is not set. Pass -DEMSDK=/path/to/emsdk or set the EMSDK "
        "environment variable to enable full Emscripten build.")
    set(_EMSDK_ROOT "")
endif()

if(_EMSDK_ROOT)
    include("${_EMSDK_ROOT}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake"
        OPTIONAL RESULT_VARIABLE _EMSCRIPTEN_PLATFORM_CMAKE)
    if(NOT _EMSCRIPTEN_PLATFORM_CMAKE)
        set(CMAKE_C_COMPILER "${_EMSDK_ROOT}/upstream/emscripten/emcc")
        set(CMAKE_CXX_COMPILER "${_EMSDK_ROOT}/upstream/emscripten/em++")
    endif()
endif()

# Force ENGINE_TARGET_PLATFORM for this toolchain.
set(ENGINE_TARGET_PLATFORM "Web" CACHE STRING
    "Target platform (forced by emscripten toolchain)" FORCE)

# WebAssembly output flags.
# -sUSE_SDL=2 uses the Emscripten SDL2 port.
# -sALLOW_MEMORY_GROWTH allows heap to expand at runtime.
# -O2 is the default; use -Oz for size-optimized shipping builds.
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-sUSE_SDL=2 -sALLOW_MEMORY_GROWTH=1 -sWASM=1 --shell-file ${CMAKE_SOURCE_DIR}/app/web/shell.html")

message(STATUS "Emscripten toolchain configured.")
