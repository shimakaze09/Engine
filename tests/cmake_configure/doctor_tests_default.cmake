# Self-test for check_tests_opt_in.cmake: copies the repository's
# CMakeLists.txt and CMakePresets.json into DOCTORED_DIR with the test
# option's default turned back ON, then runs the check on the copy, which
# must fail. Proves the check reads the default rather than passing blind.
#   cmake -DENGINE_SOURCE_DIR=<repo> -DDOCTORED_DIR=<scratch> -P doctor_tests_default.cmake

cmake_minimum_required(VERSION 3.28)

file(READ "${ENGINE_SOURCE_DIR}/CMakeLists.txt" _lists)
string(REGEX REPLACE
       "(option\\(ENGINE_BUILD_TESTS[^\n]*)OFF\\)"
       "\\1ON)" _doctored "${_lists}")
if(_doctored STREQUAL _lists)
    message(FATAL_ERROR "found no OFF default to doctor; the probe proves nothing")
endif()
file(MAKE_DIRECTORY "${DOCTORED_DIR}")
file(WRITE "${DOCTORED_DIR}/CMakeLists.txt" "${_doctored}")
file(COPY_FILE "${ENGINE_SOURCE_DIR}/CMakePresets.json"
     "${DOCTORED_DIR}/CMakePresets.json")

set(ENGINE_SOURCE_DIR "${DOCTORED_DIR}")
include("${CMAKE_CURRENT_LIST_DIR}/check_tests_opt_in.cmake")
