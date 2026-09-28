# Asserts the engine's test suites are opt-in: ENGINE_BUILD_TESTS defaults
# OFF in CMakeLists.txt, every user preset (a configure preset named
# <toolchain>-debug or <toolchain>-release) resolves it OFF, and every
# configure preset a test preset runs against resolves it ON. Run with
#   cmake -DENGINE_SOURCE_DIR=<repo> -P check_tests_opt_in.cmake
# A user who builds the engine gets the engine, not the test suites, as
# SDL3's SDL_TESTS and Godot's tests=no default; contributors opt in
# through the -dev and -bench presets.

cmake_minimum_required(VERSION 3.28)

if(NOT ENGINE_SOURCE_DIR)
    message(FATAL_ERROR "pass -DENGINE_SOURCE_DIR=<repository root>")
endif()

set(_failures "")

# ---- The option's default ----
file(STRINGS "${ENGINE_SOURCE_DIR}/CMakeLists.txt" _option_lines
     REGEX "^[ \t]*option\\(ENGINE_BUILD_TESTS[ \t]")
list(LENGTH _option_lines _option_count)
if(NOT _option_count EQUAL 1)
    list(APPEND _failures
         "CMakeLists.txt declares ENGINE_BUILD_TESTS ${_option_count} times")
elseif(NOT _option_lines MATCHES "[ \t]OFF\\)[ \t]*$")
    list(APPEND _failures "ENGINE_BUILD_TESTS does not default OFF")
endif()
set(_option_default OFF)
if(_option_lines MATCHES "[ \t]ON\\)[ \t]*$")
    set(_option_default ON)
endif()

# ---- The presets ----
file(READ "${ENGINE_SOURCE_DIR}/CMakePresets.json" _presets)
string(JSON _configure_count LENGTH "${_presets}" configurePresets)
math(EXPR _configure_last "${_configure_count} - 1")

# Resolves ENGINE_BUILD_TESTS for the configure preset at `index` the way
# CMake does: its own cacheVariables first, then each parent in `inherits`
# order, depth first. Empty when no preset in the chain sets it, which
# the caller reads as the option's default.
function(_resolve_tests index out_var)
    string(JSON _name GET "${_presets}" configurePresets ${index} name)
    string(JSON _value ERROR_VARIABLE _missing
           GET "${_presets}" configurePresets ${index} cacheVariables
           ENGINE_BUILD_TESTS)
    if(NOT _missing)
        set(${out_var} "${_value}" PARENT_SCOPE)
        return()
    endif()
    string(JSON _inherits ERROR_VARIABLE _no_parents
           GET "${_presets}" configurePresets ${index} inherits)
    if(_no_parents)
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()
    string(JSON _kind TYPE "${_presets}" configurePresets ${index} inherits)
    set(_parents "")
    if(_kind STREQUAL "STRING")
        list(APPEND _parents "${_inherits}")
    else()
        string(JSON _parent_count LENGTH "${_inherits}")
        math(EXPR _parent_last "${_parent_count} - 1")
        foreach(_p RANGE 0 ${_parent_last})
            string(JSON _parent GET "${_inherits}" ${_p})
            list(APPEND _parents "${_parent}")
        endforeach()
    endif()
    foreach(_parent IN LISTS _parents)
        foreach(_j RANGE 0 ${_configure_last})
            string(JSON _candidate GET "${_presets}" configurePresets ${_j} name)
            if(_candidate STREQUAL _parent)
                _resolve_tests(${_j} _parent_value)
                if(NOT _parent_value STREQUAL "")
                    set(${out_var} "${_parent_value}" PARENT_SCOPE)
                    return()
                endif()
            endif()
        endforeach()
    endforeach()
    set(${out_var} "" PARENT_SCOPE)
endfunction()

# Configure presets that some test preset runs against.
set(_tested "")
string(JSON _test_count ERROR_VARIABLE _no_tests LENGTH "${_presets}" testPresets)
if(NOT _no_tests AND (_test_count GREATER 0))
    math(EXPR _test_last "${_test_count} - 1")
    foreach(_t RANGE 0 ${_test_last})
        string(JSON _configure ERROR_VARIABLE _hidden
               GET "${_presets}" testPresets ${_t} configurePreset)
        if(NOT _hidden)
            list(APPEND _tested "${_configure}")
        endif()
    endforeach()
endif()

set(_user_presets 0)
foreach(_i RANGE 0 ${_configure_last})
    string(JSON _name GET "${_presets}" configurePresets ${_i} name)
    _resolve_tests(${_i} _resolved)
    if(_resolved STREQUAL "")
        set(_resolved ${_option_default})
    endif()
    if(_name MATCHES "-(debug|release)$")
        math(EXPR _user_presets "${_user_presets} + 1")
        if(_resolved)
            list(APPEND _failures
                 "user preset ${_name} builds the tests (ENGINE_BUILD_TESTS=${_resolved})")
        endif()
    endif()
    if((_name IN_LIST _tested) AND NOT _resolved)
        list(APPEND _failures
             "test presets run against ${_name}, which does not build the tests")
    endif()
endforeach()

if(_user_presets EQUAL 0)
    list(APPEND _failures "no <toolchain>-debug or -release preset found")
endif()

if(_failures)
    list(JOIN _failures "\n  " _report)
    message(FATAL_ERROR "tests are not opt-in:\n  ${_report}")
endif()
message(STATUS "tests are opt-in: default OFF, ${_user_presets} user presets OFF, every tested preset ON")
