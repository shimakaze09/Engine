engine_add_test_executable(engine_smoke
    SOURCES
    engine_smoke.cpp
    PRIVATE_DEPS
    engine_runtime
    LABELS
    gpu
)
