# Configures audio build settings for the Engine repository.

engine_add_module_library(engine_audio
    SOURCES
    src/audio.cpp
    PUBLIC_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    PUBLIC_DEPS
    engine_core
    engine_math
)

# miniaudio is a single-header library — we only need its include path.
# The implementation is compiled inside audio.cpp via MINIAUDIO_IMPLEMENTATION.
# Pinned by commit SHA like every other FetchContent dependency: a tag can
# be moved by the upstream, a commit cannot (tools/check_dependency_pins.py
# enforces this).
FetchContent_Declare(
    miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG 4a5b74bef029b3592c54b6048650ee5f972c1a48 # 0.11.21
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
)
FetchContent_MakeAvailable(miniaudio)

target_include_directories(engine_audio
    PRIVATE
    ${miniaudio_SOURCE_DIR}
)
