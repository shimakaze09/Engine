add_executable(engine_integration_ecs
    smoke_integration.cpp
)

target_compile_features(engine_integration_ecs PRIVATE cxx_std_20)

target_link_libraries(engine_integration_ecs
    PRIVATE
    engine_runtime
)

add_test(
    NAME engine_integration_ecs
    COMMAND engine_integration_ecs
)
set_tests_properties(engine_integration_ecs PROPERTIES LABELS "gpu;integration")

add_executable(engine_integration_vertical_slice
    vertical_slice_integration.cpp
)

target_compile_features(engine_integration_vertical_slice PRIVATE cxx_std_20)

target_link_libraries(engine_integration_vertical_slice
    PRIVATE
    engine_runtime
)

add_test(
    NAME engine_integration_vertical_slice
    COMMAND engine_integration_vertical_slice
)
set_tests_properties(engine_integration_vertical_slice PROPERTIES LABELS "integration")

add_executable(engine_integration_scheduler_stress
    scheduler_stress.cpp
)

target_compile_features(engine_integration_scheduler_stress PRIVATE cxx_std_20)

target_link_libraries(engine_integration_scheduler_stress
    PRIVATE
    engine_core
)

add_test(
    NAME engine_integration_scheduler_stress
    COMMAND engine_integration_scheduler_stress
)
set_tests_properties(engine_integration_scheduler_stress PROPERTIES LABELS "gpu;integration")

add_executable(engine_integration_thread_count_determinism
    thread_count_determinism.cpp
)

target_compile_features(engine_integration_thread_count_determinism PRIVATE cxx_std_20)

target_link_libraries(engine_integration_thread_count_determinism
    PRIVATE
    engine_runtime
)

add_test(
    NAME engine_integration_thread_count_determinism
    COMMAND engine_integration_thread_count_determinism
)
set_tests_properties(engine_integration_thread_count_determinism PROPERTIES LABELS "integration")

add_executable(engine_integration_determinism
    determinism_test.cpp
)

target_compile_features(engine_integration_determinism PRIVATE cxx_std_20)

target_link_libraries(engine_integration_determinism
    PRIVATE
    engine_runtime
)

add_test(
    NAME engine_integration_determinism
    COMMAND engine_integration_determinism
)
set_tests_properties(engine_integration_determinism PROPERTIES LABELS "integration")

add_executable(engine_integration_ecs_stress_50k
    ecs_stress_50k.cpp
)

target_compile_features(engine_integration_ecs_stress_50k PRIVATE cxx_std_20)

target_link_libraries(engine_integration_ecs_stress_50k
    PRIVATE
    engine_runtime
)

add_test(
    NAME engine_integration_ecs_stress_50k
    COMMAND engine_integration_ecs_stress_50k
)
set_tests_properties(engine_integration_ecs_stress_50k PROPERTIES LABELS "integration")

add_executable(engine_integration_lifecycle
    lifecycle_test.cpp
)

target_compile_features(engine_integration_lifecycle PRIVATE cxx_std_20)

target_link_libraries(engine_integration_lifecycle
    PRIVATE
    engine_runtime
)

add_test(
    NAME engine_integration_lifecycle
    COMMAND engine_integration_lifecycle
)
set_tests_properties(engine_integration_lifecycle PROPERTIES LABELS "integration")

add_executable(engine_integration_lua_lifecycle
    lua_lifecycle_test.cpp
)

target_compile_features(engine_integration_lua_lifecycle PRIVATE cxx_std_20)

target_link_libraries(engine_integration_lua_lifecycle
    PRIVATE
    engine_runtime
    engine_scripting
)

add_test(
    NAME engine_integration_lua_lifecycle
    COMMAND engine_integration_lua_lifecycle
)
set_tests_properties(engine_integration_lua_lifecycle PROPERTIES LABELS "integration")

add_executable(engine_integration_game_mode
    game_mode_test.cpp
)

target_compile_features(engine_integration_game_mode PRIVATE cxx_std_20)

