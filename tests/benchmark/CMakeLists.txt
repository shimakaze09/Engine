add_executable(engine_bench_ecs_perf
    ecs_perf_test.cpp
)

target_compile_features(engine_bench_ecs_perf PRIVATE cxx_std_20)

target_link_libraries(engine_bench_ecs_perf
    PRIVATE
    engine_core
    engine_math
)

add_test(
    NAME engine_bench_ecs_perf
    COMMAND engine_bench_ecs_perf
)

add_executable(engine_bench_physics_perf
    physics_perf_test.cpp
)

target_compile_features(engine_bench_physics_perf PRIVATE cxx_std_20)

target_link_libraries(engine_bench_physics_perf
    PRIVATE
    engine_runtime
)

add_test(
    NAME engine_bench_physics_perf
    COMMAND engine_bench_physics_perf
)
