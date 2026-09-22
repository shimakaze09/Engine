# Configures core build settings for the Engine repository.

# Generates the engine package/version and build identity header. The
# module's public include dir carries the generated tree so
# `#include "engine/core/engine_version.h"` resolves the same way whether
# the header is hand-written or generated.
#
# Written at build time, not here: the recorded source revision has to be
# the one being built, and a configure-time revision goes stale as soon as
# anyone commits without re-running CMake. The script rewrites the header
# only when its content changed, so an unchanged HEAD costs one git call
# and no recompilation.
set(ENGINE_BUILD_INFO_HEADER
    ${CMAKE_CURRENT_BINARY_DIR}/generated/engine/core/engine_version.h)

if(CMAKE_BUILD_TYPE)
    set(engine_build_type "${CMAKE_BUILD_TYPE}")
else()
    set(engine_build_type "Unspecified")
endif()

# The float flags this build actually compiled with, kept beside the ones
# set in the root CMakeLists so a determinism report names them.
if(MSVC)
    set(engine_build_float "fp:precise")
else()
    set(engine_build_float "ffp-contract=off,fno-fast-math")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(engine_build_float "${engine_build_float},fexcess-precision=standard")
    endif()
endif()

set(engine_build_info_args
    "-DTEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/include/engine/core/engine_version.h.in"
    "-DOUTPUT=${ENGINE_BUILD_INFO_HEADER}"
    "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
    "-DPROJECT_VERSION=${PROJECT_VERSION}"
    "-DPROJECT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}"
    "-DPROJECT_VERSION_MINOR=${PROJECT_VERSION_MINOR}"
    "-DPROJECT_VERSION_PATCH=${PROJECT_VERSION_PATCH}"
    "-DENGINE_BUILD_COMPILER=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
    "-DENGINE_BUILD_TYPE=${engine_build_type}"
    "-DENGINE_BUILD_PLATFORM=${CMAKE_SYSTEM_NAME}"
    "-DENGINE_BUILD_FLOAT=${engine_build_float}"
)

# Also run once now, so the header exists for the first configure of a
# fresh build tree and for tooling that reads it without building.
execute_process(
    COMMAND ${CMAKE_COMMAND} ${engine_build_info_args}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/generate_build_info.cmake
    RESULT_VARIABLE engine_build_info_result
)
if(NOT engine_build_info_result EQUAL 0)
    message(FATAL_ERROR
        "failed to generate engine_version.h (${engine_build_info_result})")
endif()

add_custom_target(engine_build_info ALL
    COMMAND ${CMAKE_COMMAND} ${engine_build_info_args}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/generate_build_info.cmake
    BYPRODUCTS ${ENGINE_BUILD_INFO_HEADER}
    COMMENT "Recording the build identity"
    VERBATIM
)

engine_add_module_library(engine_core
    SOURCES
    src/asset_identity.cpp
    src/bootstrap.cpp
    src/console.cpp
    src/cvar.cpp
    src/debug_draw.cpp
    src/durable_replace.cpp
    src/engine_stats.cpp
    src/file_read.cpp
    src/mem_tracker.cpp
    src/event_bus.cpp
    src/input.cpp
    src/input_map.cpp
    src/atomic_file.cpp
    src/json.cpp
    src/json_edit.cpp
    src/native_thread.cpp
    src/job_system.cpp
    src/linear_allocator.cpp
    src/logging.cpp
    src/platform.cpp
    src/profiler.cpp
    src/reflect.cpp
    src/service_locator.cpp
    src/touch_input.cpp
    src/vfs.cpp
    src/validation_report.cpp
    PUBLIC_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    PUBLIC_DEPS
    Threads::Threads
    ${ENGINE_SDL_TARGET}
    PCH
    src/pch.h
)

# The generated header must exist before anything compiles against it.
add_dependencies(engine_core engine_build_info)

# Precompiled header remains PRIVATE via engine_add_module_library(... PCH ...).
