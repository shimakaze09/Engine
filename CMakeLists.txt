cmake_minimum_required(VERSION 3.28)

project(Engine LANGUAGES C CXX)

include(${CMAKE_SOURCE_DIR}/cmake/EngineHelpers.cmake)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
# ---- Platform target ----
# Valid values: Win64 Linux macOS Android iOS Web
# Default auto-detected from CMake's current host/target when not specified.
if(NOT DEFINED ENGINE_TARGET_PLATFORM)
    if(WIN32)
        set(ENGINE_TARGET_PLATFORM "Win64" CACHE STRING "Target platform for the engine build")
    elseif(APPLE)
        set(ENGINE_TARGET_PLATFORM "macOS" CACHE STRING "Target platform for the engine build")
    else()
        set(ENGINE_TARGET_PLATFORM "Linux" CACHE STRING "Target platform for the engine build")
    endif()
else()
    set(ENGINE_TARGET_PLATFORM "${ENGINE_TARGET_PLATFORM}" CACHE STRING "Target platform for the engine build")
endif()

set_property(CACHE ENGINE_TARGET_PLATFORM PROPERTY STRINGS Win64 Linux macOS Android iOS Web)

set(_ENGINE_VALID_PLATFORMS Win64 Linux macOS Android iOS Web)
if(NOT ENGINE_TARGET_PLATFORM IN_LIST _ENGINE_VALID_PLATFORMS)
    message(FATAL_ERROR "ENGINE_TARGET_PLATFORM='${ENGINE_TARGET_PLATFORM}' is not valid. "
        "Choose one of: Win64 Linux macOS Android iOS Web")
endif()

message(STATUS "ENGINE_TARGET_PLATFORM: ${ENGINE_TARGET_PLATFORM}")

find_package(SDL2 CONFIG QUIET)
if(NOT SDL2_FOUND)
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "Build SDL shared library" FORCE)
    set(SDL_STATIC ON CACHE BOOL "Build SDL static library" FORCE)
    FetchContent_Declare(
        SDL2
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG fa24d868ac2f8fd558e4e914c9863411245db8fd # release-2.30.11
    )
    FetchContent_MakeAvailable(SDL2)
endif()

if(NOT SDL2_FOUND AND NOT TARGET SDL2::SDL2)
    message(FATAL_ERROR "SDL2 is required but was not found or fetched.")
endif()

if(TARGET SDL2::SDL2-static)
    set(ENGINE_SDL2_TARGET SDL2::SDL2-static)
elseif(TARGET SDL2::SDL2)
    set(ENGINE_SDL2_TARGET SDL2::SDL2)
else()
    message(FATAL_ERROR "No SDL2 CMake target is available.")
endif()

find_package(OpenGL REQUIRED)

# Silence OpenGL deprecation warnings on macOS (deprecated since 10.14 Mojave).
if(APPLE)
    add_compile_definitions(GL_SILENCE_DEPRECATION)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENGINE_DETERMINISTIC_FLOATS "Enable strict floating-point determinism flags" ON)
set(ENGINE_MAX_ENTITIES "65536" CACHE STRING "Maximum runtime world entities (compile-time constant)")

if(NOT ENGINE_MAX_ENTITIES MATCHES "^[0-9]+$")
    message(FATAL_ERROR "ENGINE_MAX_ENTITIES must be a positive integer, got '${ENGINE_MAX_ENTITIES}'")
endif()

add_compile_definitions(ENGINE_MAX_ENTITIES=${ENGINE_MAX_ENTITIES})

if(ENGINE_DETERMINISTIC_FLOATS)
    if(MSVC)
        add_compile_options(/fp:strict)
    else()
        add_compile_options(-ffp-contract=off -fno-fast-math)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            add_compile_options(-fexcess-precision=standard)
        endif()
    endif()
    message(STATUS "ENGINE_DETERMINISTIC_FLOATS: strict floating-point flags enabled")
endif()

if(MSVC)
    add_compile_options(/W4 /WX /permissive- /GR- /EHs-c-)
    # C4324: structure was padded due to alignment specifier — intentional for
    # cache-aligned ECS data (Transform, WorldTransform, etc.).
    add_compile_options(/wd4324)
    add_compile_definitions(_HAS_EXCEPTIONS=0)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti)
endif()

option(ENGINE_BUILD_TESTS "Build engine tests" ON)
option(ENGINE_BUILD_TOOLS "Build engine tools" ON)

# ---- Per-platform compile definitions ----
if(ENGINE_TARGET_PLATFORM STREQUAL "Win64")
    add_compile_definitions(ENGINE_PLATFORM_WIN64=1)
