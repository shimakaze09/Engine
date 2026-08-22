# Configures root build build settings for the Engine repository.

cmake_minimum_required(VERSION 3.28)

if(POLICY CMP0174)
    cmake_policy(SET CMP0174 NEW)
endif()

# ---- Support-matrix rejection (pre-project) ----
# The cmake_configure_rejects_* probes each spawn a fresh configure and
# assert fast failure. Rejecting BEFORE project() keeps every rejection
# free of compiler detection — which dominates a fresh configure on
# Windows and pushed the probes past their timeout on loaded CI runners.
# Host identity uses CMAKE_HOST_* because WIN32/APPLE only exist after
# project().
if(CMAKE_HOST_WIN32)
    set(_ENGINE_HOST_PLATFORM "Win64")
elseif(CMAKE_HOST_APPLE)
    set(_ENGINE_HOST_PLATFORM "macOS")
else()
    set(_ENGINE_HOST_PLATFORM "Linux")
endif()

if(DEFINED ENGINE_TARGET_PLATFORM)
    set(_ENGINE_EARLY_TARGET "${ENGINE_TARGET_PLATFORM}")
else()
    set(_ENGINE_EARLY_TARGET "${_ENGINE_HOST_PLATFORM}")
endif()

set(_ENGINE_VALID_PLATFORMS Win64 Linux macOS Android iOS Web)
if(NOT _ENGINE_EARLY_TARGET IN_LIST _ENGINE_VALID_PLATFORMS)
    message(FATAL_ERROR "ENGINE_TARGET_PLATFORM='${_ENGINE_EARLY_TARGET}' is not valid. "
        "Choose one of: Win64 Linux macOS Android iOS Web")
endif()

set(_ENGINE_UNIMPLEMENTED_PLATFORMS Android iOS)
if(_ENGINE_EARLY_TARGET IN_LIST _ENGINE_UNIMPLEMENTED_PLATFORMS)
    message(FATAL_ERROR
        "ENGINE_TARGET_PLATFORM='${_ENGINE_EARLY_TARGET}' has no toolchain, "
        "renderer backend, or dependency graph in this repo yet — it is "
        "gated on the v0.5 bgfx replatform (RHI migration) per CLAUDE.md's "
        "roadmap. Configuring for it would pass this check and then fail "
        "later during dependency discovery, compilation, or linking instead "
        "of here with a clear reason. Supported targets today: Win64, "
        "Linux, macOS (build + headless-test lane), Web (Emscripten).")
endif()

