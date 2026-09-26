# Verifies the packer refuses --graph (#681): the option wrote a JSON
# dependency graph nothing read, and it is gone. An option the packer does
# not know is a usage error, never silently ignored, so a script still
# passing it learns the flag no longer exists.

if(NOT DEFINED ASSET_PACKER OR NOT DEFINED SRC_GLTF OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "ASSET_PACKER, SRC_GLTF, WORKDIR required")
endif()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
set(output "${WORKDIR}/graph_option.mesh")

execute_process(
    COMMAND "${ASSET_PACKER}" "${SRC_GLTF}" "${output}"
            --graph "${WORKDIR}/graph.json"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
)
if(NOT result EQUAL 9)
    message(FATAL_ERROR
        "asset_packer did not refuse --graph as an unknown option "
        "(exit ${result})\nstdout: ${stdout}\nstderr: ${stderr}")
endif()
if(NOT stderr MATCHES "usage: asset_packer")
    message(FATAL_ERROR "the refusal printed no usage: ${stderr}")
endif()
if(stderr MATCHES "--graph")
    message(FATAL_ERROR "the usage still offers --graph: ${stderr}")
endif()
if(EXISTS "${output}" OR EXISTS "${WORKDIR}/graph.json")
    message(FATAL_ERROR "a refused invocation wrote output")
endif()

file(REMOVE_RECURSE "${WORKDIR}")
