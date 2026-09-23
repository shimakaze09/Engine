# Verifies an edit to any dependency the graph tracks forces a recook, not
# only to the first 64 (#571). The packer read graph-tracked dependencies
# into a 64-slot array, so a mesh with more of them was judged up to date
# however its 65th and later dependencies changed -- and since the cook
# rewrites the graph from what it read, the dropped ones left the graph
# for good.
#
# The graph keys the mesh by a hash of its path, which a script cannot
# compute, so a first cook writes the graph and the script takes the id
# from it, then adds 70 dependencies. The edited one gets the largest id:
# the 64-slot copy kept the smallest.

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
set(graph "${WORKDIR}/graph.json")

function(cook label out_var)
    execute_process(
        COMMAND "${ASSET_PACKER}" "${input}" "${output}" --graph "${graph}"
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
if(NOT EXISTS "${graph}")
    message(FATAL_ERROR "the initial cook wrote no dependency graph")
endif()

file(READ "${graph}" graph_text)
string(JSON asset_count LENGTH "${graph_text}" assets)
set(mesh_id "")
math(EXPR last_asset "${asset_count} - 1")
foreach(i RANGE ${last_asset})
    string(JSON path GET "${graph_text}" assets ${i} path)
    if(path STREQUAL input)
        string(JSON mesh_id GET "${graph_text}" assets ${i} id)
    endif()
endforeach()
if(mesh_id STREQUAL "")
    message(FATAL_ERROR "the graph does not name the mesh: ${graph_text}")
endif()

# 70 dependencies with ids 1..70; the edited one is the 70th.
set(dep_count 70)
foreach(i RANGE 1 ${dep_count})
    set(dep_path "${WORKDIR}/dep_${i}.txt")
    file(WRITE "${dep_path}" "dependency ${i}\n")
    math(EXPR id_value "${i}" OUTPUT_FORMAT HEXADECIMAL)
    string(SUBSTRING "${id_value}" 2 -1 id_hex)
    string(LENGTH "${id_hex}" id_length)
    math(EXPR pad "16 - ${id_length}")
    string(REPEAT "0" ${pad} zeros)
    set(dep_id "${zeros}${id_hex}")
    string(JSON asset_count LENGTH "${graph_text}" assets)
    string(JSON graph_text SET "${graph_text}" assets ${asset_count}
           "{\"id\":\"${dep_id}\",\"path\":\"${dep_path}\"}")
    string(JSON edge_count LENGTH "${graph_text}" edges)
    string(JSON graph_text SET "${graph_text}" edges ${edge_count}
           "{\"dependent\":\"${mesh_id}\",\"dependency\":\"${dep_id}\",\"dependentPath\":\"${input}\",\"dependencyPath\":\"${dep_path}\"}")
endforeach()
file(WRITE "${graph}" "${graph_text}")

cook("graph-extended" second_output)
cook("unchanged" third_output)
if(NOT third_output MATCHES "asset up-to-date; skipped recook")
    message(FATAL_ERROR "an unchanged cook was not skipped: ${third_output}")
endif()

file(APPEND "${WORKDIR}/dep_${dep_count}.txt" "edited\n")
cook("post-edit" fourth_output)
if(fourth_output MATCHES "asset up-to-date; skipped recook")
    message(FATAL_ERROR
        "editing the graph's 70th dependency did not force a recook: "
        "${fourth_output}")
endif()

file(REMOVE_RECURSE "${WORKDIR}")