# Web without any toolchain file cannot be an emcmake configure; reject
# before compiler detection. The authoritative EMSCRIPTEN check still
# runs post-project once the supplied toolchain has been processed.
if(_ENGINE_EARLY_TARGET STREQUAL "Web" AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    message(FATAL_ERROR
        "ENGINE_TARGET_PLATFORM=Web requires the Emscripten toolchain: "
        "configure through 'emcmake cmake ...' (emsdk).")
endif()

# A desktop target that does not match the host, with no toolchain file
# supplied, is always a host/target mistake in this natively-built-only
# repo. Web is exempt here — its gates are above and post-project.
if(NOT _ENGINE_EARLY_TARGET STREQUAL "Web"
   AND NOT DEFINED CMAKE_TOOLCHAIN_FILE
   AND NOT _ENGINE_EARLY_TARGET STREQUAL _ENGINE_HOST_PLATFORM)
    message(FATAL_ERROR
        "ENGINE_TARGET_PLATFORM='${_ENGINE_EARLY_TARGET}' does not match "
        "the host platform ('${_ENGINE_HOST_PLATFORM}'), and no "
        "cross-compilation toolchain (CMAKE_TOOLCHAIN_FILE) was supplied. "
        "This repo builds natively only today, so an unmatched target "
        "platform is always a host/target mistake, not a supported "
        "cross-compile.")
endif()

project(Engine VERSION 0.1.0 LANGUAGES C CXX)

include(${CMAKE_SOURCE_DIR}/cmake/EngineHelpers.cmake)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
# ---- Platform target ----
# Valid values: Win64 Linux macOS Android iOS Web
# Default auto-detected from CMake's current host/target when not specified.
#
# Support matrix (target platform x toolchain x renderer backend), per
# CLAUDE.md's documented platform policy (audit #178 — configure used to
# accept every value in _ENGINE_VALID_PLATFORMS regardless of whether this
# repo can actually build for it, so a mismatched or unimplemented target
# passed initial parsing and then failed deep in dependency discovery,
# compilation, or linking instead of here):
#   Win64  - native only (no cross-compile toolchain is wired up in this
#            repo). Desktop OpenGL 4.5 renderer, full editor + runtime.
#            Primary lane alongside Linux.
#   Linux  - native only. Desktop OpenGL 4.5 renderer, full editor + runtime.
#   macOS  - native only. Build + headless-test lane ONLY (owner decision
#            2026-08-09, issue #82): Apple caps OpenGL at 4.1 while
#            default.frag requires GLSL 450, so the interactive GL renderer
#            does not run there until the v0.5 bgfx replatform. Configure
#            succeeds (the AppleClang conformance lane needs it) but warns.
#   Android, iOS, Web - no toolchain, renderer backend, or dependency graph
#            exists for these yet; gated on the v0.5 bgfx replatform (RHI
#            migration) per the roadmap. Configure fails fast below.
#
# Validation, unimplemented-platform, no-toolchain-Web, and host-mismatch
# rejection all run in the pre-project block above so a rejected
# configure never pays compiler detection; this section only publishes
# the cache variable for accepted targets.
if(NOT DEFINED ENGINE_TARGET_PLATFORM)
    set(ENGINE_TARGET_PLATFORM "${_ENGINE_HOST_PLATFORM}" CACHE STRING "Target platform for the engine build")
else()
    set(ENGINE_TARGET_PLATFORM "${ENGINE_TARGET_PLATFORM}" CACHE STRING "Target platform for the engine build")
endif()

set_property(CACHE ENGINE_TARGET_PLATFORM PROPERTY STRINGS Win64 Linux macOS Android iOS Web)

# Web (#138): requires the Emscripten toolchain (emcmake) and the bgfx
# backend — the runtime ships as wasm with the share-ready HTML shell;
# tests/tools stay host-side.
if(ENGINE_TARGET_PLATFORM STREQUAL "Web")
    if(NOT EMSCRIPTEN)
        message(FATAL_ERROR
            "ENGINE_TARGET_PLATFORM=Web requires the Emscripten toolchain: "
            "configure through 'emcmake cmake ...' (emsdk).")
    endif()
    # wasm SIMD: bx compiles with -msse4.2 (Emscripten emulates the
    # intrinsics but requires the companion flag), and the engine's own
    # SSE2 math paths ride the same emulation.
    add_compile_options(-msimd128)
    # Threads for the job system (hosting needs COOP/COEP headers for
    # SharedArrayBuffer), growable memory, and a strict WebGL2 context
    # for bgfx.
    add_compile_options(-pthread)
    add_link_options(-pthread "-sPTHREAD_POOL_SIZE=8"
        "-sALLOW_MEMORY_GROWTH=1" "-sINITIAL_MEMORY=268435456"
        "-sMIN_WEBGL_VERSION=2" "-sMAX_WEBGL_VERSION=2"
        "-sSTACK_SIZE=1048576")
endif()

if(NOT CMAKE_CROSSCOMPILING AND NOT ENGINE_TARGET_PLATFORM STREQUAL _ENGINE_HOST_PLATFORM)
    message(FATAL_ERROR
        "ENGINE_TARGET_PLATFORM='${ENGINE_TARGET_PLATFORM}' does not match "
        "the platform CMake is actually configuring for ('${_ENGINE_HOST_PLATFORM}'), "
        "and no cross-compilation toolchain (CMAKE_TOOLCHAIN_FILE / "
        "CMAKE_SYSTEM_NAME) was supplied. This repo builds natively only "
        "today, so an unmatched target platform is always a host/target "
        "mistake, not a supported cross-compile — it would otherwise apply "
        "the wrong ENGINE_PLATFORM_* compile definitions to a native build "
        "instead of failing here.")
endif()

if(ENGINE_TARGET_PLATFORM STREQUAL "macOS")
    message(WARNING
        "ENGINE_TARGET_PLATFORM=macOS is a build + headless-test lane only "
        "(owner decision 2026-08-09, issue #82): OpenGL is capped at 4.1 on "
        "this platform while default.frag requires GLSL 450, so the "
        "interactive GL renderer does not run here until the v0.5 bgfx "
        "replatform. Use Win64 or Linux to run the live renderer.")
endif()

message(STATUS "ENGINE_TARGET_PLATFORM: ${ENGINE_TARGET_PLATFORM}")

set(ENGINE_ASSET_SOURCE_DIR "${CMAKE_SOURCE_DIR}/assets" CACHE PATH
    "Asset directory synced for local editor/runtime builds")
set(ENGINE_ASSET_OUTPUT_DIR "${CMAKE_BINARY_DIR}/assets" CACHE PATH
    "Build-tree asset directory used by local editor/runtime builds")

# v0.5 replatform (#138, decision 2026-07-19): the render backend option.
# bgfx is the engine's only render backend (owner decision 2026-08-21,
# #138: the legacy GL backend was deleted after the replatform reached
# parity). The cache variable survives so existing -D invocations keep
# working; any value other than bgfx is rejected.
set(ENGINE_RENDERER_BACKEND "bgfx" CACHE STRING
    "Render backend: bgfx (the only backend, v0.5 replatform #138)")
set_property(CACHE ENGINE_RENDERER_BACKEND PROPERTY STRINGS bgfx)
if(NOT ENGINE_RENDERER_BACKEND STREQUAL "bgfx")
    message(FATAL_ERROR
        "ENGINE_RENDERER_BACKEND must be 'bgfx' (the GL backend was "
        "deleted, #138); got '${ENGINE_RENDERER_BACKEND}'")
endif()

# bgfx dependency (bx/bimg via the bgfx.cmake superproject).
include(FetchContent)
    # 32 sampler stages (PUBLIC define, forwarded by the superproject):
    # the engine's deferred pass binds G-buffer + SSAO + light-data +
    # tile + IBL + shadow units up to 21, past bgfx's default 16.
    # Vulkan/Metal/desktop GL honor 32; WebGL2's 16-unit floor is the
    # web-export unit's recorded concern (#138).
    set(BGFX_CONFIG_MAX_TEXTURE_SAMPLERS 32 CACHE STRING
        "bgfx sampler stages (engine deferred pass needs units up to 21)"
        FORCE)
    # shaderc's toolchain (glslang/spirv-*) dominates a cold bgfx build;
    # lanes that never consume cooked binaries (sanitizers, analysis,
    # werror, benchmarks) turn it off — the cooked test sections skip
    # gracefully when no cook target exists.
    option(ENGINE_BGFX_SHADERC
        "Build bgfx shaderc and cook the shader manifest" ON)
    # shaderc is a host tool; wasm builds consume host-cooked binaries.
    if(EMSCRIPTEN)
        set(BGFX_BUILD_TOOLS OFF CACHE BOOL "bgfx shaderc/tools" FORCE)
        # bgfx defaults browser GL contexts to ES 2.0 (WebGL1); the cooked
        # essl shaders are 300_es, so require a WebGL2 context.
        set(BGFX_OPENGLES_VERSION 30 CACHE STRING
            "bgfx minimum GLES context version (WebGL2)" FORCE)
    elseif(NOT ENGINE_BGFX_SHADERC)
        set(BGFX_BUILD_TOOLS OFF CACHE BOOL "bgfx shaderc/tools" FORCE)
    else()
        set(BGFX_BUILD_TOOLS ON CACHE BOOL "bgfx shaderc/tools" FORCE)
    endif()
    set(BGFX_BUILD_EXAMPLES OFF CACHE BOOL "bgfx examples" FORCE)
    set(BGFX_INSTALL OFF CACHE BOOL "bgfx install rules" FORCE)
    set(BGFX_AMALGAMATED OFF CACHE BOOL "bgfx amalgamated build" FORCE)
    FetchContent_Declare(
        bgfx
        GIT_REPOSITORY https://github.com/bkaradzic/bgfx.cmake.git
        GIT_TAG 12428216cfa33eb34dc91187ad3a3c92196c9849 # v1.153.9398-566
    )
    if(DEFINED CMAKE_WARN_DEPRECATED)
        set(_ENGINE_PREV_CMAKE_WARN_DEPRECATED "${CMAKE_WARN_DEPRECATED}")
    endif()
    set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL
        "Suppress third-party CMake deprecation warnings" FORCE)
    FetchContent_MakeAvailable(bgfx)
    if(DEFINED _ENGINE_PREV_CMAKE_WARN_DEPRECATED)
        set(CMAKE_WARN_DEPRECATED "${_ENGINE_PREV_CMAKE_WARN_DEPRECATED}"
            CACHE BOOL "Show CMake deprecation warnings" FORCE)
    else()
        unset(CMAKE_WARN_DEPRECATED CACHE)
    endif()
    if(NOT TARGET bgfx)
        message(FATAL_ERROR "bgfx backend requested but the bgfx target was "
                            "not created by the fetched superproject.")
    endif()
    # The texture ENCODER uses real x86 intrinsics and only texturec (a
    # host tool) links it; keep it out of wasm builds.
    if(EMSCRIPTEN AND TARGET bimg_encode)
        set_target_properties(bimg_encode PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()

find_package(SDL3 CONFIG QUIET)
if(NOT SDL3_FOUND)
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "Build SDL shared library" FORCE)
    set(SDL_STATIC ON CACHE BOOL "Build SDL static library" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "Build SDL test library" FORCE)
    FetchContent_Declare(
        SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG f87239e71e42da91ca317a12eefb82cfbf3393eb # release-3.4.12
    )
    if(DEFINED CMAKE_WARN_DEPRECATED)
        set(_ENGINE_PREV_CMAKE_WARN_DEPRECATED "${CMAKE_WARN_DEPRECATED}")
    endif()
    set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL
        "Suppress third-party CMake deprecation warnings" FORCE)
    FetchContent_MakeAvailable(SDL3)
    if(DEFINED _ENGINE_PREV_CMAKE_WARN_DEPRECATED)
        set(CMAKE_WARN_DEPRECATED "${_ENGINE_PREV_CMAKE_WARN_DEPRECATED}"
            CACHE BOOL "Show CMake deprecation warnings" FORCE)
    else()
        unset(CMAKE_WARN_DEPRECATED CACHE)
    endif()
