# Runs the real engine_player binary and checks its exit code and output,
# for the cases a user meets: the sample project plays, headless, from a
# working directory other than the project's (#776) and exits 0; a path
# with no project, and a frame count that is not one, are refused with
# exit 1 and a reason; a project whose startup scene does not load stops
# the player with exit 3 rather than playing an empty world; the startup
# scene's on_begin_play runs once. Every case is --headless, so no error
# box waits for a click.
#
# Inputs: PLAYER (the executable), SAMPLE (the sample project directory),
# SAMPLE_SOURCE (the sample's source tree, which no test writes to, so a
# copy of it never races the scratch files other tests make in SAMPLE),
# SCRATCH (a directory this test may replace).

foreach(var PLAYER SAMPLE SAMPLE_SOURCE SCRATCH)
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

# A lane without cooked shaders logs the renderer's missing programs; what
# the player owns (the engine, the project, its scenes and scripts) must
# log no error.
set(player_errors "\\[Error\\]\\[(engine|project|scene|scripting|player|assets)\\]")
expect_player(sample 0 "player: running 'island'" "${player_errors}"
    --headless --max-frames 120 "${SAMPLE}")
expect_player(missing_project 1 "no project found" ""
    --headless "${SCRATCH}/nowhere")
expect_player(bad_frame_count 1
    "--max-frames: the frame count must be a positive whole number.*Usage: engine_player"
    "" --headless --max-frames 0)
expect_player(unknown_option 1 "--bogus: unknown option" "" --headless --bogus)

# A project like the sample whose startup scene is not a scene.
set(broken "${SCRATCH}/broken")
file(MAKE_DIRECTORY "${broken}/assets")
file(COPY "${SAMPLE}/island.project" DESTINATION "${broken}")
file(WRITE "${broken}/assets/main.scene" "{ not a scene")
file(WRITE "${broken}/assets/main.lua" "return {}\n")
expect_player(broken_startup_scene 3
    "the startup scene 'assets/main.scene' could not load; the player stops"
    "" --headless --max-frames 120 "${broken}")

# The startup scene's main script begins play exactly once at boot. The
# player used to play its first frame on the bootstrap World, so the
# script's on_begin_play ran there and then again in the loaded scene, and
# every side effect it had outside the World happened twice.
set(once "${SCRATCH}/once")
file(COPY "${SAMPLE_SOURCE}/" DESTINATION "${once}")
file(WRITE "${once}/assets/main.lua"
    "local M = {}\n"
    "function M.on_begin_play(_self)\n"
    "  engine.log('BEGIN_PLAY_MARK')\n"
    "end\n"
    "return M\n")
execute_process(
    COMMAND "${PLAYER}" --headless --max-frames 30 "${once}"
    WORKING_DIRECTORY "${SCRATCH}"
    RESULT_VARIABLE code
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    TIMEOUT 120)
string(REGEX MATCHALL "BEGIN_PLAY_MARK" marks "${out}${err}")
list(LENGTH marks markCount)
if(NOT code STREQUAL "0" OR NOT markCount EQUAL 1)
    message(FATAL_ERROR
        "begin_play_once: exit ${code}, on_begin_play ran ${markCount} "
        "time(s), expected exit 0 and once:\n${out}${err}")
endif()
message(STATUS "begin_play_once: on_begin_play ran once")

file(REMOVE_RECURSE "${SCRATCH}")
