# Verifies a recook triggered by an import-settings change (scaleFactor
# edit in the .meta.json sidecar) also regenerates the thumbnail (audit
# M-28): the thumbnail skip-gate used to hash only the source bytes, so a
# settings-driven recook re-listed a stale PNG in the cook manifest.

if(NOT DEFINED ASSET_PACKER OR NOT DEFINED SRC_GLTF OR NOT DEFINED SRC_BIN
   OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "ASSET_PACKER, SRC_GLTF, SRC_BIN, WORKDIR required")
endif()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
get_filename_component(gltf_name "${SRC_GLTF}" NAME)
file(COPY "${SRC_GLTF}" "${SRC_BIN}" DESTINATION "${WORKDIR}")
set(output "${WORKDIR}/thumb_settings.mesh")

execute_process(
    COMMAND "${ASSET_PACKER}" "${WORKDIR}/${gltf_name}" "${output}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE cook_output
    ERROR_VARIABLE cook_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "initial cook failed: ${cook_error}")
endif()
if(NOT cook_output MATCHES "generated thumbnail")
    message(FATAL_ERROR "initial cook did not generate a thumbnail")
endif()

file(WRITE "${output}.meta.json"
    "{\"importSettings\":{\"scaleFactor\":2.0}}")

execute_process(
    COMMAND "${ASSET_PACKER}" "${WORKDIR}/${gltf_name}" "${output}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE recook_output
    ERROR_VARIABLE recook_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "settings-change recook failed: ${recook_error}")
endif()
if(recook_output MATCHES "asset up-to-date")
    message(FATAL_ERROR "settings change did not trigger a recook")
endif()
if(recook_output MATCHES "thumbnail up-to-date")
    message(FATAL_ERROR
        "settings-change recook skipped the thumbnail; the skip-gate hash "
        "must include the import-settings hash")
endif()
if(NOT recook_output MATCHES "generated thumbnail")
    message(FATAL_ERROR "settings-change recook did not regenerate the "
        "thumbnail")
endif()

file(REMOVE_RECURSE "${WORKDIR}")
