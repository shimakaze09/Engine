# Drives engine_validate's --bake-navmesh and --check-navmesh end to end:
# - a scene's NavMeshSurface is baked from the command line to its file,
#   with a sidecar identity;
# - the editor's own Bake of the same scene writes byte-identical bytes;
# - --check-navmesh passes on the baked file and fails, naming the
#   surface, once the floor collider has moved;
# - the two modes together are a usage error.
#
# Inputs: VALIDATE (engine_validate), PROBE (editor_nav_mesh_bake_probe),
# WORKDIR (scratch directory, recreated).

foreach(_var VALIDATE PROBE WORKDIR)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "${_var} not set")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORKDIR}")
set(_assets "${WORKDIR}/assets")
file(MAKE_DIRECTORY "${_assets}")
set(_scene "${_assets}/level.scene")

function(run_checked)
    cmake_parse_arguments(RUN "" "EXPECT;LABEL" "COMMAND" ${ARGN})
    execute_process(COMMAND ${RUN_COMMAND}
        RESULT_VARIABLE _code
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    if(NOT _code EQUAL RUN_EXPECT)
        message(FATAL_ERROR "${RUN_LABEL}: exit ${_code}, expected "
            "${RUN_EXPECT}\nstdout:\n${_out}\nstderr:\n${_err}")
    endif()
    set(RUN_OUT "${_out}${_err}" PARENT_SCOPE)
endfunction()

# The scene: a floor with its top at y = 0 and a surface over it.
run_checked(LABEL "write the scene" EXPECT 0
    COMMAND "${PROBE}" write-scene "${_scene}" 0)

# Before any bake the check names the missing file and fails.
run_checked(LABEL "check before a bake" EXPECT 1
    COMMAND "${VALIDATE}" --check-navmesh --assets "${_assets}" "${_scene}")
if(NOT RUN_OUT MATCHES "nav_mesh_out_of_date assets/level.navmesh")
    message(FATAL_ERROR "the check did not name the unbaked surface:\n"
        "${RUN_OUT}")
endif()

# The command-line bake writes the file and its identity.
run_checked(LABEL "bake from the command line" EXPECT 0
    COMMAND "${VALIDATE}" --bake-navmesh --assets "${_assets}" "${_scene}")
if(NOT RUN_OUT MATCHES "baked [1-9][0-9]* polygons into assets/level.navmesh")
    message(FATAL_ERROR "the bake did not report the file:\n${RUN_OUT}")
endif()
foreach(_file level.navmesh level.navmesh.meta)
    if(NOT EXISTS "${_assets}/${_file}")
        message(FATAL_ERROR "the bake did not write ${_file}")
    endif()
endforeach()

# The editor's Bake of the same scene writes the same bytes.
run_checked(LABEL "bake through the editor" EXPECT 0
    COMMAND "${PROBE}" editor-bake "${_assets}" "${_scene}"
        "assets/editor.navmesh")
file(SHA256 "${_assets}/level.navmesh" _cli_hash)
file(SHA256 "${_assets}/editor.navmesh" _editor_hash)
if(NOT _cli_hash STREQUAL _editor_hash)
    message(FATAL_ERROR "the command-line bake (${_cli_hash}) differs from "
        "the editor's (${_editor_hash})")
endif()

# The file just baked is current.
run_checked(LABEL "check after the bake" EXPECT 0
    COMMAND "${VALIDATE}" --check-navmesh --assets "${_assets}" "${_scene}")
if(NOT RUN_OUT MATCHES "nav_mesh_current assets/level.navmesh")
    message(FATAL_ERROR "the check did not report the file current:\n"
        "${RUN_OUT}")
endif()

# The floor rises half a metre: the committed mesh is stale.
run_checked(LABEL "move the floor" EXPECT 0
    COMMAND "${PROBE}" write-scene "${_scene}" 0.5)
run_checked(LABEL "check after the floor moved" EXPECT 1
    COMMAND "${VALIDATE}" --check-navmesh --assets "${_assets}" "${_scene}")
if(NOT RUN_OUT MATCHES "nav_mesh_out_of_date assets/level.navmesh.*stale")
    message(FATAL_ERROR "the check did not name the stale surface:\n"
        "${RUN_OUT}")
endif()

# Baking and checking at once is a usage error.
run_checked(LABEL "both modes" EXPECT 2
    COMMAND "${VALIDATE}" --bake-navmesh --check-navmesh --assets "${_assets}"
        "${_scene}")

file(REMOVE_RECURSE "${WORKDIR}")
message(STATUS "engine_validate navmesh bake and check: ok")
