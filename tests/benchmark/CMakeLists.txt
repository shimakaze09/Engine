engine_add_test_executable(engine_bench_ecs_perf
    SOURCES
    ecs_perf_test.cpp
    PRIVATE_DEPS
    engine_core
    engine_math
)

engine_add_test_executable(engine_bench_physics_perf
    SOURCES
    physics_perf_test.cpp
    PRIVATE_DEPS
    engine_runtime
)
