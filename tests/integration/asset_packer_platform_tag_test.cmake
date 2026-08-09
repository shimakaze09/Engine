# Verifies the platform tag in the cook key (issue #81): same-platform
# reruns skip, a different --platform recooks, a pre-platform stamp
# (no PLATFORM line) invalidates exactly once, repeated cooks stay
# byte-identical, and a whitespace tag is rejected.

if(NOT DEFINED ASSET_PACKER OR NOT DEFINED SRC_GLTF OR NOT DEFINED SRC_BIN
   OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "ASSET_PACKER, SRC_GLTF, SRC_BIN, WORKDIR required")
endif()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
get_filename_component(gltf_name "${SRC_GLTF}" NAME)
file(COPY "${SRC_GLTF}" "${SRC_BIN}" DESTINATION "${WORKDIR}")
set(input "${WORKDIR}/${gltf_name}")
set(output "${WORKDIR}/coin.mesh")
set(stamp "${output}.cookstamp")

execute_process(
    COMMAND "${ASSET_PACKER}" "${input}" "${output}"
    RESULT_VARIABLE result
    ERROR_VARIABLE cook_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "initial cook failed: ${cook_error}")
endif()
file(READ "${stamp}" stamp_text)
if(NOT stamp_text MATCHES "PLATFORM [^ \n]+\n")
    message(FATAL_ERROR "stamp carries no platform tag: ${stamp_text}")
endif()

# Boundary: rerun under the same default platform must skip the cook.
execute_process(
    COMMAND "${ASSET_PACKER}" "${input}" "${output}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE rerun_output
)
if(NOT result EQUAL 0 OR NOT rerun_output MATCHES "up-to-date")
    message(FATAL_ERROR "same-platform rerun recooked: ${rerun_output}")
endif()

# A different target platform must invalidate the cook, then stabilize.
execute_process(
    COMMAND "${ASSET_PACKER}" "${input}" "${output}" --platform WebTest
    RESULT_VARIABLE result
    OUTPUT_VARIABLE cross_output
)
if(NOT result EQUAL 0 OR NOT cross_output MATCHES "packed mesh")
    message(FATAL_ERROR "platform switch did not recook: ${cross_output}")
endif()
file(READ "${stamp}" stamp_text)
if(NOT stamp_text MATCHES "PLATFORM WebTest\n")
    message(FATAL_ERROR "stamp did not record the platform: ${stamp_text}")
endif()
execute_process(
    COMMAND "${ASSET_PACKER}" "${input}" "${output}" --platform WebTest
    RESULT_VARIABLE result
    OUTPUT_VARIABLE settle_output
)
if(NOT result EQUAL 0 OR NOT settle_output MATCHES "up-to-date")
    message(FATAL_ERROR "same --platform rerun recooked: ${settle_output}")
endif()

# Determinism: a forced same-platform recook must reproduce the cooked
# mesh and the stamp byte for byte.
file(COPY_FILE "${output}" "${WORKDIR}/coin.mesh.first")
file(COPY_FILE "${stamp}" "${WORKDIR}/coin.stamp.first")
execute_process(
    COMMAND "${ASSET_PACKER}" "${input}" "${output}" --platform WebTest --force
    RESULT_VARIABLE result
    ERROR_VARIABLE force_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "forced recook failed: ${force_error}")
endif()
execute_process(
    COMMAND ${CMAKE_COMMAND} -E compare_files
        "${output}" "${WORKDIR}/coin.mesh.first"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "recooked mesh bytes differ from the first cook")
endif()
execute_process(
    COMMAND ${CMAKE_COMMAND} -E compare_files
        "${stamp}" "${WORKDIR}/coin.stamp.first"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "recooked stamp bytes differ from the first cook")
endif()

# Migration: stripping the PLATFORM line simulates a pre-platform stamp,
# which must recook exactly once and then stay stable.
file(READ "${stamp}" stamp_text)
string(REGEX REPLACE "PLATFORM [^\n]*\n" "" stripped_text "${stamp_text}")
file(WRITE "${stamp}" "${stripped_text}")
execute_process(
    COMMAND "${ASSET_PACKER}" "${input}" "${output}" --platform WebTest
    RESULT_VARIABLE result
    OUTPUT_VARIABLE migrate_output
)
if(NOT result EQUAL 0 OR NOT migrate_output MATCHES "packed mesh")
    message(FATAL_ERROR "pre-platform stamp did not recook: ${migrate_output}")
endif()
execute_process(
    COMMAND "${ASSET_PACKER}" "${input}" "${output}" --platform WebTest
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stable_output
)
if(NOT result EQUAL 0 OR NOT stable_output MATCHES "up-to-date")
    message(FATAL_ERROR "post-migration rerun recooked: ${stable_output}")
endif()

# Malformed input: a whitespace platform tag must be rejected.
execute_process(
    COMMAND "${ASSET_PACKER}" "${input}" "${output}" --platform "two words"
    RESULT_VARIABLE result
    ERROR_VARIABLE badtag_error
)
if(result EQUAL 0)
    message(FATAL_ERROR "whitespace platform tag was accepted")
endif()
if(NOT badtag_error MATCHES "invalid platform tag")
    message(FATAL_ERROR "bad tag rejected without diagnostic: ${badtag_error}")
endif()

file(REMOVE_RECURSE "${WORKDIR}")
