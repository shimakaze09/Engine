# Verifies that asset_packer refuses a glTF accessor it cannot bound.
#
# cgltf_validate checks an accessor against its buffer view only when it has
# one. An accessor with neither a buffer view nor sparse data is legal glTF,
# reads as zeros, and is accepted with any count the JSON number parser
# produces. Sizing a buffer from that count wraps: at the fixture's count of
# 2^61 with the 8-float stride the product is exactly 2^64, so the vertex
# buffer is allocated empty and the fill loop writes into it — a heap
# overflow reachable from any imported file (issue #526, red on base with
# SIGSEGV). A small count is refused by the same rule, because the defect is
# that the accessor cannot be bounded, not that its count is large: before
# the fix that case cooked a silently all-zero mesh.

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
        "asset_packer accepted an accessor with no bufferView\n"
        "stdout: ${stdout}\n"
        "stderr: ${stderr}")
endif()

# A crash is also a non-zero result, so the diagnostic is what distinguishes
# a refusal from the overflow this test exists to prevent.
string(FIND "${stderr}" "cannot be bounded against any buffer" error_position)
if(error_position EQUAL -1)
    message(FATAL_ERROR
        "asset_packer failed without the unbounded-accessor diagnostic; a "
        "crash would also exit non-zero, so the message is the evidence "
        "that the accessor was refused rather than allocated from\n"
        "result: ${result}\n"
        "stdout: ${stdout}\n"
        "stderr: ${stderr}")
endif()

if(EXISTS "${OUTPUT}")
    message(FATAL_ERROR
        "asset_packer wrote ${OUTPUT} for an accessor it could not bound")
endif()