endif()

if(NOT SDL3_FOUND AND NOT TARGET SDL3::SDL3-static AND NOT TARGET SDL3::SDL3)
    message(FATAL_ERROR "SDL3 is required but was not found or fetched.")
endif()

if(TARGET SDL3::SDL3-static)
    set(ENGINE_SDL_TARGET SDL3::SDL3-static)
elseif(TARGET SDL3::SDL3)
    set(ENGINE_SDL_TARGET SDL3::SDL3)
else()
    message(FATAL_ERROR "No SDL3 CMake target is available.")
endif()

if(APPLE)
    foreach(_engine_sdl_target SDL3-static SDL3-shared)
        if(TARGET ${_engine_sdl_target})
            target_compile_options(${_engine_sdl_target} PRIVATE -Wno-deprecated-declarations)
        endif()
    endforeach()
    unset(_engine_sdl_target)

    # Silence OpenGL and third-party dependency deprecation noise on macOS.
    add_compile_definitions(GL_SILENCE_DEPRECATION)
    add_link_options(-Wl,-no_warn_duplicate_libraries)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENGINE_DETERMINISTIC_FLOATS "Enable strict floating-point determinism flags" ON)
set(ENGINE_MAX_ENTITIES "65536" CACHE STRING "Maximum runtime world entities (compile-time constant)")

