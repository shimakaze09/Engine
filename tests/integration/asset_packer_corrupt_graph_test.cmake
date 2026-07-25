# Verifies that asset_packer does not silently discard a corrupt graph.

execute_process(
    COMMAND "${ASSET_PACKER}" "${INPUT}" "${OUTPUT}" --graph "${GRAPH}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
)

if(result EQUAL 0)
    message(FATAL_ERROR
        "asset_packer unexpectedly accepted a corrupt dependency graph\n"
        "stdout: ${stdout}\n"
        "stderr: ${stderr}")
endif()

string(FIND "${stderr}" "failed to read dependency graph" error_position)
if(error_position EQUAL -1)
    message(FATAL_ERROR
        "asset_packer did not report the corrupt dependency graph\n"
        "stdout: ${stdout}\n"
        "stderr: ${stderr}")
endif()
