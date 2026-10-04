# Verifies a folder's mesh import settings reach the cook of the meshes
# below it:
# - a mesh block in the folder's .meta changes the source's IMPORT_HASH;
# - the source's own block overrides the folder's, so own settings equal
#   to the defaults cook to the first, default hash again;
# - a folder sidecar that will not read refuses the cook (exit 22) rather
#   than cooking at the defaults.
#
# Inputs: ASSET_PACKER, SRC_GLTF, SRC_BIN, WORKDIR (recreated).

foreach(_var ASSET_PACKER SRC_GLTF SRC_BIN WORKDIR)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "${_var} not set")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORKDIR}")
set(_folder "${WORKDIR}/props")
file(MAKE_DIRECTORY "${_folder}")
get_filename_component(_name "${SRC_GLTF}" NAME)
file(COPY "${SRC_GLTF}" "${SRC_BIN}" DESTINATION "${_folder}")
set(_source "${_folder}/${_name}")
set(_output "${_folder}/folder_settings.mesh")

execute_process(COMMAND "${ASSET_PACKER}" --init-meta "${WORKDIR}"
    RESULT_VARIABLE _code ERROR_VARIABLE _err)
if(NOT _code EQUAL 0 OR NOT EXISTS "${_folder}.meta")
    message(FATAL_ERROR "--init-meta did not identify the folder: ${_err}")
endif()
file(READ "${_folder}.meta" _folder_meta)
if(NOT _folder_meta MATCHES "\"guid\": \"([0-9a-f-]+)\"")
    message(FATAL_ERROR "the folder sidecar has no guid:\n${_folder_meta}")
endif()
set(_guid "${CMAKE_MATCH_1}")

function(cook_hash out_var label)
    execute_process(COMMAND "${ASSET_PACKER}" "${_source}" "${_output}"
        RESULT_VARIABLE _code ERROR_VARIABLE _err)
    if(NOT _code EQUAL 0)
        message(FATAL_ERROR "${label}: cook failed (${_code}): ${_err}")
    endif()
    file(READ "${_output}.cookstamp" _stamp)
    if(NOT _stamp MATCHES "IMPORT_HASH ([0-9a-f]+)")
        message(FATAL_ERROR "${label}: the stamp records no IMPORT_HASH")
    endif()
    set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

cook_hash(_defaults "the first cook")

file(WRITE "${_folder}.meta" "{\n  \"schemaVersion\": 1,\n  \"guid\": \"${_guid}\",\n  \"folder\": true,\n  \"importSettings\": {\n    \"mesh\": {\n      \"scaleFactor\": 2.5\n    }\n  }\n}\n")
cook_hash(_folder_hash "the cook under the folder's settings")
if(_folder_hash STREQUAL _defaults)
    message(FATAL_ERROR "the folder's scale did not change the cook key "
        "(${_folder_hash})")
endif()

file(READ "${_source}.meta" _source_meta)
string(REGEX REPLACE "\n}\n?$" ",\n  \"importSettings\": {\n    \"scaleFactor\": 1\n  }\n}\n" _source_meta "${_source_meta}")
file(WRITE "${_source}.meta" "${_source_meta}")
cook_hash(_own_hash "the cook under the source's own settings")
if(NOT _own_hash STREQUAL _defaults)
    message(FATAL_ERROR "the source's own default block did not override "
        "the folder's: ${_own_hash}, defaults ${_defaults}\n${_source_meta}")
endif()

file(WRITE "${_folder}.meta" "{\n  \"schemaVersion\": 1,\n  \"guid\": \"${_guid}\",\n  \"folder\": true,\n  \"importSettings\": {\"meshes\": {}}\n}\n")
file(READ "${_source}.meta" _source_meta)
string(REGEX REPLACE ",\n  \"importSettings\": {\n    \"scaleFactor\": 1\n  }" "" _source_meta "${_source_meta}")
file(WRITE "${_source}.meta" "${_source_meta}")
execute_process(COMMAND "${ASSET_PACKER}" "${_source}" "${_output}"
    RESULT_VARIABLE _code ERROR_VARIABLE _err)
if(NOT _code EQUAL 22 OR NOT _err MATCHES "could not be read; fix or remove it: [^\n]*props\\.meta")
    message(FATAL_ERROR "a malformed folder sidecar did not refuse the cook "
        "naming it (exit ${_code}): ${_err}")
endif()

file(REMOVE_RECURSE "${WORKDIR}")
message(STATUS "asset_packer folder settings: ok")
