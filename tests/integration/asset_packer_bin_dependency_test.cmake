# Verifies external glTF .bin edits force a recook without --graph
# (PR #51 review): dependency correctness is a cooker invariant, not a
# side effect of the optional dependency-graph flag.

if(NOT DEFINED ASSET_PACKER OR NOT DEFINED SRC_GLTF OR NOT DEFINED SRC_BIN
   OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "ASSET_PACKER, SRC_GLTF, SRC_BIN, WORKDIR required")
endif()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
get_filename_component(gltf_name "${SRC_GLTF}" NAME)
get_filename_component(bin_name "${SRC_BIN}" NAME)
file(COPY "${SRC_GLTF}" "${SRC_BIN}" DESTINATION "${WORKDIR}")
set(input "${WORKDIR}/${gltf_name}")
set(bin "${WORKDIR}/${bin_name}")
set(output "${WORKDIR}/bin_dep.mesh")

execute_process(
    COMMAND "${ASSET_PACKER}" "${input}" "${output}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE first_output
    ERROR_VARIABLE first_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "initial cook failed: ${first_error}")
endif()
if(NOT EXISTS "${output}")
    message(FATAL_ERROR "initial cook produced no output")
endif()

execute_process(
    COMMAND "${ASSET_PACKER}" "${input}" "${output}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE second_output
    ERROR_VARIABLE second_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "unchanged recook failed: ${second_error}")
endif()
if(NOT second_output MATCHES "asset up-to-date; skipped recook")
    message(FATAL_ERROR
        "unchanged source was not skipped: ${second_output}")
endif()

# Change the external buffer's content hash while leaving the .gltf
# untouched; cgltf reads only the declared byteLength, so a trailing
# byte keeps the source cookable.
file(APPEND "${bin}" "x")

execute_process(
    COMMAND "${ASSET_PACKER}" "${input}" "${output}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE third_output
    ERROR_VARIABLE third_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "post-edit recook failed: ${third_error}")
endif()
if(third_output MATCHES "asset up-to-date; skipped recook")
    message(FATAL_ERROR
        "edited .bin did not force a recook without --graph: ${third_output}")
endif()

file(REMOVE_RECURSE "${WORKDIR}")
