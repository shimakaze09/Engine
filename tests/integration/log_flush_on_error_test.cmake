# Runs log_flush_helper with its stdout on a pipe -- fully buffered, the
# way a redirected log or a CI capture is -- and requires that an Error
# line logged before an unflushed exit still arrives.
#
# This is the "the last lines before the crash are missing" case: stdout is
# only flushed at exit, and a process that dies does not get there. An
# Error is the level worth paying a syscall for.
#
# Inputs: HELPER (the helper executable).

if(NOT DEFINED HELPER)
    message(FATAL_ERROR "HELPER is required")
endif()

execute_process(
    COMMAND "${HELPER}"
    OUTPUT_VARIABLE captured
    ERROR_VARIABLE captured_err
    RESULT_VARIABLE helper_result
)

if(NOT helper_result EQUAL 0)
    message(FATAL_ERROR
        "helper exited ${helper_result}\nstdout:\n${captured}\n"
        "stderr:\n${captured_err}")
endif()

if(captured MATCHES "HELPER-FAILED")
    message(FATAL_ERROR "helper could not set up:\n${captured}")
endif()

if(NOT captured MATCHES "error-line-must-survive")
    message(FATAL_ERROR
        "an Error line logged before an unflushed exit was lost; stdout was:\n"
        "${captured}")
endif()

# The other half, and what keeps this test able to fail: if the stream were
# unbuffered or flushed wholesale, everything would arrive and the check
# above would pass no matter what the logging layer did. The Info line was
# logged after the Error, so a selective flush leaves it in the buffer and
# it must be absent.
if(captured MATCHES "info-line-must-not-survive")
    message(FATAL_ERROR
        "the Info line behind the Error also arrived, so this run proves "
        "nothing about flushing on Error -- stdout was not buffered, or "
        "every level is being flushed. stdout was:\n${captured}")
endif()

message("an Error line survived an unflushed exit; the Info line behind it "
        "did not")
