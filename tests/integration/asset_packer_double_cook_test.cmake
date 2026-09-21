# Cooks the rigged character twice, each time from an empty output
# directory at the same path, and requires every file the packer wrote to
# be byte-identical between the two runs: mesh, metadata, hull, skeleton,
# clips, cook stamp and thumbnail alike. This is the determinism contract
# tested on the real asset_packer binary rather than on a copy of its
# code. The output set must include a skeleton and a clip, so a packer
# that quietly stopped producing them cannot pass on a smaller comparison.
#
# Inputs: ASSET_PACKER, SRC_GLTF and SRC_BIN (the asset and its buffer),
# WORKDIR (scratch directory; wiped first).

foreach(var ASSET_PACKER SRC_GLTF SRC_BIN WORKDIR)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "${var} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}/src")
file(COPY "${SRC_GLTF}" "${SRC_BIN}" DESTINATION "${WORKDIR}/src")
get_filename_component(gltf_name "${SRC_GLTF}" NAME)
get_filename_component(stem "${SRC_GLTF}" NAME_WE)
set(input "${WORKDIR}/src/${gltf_name}")
set(out_dir "${WORKDIR}/out")
set(output "${out_dir}/${stem}.mesh")

# Runs one clean cook into out_dir and lists what it produced, relative to
# out_dir, in the variable named by list_var.
function(cook_clean list_var)
    file(REMOVE_RECURSE "${out_dir}")
    file(MAKE_DIRECTORY "${out_dir}")
    execute_process(
        COMMAND "${ASSET_PACKER}" "${input}" "${output}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE cook_output
        ERROR_VARIABLE cook_error
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "cook failed (${result}): ${cook_output}${cook_error}")
    endif()
    file(GLOB_RECURSE produced LIST_DIRECTORIES false RELATIVE "${out_dir}"
        "${out_dir}/*")
    list(SORT produced)
    set(${list_var} "${produced}" PARENT_SCOPE)
endfunction()

cook_clean(first_files)
file(RENAME "${out_dir}" "${WORKDIR}/first")
cook_clean(second_files)

if(NOT first_files STREQUAL second_files)
    message(FATAL_ERROR "the two cooks produced different file sets:\n"
        "  first:  ${first_files}\n  second: ${second_files}")
endif()

set(saw_skeleton FALSE)
set(saw_clip FALSE)
set(saw_metadata FALSE)
set(differing "")
foreach(rel ${first_files})
    if(rel MATCHES "\\.skel$")
        set(saw_skeleton TRUE)
    elseif(rel MATCHES "\\.anim$")
        set(saw_clip TRUE)
    elseif(rel MATCHES "\\.cookmeta$")
        set(saw_metadata TRUE)
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E compare_files
            "${WORKDIR}/first/${rel}" "${out_dir}/${rel}"
        RESULT_VARIABLE same
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(NOT same EQUAL 0)
        list(APPEND differing "${rel}")
    endif()
endforeach()

if(NOT saw_skeleton OR NOT saw_clip OR NOT saw_metadata)
    message(FATAL_ERROR "the cook did not produce a skeleton, a clip and "
        "metadata (files: ${first_files}); the comparison would be over an "
        "incomplete output set")
endif()
if(differing)
    message(FATAL_ERROR "outputs differ between two clean cooks of the same "
        "input: ${differing}")
endif()
list(LENGTH first_files count)
message("both cooks produced ${count} byte-identical files: ${first_files}")
