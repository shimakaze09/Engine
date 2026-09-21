# Verifies the authored import-settings authority end to end: a setting
# changed through the EDITOR's real save path is the setting the COOK
# reads on its next run.
#
# This is the regression for the split that existed before: the packer
# read authored settings from the source's ".meta" while the Inspector
# read and wrote the cooked ".cookmeta", so an author could change a
# setting and watch the next cook ignore it. Writing the sidecar bytes
# directly from this script would pass even with that split restored,
# which is why the edit goes through the editor binary.

if(NOT DEFINED ASSET_PACKER OR NOT DEFINED SETTINGS_WRITER
   OR NOT DEFINED SRC_GLTF OR NOT DEFINED SRC_BIN OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR
        "ASSET_PACKER, SETTINGS_WRITER, SRC_GLTF, SRC_BIN, WORKDIR required")
endif()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
get_filename_component(gltf_name "${SRC_GLTF}" NAME)
file(COPY "${SRC_GLTF}" "${SRC_BIN}" DESTINATION "${WORKDIR}")
set(source "${WORKDIR}/${gltf_name}")
set(output "${WORKDIR}/editor_settings.mesh")

# The source needs an identity before it can carry authored settings;
# the editor refuses to mint one, which is itself the contract.
execute_process(
    COMMAND "${ASSET_PACKER}" --init-meta "${WORKDIR}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE init_output
    ERROR_VARIABLE init_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "--init-meta failed: ${init_error}")
endif()
if(NOT EXISTS "${source}.meta")
    message(FATAL_ERROR "--init-meta did not identify the source")
endif()

execute_process(
    COMMAND "${ASSET_PACKER}" "${source}" "${output}"
    RESULT_VARIABLE result
    ERROR_VARIABLE cook_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "initial cook failed: ${cook_error}")
endif()

file(READ "${output}.cookstamp" stamp_before)
if(NOT stamp_before MATCHES "IMPORT_HASH ([0-9a-f]+)")
    message(FATAL_ERROR "the first cook stamp records no IMPORT_HASH")
endif()
set(hash_before "${CMAKE_MATCH_1}")

# The edit an author makes, through the panel's own save entry point.
execute_process(
    COMMAND "${SETTINGS_WRITER}" "${source}" "3.5"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE writer_output
    ERROR_VARIABLE writer_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the editor save path failed: ${writer_error}")
endif()

# The editor must not have written authored settings anywhere else.
file(READ "${source}.meta" sidecar_text)
if(NOT sidecar_text MATCHES "3\\.5")
    message(FATAL_ERROR
        "the editor did not write the setting into the source's .meta: "
        "${sidecar_text}")
endif()

execute_process(
    COMMAND "${ASSET_PACKER}" "${source}" "${output}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE recook_output
    ERROR_VARIABLE recook_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the recook failed: ${recook_error}")
endif()
if(recook_output MATCHES "asset up-to-date")
    message(FATAL_ERROR
        "the cook ignored the editor's change: it reported the asset "
        "up-to-date after a settings edit")
endif()

file(READ "${output}.cookstamp" stamp_after)
if(NOT stamp_after MATCHES "IMPORT_HASH ([0-9a-f]+)")
    message(FATAL_ERROR "the second cook stamp records no IMPORT_HASH")
endif()
set(hash_after "${CMAKE_MATCH_1}")
if(hash_before STREQUAL hash_after)
    message(FATAL_ERROR
        "the cook key did not change, so the cook did not read the "
        "editor's setting: ${hash_before}")
endif()

# And the cook is a function of the authored value, not of having been
# run twice: putting the original setting back restores the first key.
execute_process(
    COMMAND "${SETTINGS_WRITER}" "${source}" "1"
    RESULT_VARIABLE result
    ERROR_VARIABLE restore_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the editor restore failed: ${restore_error}")
endif()
execute_process(
    COMMAND "${ASSET_PACKER}" "${source}" "${output}"
    RESULT_VARIABLE result
    ERROR_VARIABLE final_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the restoring cook failed: ${final_error}")
endif()
file(READ "${output}.cookstamp" stamp_restored)
if(NOT stamp_restored MATCHES "IMPORT_HASH ([0-9a-f]+)")
    message(FATAL_ERROR "the third cook stamp records no IMPORT_HASH")
endif()
if(NOT hash_before STREQUAL "${CMAKE_MATCH_1}")
    message(FATAL_ERROR
        "restoring the authored setting did not restore the cook key: "
        "${hash_before} became ${CMAKE_MATCH_1}")
endif()

file(REMOVE_RECURSE "${WORKDIR}")
message(STATUS "editor settings reach the cook: ${hash_before} -> ${hash_after} -> ${hash_before}")
