# Configures engine helpers build settings for the Engine repository.

include(CMakeParseArguments)

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
    set(options)
    set(oneValueArgs RUNTIME_OUTPUT_DIRECTORY)
    set(multiValueArgs SOURCES PUBLIC_INCLUDE_DIRS PRIVATE_INCLUDE_DIRS PUBLIC_DEPS PRIVATE_DEPS COMPILE_DEFINITIONS)
    cmake_parse_arguments(ENGINE_EXE "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT ENGINE_EXE_SOURCES)
        message(FATAL_ERROR "engine_add_executable_target(${target}) requires SOURCES")
    endif()

    add_executable(${target} ${ENGINE_EXE_SOURCES})
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