elseif(ENGINE_TARGET_PLATFORM STREQUAL "Linux")
    add_compile_definitions(ENGINE_PLATFORM_LINUX=1)
elseif(ENGINE_TARGET_PLATFORM STREQUAL "macOS")
    add_compile_definitions(ENGINE_PLATFORM_MACOS=1)
elseif(ENGINE_TARGET_PLATFORM STREQUAL "Android")
    add_compile_definitions(ENGINE_PLATFORM_ANDROID=1)
elseif(ENGINE_TARGET_PLATFORM STREQUAL "iOS")
    add_compile_definitions(ENGINE_PLATFORM_IOS=1)
elseif(ENGINE_TARGET_PLATFORM STREQUAL "Web")
    add_compile_definitions(ENGINE_PLATFORM_WEB=1)
endif()

# ---- Sanitizer lane (ASAN + UBSAN; GCC/Clang only) ----
# Enable with: cmake -S . -B build -DENGINE_SANITIZERS=ON
option(ENGINE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(ENGINE_SANITIZERS)
    if(MSVC)
        message(WARNING "ENGINE_SANITIZERS is not supported with MSVC; option ignored.")
    else()
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
        message(STATUS "ENGINE_SANITIZERS: ASAN + UBSAN enabled")
    endif()
endif()

# ---- Static analysis target (cppcheck) ----
# Run with: cmake --build build --target analysis
# ---- P1-M1-A3: Incremental compilation audit results ----
# A3a (include audit): 0 public headers leak SDL/GL/Lua/ImGui types. All
#      engine module boundaries are clean. The heaviest public headers are
#      world.h (10 includes) and render_prep_pipeline.h (8 includes); all
#      includes justified by types used. No unnecessary transitive includes.
# A3b (#pragma once audit): 54/54 engine headers use #pragma once.
#      Zero #ifndef include guards found.
# A3c (rebuild timing): Touching core/include/engine/core/logging.h triggers
#      34 TUs to recompile in ~36s (MSVC, Debug, Win64, 2025-04-11).
#      logging.h is in the core PCH — touching it invalidates the PCH, which
#      is expected behaviour. Non-PCH headers like sparse_set.h or math types
#      would trigger fewer TUs.

find_program(CPPCHECK_EXE NAMES cppcheck DOC "Path to cppcheck")
if(CPPCHECK_EXE)
    add_custom_target(analysis
        COMMAND ${CPPCHECK_EXE}
        --project=${CMAKE_BINARY_DIR}/compile_commands.json
        --enable=warning,performance,portability
        --std=c++20
        --error-exitcode=1
        --suppress=missingIncludeSystem
        --suppress=unmatchedSuppression
        --suppress=preprocessorErrorDirective
        --suppress=*:*_deps/*
        --suppress=objectIndex:*/math/src/ray.cpp
        --suppress=arithOperationsOnVoidPointer:*/core/src/json.cpp
        --suppress=duplInheritedMember:*/runtime/include/engine/runtime/world.h
        --suppress=duplInheritedMember:*/runtime/src/world.cpp
        -i "${CMAKE_BINARY_DIR}/_deps"
        -i "${CMAKE_SOURCE_DIR}/tests"
        -i "${CMAKE_SOURCE_DIR}/tools"
        COMMENT "Running cppcheck static analysis"
        VERBATIM
    )
    message(STATUS "Static analysis target 'analysis' configured (cppcheck: ${CPPCHECK_EXE})")
else()
    add_custom_target(analysis
        COMMAND ${CMAKE_COMMAND} -E echo
        "cppcheck not found; install cppcheck to enable the analysis target."
        COMMENT "Static analysis target (cppcheck not available)"
    )
    message(STATUS "cppcheck not found; 'analysis' target will print a warning.")
endif()

include(FetchContent)
FetchContent_Declare(
    lua
    GIT_REPOSITORY https://github.com/lua/lua.git
    GIT_TAG 6443185167c77adcc8552a3fee7edab7895db1a9 # v5.4.6
)
FetchContent_MakeAvailable(lua)

set(ENGINE_LUA_SOURCE_DIR ${lua_SOURCE_DIR})
add_library(engine_lua STATIC
    ${ENGINE_LUA_SOURCE_DIR}/lapi.c
    ${ENGINE_LUA_SOURCE_DIR}/lauxlib.c
    ${ENGINE_LUA_SOURCE_DIR}/lbaselib.c
    ${ENGINE_LUA_SOURCE_DIR}/lcode.c
    ${ENGINE_LUA_SOURCE_DIR}/lcorolib.c
    ${ENGINE_LUA_SOURCE_DIR}/lctype.c
    ${ENGINE_LUA_SOURCE_DIR}/ldblib.c
    ${ENGINE_LUA_SOURCE_DIR}/ldebug.c
    ${ENGINE_LUA_SOURCE_DIR}/ldo.c
    ${ENGINE_LUA_SOURCE_DIR}/ldump.c
    ${ENGINE_LUA_SOURCE_DIR}/lfunc.c
    ${ENGINE_LUA_SOURCE_DIR}/lgc.c
    ${ENGINE_LUA_SOURCE_DIR}/linit.c
    ${ENGINE_LUA_SOURCE_DIR}/liolib.c
    ${ENGINE_LUA_SOURCE_DIR}/llex.c
    ${ENGINE_LUA_SOURCE_DIR}/lmathlib.c
    ${ENGINE_LUA_SOURCE_DIR}/lmem.c
    ${ENGINE_LUA_SOURCE_DIR}/loadlib.c
    ${ENGINE_LUA_SOURCE_DIR}/lobject.c
    ${ENGINE_LUA_SOURCE_DIR}/lopcodes.c
    ${ENGINE_LUA_SOURCE_DIR}/loslib.c
    ${ENGINE_LUA_SOURCE_DIR}/lparser.c
    ${ENGINE_LUA_SOURCE_DIR}/lstate.c
    ${ENGINE_LUA_SOURCE_DIR}/lstring.c
    ${ENGINE_LUA_SOURCE_DIR}/lstrlib.c
    ${ENGINE_LUA_SOURCE_DIR}/ltable.c
    ${ENGINE_LUA_SOURCE_DIR}/ltablib.c
    ${ENGINE_LUA_SOURCE_DIR}/ltm.c
    ${ENGINE_LUA_SOURCE_DIR}/lundump.c
    ${ENGINE_LUA_SOURCE_DIR}/lutf8lib.c
    ${ENGINE_LUA_SOURCE_DIR}/lvm.c
    ${ENGINE_LUA_SOURCE_DIR}/lzio.c
)

target_include_directories(engine_lua PUBLIC ${ENGINE_LUA_SOURCE_DIR})

if(WIN32)
    target_compile_definitions(engine_lua PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(MSVC)
    target_compile_options(engine_lua PRIVATE /W0 /WX-)
else()
    target_compile_options(engine_lua PRIVATE -w -Wno-error)
endif()

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG 329c5a6b3be75ebf54506d3ae94b836ffcf19fa0 # docking branch snapshot
)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(
    imguizmo
    GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
    GIT_TAG a15acd87a3f3241a29ea1363ceafc680dca3a96b # master snapshot
)
FetchContent_MakeAvailable(imguizmo)

add_library(engine_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${imguizmo_SOURCE_DIR}/ImGuizmo.cpp
)

target_include_directories(engine_imgui
    PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imguizmo_SOURCE_DIR}
)

