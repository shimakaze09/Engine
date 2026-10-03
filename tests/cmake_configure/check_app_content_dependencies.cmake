# Asserts an executable depends on every target that produces the content
# it loads at startup (engine content, the sample project, the shader
# cook), so building it alone still produces that content (#1215). Run with
#   cmake -DTARGET_NAME=<executable> -DDEPENDENCIES=<a|b|...>
#         -DEXPECTED=<a|b|...> -P check_app_content_dependencies.cmake
# where DEPENDENCIES is the target's MANUALLY_ADDED_DEPENDENCIES.

cmake_minimum_required(VERSION 3.28)

if(NOT TARGET_NAME OR NOT EXPECTED)
    message(FATAL_ERROR "pass -DTARGET_NAME, -DDEPENDENCIES and -DEXPECTED")
endif()

string(REPLACE "|" ";" _dependencies "${DEPENDENCIES}")
string(REPLACE "|" ";" _expected "${EXPECTED}")
set(_missing "")
foreach(_content IN LISTS _expected)
    if(NOT _content IN_LIST _dependencies)
        list(APPEND _missing "${_content}")
    endif()
endforeach()

if(_missing)
    list(JOIN _missing ", " _missing_text)
    message(FATAL_ERROR
        "${TARGET_NAME} does not depend on ${_missing_text}: building it "
        "alone leaves out content it loads at startup")
endif()
message(STATUS "${TARGET_NAME} depends on everything it loads at startup")
