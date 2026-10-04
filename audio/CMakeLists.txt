# Configures audio build settings for the Engine repository.

# The device start a platform needs when its audio awaits the user's first
# gesture: the browser's, or the plain one.
if(ENGINE_TARGET_PLATFORM STREQUAL "Web")
    set(_ENGINE_AUDIO_DEVICE_START src/audio_device_start_web.cpp)
else()
    set(_ENGINE_AUDIO_DEVICE_START src/audio_device_start_native.cpp)
endif()

engine_add_module_library(engine_audio
    SOURCES
    src/audio.cpp
    ${_ENGINE_AUDIO_DEVICE_START}
    src/miniaudio_impl.cpp
    PUBLIC_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    PUBLIC_DEPS
    engine_core
    engine_math
    PRIVATE_DEPS
    engine_content
)

# miniaudio is a single-header library — we only need its include path.
# The implementation is compiled in miniaudio_impl.cpp, which also compiles
# the stb_vorbis decoder shipped in the same checkout (extras/).
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
# Both translation units must see the same miniaudio configuration.
target_compile_definitions(engine_audio PRIVATE MA_NO_GENERATION)
# MSVC's optimizer reports stb_vorbis's possibly uninitialized locals and
# unreachable code from the command-line warning state, which the pragma
# region in miniaudio_impl.cpp cannot reach, so that one third-party file
# drops them here.
set_source_files_properties(src/miniaudio_impl.cpp PROPERTIES
    COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/wd4701;/wd4702;/wd4703>")
