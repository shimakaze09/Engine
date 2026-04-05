add_library(engine_audio STATIC
    src/audio.cpp
)

target_include_directories(engine_audio
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(engine_audio PUBLIC cxx_std_20)

target_link_libraries(engine_audio
    PUBLIC
    engine_core
    engine_math
)

# miniaudio is a single-header library — we only need its include path.
# The implementation is compiled inside audio.cpp via MINIAUDIO_IMPLEMENTATION.
FetchContent_Declare(
    miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG 0.11.21
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
)
FetchContent_MakeAvailable(miniaudio)

target_include_directories(engine_audio
    PRIVATE
    ${miniaudio_SOURCE_DIR}
)
