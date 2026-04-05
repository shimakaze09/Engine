add_library(engine_core STATIC
    src/bootstrap.cpp
    src/json.cpp
    src/job_system.cpp
    src/linear_allocator.cpp
    src/logging.cpp
    src/platform.cpp
    src/reflect.cpp
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
