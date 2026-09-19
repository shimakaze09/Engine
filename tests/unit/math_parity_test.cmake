# Runs the native build of math_parity_battery (SSE2 paths where the host
# has them) and the build forced to ENGINE_MATH_SSE2=0, then requires their
# exact hexadecimal outputs to match line for line. The first line of each
# output names which path it compiled; unless one side is SSE2 and the
# other scalar there is nothing to compare, and the test reports SKIPPED
# rather than passing on two identical scalar runs.
#
# Inputs: NATIVE and SCALAR, the two executables.

cmake_policy(SET CMP0007 NEW)

foreach(var NATIVE SCALAR)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "${var} is required")
    endif()
endforeach()

execute_process(
    COMMAND "${NATIVE}"
    OUTPUT_VARIABLE nativeOut
    RESULT_VARIABLE nativeRc
)
execute_process(
    COMMAND "${SCALAR}"
    OUTPUT_VARIABLE scalarOut
    RESULT_VARIABLE scalarRc
)
if(NOT nativeRc EQUAL 0)
    message(FATAL_ERROR "native battery exited ${nativeRc}")
endif()
if(NOT scalarRc EQUAL 0)
    message(FATAL_ERROR "scalar battery exited ${scalarRc}")
endif()

string(REGEX REPLACE "\n" ";" nativeLines "${nativeOut}")
string(REGEX REPLACE "\n" ";" scalarLines "${scalarOut}")
list(GET nativeLines 0 nativeMode)
list(GET scalarLines 0 scalarMode)
if(NOT scalarMode STREQUAL "sse2=0")
    message(FATAL_ERROR "scalar build reports '${scalarMode}'; the "
        "ENGINE_MATH_SSE2=0 definition did not reach math_detail.h")
endif()
if(NOT nativeMode STREQUAL "sse2=1")
    message("SKIPPED: host has no SSE2 (native reports '${nativeMode}'); "
        "nothing to compare against the scalar build")
    return()
endif()
list(REMOVE_AT nativeLines 0)
list(REMOVE_AT scalarLines 0)

list(LENGTH nativeLines nativeCount)
list(LENGTH scalarLines scalarCount)
if(NOT nativeCount EQUAL scalarCount)
    message(FATAL_ERROR "line counts differ: native ${nativeCount}, "
        "scalar ${scalarCount}")
endif()

if(nativeLines STREQUAL scalarLines)
    message("SSE2 and scalar paths agree on all ${nativeCount} result lines")
    return()
endif()

# Locate the first differing line. list(GET) is linear in the list length,
# so a per-line scan of the whole output is quadratic; comparing 512-line
# chunks first keeps the search cheap even on tens of thousands of lines.
set(chunk 512)
math(EXPR last "${nativeCount} - 1")
foreach(chunkStart RANGE 0 ${last} ${chunk})
    list(SUBLIST nativeLines ${chunkStart} ${chunk} nativeChunk)
    list(SUBLIST scalarLines ${chunkStart} ${chunk} scalarChunk)
    if(nativeChunk STREQUAL scalarChunk)
        continue()
    endif()
    list(LENGTH nativeChunk chunkLength)
    math(EXPR chunkLast "${chunkLength} - 1")
    foreach(offset RANGE 0 ${chunkLast})
        list(GET nativeChunk ${offset} nativeLine)
        list(GET scalarChunk ${offset} scalarLine)
        if(NOT nativeLine STREQUAL scalarLine)
            math(EXPR index "${chunkStart} + ${offset}")
            message(FATAL_ERROR "SSE2 and scalar paths disagree; first "
                "differing result line ${index} of ${nativeCount}:\n"
                "  sse2:   ${nativeLine}\n  scalar: ${scalarLine}")
        endif()
    endforeach()
endforeach()
message(FATAL_ERROR "outputs differ but no differing line was located")