if(NOT ENGINE_MAX_ENTITIES MATCHES "^[0-9]+$" OR
   ENGINE_MAX_ENTITIES LESS 1)
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
    foreach(_engine_flag_var
            CMAKE_CXX_FLAGS
            CMAKE_CXX_FLAGS_DEBUG
            CMAKE_CXX_FLAGS_RELEASE
            CMAKE_CXX_FLAGS_RELWITHDEBINFO
            CMAKE_CXX_FLAGS_MINSIZEREL)
        if(DEFINED ${_engine_flag_var})
            string(REGEX REPLACE "[/-]EH[a-zA-Z0-9-]*" ""
                _engine_cleaned_flags "${${_engine_flag_var}}")
            set(${_engine_flag_var} "${_engine_cleaned_flags}" CACHE STRING
                "CXX flags with default exception mode removed" FORCE)
        endif()
    endforeach()
    add_compile_definitions(_HAS_EXCEPTIONS=0)
endif()

# Warning/conformance flags are per-target: engine_apply_strict_compile_options
# in cmake/EngineHelpers.cmake, applied by the module/executable helpers so
# third-party FetchContent targets never inherit them.

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

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_custom_target(source_comment_audit
        COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/check_source_comments.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Checking source file-level comments"
        VERBATIM
    )
