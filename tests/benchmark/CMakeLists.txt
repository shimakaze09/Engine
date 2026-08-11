# Verifies tests behavior for the Engine test suite.

engine_add_test_executable(engine_bench_ecs_perf
    LABELS benchmark
    SOURCES
    ecs_perf_test.cpp
    PRIVATE_DEPS
    engine_core
    engine_math
)

engine_add_test_executable(engine_bench_physics_perf
    LABELS benchmark
    SOURCES
    physics_perf_test.cpp
    PRIVATE_DEPS
    engine_runtime
)

engine_add_test_executable(engine_bench_instancing_batch
    LABELS benchmark
    SOURCES
    instancing_batch_test.cpp
    PRIVATE_DEPS
    engine_renderer
)
