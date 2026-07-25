# Verifies corrupt texture inputs fail through the asset-packer CLI.

if(NOT DEFINED ASSET_PACKER OR NOT DEFINED INPUT OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "ASSET_PACKER, INPUT, and OUTPUT are required")
endif()

execute_process(
    COMMAND "${ASSET_PACKER}" "${INPUT}" "${OUTPUT}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE standard_output
    ERROR_VARIABLE standard_error
)

if(result EQUAL 0)
    message(FATAL_ERROR "asset_packer incorrectly accepted corrupt texture data")
endif()

if(NOT standard_error MATCHES "thumbnail: failed to load")
    message(FATAL_ERROR
        "asset_packer failed through the wrong path: ${standard_error}")
endif()
