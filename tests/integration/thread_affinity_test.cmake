# ENGINE_ASSERT_MAIN_THREAD must fire when a worker reaches a main-thread-
# only entry -- here the render device accessor, which render-prep workers
# were reaching every frame until the check existed and caught them.
#
# Two cases so the abort cannot pass for free: the same entry on the main
# thread must return normally.
#
# Inputs: HELPER.

if(NOT DEFINED HELPER)
    message(FATAL_ERROR "HELPER is required")
endif()

execute_process(COMMAND "${HELPER}" main
    OUTPUT_VARIABLE main_out ERROR_VARIABLE main_err
    RESULT_VARIABLE main_result)
if(main_out MATCHES "SKIPPED:")
    message("SKIPPED: thread-affinity checks are debug-only in this build")
    return()
endif()
if(NOT main_result EQUAL 0 OR NOT main_out MATCHES "HELPER-OK")
    message(FATAL_ERROR
        "the main thread tripped its own check (${main_result}):\n"
        "${main_out}\n${main_err}")
endif()

execute_process(COMMAND "${HELPER}" worker
    OUTPUT_VARIABLE worker_out ERROR_VARIABLE worker_err
    RESULT_VARIABLE worker_result)
if(NOT worker_out MATCHES "HELPER-READY")
    message(FATAL_ERROR "the helper never got to the worker:\n${worker_out}")
endif()
if(worker_out MATCHES "HELPER-FAILED")
    message(FATAL_ERROR
        "a worker reached the render device without the check firing:\n"
        "${worker_out}")
endif()
if(worker_result EQUAL 0)
    message(FATAL_ERROR "the worker case exited 0:\n${worker_out}")
endif()
set(combined "${worker_out}${worker_err}")
if(NOT combined MATCHES "assertion failed: ::engine::core::is_main_thread\\(\\)")
    message(FATAL_ERROR
        "the abort did not name the main-thread check:\n${combined}")
endif()
if(NOT combined MATCHES "render_device")
    message(FATAL_ERROR
        "the abort did not name the entry that was reached:\n${combined}")
endif()

message("a worker reaching the render device aborts naming the check; the "
        "main thread passes")
