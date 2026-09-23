# Verifies a source whose authored sidecar cannot be read is refused, not
# cooked at the default import settings. The packer ignored the read
# result, so a sidecar whose settings were malformed -- a scale typed as a
# string -- cooked at scale 1 and reported success.

if(NOT DEFINED ASSET_PACKER OR NOT DEFINED SRC_GLTF OR NOT DEFINED SRC_BIN
   OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "ASSET_PACKER, SRC_GLTF, SRC_BIN, WORKDIR required")
endif()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
get_filename_component(gltf_name "${SRC_GLTF}" NAME)
file(COPY "${SRC_GLTF}" "${SRC_BIN}" DESTINATION "${WORKDIR}")
set(input "${WORKDIR}/${gltf_name}")
set(output "${WORKDIR}/malformed_sidecar.mesh")

file(WRITE "${input}.meta"
     "{\"schemaVersion\":1,"
     "\"guid\":\"379c998f-6eb1-4d93-b620-eeca50ff1083\","
     "\"importSettings\":{\"scaleFactor\":\"0.01\"}}\n")

execute_process(
    COMMAND "${ASSET_PACKER}" "${input}" "${output}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE cook_output
    ERROR_VARIABLE cook_error
)
if(result EQUAL 0)
    message(FATAL_ERROR
        "a malformed authored sidecar cooked at the defaults: ${cook_output}")
endif()
if(NOT cook_error MATCHES "sidecar could not be read")
    message(FATAL_ERROR "the refusal does not name the sidecar: ${cook_error}")
endif()
if(EXISTS "${output}")
    message(FATAL_ERROR "a refused cook wrote ${output}")
endif()

file(REMOVE_RECURSE "${WORKDIR}")
