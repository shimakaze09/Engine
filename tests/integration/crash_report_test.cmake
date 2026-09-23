# Runs crash_report_helper twice and checks the crash report names all four
# facts: the build, the frame, the stage and the thread. A crash that names
# none of them cannot be matched to a binary or a moment, which is the gap
# this exists to close.
#
# Two cases, because a single one cannot tell two failures apart:
#   report  writes the report directly, proving its content
#   segv    faults for real inside a stage, proving a handler is installed
#           and that it survives the faulting context
#
# Inputs: HELPER (the helper executable) and ASSET_ROOT (the directory the
# helper runs in, so the engine finds the bundled assets it bootstraps
# from).

foreach(var HELPER ASSET_ROOT)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "${var} is required")
    endif()
endforeach()

# Requires every field, and that the stage and frame are not their idle
# values -- "stage=between-frames" or "frame=0" would pass a bare
# "is the field present" check while proving nothing was published.
function(check_report text context require_live)
    foreach(field "ENGINE CRASH" "build=" "frame=" "stage=" "thread=")
        if(NOT text MATCHES "${field}")
            message(FATAL_ERROR
                "${context}: the report has no \"${field}\":\n${text}")
        endif()
    endforeach()
    if(text MATCHES "@")
        message(FATAL_ERROR
            "${context}: the build id carries an unsubstituted token:\n${text}")
    endif()
    if(NOT require_live)
        return()
    endif()
    if(NOT text MATCHES "stage=diagnostics")
        message(FATAL_ERROR
            "${context}: the fault was raised inside stage_diagnostics, but "
            "the report names another stage -- the stage the pipeline "
            "publishes is not reaching the report:\n${text}")
    endif()
    if(NOT text MATCHES "frame=[1-9]")
        message(FATAL_ERROR
            "${context}: the fault was raised after several frames, but the "
            "report says frame 0 -- the frame index is not reaching the "
            "report:\n${text}")
    endif()
endfunction()

# --- Case 1: the report itself.
execute_process(
    COMMAND "${HELPER}" report
    WORKING_DIRECTORY "${ASSET_ROOT}"
    OUTPUT_VARIABLE direct_out
    ERROR_VARIABLE direct_err
    RESULT_VARIABLE direct_result
)
if(NOT direct_result EQUAL 0)
    message(FATAL_ERROR
        "the helper failed to write a report (${direct_result}):\n"
        "${direct_out}\n${direct_err}")
endif()
check_report("${direct_err}" "written directly" FALSE)

# --- Case 2: a real fault.
execute_process(
    COMMAND "${HELPER}" segv
    WORKING_DIRECTORY "${ASSET_ROOT}"
    OUTPUT_VARIABLE segv_out
    ERROR_VARIABLE segv_err
    RESULT_VARIABLE segv_result
)

if(segv_out MATCHES "HELPER-FAILED")
    message(FATAL_ERROR "the helper could not set up:\n${segv_out}")
endif()
if(NOT segv_out MATCHES "HELPER-READY")
    message(FATAL_ERROR
        "the helper never reached the fault:\n${segv_out}\n${segv_err}")
endif()

# The handler restores the previous disposition and re-raises, so the
# process must still die of the signal rather than exiting cleanly: a zero
# status would mean the report was written but the fault was swallowed,
# which would hide crashes instead of documenting them.
if(segv_result EQUAL 0)
    message(FATAL_ERROR
        "the helper faulted but exited 0; the handler swallowed the signal "
        "instead of re-raising it:\n${segv_err}")
endif()

check_report("${segv_err}" "after a real fault" TRUE)

message("crash report names the build, frame, stage and thread, and the "
        "fault still killed the process (${segv_result})")
