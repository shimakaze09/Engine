# Verifies the editor's drag-spawn entity naming (issue #86 L-07): a
# dragged asset filename that fits NameComponent's fixed 32-byte field
# spawns silently as before, but one that would overflow it must log a
# truncation warning through the production execute_asset_spawn path
# instead of clipping the name with no diagnostic.

if(NOT DEFINED SPAWN_HOST)
    message(FATAL_ERROR "SPAWN_HOST required")
endif()

execute_process(
    COMMAND "${SPAWN_HOST}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE spawn_output
    ERROR_VARIABLE spawn_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "spawn host failed: ${spawn_error}")
endif()

# Boundary: a short filename spawns with its full name and no warning.
if(NOT spawn_output MATCHES "SPAWN_NAME len=10 name=short_name")
    message(FATAL_ERROR "short name was not preserved verbatim: ${spawn_output}")
endif()

# The long filename's stem (50 'x' chars) must still clip to the 31-char
# field (naming stays cosmetic, the spawn itself must not fail)...
if(NOT spawn_output MATCHES "SPAWN_NAME len=31 name=x+")
    message(FATAL_ERROR "long name was not bounded to 31 chars: ${spawn_output}")
endif()

# ...but the clip must now be diagnosable instead of silent.
string(REGEX MATCHALL "asset spawn name truncated" warn_matches "${spawn_output}")
list(LENGTH warn_matches warn_count)
if(NOT warn_count EQUAL 1)
    message(FATAL_ERROR
        "expected exactly one truncation warning (short name must not "
        "warn), got ${warn_count}: ${spawn_output}")
endif()
