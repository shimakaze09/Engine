# Verifies the up-to-date check covers every manifest-listed output
# (issue #55): a deleted .skel or .meta.json sidecar must force a recook
# that regenerates it instead of being skipped because the .mesh alone
# still matches the stamp.

if(NOT DEFINED ASSET_PACKER OR NOT DEFINED SRC_GLTF OR NOT DEFINED SRC_BIN
   OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "ASSET_PACKER, SRC_GLTF, SRC_BIN, WORKDIR required")
endif()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
get_filename_component(gltf_name "${SRC_GLTF}" NAME)
file(COPY "${SRC_GLTF}" "${SRC_BIN}" DESTINATION "${WORKDIR}")
set(input "${WORKDIR}/${gltf_name}")
set(output "${WORKDIR}/character.mesh")

execute_process(
    COMMAND "${ASSET_PACKER}" "${input}" "${output}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE cook_output
    ERROR_VARIABLE cook_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "initial cook failed: ${cook_error}")
endif()
if(NOT EXISTS "${WORKDIR}/character.skel")
    message(FATAL_ERROR "initial cook wrote no character.skel")
endif()

file(REMOVE "${WORKDIR}/character.skel")

execute_process(
    COMMAND "${ASSET_PACKER}" "${input}" "${output}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE skel_output
    ERROR_VARIABLE skel_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "recook after .skel delete failed: ${skel_error}")
endif()
if(skel_output MATCHES "asset up-to-date; skipped recook")
    message(FATAL_ERROR
        "deleted character.skel did not force a recook: ${skel_output}")
endif()
if(NOT EXISTS "${WORKDIR}/character.skel")
    message(FATAL_ERROR "recook did not regenerate character.skel")
endif()

file(REMOVE "${output}.meta.json")

execute_process(
    COMMAND "${ASSET_PACKER}" "${input}" "${output}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE meta_output
    ERROR_VARIABLE meta_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "recook after .meta.json delete failed: ${meta_error}")
endif()
if(meta_output MATCHES "asset up-to-date; skipped recook")
    message(FATAL_ERROR
        "deleted .meta.json did not force a recook: ${meta_output}")
endif()
if(NOT EXISTS "${output}.meta.json")
    message(FATAL_ERROR "recook did not regenerate the .meta.json sidecar")
endif()

execute_process(
    COMMAND "${ASSET_PACKER}" "${input}" "${output}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE final_output
    ERROR_VARIABLE final_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "verification recook failed: ${final_error}")
endif()
if(NOT final_output MATCHES "asset up-to-date; skipped recook")
    message(FATAL_ERROR
        "complete output set was not skipped as up to date: ${final_output}")
endif()

file(REMOVE_RECURSE "${WORKDIR}")
