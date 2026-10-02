# Configures engine helpers build settings for the Engine repository.

include(CMakeParseArguments)
include(FetchContent)

# Embedded in every engine executable on Windows: UTF-8 as the active code
# page, so a UTF-8 path names the same file there as everywhere else.
set(ENGINE_WINDOWS_UTF8_MANIFEST "${CMAKE_CURRENT_LIST_DIR}/windows_utf8.manifest")

# Adds a fetched third-party CMake project under the policy defaults its
# code was written for, wherever the project leaves a policy unset. A
# macro, so FetchContent's <name>_SOURCE_DIR and <name>_BINARY_DIR reach
# the caller's scope.
#
# CMP0219 (CMake 4.4): code written before 4.4 doubles the backslashes in
# macro arguments for the old unescaping rule, so it defaults to OLD.
# SDL3's ELF dlopen-notes check depends on it: under NEW its test source
# stops compiling and the notes are silently dropped. A
# CMAKE_POLICY_DEFAULT_CMP0219 the user sets wins.
macro(engine_make_third_party_available name)
    if(NOT DEFINED CMAKE_POLICY_DEFAULT_CMP0219)
        set(CMAKE_POLICY_DEFAULT_CMP0219 OLD)
        set(_ENGINE_DEFAULTED_CMP0219 TRUE)
    endif()
    FetchContent_MakeAvailable(${name})
    if(_ENGINE_DEFAULTED_CMP0219)
        unset(CMAKE_POLICY_DEFAULT_CMP0219)
        unset(_ENGINE_DEFAULTED_CMP0219)
    endif()
endmacro()

function(engine_set_cxx23 target visibility)
    target_compile_features(${target} ${visibility} cxx_std_23)
endfunction()

# Applies first-party warning/conformance flags (third-party never inherits; /wd4324 allows intentional alignment padding).
function(engine_apply_strict_compile_options target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /GR- /EHs-c- /wd4324)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti)
        # Clang >= 19 pedantically flags __COUNTER__ (the reflection
        # macros' anchor) as a C2y extension; every supported compiler
        # implements it. Version-guarded: older clangs reject unknown
        # warning groups under -Werror.
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND
           CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 19)
            target_compile_options(${target} PRIVATE -Wno-c2y-extensions)
        endif()
    endif()
endfunction()

