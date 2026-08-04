# Verifies a cook stamp without an output manifest forces a recook
# instead of certifying the outputs or failing to parse (issue #55):
# both a current-version stamp whose OUTPUT lines were stripped and a
# pre-manifest TOOL_VERSION 2 stamp must recook, and the recook must
# restore a manifest-bearing stamp.

if(NOT DEFINED ASSET_PACKER OR NOT DEFINED SRC_GLTF OR NOT DEFINED SRC_BIN
   OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "ASSET_PACKER, SRC_GLTF, SRC_BIN, WORKDIR required")
endif()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
get_filename_component(gltf_name "${SRC_GLTF}" NAME)
file(COPY "${SRC_GLTF}" "${SRC_BIN}" DESTINATION "${WORKDIR}")
set(input "${WORKDIR}/${gltf_name}")
set(output "${WORKDIR}/legacy_stamp.mesh")

execute_process(
    COMMAND "${ASSET_PACKER}" "${input}" "${output}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE cook_output
    ERROR_VARIABLE cook_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "initial cook failed: ${cook_error}")
endif()
if(NOT EXISTS "${output}.cookstamp")
    message(FATAL_ERROR "initial cook wrote no cook stamp")
endif()
file(READ "${output}.cookstamp" fresh_stamp)
if(NOT fresh_stamp MATCHES "OUTPUT ")
    message(FATAL_ERROR "fresh stamp carries no output manifest")
endif()

# Phase A: strip the manifest but keep the current tool version and all
# matching hashes; the manifest-less stamp must not certify the cook.
string(REGEX REPLACE "OUTPUT [^\n]*\n" "" stripped_stamp "${fresh_stamp}")
file(WRITE "${output}.cookstamp" "${stripped_stamp}")

execute_process(
    COMMAND "${ASSET_PACKER}" "${input}" "${output}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE strip_output
    ERROR_VARIABLE strip_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "recook after manifest strip failed: ${strip_error}")
endif()
if(strip_output MATCHES "asset up-to-date; skipped recook")
    message(FATAL_ERROR
        "manifest-less stamp was accepted as up to date: ${strip_output}")
endif()

# Phase B: a pre-manifest TOOL_VERSION 2 stamp (the legacy on-disk
# format) with matching hashes must recook exactly once.
string(REGEX REPLACE "TOOL_VERSION [0-9]+" "TOOL_VERSION 2" legacy_stamp
    "${stripped_stamp}")
file(WRITE "${output}.cookstamp" "${legacy_stamp}")

execute_process(
    COMMAND "${ASSET_PACKER}" "${input}" "${output}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE legacy_output
    ERROR_VARIABLE legacy_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "recook after legacy stamp failed: ${legacy_error}")
endif()
if(legacy_output MATCHES "asset up-to-date; skipped recook")
    message(FATAL_ERROR
        "legacy TOOL_VERSION 2 stamp was accepted as up to date: "
        "${legacy_output}")
endif()
file(READ "${output}.cookstamp" migrated_stamp)
if(NOT migrated_stamp MATCHES "OUTPUT ")
    message(FATAL_ERROR "migration recook restored no output manifest")
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
        "migrated stamp was not accepted as up to date: ${final_output}")
endif()

file(REMOVE_RECURSE "${WORKDIR}")
