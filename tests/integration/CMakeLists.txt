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
