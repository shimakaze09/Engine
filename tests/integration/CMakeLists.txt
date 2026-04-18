engine_add_test_executable(engine_integration_ecs
    SOURCES
    smoke_integration.cpp
    PRIVATE_DEPS
    engine_runtime
    LABELS
    gpu;integration
)

engine_add_test_executable(engine_integration_vertical_slice
    SOURCES
    vertical_slice_integration.cpp
    PRIVATE_DEPS
    engine_runtime
    LABELS
    integration
)

engine_add_test_executable(engine_integration_scheduler_stress
    SOURCES
    scheduler_stress.cpp
    PRIVATE_DEPS
    engine_core
    LABELS
    gpu;integration
)

engine_add_test_executable(engine_integration_thread_count_determinism
    SOURCES
    thread_count_determinism.cpp
    PRIVATE_DEPS
    engine_runtime
    LABELS
    integration
)

engine_add_test_executable(engine_integration_determinism
    SOURCES
    determinism_test.cpp
    PRIVATE_DEPS
    engine_runtime
    LABELS
    integration
)

engine_add_test_executable(engine_integration_ecs_stress_50k
    SOURCES
    ecs_stress_50k.cpp
    PRIVATE_DEPS
    engine_runtime
    LABELS
    integration
)

engine_add_test_executable(engine_integration_lifecycle
    SOURCES
    lifecycle_test.cpp
    PRIVATE_DEPS
    engine_runtime
    LABELS
    integration
)

engine_add_test_executable(engine_integration_lua_lifecycle
    SOURCES
    lua_lifecycle_test.cpp
    PRIVATE_DEPS
    engine_runtime
    engine_scripting
    LABELS
    integration
)

engine_add_test_executable(engine_integration_game_mode
    SOURCES
    game_mode_test.cpp
    PRIVATE_DEPS
    engine_runtime
    LABELS
    integration
)

engine_add_test_executable(engine_integration_timer
    SOURCES
    timer_test.cpp
    PRIVATE_DEPS
    engine_runtime
    LABELS
    integration
)

engine_add_test_executable(engine_integration_camera
    SOURCES
    camera_test.cpp
    PRIVATE_DEPS
    engine_runtime
    LABELS
    integration
)

engine_add_test_executable(engine_integration_coroutine
    SOURCES
    coroutine_test.cpp
    PRIVATE_DEPS
    engine_runtime
    engine_scripting
    LABELS
    integration
)

engine_add_test_executable(engine_integration_sandbox
    SOURCES
    sandbox_test.cpp
    PRIVATE_DEPS
    engine_runtime
    engine_scripting
    LABELS
    integration
)

# ---- P1-M4-C / P1-M4-D: Async Streaming & LRU ----

engine_add_test_executable(engine_integration_async_load
    SOURCES
    async_load_test.cpp
    PRIVATE_DEPS
    engine_renderer
    LABELS
    integration
)

engine_add_test_executable(engine_integration_streaming_budget
    SOURCES
    streaming_budget_test.cpp
    PRIVATE_DEPS
    engine_renderer
    LABELS
    integration
)

engine_add_test_executable(engine_integration_hotreload
    SOURCES
    hotreload_test.cpp
    PRIVATE_DEPS
    engine_runtime
    engine_scripting
    LABELS
    integration
)

engine_add_test_executable(engine_integration_bindgen
    SOURCES
    bindgen_test.cpp
    PRIVATE_DEPS
    engine_runtime
    engine_scripting
    LABELS
    integration
)

engine_add_test_executable(engine_integration_dap
    SOURCES
    dap_test.cpp
    PRIVATE_DEPS
    engine_runtime
    engine_scripting
    LABELS
    integration
)

if(WIN32)
    target_link_libraries(engine_integration_dap PRIVATE ws2_32)
endif()

# ---- P1-M4-B: Dependency Graph ----

engine_add_test_executable(engine_integration_asset_dep_load
    SOURCES
    asset_dep_load_test.cpp
    PRIVATE_DEPS
    engine_renderer
    LABELS
    integration
)
