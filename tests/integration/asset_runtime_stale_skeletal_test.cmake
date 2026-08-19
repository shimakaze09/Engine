# Verifies the runtime skeleton/animation loaders surface cooked-asset
# staleness (issue #91): a cooked .skel/.anim whose source glTF changed
# after the last cook must log a once-per-asset warning through the
# production load path by reusing the owning mesh's .meta.json sidecar,
# while a fresh cook and a sidecar-less mesh stay silent.

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
set(output "${WORKDIR}/character.mesh")

execute_process(
    COMMAND "${ASSET_PACKER}" "${source}" "${output}"
    RESULT_VARIABLE result
    ERROR_VARIABLE cook_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "cook failed: ${cook_error}")
endif()
if(NOT EXISTS "${WORKDIR}/character.skel")
    message(FATAL_ERROR "cook wrote no character.skel")
endif()
if(NOT EXISTS "${WORKDIR}/character.walk.anim")
    message(FATAL_ERROR "cook wrote no character.walk.anim")
endif()
if(NOT EXISTS "${output}.meta.json")
    message(FATAL_ERROR "cook wrote no mesh metadata sidecar")
endif()

# Boundary: a fresh cook must load the skeleton and clip without any
# staleness warning.
execute_process(
    COMMAND "${STALE_HOST}" "${WORKDIR}" "character.skel" "character.walk.anim"
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

# Change the source after the cook; the skeleton/clip loaders must surface
# staleness by way of the owning mesh's unchanged sidecar.
file(APPEND "${source}" "\n")
execute_process(
    COMMAND "${STALE_HOST}" "${WORKDIR}" "character.skel" "character.walk.anim"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stale_output
    ERROR_VARIABLE stale_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "stale-source load failed: ${stale_error}")
endif()
if(NOT stale_output MATCHES "stale cooked asset")
    message(FATAL_ERROR
        "runtime loaded a stale skeleton/clip silently: ${stale_output}")
endif()

# Boundary: the skeleton load, the clip load, and the repeated skeleton
# load all share the mesh's once-per-asset CAS entry, so exactly one
# warning fires across all three calls in the single process above.
string(REGEX MATCHALL "stale cooked asset" stale_matches "${stale_output}")
list(LENGTH stale_matches stale_count)
if(NOT stale_count EQUAL 1)
    message(FATAL_ERROR
        "expected exactly one staleness warning, got ${stale_count}")
endif()

# Boundary (#211): removing the stamped mesh sidecar while the cook stamp
# still certifies it is a torn generation, and the skeletal loads that
# route through the owning mesh must now be rejected.
file(REMOVE "${output}.meta.json")
execute_process(
    COMMAND "${STALE_HOST}" "${WORKDIR}" "character.skel" "character.walk.anim"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE torn_output
    ERROR_VARIABLE torn_error
)
if(result EQUAL 0)
    message(FATAL_ERROR
        "skeletal load accepted a torn cook generation (stamped sidecar missing)")
endif()

# Boundary: no owning mesh sidecars or cook stamp (builtin/procedural
# skeletal assets, never-certified content) loads silently.
file(REMOVE "${output}.cookstamp")
execute_process(
    COMMAND "${STALE_HOST}" "${WORKDIR}" "character.skel" "character.walk.anim"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE bare_output
    ERROR_VARIABLE bare_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "sidecar-less load failed: ${bare_error}")
endif()
if(bare_output MATCHES "stale cooked asset")
    message(FATAL_ERROR
        "sidecar-less skeleton/clip falsely reported stale: ${bare_output}")
endif()

file(REMOVE_RECURSE "${WORKDIR}")
