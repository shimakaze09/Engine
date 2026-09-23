# Requires ENGINE_ASSERT to fire in a translation unit compiled with
# NDEBUG, which is where <cassert> compiled itself out and left the bounds
# check that was supposed to catch the bug as undefined behaviour.
#
# Two cases, because the abort case alone cannot fail usefully: a helper
# that aborted unconditionally would pass it. The in-range case requires
# the same helper to read a valid slot, print the stored value and exit 0.
#
# Inputs: HELPER (the helper executable).

if(NOT DEFINED HELPER)
    message(FATAL_ERROR "HELPER is required")
endif()

# --- The contract held: no abort, and the value is the one stored.
execute_process(
    COMMAND "${HELPER}" in-range
    OUTPUT_VARIABLE ok_out
    ERROR_VARIABLE ok_err
    RESULT_VARIABLE ok_result
)
if(ok_out MATCHES "HELPER-FAILED")
    message(FATAL_ERROR "the in-range case did not set up:\n${ok_out}")
endif()
if(NOT ok_result EQUAL 0)
    message(FATAL_ERROR
        "a valid read exited ${ok_result}; ENGINE_ASSERT fires when the "
        "contract holds:\n${ok_out}\n${ok_err}")
endif()
if(NOT ok_out MATCHES "HELPER-OK entity=1 component=42")
    message(FATAL_ERROR
        "a valid read returned the wrong value:\n${ok_out}")
endif()

# --- The contract broken: must abort, naming the condition.
execute_process(
    COMMAND "${HELPER}" out-of-range
    OUTPUT_VARIABLE bad_out
    ERROR_VARIABLE bad_err
    RESULT_VARIABLE bad_result
)

if(bad_out MATCHES "HELPER-FAILED NDEBUG")
    message(FATAL_ERROR
        "the helper was not compiled with NDEBUG, so this run says nothing "
        "about the shipped configuration:\n${bad_out}")
endif()
if(NOT bad_out MATCHES "HELPER-READY")
    message(FATAL_ERROR
        "the helper never reached the out-of-range read:\n${bad_out}")
endif()
if(bad_out MATCHES "HELPER-FAILED no abort")
    message(FATAL_ERROR
        "the out-of-range read returned instead of aborting -- with NDEBUG "
        "the bounds check is gone:\n${bad_out}")
endif()
if(bad_result EQUAL 0)
    message(FATAL_ERROR
        "the out-of-range read exited 0:\n${bad_out}\n${bad_err}")
endif()

# The diagnostic has to name the broken condition, not just die: an abort
# with no message leaves the reader where a missing check did.
set(combined "${bad_out}${bad_err}")
if(NOT combined MATCHES "assertion failed")
    message(FATAL_ERROR
        "the abort printed no assertion diagnostic:\n${combined}")
endif()
if(NOT combined MATCHES "denseIndex < m_count")
    message(FATAL_ERROR
        "the diagnostic does not name the condition that broke:\n${combined}")
endif()
if(NOT combined MATCHES "sparse_set.h")
    message(FATAL_ERROR
        "the diagnostic does not name the file:\n${combined}")
endif()

message("ENGINE_ASSERT fires under NDEBUG and names the condition; a valid "
        "read is untouched (abort status ${bad_result})")