endif()

option(ENGINE_REQUIRE_ANALYSIS_TOOL
    "Require cppcheck or clang-tidy and create a failing analysis target when neither is found"
    OFF
)
find_program(CPPCHECK_EXE NAMES cppcheck DOC "Path to cppcheck")
find_program(CLANG_TIDY_EXE NAMES clang-tidy DOC "Path to clang-tidy")
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
        --suppress=arithOperationsOnVoidPointer:*/core/src/json.cpp
        --suppress=duplInheritedMember:*/runtime/include/engine/runtime/world.h
        --suppress=duplInheritedMember:*/runtime/src/world.cpp
        # False positive: create_program is an opaque function pointer, so
        # cppcheck assumes the returned DeviceProgramHandle (a plain
        # uint32) captures the variant source buffers passed to it (#165).
        --suppress=returnDanglingLifetime:*/renderer/src/shader_system.cpp
        -i "${CMAKE_BINARY_DIR}/_deps"
        -i "${CMAKE_SOURCE_DIR}/tests"
        -i "${CMAKE_SOURCE_DIR}/tools"
        COMMENT "Running cppcheck static analysis"
        VERBATIM
    )
    message(STATUS "Static analysis target 'analysis' configured (cppcheck: ${CPPCHECK_EXE})")
elseif(CLANG_TIDY_EXE AND Python3_Interpreter_FOUND)
    add_custom_target(analysis
        COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/run_clang_tidy_analysis.py"
        "${CMAKE_BINARY_DIR}/compile_commands.json"
        "${CLANG_TIDY_EXE}"
        "${CMAKE_BINARY_DIR}"
        COMMENT "Running clang-tidy static analysis"
        VERBATIM
    )
    message(STATUS "Static analysis target 'analysis' configured (clang-tidy: ${CLANG_TIDY_EXE})")
elseif(ENGINE_REQUIRE_ANALYSIS_TOOL)
    add_custom_target(analysis
        COMMAND ${CMAKE_COMMAND} -E echo
        "No static analyzer found. Install cppcheck or clang-tidy."
        COMMAND ${CMAKE_COMMAND} -E false
        COMMENT "Static analysis target unavailable"
    )
    message(WARNING "No static analyzer found; 'analysis' target will fail.")
else()
    message(STATUS "Static analysis target 'analysis' not configured; install cppcheck or clang-tidy, or set ENGINE_REQUIRE_ANALYSIS_TOOL=ON to require one.")
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
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imguizmo_SOURCE_DIR}/ImGuizmo.cpp
)

target_include_directories(engine_imgui
    PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imguizmo_SOURCE_DIR}
)

target_compile_features(engine_imgui PUBLIC cxx_std_23)

target_link_libraries(engine_imgui
    PUBLIC
    ${ENGINE_SDL_TARGET}
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

if(EXISTS "${ENGINE_ASSET_SOURCE_DIR}")
    add_custom_target(copy_assets ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${ENGINE_ASSET_SOURCE_DIR}"
        "${ENGINE_ASSET_OUTPUT_DIR}"
        COMMENT "Syncing assets to build directory"
    )
endif()

add_subdirectory(core)
add_subdirectory(math)
add_subdirectory(content)
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

# bgfx shader cook (#138 Phase C): when the bgfx backend and tools are
# both built, the packer cooks the manifest's shader variants per
# profile into the build-tree asset dir with shaderc from the fetched
# superproject; the cook stamp keeps incremental builds a no-op.
if(ENGINE_BUILD_TOOLS AND ENGINE_BGFX_SHADERC AND TARGET copy_assets)
    add_custom_target(bgfx_shader_cook ALL
        COMMAND $<TARGET_FILE:asset_packer>
            --shader-manifest
            "${ENGINE_ASSET_SOURCE_DIR}/shaders/bgfx/shaders.json"
            --shader-out "${ENGINE_ASSET_OUTPUT_DIR}/shaders/bgfx/cooked"
            --shaderc $<TARGET_FILE:shaderc>
            --shader-include "${bgfx_SOURCE_DIR}/bgfx/src"
        COMMENT "Cooking bgfx shader variants"
    )
    add_dependencies(bgfx_shader_cook asset_packer shaderc copy_assets)
endif()

if(ENGINE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