target_compile_features(engine_imgui PUBLIC cxx_std_20)

target_link_libraries(engine_imgui
    PUBLIC
    ${ENGINE_SDL2_TARGET}
    OpenGL::GL
)

if(MSVC)
    target_compile_options(engine_imgui PRIVATE /W0 /WX-)
else()
    target_compile_options(engine_imgui PRIVATE -w -Wno-error)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i[3-6]86")
    target_compile_definitions(engine_imgui PRIVATE __PRFCHWINTRIN_H)
endif()

if(ENGINE_BUILD_TOOLS)
    FetchContent_Declare(
        cgltf
        GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
        GIT_TAG 52c23814dbb60c2fbb54750ddf41d342d432a498 # v1.14
    )
    FetchContent_MakeAvailable(cgltf)
    set(ENGINE_CGLTF_INCLUDE_DIR ${cgltf_SOURCE_DIR})
endif()

FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 31c1ad37456438565541f4919958214b6e762fb4 # master snapshot
)
FetchContent_MakeAvailable(stb)
set(ENGINE_STB_INCLUDE_DIR ${stb_SOURCE_DIR})

if(EXISTS ${CMAKE_SOURCE_DIR}/assets)
    file(COPY ${CMAKE_SOURCE_DIR}/assets DESTINATION ${CMAKE_BINARY_DIR})
    add_custom_target(copy_assets ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets"
        "${CMAKE_BINARY_DIR}/assets"
        COMMENT "Syncing assets to build directory"
    )
endif()

add_subdirectory(core)
add_subdirectory(math)
add_subdirectory(physics)
add_subdirectory(scripting)
add_subdirectory(renderer)
add_subdirectory(audio)
add_subdirectory(runtime)
add_subdirectory(editor)
add_subdirectory(app)

if(ENGINE_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(ENGINE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