function(engine_add_module_library target)
    set(options)
    set(oneValueArgs PCH)
    set(multiValueArgs SOURCES PUBLIC_INCLUDE_DIRS PRIVATE_INCLUDE_DIRS PUBLIC_DEPS PRIVATE_DEPS)
    cmake_parse_arguments(ENGINE_MOD "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT ENGINE_MOD_SOURCES)
        message(FATAL_ERROR "engine_add_module_library(${target}) requires SOURCES")
    endif()

    add_library(${target} STATIC ${ENGINE_MOD_SOURCES})
    engine_set_cxx23(${target} PUBLIC)
    engine_apply_strict_compile_options(${target})

    if(ENGINE_MOD_PUBLIC_INCLUDE_DIRS)
        target_include_directories(${target} PUBLIC ${ENGINE_MOD_PUBLIC_INCLUDE_DIRS})
    endif()

    if(ENGINE_MOD_PRIVATE_INCLUDE_DIRS)
        target_include_directories(${target} PRIVATE ${ENGINE_MOD_PRIVATE_INCLUDE_DIRS})
    endif()

    if(ENGINE_MOD_PUBLIC_DEPS)
        target_link_libraries(${target} PUBLIC ${ENGINE_MOD_PUBLIC_DEPS})
    endif()

    if(ENGINE_MOD_PRIVATE_DEPS)
        target_link_libraries(${target} PRIVATE ${ENGINE_MOD_PRIVATE_DEPS})
    endif()

    if(ENGINE_MOD_PCH)
        target_precompile_headers(${target} PRIVATE ${ENGINE_MOD_PCH})
    endif()
endfunction()

function(engine_add_header_library target)
    set(options)
    set(oneValueArgs)
    set(multiValueArgs PUBLIC_INCLUDE_DIRS PUBLIC_DEPS)
    cmake_parse_arguments(ENGINE_HDR "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    add_library(${target} INTERFACE)
    engine_set_cxx23(${target} INTERFACE)

    if(ENGINE_HDR_PUBLIC_INCLUDE_DIRS)
        target_include_directories(${target} INTERFACE ${ENGINE_HDR_PUBLIC_INCLUDE_DIRS})
    endif()

    if(ENGINE_HDR_PUBLIC_DEPS)
        target_link_libraries(${target} INTERFACE ${ENGINE_HDR_PUBLIC_DEPS})
    endif()
endfunction()

function(engine_add_executable_target target)
    set(options GUI)
    set(oneValueArgs RUNTIME_OUTPUT_DIRECTORY)
    set(multiValueArgs SOURCES PUBLIC_INCLUDE_DIRS PRIVATE_INCLUDE_DIRS PUBLIC_DEPS PRIVATE_DEPS COMPILE_DEFINITIONS)
    cmake_parse_arguments(ENGINE_EXE "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT ENGINE_EXE_SOURCES)
        message(FATAL_ERROR "engine_add_executable_target(${target}) requires SOURCES")
    endif()

    add_executable(${target} ${ENGINE_EXE_SOURCES})
    if(WIN32)
        target_sources(${target} PRIVATE "${ENGINE_WINDOWS_UTF8_MANIFEST}")
    endif()
    # GUI: a windowed application that runs without a terminal on every
    # desktop platform, each the way that platform defines it:
    # - Windows: the GUI subsystem, so no console window opens beside it
    #   (what SDL3's SDL_main.h and every shipping engine do). The entry
    #   point stays the portable int main: the CRT's mainCRTStartup calls
    #   it, with the UTF-8 argv the manifest above gives, so no WinMain and
    #   no SDL reach app code.
    # - macOS: an .app bundle; Finder opens a bare executable in Terminal
    #   and launches a bundle as an application.
    # - Linux: a freedesktop desktop entry beside the binary with
    #   Terminal=false, which is what launchers and file managers run; its
    #   Path key starts the application in its own directory, where the
    #   build copied its content.
    if(ENGINE_EXE_GUI AND (ENGINE_TARGET_PLATFORM STREQUAL "Win64"))
        set_target_properties(${target} PROPERTIES WIN32_EXECUTABLE TRUE)
        if(MSVC)
            target_link_options(${target} PRIVATE "/ENTRY:mainCRTStartup")
        else()
            target_link_options(${target} PRIVATE "-Wl,--entry,mainCRTStartup")
        endif()
    elseif(ENGINE_EXE_GUI AND (ENGINE_TARGET_PLATFORM STREQUAL "macOS"))
        set_target_properties(${target} PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_BUNDLE_NAME "${target}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "org.engine.${target}")
    elseif(ENGINE_EXE_GUI AND (ENGINE_TARGET_PLATFORM STREQUAL "Linux"))
        file(GENERATE
            OUTPUT "$<TARGET_FILE_DIR:${target}>/${target}.desktop"
            CONTENT "[Desktop Entry]
Type=Application
Name=${target}
Exec=\"$<TARGET_FILE:${target}>\"
Path=$<TARGET_FILE_DIR:${target}>
Terminal=false
Categories=Development;
")
    endif()
    engine_set_cxx23(${target} PRIVATE)
    engine_apply_strict_compile_options(${target})

    if(ENGINE_EXE_PUBLIC_INCLUDE_DIRS)
        target_include_directories(${target} PUBLIC ${ENGINE_EXE_PUBLIC_INCLUDE_DIRS})
    endif()

    if(ENGINE_EXE_PRIVATE_INCLUDE_DIRS)
        target_include_directories(${target} PRIVATE ${ENGINE_EXE_PRIVATE_INCLUDE_DIRS})
    endif()

    if(ENGINE_EXE_PUBLIC_DEPS)
        target_link_libraries(${target} PUBLIC ${ENGINE_EXE_PUBLIC_DEPS})
    endif()

    if(ENGINE_EXE_PRIVATE_DEPS)
        target_link_libraries(${target} PRIVATE ${ENGINE_EXE_PRIVATE_DEPS})
    endif()

    if(ENGINE_EXE_COMPILE_DEFINITIONS)
        target_compile_definitions(${target} PRIVATE ${ENGINE_EXE_COMPILE_DEFINITIONS})
    endif()

    if(ENGINE_EXE_RUNTIME_OUTPUT_DIRECTORY)
        set_target_properties(${target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${ENGINE_EXE_RUNTIME_OUTPUT_DIRECTORY}"
        )
    endif()
endfunction()

# Web only: links <target> as an .html page from <shell> with the sample
# project's content preloaded at assets/ and the engine content tree,
# host-cooked shaders included, at engine_assets/: the roots a default
# EngineConfig mounts from the page's working directory.
function(engine_package_web_page target shell)
    set_target_properties(${target} PROPERTIES
        SUFFIX ".html"
        LINK_DEPENDS "${ENGINE_WEB_COOK_STAMP}")
    target_link_options(${target} PRIVATE
        "SHELL:--preload-file ${ENGINE_SAMPLE_PROJECT_OUTPUT_DIR}/assets@assets"
        "SHELL:--preload-file ${ENGINE_ENGINE_ASSET_OUTPUT_DIR}@engine_assets"
        "SHELL:--shell-file ${shell}")
    add_dependencies(${target} copy_sample_project copy_engine_assets
        web_cooked_shaders)
endfunction()

function(engine_add_test_executable target)
    set(options)
    set(oneValueArgs LABELS)
    set(multiValueArgs SOURCES PUBLIC_INCLUDE_DIRS PRIVATE_INCLUDE_DIRS PUBLIC_DEPS PRIVATE_DEPS COMPILE_DEFINITIONS)
    cmake_parse_arguments(ENGINE_TEST "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    engine_add_executable_target(${target}
        SOURCES ${ENGINE_TEST_SOURCES}
        PUBLIC_INCLUDE_DIRS ${ENGINE_TEST_PUBLIC_INCLUDE_DIRS}
        PRIVATE_INCLUDE_DIRS ${ENGINE_TEST_PRIVATE_INCLUDE_DIRS}
        PUBLIC_DEPS ${ENGINE_TEST_PUBLIC_DEPS}
        PRIVATE_DEPS ${ENGINE_TEST_PRIVATE_DEPS}
        COMPILE_DEFINITIONS ${ENGINE_TEST_COMPILE_DEFINITIONS}
    )

    add_test(NAME ${target} COMMAND ${target})
    if(ENGINE_TEST_LABELS)
        set_tests_properties(${target} PROPERTIES LABELS "${ENGINE_TEST_LABELS}")
    endif()
endfunction()
