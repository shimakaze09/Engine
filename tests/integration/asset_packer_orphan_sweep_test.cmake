# Verifies --sweep-orphans retires pre-manifest cooked sidecars (issue
# #81): orphans sharing the output's base name are deleted, while
# manifest-listed outputs, sibling assets' files, and stem-owned sidecars
# survive; includes deletion-failure and corrupt-stamp fault injection.

if(NOT DEFINED ASSET_PACKER OR NOT DEFINED SRC_GLTF OR NOT DEFINED SRC_BIN
   OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "ASSET_PACKER, SRC_GLTF, SRC_BIN, WORKDIR required")
endif()

if(EXISTS "${WORKDIR}")
    file(CHMOD "${WORKDIR}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
endif()
file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
get_filename_component(gltf_name "${SRC_GLTF}" NAME)
file(COPY "${SRC_GLTF}" "${SRC_BIN}" DESTINATION "${WORKDIR}")
set(input "${WORKDIR}/${gltf_name}")
set(output "${WORKDIR}/coin.mesh")

execute_process(
    COMMAND "${ASSET_PACKER}" "${input}" "${output}"
    RESULT_VARIABLE result
    ERROR_VARIABLE cook_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "initial cook failed: ${cook_error}")
endif()

# Plant pre-manifest orphans plus files the sweep must never touch.
file(WRITE "${WORKDIR}/coin.old.anim" "orphan")
file(WRITE "${WORKDIR}/coin.skel" "orphan")
file(WRITE "${WORKDIR}/tree.hull" "unrelated")
file(WRITE "${WORKDIR}/coin.extra.mesh" "sibling asset")
file(WRITE "${WORKDIR}/coin.extra.mesh.hull" "sibling hull")
file(WRITE "${WORKDIR}/coin.extra.walk.anim" "sibling clip")

# Boundary: without the flag the orphans must survive (opt-in sweep).
execute_process(
    COMMAND "${ASSET_PACKER}" "${input}" "${output}"
    RESULT_VARIABLE result
    ERROR_VARIABLE noflag_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "up-to-date run failed: ${noflag_error}")
endif()
if(NOT EXISTS "${WORKDIR}/coin.old.anim")
    message(FATAL_ERROR "orphan removed without --sweep-orphans")
endif()

execute_process(
    COMMAND "${ASSET_PACKER}" "${input}" "${output}" --sweep-orphans
    RESULT_VARIABLE result
    OUTPUT_VARIABLE sweep_output
    ERROR_VARIABLE sweep_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "sweep run failed: ${sweep_error}")
endif()
if(EXISTS "${WORKDIR}/coin.old.anim")
    message(FATAL_ERROR "pre-manifest orphan coin.old.anim survived the sweep")
endif()
if(EXISTS "${WORKDIR}/coin.skel")
    message(FATAL_ERROR "pre-manifest orphan coin.skel survived the sweep")
endif()
foreach(kept "coin.mesh" "coin.mesh.meta.json" "coin.mesh.cookstamp"
        "tree.hull" "coin.extra.mesh" "coin.extra.mesh.hull"
        "coin.extra.walk.anim")
    if(NOT EXISTS "${WORKDIR}/${kept}")
        message(FATAL_ERROR "sweep deleted a protected file: ${kept}")
    endif()
endforeach()

# Boundary: a second sweep with zero orphans is a clean no-op.
execute_process(
    COMMAND "${ASSET_PACKER}" "${input}" "${output}" --sweep-orphans
    RESULT_VARIABLE result
    OUTPUT_VARIABLE noop_output
    ERROR_VARIABLE noop_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "no-op sweep failed: ${noop_error}")
endif()
if(noop_output MATCHES "removed orphan")
    message(FATAL_ERROR "no-op sweep removed something: ${noop_output}")
endif()

# Fault injection (parse boundary): a corrupted stamp forces a recook and
# the sweep still runs against the fresh manifest.
file(WRITE "${output}.cookstamp" "garbage not a stamp\n")
file(WRITE "${WORKDIR}/coin.old.anim" "orphan again")
execute_process(
    COMMAND "${ASSET_PACKER}" "${input}" "${output}" --sweep-orphans
    RESULT_VARIABLE result
    ERROR_VARIABLE corrupt_error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "corrupt-stamp sweep run failed: ${corrupt_error}")
endif()
if(EXISTS "${WORKDIR}/coin.old.anim")
    message(FATAL_ERROR "orphan survived the corrupt-stamp recook sweep")
endif()
if(NOT EXISTS "${output}")
    message(FATAL_ERROR "corrupt-stamp recook lost the cooked mesh")
endif()

# Fault injection (delete boundary, POSIX): an undeletable orphan must
# fail the sweep with a nonzero exit while the cook itself stays valid.
if(NOT CMAKE_HOST_WIN32)
    file(WRITE "${WORKDIR}/coin.old.anim" "undeletable orphan")
    file(CHMOD "${WORKDIR}" PERMISSIONS OWNER_READ OWNER_EXECUTE)
    execute_process(
        COMMAND "${ASSET_PACKER}" "${input}" "${output}" --sweep-orphans
        RESULT_VARIABLE result
        ERROR_VARIABLE locked_error
    )
    file(CHMOD "${WORKDIR}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    if(result EQUAL 0)
        message(FATAL_ERROR "sweep reported success despite a locked orphan")
    endif()
    if(NOT locked_error MATCHES "failed to remove orphan")
        message(FATAL_ERROR
            "locked-orphan sweep failed without diagnostic: ${locked_error}")
    endif()
    if(NOT EXISTS "${WORKDIR}/coin.old.anim")
        message(FATAL_ERROR "locked orphan vanished despite failure exit")
    endif()
    execute_process(
        COMMAND "${ASSET_PACKER}" "${input}" "${output}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE after_output
        ERROR_VARIABLE after_error
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "cook invalid after failed sweep: ${after_error}")
    endif()
    if(NOT after_output MATCHES "up-to-date")
        message(FATAL_ERROR
            "failed sweep invalidated the cook stamp: ${after_output}")
    endif()
endif()

file(REMOVE_RECURSE "${WORKDIR}")
