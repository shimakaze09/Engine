# A graphics-device fatal ends the process without losing the unsaved
# scene (#1045). The renderer used to abort(); it now ends through
# core::terminate_after_fatal, which saves a recovery copy first and exits
# with the device-fatal code.
#
# Runs the helper's "device" mode, which opens an unsaved scene and ends
# the way the renderer does, then checks the exit code, the message and
# the recovery file the message names.
#
# Inputs: HELPER (engine_integration_fatal_recovery).

if(NOT DEFINED HELPER)
    message(FATAL_ERROR "HELPER is required")
endif()

execute_process(
    COMMAND "${HELPER}" device
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    RESULT_VARIABLE result
)
if(NOT result EQUAL 4)
    message(FATAL_ERROR
        "a device fatal exited ${result}, not the device-fatal code 4 "
        "(an abort, or no exit through terminate_after_fatal):\n${out}\n${err}")
endif()
if(NOT err MATCHES "The engine has to close: test: the graphics device was lost")
    message(FATAL_ERROR "the fatal did not say why the engine closed:\n${err}")
endif()
if(NOT err MATCHES "The unsaved scene was saved to:\n([^\n]+)\n")
    message(FATAL_ERROR "the fatal named no recovery copy:\n${err}")
endif()
set(recovery "${CMAKE_MATCH_1}")
if(NOT EXISTS "${recovery}")
    message(FATAL_ERROR "the named recovery copy does not exist: ${recovery}")
endif()
file(READ "${recovery}" scene)
file(REMOVE "${recovery}")
if(NOT scene MATCHES "\"AuthoredEdit\"")
    message(FATAL_ERROR "the recovery copy lacks the unsaved edit:\n${scene}")
endif()
message(STATUS "device fatal exited 4 and saved ${recovery}")
