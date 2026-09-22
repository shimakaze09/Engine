# Verifies the cooked mesh sidecar describes what was cooked (#571): the
# mesh and primitive the cook actually took, written signed, and the
# vertex layout of the file beside it. A negative index was written
# unsigned (18446744073709551615) and a skinned mesh was labelled
# position_normal although every skinned vertex carries texcoords, joints
# and weights.

if(NOT DEFINED ASSET_PACKER OR NOT DEFINED SRC_GLTF OR NOT DEFINED SRC_BIN
   OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "ASSET_PACKER, SRC_GLTF, SRC_BIN, WORKDIR required")
endif()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
get_filename_component(gltf_name "${SRC_GLTF}" NAME)
file(COPY "${SRC_GLTF}" "${SRC_BIN}" DESTINATION "${WORKDIR}")
set(input "${WORKDIR}/${gltf_name}")
set(output "${WORKDIR}/labels.mesh")

# An index out of range selects the first mesh; the sidecar must say so.
file(WRITE "${input}.meta"
     "{\"schemaVersion\":1,"
     "\"guid\":\"379c998f-6eb1-4d93-b620-eeca50ff1083\","
     "\"importSettings\":{\"meshIndex\":-1}}\n")

execute_process(
    COMMAND "${ASSET_PACKER}" "${input}" "${output}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE cook_output
    ERROR_VARIABLE cook_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "cook failed: ${cook_error}")
endif()

file(READ "${output}.cookmeta" meta)
string(JSON format_version GET "${meta}" assetFormatVersion)
string(JSON mesh_index GET "${meta}" importSettings meshIndex)
string(JSON primitive_index GET "${meta}" importSettings primitiveIndex)
string(JSON layout GET "${meta}" importSettings interleavedLayout)

if(NOT format_version EQUAL 3)
    message(FATAL_ERROR
        "the fixture must cook as a skinned (v3) mesh, got ${format_version}")
endif()
if(NOT mesh_index STREQUAL "0" OR NOT primitive_index STREQUAL "0")
    message(FATAL_ERROR
        "sidecar records mesh ${mesh_index} primitive ${primitive_index}, "
        "but the cook took mesh 0 primitive 0")
endif()
if(NOT layout STREQUAL "position_normal_texcoord_joints_weights")
    message(FATAL_ERROR "a skinned mesh is labelled ${layout}")
endif()

file(REMOVE_RECURSE "${WORKDIR}")
