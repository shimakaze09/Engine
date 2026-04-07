cmake_minimum_required(VERSION 3.28)

project(Engine LANGUAGES C CXX)

find_package(Threads REQUIRED)
find_package(SDL2 CONFIG QUIET)
if(NOT SDL2_FOUND)
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "Build SDL shared library" FORCE)
    set(SDL_STATIC ON CACHE BOOL "Build SDL static library" FORCE)
    FetchContent_Declare(
        SDL2
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-2.30.11
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

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /WX /permissive- /GR- /EHs-c-)
    add_compile_definitions(_HAS_EXCEPTIONS=0)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti)
endif()

option(ENGINE_BUILD_TESTS "Build engine tests" ON)
option(ENGINE_BUILD_TOOLS "Build engine tools" ON)

include(FetchContent)
FetchContent_Declare(
    lua
    GIT_REPOSITORY https://github.com/lua/lua.git
    GIT_TAG v5.4.6
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

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG docking
)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(
    imguizmo
    GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
    GIT_TAG master
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

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_definitions(engine_imgui PRIVATE __PRFCHWINTRIN_H)
endif()

if(ENGINE_BUILD_TOOLS)
    FetchContent_Declare(
        cgltf
        GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
        GIT_TAG v1.14
    )
    FetchContent_MakeAvailable(cgltf)
    set(ENGINE_CGLTF_INCLUDE_DIR ${cgltf_SOURCE_DIR})
endif()

FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG master
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
