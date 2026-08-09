# Verifies the runtime mesh loader surfaces cooked-asset staleness
# (issue #81): a cooked mesh whose source changed after the last cook must
# log a once-per-asset warning through the production load path, while a
# fresh cook and a sidecar-less mesh stay silent.

if(NOT DEFINED ASSET_PACKER OR NOT DEFINED STALE_HOST OR NOT DEFINED SRC_GLTF
   OR NOT DEFINED SRC_BIN OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR
        "ASSET_PACKER, STALE_HOST, SRC_GLTF, SRC_BIN, WORKDIR required")
endif()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
get_filename_component(gltf_name "${SRC_GLTF}" NAME)
file(COPY "${SRC_GLTF}" "${SRC_BIN}" DESTINATION "${WORKDIR}")
set(source "${WORKDIR}/${gltf_name}")
set(output "${WORKDIR}/stale_runtime.mesh")

execute_process(
    COMMAND "${ASSET_PACKER}" "${source}" "${output}"
    RESULT_VARIABLE result
    ERROR_VARIABLE cook_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "cook failed: ${cook_error}")
endif()

# Boundary: a fresh cook must load without any staleness warning.
execute_process(
    COMMAND "${STALE_HOST}" "${output}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE fresh_output
    ERROR_VARIABLE fresh_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "fresh-cook load failed: ${fresh_error}")
endif()
if(fresh_output MATCHES "stale cooked asset")
    message(FATAL_ERROR "fresh cook falsely reported stale: ${fresh_output}")
endif()

# Change the source after the cook; the runtime must surface staleness.
file(APPEND "${source}" "\n")
execute_process(
    COMMAND "${STALE_HOST}" "${output}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stale_output
    ERROR_VARIABLE stale_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "stale-source load failed: ${stale_error}")
endif()
if(NOT stale_output MATCHES "stale cooked asset")
    message(FATAL_ERROR
        "runtime loaded a stale cooked mesh silently: ${stale_output}")
endif()

# Boundary: the warning is once per asset even across repeated loads.
string(REGEX MATCHALL "stale cooked asset" stale_matches "${stale_output}")
list(LENGTH stale_matches stale_count)
if(NOT stale_count EQUAL 1)
    message(FATAL_ERROR
        "expected exactly one staleness warning, got ${stale_count}")
endif()

# Boundary: a mesh without a metadata sidecar loads silently.
file(REMOVE "${output}.meta.json")
execute_process(
    COMMAND "${STALE_HOST}" "${output}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE bare_output
    ERROR_VARIABLE bare_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "sidecar-less load failed: ${bare_error}")
endif()
if(bare_output MATCHES "stale cooked asset")
    message(FATAL_ERROR
        "sidecar-less mesh falsely reported stale: ${bare_output}")
endif()

file(REMOVE_RECURSE "${WORKDIR}")
