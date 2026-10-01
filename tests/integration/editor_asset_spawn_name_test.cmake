# Verifies the editor's drag-spawn entity naming: a dragged asset filename
# that fits NameComponent's 127-byte field spawns whole and silently, a
# 50-character one included (it was cut to 31 before), and one that would
# overflow it is cut with one truncation warning through the production
# execute_asset_instantiate path instead of clipping with no diagnostic.

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

# A 50-character asset name, as long as real asset names run, is kept
# whole and does not warn.
if(NOT spawn_output MATCHES "SPAWN_NAME len=50 name=x+\n")
    message(FATAL_ERROR "a 50-character name was not kept whole: ${spawn_output}")
endif()

# The 150-character stem must still clip to the 127-byte field (naming
# stays cosmetic, the spawn itself must not fail)...
if(NOT spawn_output MATCHES "SPAWN_NAME len=127 name=y+")
    message(FATAL_ERROR "long name was not bounded to 127 bytes: ${spawn_output}")
endif()

# ...but the clip must now be diagnosable instead of silent.
string(REGEX MATCHALL "asset spawn name truncated" warn_matches "${spawn_output}")
list(LENGTH warn_matches warn_count)
if(NOT warn_count EQUAL 1)
    message(FATAL_ERROR
        "expected exactly one truncation warning (the short and 50-character "
        "names must not warn), got ${warn_count}: ${spawn_output}")
endif()
