# Verifies that asset_packer refuses a glTF whose accessor overruns its
# buffer view instead of cooking bytes from beyond the buffer.

if(NOT DEFINED ASSET_PACKER OR NOT DEFINED INPUT OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "ASSET_PACKER, INPUT, OUTPUT required")
endif()

file(REMOVE "${OUTPUT}")

execute_process(
    COMMAND "${ASSET_PACKER}" "${INPUT}" "${OUTPUT}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
)

if(result EQUAL 0)
    message(FATAL_ERROR
        "asset_packer unexpectedly accepted a glTF whose accessor overruns "
        "its buffer view\n"
        "stdout: ${stdout}\n"
        "stderr: ${stderr}")
endif()

string(FIND "${stderr}" "glTF failed validation (data too short)" error_position)
if(error_position EQUAL -1)
    message(FATAL_ERROR
        "asset_packer did not report the validation failure\n"
        "stdout: ${stdout}\n"
        "stderr: ${stderr}")
endif()

if(EXISTS "${OUTPUT}")
    message(FATAL_ERROR
        "asset_packer wrote ${OUTPUT} for a glTF that failed validation")
endif()