target_link_libraries(engine_integration_game_mode
    PRIVATE
    engine_runtime
)

add_test(
    NAME engine_integration_game_mode
    COMMAND engine_integration_game_mode
)
set_tests_properties(engine_integration_game_mode PROPERTIES LABELS "integration")

add_executable(engine_integration_timer
    timer_test.cpp
)

target_compile_features(engine_integration_timer PRIVATE cxx_std_20)

target_link_libraries(engine_integration_timer
    PRIVATE
    engine_runtime
)

add_test(
    NAME engine_integration_timer
    COMMAND engine_integration_timer
)
set_tests_properties(engine_integration_timer PROPERTIES LABELS "integration")

add_executable(engine_integration_camera
    camera_test.cpp
)

target_compile_features(engine_integration_camera PRIVATE cxx_std_20)

target_link_libraries(engine_integration_camera
    PRIVATE
    engine_runtime
)

add_test(
    NAME engine_integration_camera
    COMMAND engine_integration_camera
)
set_tests_properties(engine_integration_camera PROPERTIES LABELS "integration")

add_executable(engine_integration_coroutine
    coroutine_test.cpp
)

target_compile_features(engine_integration_coroutine PRIVATE cxx_std_20)

target_link_libraries(engine_integration_coroutine
    PRIVATE
    engine_runtime
    engine_scripting
)

add_test(
    NAME engine_integration_coroutine
    COMMAND engine_integration_coroutine
)
set_tests_properties(engine_integration_coroutine PROPERTIES LABELS "integration")

add_executable(engine_integration_sandbox
    sandbox_test.cpp
)

target_compile_features(engine_integration_sandbox PRIVATE cxx_std_20)

target_link_libraries(engine_integration_sandbox
    PRIVATE
    engine_runtime
    engine_scripting
)

add_test(
    NAME engine_integration_sandbox
    COMMAND engine_integration_sandbox
)
set_tests_properties(engine_integration_sandbox PROPERTIES LABELS "integration")

add_executable(engine_integration_hotreload
    hotreload_test.cpp
)

target_compile_features(engine_integration_hotreload PRIVATE cxx_std_20)

target_link_libraries(engine_integration_hotreload
    PRIVATE
    engine_runtime
    engine_scripting
)

add_test(
    NAME engine_integration_hotreload
    COMMAND engine_integration_hotreload
)
set_tests_properties(engine_integration_hotreload PROPERTIES LABELS "integration")

add_executable(engine_integration_bindgen
    bindgen_test.cpp
)

target_compile_features(engine_integration_bindgen PRIVATE cxx_std_20)

target_link_libraries(engine_integration_bindgen
    PRIVATE
    engine_runtime
    engine_scripting
)

add_test(
    NAME engine_integration_bindgen
    COMMAND engine_integration_bindgen
)
set_tests_properties(engine_integration_bindgen PROPERTIES LABELS "integration")

add_executable(engine_integration_dap
    dap_test.cpp
)

target_compile_features(engine_integration_dap PRIVATE cxx_std_20)

target_link_libraries(engine_integration_dap
    PRIVATE
    engine_runtime
    engine_scripting
)

if(WIN32)
    target_link_libraries(engine_integration_dap PRIVATE ws2_32)
endif()

add_test(
    NAME engine_integration_dap
    COMMAND engine_integration_dap
)
set_tests_properties(engine_integration_dap PROPERTIES LABELS "integration")

# ---- P1-M4-B: Dependency Graph ----

add_executable(engine_integration_asset_dep_load
    asset_dep_load_test.cpp
)

target_compile_features(engine_integration_asset_dep_load PRIVATE cxx_std_20)

target_link_libraries(engine_integration_asset_dep_load
    PRIVATE
    engine_renderer
)

add_test(
    NAME engine_integration_asset_dep_load
    COMMAND engine_integration_asset_dep_load
)
set_tests_properties(engine_integration_asset_dep_load PROPERTIES LABELS "integration")
