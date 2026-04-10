add_library(engine_core STATIC
    src/bootstrap.cpp
    src/console.cpp
    src/cvar.cpp
    src/debug_draw.cpp
    src/event_bus.cpp
    src/input.cpp
    src/json.cpp
    src/job_system.cpp
    src/linear_allocator.cpp
    src/logging.cpp
    src/platform.cpp
    src/profiler.cpp
    src/reflect.cpp
    src/vfs.cpp
)

target_include_directories(engine_core
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(engine_core PUBLIC cxx_std_20)

target_link_libraries(engine_core
    PUBLIC
    Threads::Threads
    ${ENGINE_SDL2_TARGET}
)

# Precompiled header — improves incremental rebuild time for engine_core.
# PRIVATE so downstream consumers are not forced to use it.
target_precompile_headers(engine_core PRIVATE src/pch.h)
