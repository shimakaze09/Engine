# Runs the real engine_player binary and checks its exit code and output,
# for the cases a user meets: the sample project plays, headless, from a
# working directory other than the project's (#776) and exits 0; a path
# with no project, and a frame count that is not one, are refused with
# exit 1 and a reason; a project whose startup scene does not load stops
# the player with exit 3 rather than playing an empty world.
#
# Inputs: PLAYER (the executable), SAMPLE (the sample project directory),
# SCRATCH (a directory this test may replace).

foreach(var PLAYER SAMPLE SCRATCH)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "${var} is required")
    endif()
endforeach()

# Runs the player with ARGN and fails unless it exits `expected` with
# output matching `pattern` (and, when `forbidden` is not empty, none
# matching it).
function(expect_player case expected pattern forbidden)
    execute_process(
        COMMAND "${PLAYER}" ${ARGN}
        WORKING_DIRECTORY "${SCRATCH}"
        RESULT_VARIABLE code
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err
        TIMEOUT 120)
    set(text "${out}${err}")
    if(NOT code STREQUAL "${expected}")
        message(FATAL_ERROR
            "${case}: exit ${code}, expected ${expected}:\n${text}")
    endif()
    if(NOT text MATCHES "${pattern}")
        message(FATAL_ERROR "${case}: no \"${pattern}\" in:\n${text}")
    endif()
    if(NOT forbidden STREQUAL "" AND text MATCHES "${forbidden}")
        message(FATAL_ERROR "${case}: \"${forbidden}\" in:\n${text}")
    endif()
    message(STATUS "${case}: exit ${code} as expected")
endfunction()

file(REMOVE_RECURSE "${SCRATCH}")
file(MAKE_DIRECTORY "${SCRATCH}")

expect_player(sample 0 "player: running 'island'" "\\[Error\\]"
    --headless --max-frames 120 "${SAMPLE}")
expect_player(missing_project 1 "no project found" ""
    --headless "${SCRATCH}/nowhere")
expect_player(bad_frame_count 1
    "--max-frames: the frame count must be a positive whole number.*Usage: engine_player"
    "" --max-frames 0)
expect_player(unknown_option 1 "--bogus: unknown option" "" --bogus)

# A project like the sample whose startup scene is not a scene.
set(broken "${SCRATCH}/broken")
file(MAKE_DIRECTORY "${broken}/assets")
file(COPY "${SAMPLE}/island.project" DESTINATION "${broken}")
file(WRITE "${broken}/assets/main.scene" "{ not a scene")
file(WRITE "${broken}/assets/main.lua" "return {}\n")
expect_player(broken_startup_scene 3
    "the startup scene 'assets/main.scene' could not load; the player stops"
    "" --headless --max-frames 120 "${broken}")

file(REMOVE_RECURSE "${SCRATCH}")
