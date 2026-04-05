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
