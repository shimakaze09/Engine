# Configures core build settings for the Engine repository.

# Generates the engine package/version header from the root project()
# VERSION (issue #86 L-06); the module's public include dir carries the
# generated tree so `#include "engine/core/engine_version.h"` resolves the
# same way whether the header is hand-written or generated.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/engine/core/engine_version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/engine/core/engine_version.h
    @ONLY
)

engine_add_module_library(engine_core
    SOURCES
    src/bootstrap.cpp
    src/console.cpp
    src/cvar.cpp
    src/debug_draw.cpp
    src/engine_stats.cpp
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
    PUBLIC_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    PUBLIC_DEPS
    Threads::Threads
    ${ENGINE_SDL_TARGET}
    PCH
    src/pch.h
)

# Precompiled header remains PRIVATE via engine_add_module_library(... PCH ...).
