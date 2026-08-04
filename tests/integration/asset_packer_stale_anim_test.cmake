# Verifies a recook after an animation clip rename removes the old
# clip's cooked .anim (issue #55): the fresh stamp's output manifest
# owns the sidecar set, so a renamed/removed clip cannot leave a stale
# .anim the stamp implicitly certifies.

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
if(NOT EXISTS "${WORKDIR}/character.walk.anim")
    message(FATAL_ERROR "initial cook wrote no character.walk.anim")
endif()
if(NOT EXISTS "${WORKDIR}/character.skel")
    message(FATAL_ERROR "initial cook wrote no character.skel")
endif()

# Rename the "walk" clip in the source; the changed source hash forces
# the recook, and the manifest must retire the old clip's output.
file(READ "${input}" gltf_json)
string(REPLACE "\"name\":\"walk\"" "\"name\":\"stride\"" gltf_json
    "${gltf_json}")
file(WRITE "${input}" "${gltf_json}")

execute_process(
    COMMAND "${ASSET_PACKER}" "${input}" "${output}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE recook_output
    ERROR_VARIABLE recook_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "post-rename recook failed: ${recook_error}")
endif()
if(NOT EXISTS "${WORKDIR}/character.stride.anim")
    message(FATAL_ERROR "renamed clip produced no character.stride.anim")
endif()
if(EXISTS "${WORKDIR}/character.walk.anim")
    message(FATAL_ERROR
        "stale character.walk.anim survived the clip rename recook")
endif()
if(NOT EXISTS "${output}.cookstamp")
    message(FATAL_ERROR "recook wrote no cook stamp")
endif()
file(READ "${output}.cookstamp" stamp)
if(stamp MATCHES "walk\\.anim")
    message(FATAL_ERROR "fresh stamp still lists the removed walk clip")
endif()
if(NOT stamp MATCHES "stride\\.anim")
    message(FATAL_ERROR "fresh stamp does not list the renamed clip output")
endif()

file(REMOVE_RECURSE "${WORKDIR}")
