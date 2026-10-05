# Verifies the packer cooks a source only through the importer that claims
# its type: a file of a type no importer cooks (a Lua script, a material)
# is refused by name with no output written, rather than being handed to
# the glTF parser, and a glTF still cooks through its importer.

if(NOT DEFINED ASSET_PACKER OR NOT DEFINED SRC_GLTF OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "ASSET_PACKER, SRC_GLTF, WORKDIR required")
endif()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")

foreach(source "logic.lua" "stone.mat")
    file(WRITE "${WORKDIR}/${source}" "-- not cooked\n")
    execute_process(
        COMMAND "${ASSET_PACKER}" "${WORKDIR}/${source}"
                "${WORKDIR}/${source}.out"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
    )
    if(NOT result EQUAL 2)
        message(FATAL_ERROR
            "asset_packer did not refuse ${source} (exit ${result})\n"
            "stdout: ${stdout}\nstderr: ${stderr}")
    endif()
    if(NOT stderr MATCHES "no importer cooks [^\n]*${source}")
        message(FATAL_ERROR "the refusal does not name ${source}: ${stderr}")
    endif()
    if(stderr MATCHES "glTF")
        message(FATAL_ERROR "${source} reached the glTF importer: ${stderr}")
    endif()
    if(EXISTS "${WORKDIR}/${source}.out" OR
       EXISTS "${WORKDIR}/${source}.out.cookstamp")
        message(FATAL_ERROR "a refused ${source} still wrote an output")
    endif()
endforeach()

get_filename_component(gltf_name "${SRC_GLTF}" NAME)
get_filename_component(gltf_stem "${SRC_GLTF}" NAME_WE)
get_filename_component(gltf_dir "${SRC_GLTF}" DIRECTORY)
file(COPY "${SRC_GLTF}" "${gltf_dir}/${gltf_stem}.bin" DESTINATION "${WORKDIR}")
execute_process(
    COMMAND "${ASSET_PACKER}" "${WORKDIR}/${gltf_name}" "${WORKDIR}/claimed.mesh"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
)
if(NOT result EQUAL 0 OR NOT stdout MATCHES "packed mesh: " OR
   NOT EXISTS "${WORKDIR}/claimed.mesh.cookstamp")
    message(FATAL_ERROR
        "the glTF importer did not cook and stamp ${gltf_name} (exit "
        "${result})\nstdout: ${stdout}\nstderr: ${stderr}")
endif()
