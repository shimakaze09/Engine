# Verifies an edit to any dependency the cook tracks forces a recook, not
# only to the first 64 (#571). The packer once read tracked dependencies
# into a 64-slot array, so a mesh with more of them was judged up to date
# however its 65th and later dependencies changed. The cook takes 70
# explicit dependencies here, and the last one is edited.

if(NOT DEFINED ASSET_PACKER OR NOT DEFINED SRC_GLTF OR NOT DEFINED SRC_BIN
   OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "ASSET_PACKER, SRC_GLTF, SRC_BIN, WORKDIR required")
endif()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
get_filename_component(gltf_name "${SRC_GLTF}" NAME)
file(COPY "${SRC_GLTF}" "${SRC_BIN}" DESTINATION "${WORKDIR}")
set(input "${WORKDIR}/${gltf_name}")
set(output "${WORKDIR}/many_deps.mesh")

set(dep_count 70)
set(dep_args "")
foreach(i RANGE 1 ${dep_count})
    set(dep_path "${WORKDIR}/dep_${i}.txt")
    file(WRITE "${dep_path}" "dependency ${i}\n")
    list(APPEND dep_args --dep "${dep_path}")
endforeach()

function(cook label out_var)
    execute_process(
        COMMAND "${ASSET_PACKER}" "${input}" "${output}" ${dep_args}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE cook_output
        ERROR_VARIABLE cook_error
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${label} cook failed: ${cook_error}")
    endif()
    set(${out_var} "${cook_output}" PARENT_SCOPE)
endfunction()

cook("initial" first_output)
cook("unchanged" second_output)
if(NOT second_output MATCHES "asset up-to-date; skipped recook")
    message(FATAL_ERROR "an unchanged cook was not skipped: ${second_output}")
endif()

file(APPEND "${WORKDIR}/dep_${dep_count}.txt" "edited\n")
cook("post-edit" third_output)
if(third_output MATCHES "asset up-to-date; skipped recook")
    message(FATAL_ERROR
        "editing the 70th dependency did not force a recook: "
        "${third_output}")
endif()

file(REMOVE_RECURSE "${WORKDIR}")
