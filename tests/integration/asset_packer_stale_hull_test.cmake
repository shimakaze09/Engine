# Verifies a recook whose geometry is structurally hull-less removes the
# hull sidecar an earlier cook of the same output produced (PR #51
# review): the fresh stamp must never certify a stale .hull.

if(NOT DEFINED ASSET_PACKER OR NOT DEFINED SRC_GLTF OR NOT DEFINED SRC_BIN
   OR NOT DEFINED HULLLESS_GLTF OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR
        "ASSET_PACKER, SRC_GLTF, SRC_BIN, HULLLESS_GLTF, WORKDIR required")
endif()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
get_filename_component(gltf_name "${SRC_GLTF}" NAME)
file(COPY "${SRC_GLTF}" "${SRC_BIN}" DESTINATION "${WORKDIR}")
file(COPY "${HULLLESS_GLTF}" DESTINATION "${WORKDIR}")
get_filename_component(hullless_name "${HULLLESS_GLTF}" NAME)
set(output "${WORKDIR}/stale_hull.mesh")

execute_process(
    COMMAND "${ASSET_PACKER}" "${WORKDIR}/${gltf_name}" "${output}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE cook_output
    ERROR_VARIABLE cook_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "hull-producing cook failed: ${cook_error}")
endif()
if(NOT EXISTS "${output}.hull")
    message(FATAL_ERROR "hull-producing cook wrote no .hull sidecar")
endif()

execute_process(
    COMMAND "${ASSET_PACKER}" "${WORKDIR}/${hullless_name}" "${output}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE recook_output
    ERROR_VARIABLE recook_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "hull-less recook failed: ${recook_error}")
endif()
if(EXISTS "${output}.hull")
    message(FATAL_ERROR
        "stale .hull sidecar survived a hull-less recook of ${output}")
endif()

file(REMOVE_RECURSE "${WORKDIR}")
